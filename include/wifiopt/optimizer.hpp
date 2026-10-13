#pragma once

#include "wifiopt/blackbox.hpp"
#include "wifiopt/storage.hpp"
#include "wifiopt/wlan_client.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace wifiopt {

struct ConnectionQuality {
  int rssi_dbm{-100};
  unsigned signal_quality{};
};

struct OptimizeResult {
  bool action_taken{};
  bool would_switch{};
  bool dry_run{};
  bool success{};
  std::string message;
  std::optional<RankedAccessPoint> recommended;
  std::optional<ConnectResult> connection;
  std::optional<double> improvement;
  double threshold{};
  bool cooldown_active{};
  std::optional<ConnectionQuality> before_quality;
  std::optional<ConnectionQuality> after_quality;
  std::optional<IpVerificationResult> ip_verification;
};

enum class OptimizationMode { normal, gaming, call, manual };

std::optional<OptimizationMode> parse_optimization_mode(std::string_view value) noexcept;
std::string_view optimization_mode_name(OptimizationMode mode) noexcept;
double mode_hysteresis_threshold(const AutoConfig& config, OptimizationMode mode) noexcept;
bool mode_allows_automatic_switch(OptimizationMode mode) noexcept;

struct ProcessDetectionResult {
  bool success{};
  bool matched{};
  std::string matched_name;
  std::uint32_t error_code{};
};

ProcessDetectionResult detect_monitored_process(
    const std::vector<std::string>& process_names) noexcept;

class Optimizer {
 public:
  explicit Optimizer(Storage& storage, BlackBox* blackbox = nullptr)
      : storage_(storage), blackbox_(blackbox) {}
  std::vector<RankedAccessPoint> rank(WlanClient& wlan, const std::string& ssid);
  OptimizeResult optimize_once(WlanClient& wlan, const AutoConfig& config,
                               bool automatic = false, bool dry_run = false);
 private:
  Storage& storage_;
  BlackBox* blackbox_{};
};

bool process_is_running(std::uint32_t pid) noexcept;
bool start_daemon_process(const std::filesystem::path& executable, const std::string& ssid,
                          std::uint32_t& pid, std::string& error);
int run_daemon(const std::filesystem::path& executable, Storage& storage);

} // namespace wifiopt
