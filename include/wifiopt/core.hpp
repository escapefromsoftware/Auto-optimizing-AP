#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wifiopt {

struct BssLoad {
  std::uint16_t station_count{};
  std::uint8_t channel_utilization{};
  std::uint16_t available_admission_capacity{};
};

struct InformationElements {
  bool ht_capabilities{};
  bool vht_capabilities{};
  bool extended_capabilities{};
  std::optional<bool> bss_transition_management;
  unsigned vendor_specific_count{};
};

struct ConnectionHistorySample {
  std::int64_t unix_time{};
  bool success{};
};

struct AccessPoint {
  std::string ssid;
  std::string bssid;
  int rssi_dbm{-100};
  unsigned signal_quality{};
  std::uint32_t frequency_khz{};
  int channel{};
  bool current{};
  std::optional<BssLoad> bss_load;
  std::optional<std::string> phy_standard;
  std::optional<std::uint16_t> beacon_interval_tu;
  std::optional<bool> ht_capabilities;
  std::optional<bool> vht_capabilities;
  std::optional<bool> extended_capabilities;
  std::optional<bool> bss_transition_management;
  std::optional<unsigned> vendor_specific_ie_count;
  std::optional<std::int64_t> last_seen_unix;
  std::optional<std::uint64_t> receive_link_speed_bps;
  std::optional<std::uint64_t> transmit_link_speed_bps;
  // Copied only after WlanClient validates ulIeOffset/ulIeSize against the
  // owning WLAN_BSS_LIST buffer. Used by the read-only inspect command.
  std::vector<std::uint8_t> raw_information_elements;
  unsigned cochannel_neighbors{};
  unsigned history_attempts{};
  unsigned history_successes{};
  unsigned recent_failures{};
  // Kept separate from the aggregate counters so the default score remains
  // byte-for-byte compatible. Used only by an explicitly enabled decay policy.
  std::vector<ConnectionHistorySample> history_samples;
};

struct ScoreResult {
  double score{};
  double quality_score{};
  std::vector<std::string> reasons;
};

struct RankedAccessPoint {
  AccessPoint ap;
  ScoreResult scoring;
};

struct ScorePolicy {
  double rssi_weight{32.0};
  double signal_quality_weight{13.0};
  double bss_load_weight{20.0};
  double bss_load_neutral_score{10.0};
  double channel_interference_weight{8.0};
  double channel_interference_decay{0.35};
  double connection_history_weight{17.0};
  double connection_history_neutral_score{8.5};
  double current_ap_bonus{8.0};
  double recent_failure_penalty{7.0};
  std::optional<double> connection_history_half_life_days;
  std::optional<std::int64_t> history_reference_unix;
};

struct ConnectionRecord {
  std::int64_t unix_time{};
  std::string ssid;
  std::string requested_bssid;
  std::string actual_bssid;
  bool success{};
  std::uint32_t error_code{};
  std::string detail;
  std::optional<bool> ip_address_verified;
  std::optional<std::uint64_t> duration_ms;
};

struct AutoConfig {
  bool enabled{false};
  unsigned interval_seconds{60};
  double min_improvement{12.0};
  unsigned cooldown_seconds{600};
  std::string target_ssid{"AP-TUT5"};
  std::string mode{"normal"};
  double gaming_additional_margin{8.0};
  double call_additional_margin{15.0};
  bool process_detection_enabled{false};
  std::vector<std::string> call_process_names{
      "Discord.exe", "Teams.exe", "ms-teams.exe", "Zoom.exe"};
};

struct DaemonState {
  std::uint32_t pid{};
  std::int64_t heartbeat_unix{};
  std::string status{"stopped"};
  std::string ssid;
  std::string bssid;
  std::string detail;
};

std::optional<BssLoad> parse_bss_load(std::span<const std::uint8_t> ies) noexcept;
std::optional<InformationElements> parse_information_elements(
    std::span<const std::uint8_t> ies) noexcept;
int frequency_to_channel(std::uint32_t frequency_khz) noexcept;
bool valid_bssid(const std::string& value) noexcept;
std::string normalize_bssid(std::string value);
ScoreResult score_access_point(const AccessPoint& ap, const ScorePolicy& policy = {});
std::vector<RankedAccessPoint> rank_access_points(std::vector<AccessPoint> aps,
                                                  const ScorePolicy& policy = {});
bool requested_bssid_matched(const std::string& requested, const std::string& actual) noexcept;

class RetryLimiter {
 public:
  RetryLimiter(unsigned max_attempts, std::chrono::milliseconds minimum_interval);
  bool allow(std::chrono::steady_clock::time_point now) noexcept;
  unsigned attempts() const noexcept { return attempts_; }
 private:
  unsigned max_attempts_{};
  unsigned attempts_{};
  std::chrono::milliseconds minimum_interval_{};
  std::optional<std::chrono::steady_clock::time_point> last_attempt_;
};

} // namespace wifiopt
