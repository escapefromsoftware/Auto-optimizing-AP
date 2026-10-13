#include "wifiopt/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace wifiopt {

std::optional<BssLoad> parse_bss_load(std::span<const std::uint8_t> ies) noexcept {
  std::size_t pos = 0;
  while (pos < ies.size()) {
    if (ies.size() - pos < 2) return std::nullopt;
    const auto id = ies[pos];
    const auto len = static_cast<std::size_t>(ies[pos + 1]);
    pos += 2;
    if (len > ies.size() - pos) return std::nullopt;
    if (id == 11) {
      if (len < 5) return std::nullopt;
      return BssLoad{
          static_cast<std::uint16_t>(ies[pos] | (static_cast<std::uint16_t>(ies[pos + 1]) << 8)),
          ies[pos + 2],
          static_cast<std::uint16_t>(ies[pos + 3] | (static_cast<std::uint16_t>(ies[pos + 4]) << 8))};
    }
    pos += len;
  }
  return std::nullopt;
}

std::optional<InformationElements> parse_information_elements(
    std::span<const std::uint8_t> ies) noexcept {
  InformationElements result;
  std::size_t pos = 0;
  while (pos < ies.size()) {
    if (ies.size() - pos < 2) return std::nullopt;
    const auto id = ies[pos];
    const auto len = static_cast<std::size_t>(ies[pos + 1]);
    pos += 2;
    if (len > ies.size() - pos) return std::nullopt;

    switch (id) {
      case 45:
        result.ht_capabilities = true;
        break;
      case 127:
        result.extended_capabilities = true;
        // Extended Capabilities bit 19 is octet 3, bit 3 (zero-based bit 19).
        if (len >= 3) {
          const bool supported = (ies[pos + 2] & 0x08U) != 0;
          if (!result.bss_transition_management || supported) {
            result.bss_transition_management = supported;
          }
        }
        break;
      case 191:
        result.vht_capabilities = true;
        break;
      case 221:
        ++result.vendor_specific_count;
        break;
      default:
        break;
    }
    pos += len;
  }
  return result;
}

int frequency_to_channel(std::uint32_t khz) noexcept {
  const auto mhz = static_cast<int>(khz / 1000);
  if (mhz == 2484) return 14;
  if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;
  if (mhz == 5935) return 2;
  if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5;
  if (mhz >= 5000 && mhz <= 5895) return (mhz - 5000) / 5;
  return 0;
}

std::string normalize_bssid(std::string value) {
  std::replace(value.begin(), value.end(), '-', ':');
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool valid_bssid(const std::string& value) noexcept {
  if (value.size() != 17) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if ((i + 1) % 3 == 0) {
      if (value[i] != ':' && value[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

static double clamp(double v, double lo, double hi) { return std::max(lo, std::min(v, hi)); }

ScoreResult score_access_point(const AccessPoint& ap, const ScorePolicy& policy) {
  ScoreResult result;
  const double rssi_quality = clamp((ap.rssi_dbm + 90.0) / 40.0, 0.0, 1.0);
  const double signal = 32.0 * rssi_quality + 13.0 * clamp(ap.signal_quality / 100.0, 0.0, 1.0);
  result.score += signal;
  {
    std::ostringstream s; s << "signal " << std::fixed << std::setprecision(1) << signal << "/45 ("
                            << ap.rssi_dbm << " dBm, " << ap.signal_quality << "%)";
    result.reasons.push_back(s.str());
  }

  double load = 10.0;
  if (ap.bss_load) {
    load = 20.0 * (1.0 - ap.bss_load->channel_utilization / 255.0);
    result.score += load;
    std::ostringstream s; s << "BSS channel utilization "
                            << std::lround(ap.bss_load->channel_utilization * 100.0 / 255.0)
                            << "% => " << std::fixed << std::setprecision(1) << load << "/20"
                            << "; stations=" << ap.bss_load->station_count
                            << " (station count is not traffic load)";
    result.reasons.push_back(s.str());
  } else {
    result.score += 10.0;
    result.reasons.push_back("BSS Load unavailable: neutral 10/20; utilization not invented");
  }

  const double overlap = 8.0 / (1.0 + 0.35 * ap.cochannel_neighbors);
  result.score += overlap;
  {
    std::ostringstream s; s << "co-channel neighbors " << ap.cochannel_neighbors << " => "
                            << std::fixed << std::setprecision(1) << overlap << "/8";
    result.reasons.push_back(s.str());
  }
  result.quality_score = signal + load + overlap;

  double history = 8.5;
  if (ap.history_attempts > 0) {
    const double smoothed = (ap.history_successes + 1.0) / (ap.history_attempts + 2.0);
    history = 17.0 * smoothed;
  }
  result.score += history;
  {
    std::ostringstream s; s << "connection history " << ap.history_successes << "/" << ap.history_attempts
                            << " => " << std::fixed << std::setprecision(1) << history << "/17";
    result.reasons.push_back(s.str());
  }

  const double failure_penalty = std::min(21.0, ap.recent_failures * policy.recent_failure_penalty);
  result.score -= failure_penalty;
  if (failure_penalty > 0) result.reasons.push_back("recent failures penalty -" + std::to_string(static_cast<int>(failure_penalty)));
  if (ap.current) {
    result.score += policy.current_ap_bonus;
    result.reasons.push_back("current AP hysteresis bonus +" + std::to_string(static_cast<int>(policy.current_ap_bonus)));
  }
  result.score = clamp(result.score, 0.0, 100.0);
  return result;
}

std::vector<RankedAccessPoint> rank_access_points(std::vector<AccessPoint> aps, const ScorePolicy& policy) {
  std::vector<RankedAccessPoint> ranked;
  ranked.reserve(aps.size());
  for (auto& ap : aps) ranked.push_back({std::move(ap), {}});
  for (auto& item : ranked) item.scoring = score_access_point(item.ap, policy);
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (std::abs(a.scoring.score - b.scoring.score) > 0.0001) return a.scoring.score > b.scoring.score;
    if (a.ap.current != b.ap.current) return a.ap.current;
    if (a.ap.rssi_dbm != b.ap.rssi_dbm) return a.ap.rssi_dbm > b.ap.rssi_dbm;
    return normalize_bssid(a.ap.bssid) < normalize_bssid(b.ap.bssid);
  });
  return ranked;
}

bool requested_bssid_matched(const std::string& requested, const std::string& actual) noexcept {
  return normalize_bssid(requested) == normalize_bssid(actual);
}

RetryLimiter::RetryLimiter(unsigned max_attempts, std::chrono::milliseconds minimum_interval)
    : max_attempts_(max_attempts), minimum_interval_(minimum_interval) {}

bool RetryLimiter::allow(std::chrono::steady_clock::time_point now) noexcept {
  if (attempts_ >= max_attempts_) return false;
  if (last_attempt_ && now - *last_attempt_ < minimum_interval_) return false;
  ++attempts_;
  last_attempt_ = now;
  return true;
}

} // namespace wifiopt
