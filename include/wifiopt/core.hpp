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

struct AccessPoint {
  std::string ssid;
  std::string bssid;
  int rssi_dbm{-100};
  unsigned signal_quality{};
  std::uint32_t frequency_khz{};
  int channel{};
  bool current{};
  std::optional<BssLoad> bss_load;
  unsigned cochannel_neighbors{};
  unsigned history_attempts{};
  unsigned history_successes{};
  unsigned recent_failures{};
};

struct ScoreResult {
  double score{};
  std::vector<std::string> reasons;
};

struct RankedAccessPoint {
  AccessPoint ap;
  ScoreResult scoring;
};

struct ScorePolicy {
  double current_ap_bonus{8.0};
  double recent_failure_penalty{7.0};
};

struct ConnectionRecord {
  std::int64_t unix_time{};
  std::string ssid;
  std::string requested_bssid;
  std::string actual_bssid;
  bool success{};
  std::uint32_t error_code{};
  std::string detail;
};

struct AutoConfig {
  bool enabled{false};
  unsigned interval_seconds{60};
  double min_improvement{12.0};
  unsigned cooldown_seconds{600};
  std::string target_ssid{"AP-TUT5"};
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
