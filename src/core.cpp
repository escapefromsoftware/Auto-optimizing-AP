#include "wifiopt/core.hpp"

#include <algorithm>
#include <chrono>
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

static std::string policy_number(double value) {
  std::ostringstream out;
  out << std::defaultfloat << value;
  return out.str();
}

ScoreResult score_access_point(const AccessPoint& ap, const ScorePolicy& policy) {
  ScoreResult result;
  const double rssi_quality = clamp((ap.rssi_dbm + 90.0) / 40.0, 0.0, 1.0);
  const double signal = policy.rssi_weight * rssi_quality +
                        policy.signal_quality_weight * clamp(ap.signal_quality / 100.0, 0.0, 1.0);
  result.score += signal;
  {
    std::ostringstream s; s << "signal " << std::fixed << std::setprecision(1) << signal << "/"
                            << policy_number(policy.rssi_weight + policy.signal_quality_weight) << " ("
                            << ap.rssi_dbm << " dBm, " << ap.signal_quality << "%)";
    result.reasons.push_back(s.str());
  }

  double load = policy.bss_load_neutral_score;
  if (ap.bss_load) {
    load = policy.bss_load_weight * (1.0 - ap.bss_load->channel_utilization / 255.0);
    result.score += load;
    std::ostringstream s; s << "BSS channel utilization "
                            << std::lround(ap.bss_load->channel_utilization * 100.0 / 255.0)
                            << "% => " << std::fixed << std::setprecision(1) << load << "/"
                            << policy_number(policy.bss_load_weight)
                            << "; stations=" << ap.bss_load->station_count
                            << " (station count is not traffic load)";
    result.reasons.push_back(s.str());
  } else {
    result.score += load;
    std::ostringstream s;
    s << "BSS Load unavailable: neutral " << policy_number(load) << "/"
      << policy_number(policy.bss_load_weight) << "; utilization not invented";
    result.reasons.push_back(s.str());
  }

  const double overlap = policy.channel_interference_weight /
                         (1.0 + policy.channel_interference_decay * ap.cochannel_neighbors);
  result.score += overlap;
  {
    std::ostringstream s; s << "co-channel neighbors " << ap.cochannel_neighbors << " => "
                            << std::fixed << std::setprecision(1) << overlap << "/"
                            << policy_number(policy.channel_interference_weight);
    result.reasons.push_back(s.str());
  }
  result.quality_score = signal + load + overlap;

  double history = policy.connection_history_neutral_score;
  bool decayed_history = false;
  double weighted_attempts = 0.0;
  double weighted_successes = 0.0;
  if (policy.connection_history_half_life_days && !ap.history_samples.empty()) {
    const auto reference = policy.history_reference_unix.value_or(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const double half_life_seconds = *policy.connection_history_half_life_days * 86400.0;
    for (const auto& sample : ap.history_samples) {
      const double age = static_cast<double>(std::max<std::int64_t>(0, reference - sample.unix_time));
      const double weight = std::exp2(-age / half_life_seconds);
      weighted_attempts += weight;
      if (sample.success) weighted_successes += weight;
    }
    const double smoothed = (weighted_successes + 1.0) / (weighted_attempts + 2.0);
    history = policy.connection_history_weight * smoothed;
    decayed_history = true;
  } else if (ap.history_attempts > 0) {
    const double smoothed = (ap.history_successes + 1.0) / (ap.history_attempts + 2.0);
    history = policy.connection_history_weight * smoothed;
  }
  result.score += history;
  {
    std::ostringstream s;
    if (decayed_history) {
      s << "time-decayed connection history " << std::fixed << std::setprecision(2)
        << weighted_successes << "/" << weighted_attempts;
    } else {
      s << "connection history " << ap.history_successes << "/" << ap.history_attempts;
    }
    s << " => " << std::fixed << std::setprecision(1) << history << "/"
      << policy_number(policy.connection_history_weight);
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
