#include "wifiopt/gui_logic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>

namespace wifiopt {
namespace {

std::optional<std::uint64_t> bssid_number(const std::string& bssid) {
  if (!valid_bssid(bssid)) return std::nullopt;
  const auto normalized = normalize_bssid(bssid);
  std::uint64_t value = 0;
  for (char c : normalized) {
    if (c == ':') continue;
    value <<= 4;
    if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
    else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
    else return std::nullopt;
  }
  return value;
}

double success_rate(const AccessPoint& ap) {
  if (ap.history_attempts == 0) return -1.0;
  return static_cast<double>(ap.history_successes) / ap.history_attempts;
}

std::string label(std::size_t index) {
  if (index < 26) return std::string("AP ") + static_cast<char>('A' + index);
  return "AP " + std::to_string(index + 1);
}

} // namespace

PhysicalApAssessment assess_same_physical_ap(const AccessPoint& first,
                                             const AccessPoint& second) {
  PhysicalApAssessment result;
  const bool frequency_matches = first.frequency_khz != 0 &&
                                 first.frequency_khz == second.frequency_khz;
  const bool channel_matches = first.channel > 0 && first.channel == second.channel;
  const auto first_number = bssid_number(first.bssid);
  const auto second_number = bssid_number(second.bssid);
  bool bssids_close = false;
  if (first_number && second_number && *first_number != *second_number) {
    const auto difference = *first_number > *second_number
                                ? *first_number - *second_number
                                : *second_number - *first_number;
    bssids_close = difference <= 16;
  }

  if (frequency_matches) result.evidence.emplace_back("the center frequency matches");
  if (channel_matches) result.evidence.emplace_back("the channel matches");
  if (bssids_close) result.evidence.emplace_back("the BSSID values are numerically close (within 16)");
  result.possibly_same = frequency_matches && channel_matches && bssids_close;

  if (result.possibly_same) {
    result.explanation =
        "These AP entries may be radios or SSIDs provided by the same physical device because "
        "their frequency and channel match and their BSSID values are close. This is only a "
        "heuristic; BSSID similarity is not conclusive proof of common physical hardware.";
  } else {
    result.explanation =
        "The available frequency, channel, and BSSID evidence is insufficient to infer a common "
        "physical device. BSSID similarity alone would not be conclusive proof.";
  }
  return result;
}

std::string build_ap_recommendation(std::span<const RankedAccessPoint> access_points) {
  if (access_points.size() < 2) return "Select at least two access points for comparison.";
  const auto best = std::max_element(access_points.begin(), access_points.end(),
                                     [](const auto& left, const auto& right) {
                                       return left.scoring.score < right.scoring.score;
                                     });
  const auto best_index = static_cast<std::size_t>(best - access_points.begin());
  std::ostringstream out;
  out << label(best_index) << " is recommended because it has the highest selection score ("
      << std::fixed << std::setprecision(1) << best->scoring.score << ").";

  const auto strongest = std::max_element(access_points.begin(), access_points.end(),
                                          [](const auto& left, const auto& right) {
                                            return left.ap.rssi_dbm < right.ap.rssi_dbm;
                                          });
  if (strongest == best) out << " It also has the strongest RSSI among the selected APs.";
  else out << " It does not have the strongest RSSI, so the recommendation also reflects load, history, and scoring policy.";

  const double best_rate = success_rate(best->ap);
  bool has_history = false;
  bool best_history = best_rate >= 0.0;
  for (const auto& item : access_points) {
    const double rate = success_rate(item.ap);
    has_history = has_history || rate >= 0.0;
    if (rate > best_rate) best_history = false;
  }
  if (!has_history) out << " Connection-success history is unavailable for all selected APs.";
  else if (best_history) out << " Its recorded connection success rate is tied for or better than the other available histories.";
  else out << " Another AP has a better recorded success rate; review that trade-off before switching.";

  if (std::all_of(access_points.begin(), access_points.end(),
                  [](const auto& item) { return !item.ap.bss_load.has_value(); })) {
    out << " BSS Load information is unavailable for all selected APs.";
  } else if (std::any_of(access_points.begin(), access_points.end(),
                         [](const auto& item) { return !item.ap.bss_load.has_value(); })) {
    out << " BSS Load information is unavailable for some selected APs, so load comparisons are incomplete.";
  }
  return out.str();
}

} // namespace wifiopt
