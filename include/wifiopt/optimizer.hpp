#pragma once

#include "wifiopt/storage.hpp"
#include "wifiopt/wlan_client.hpp"

#include <filesystem>
#include <optional>

namespace wifiopt {

struct OptimizeResult {
  bool action_taken{};
  bool success{};
  std::string message;
  std::optional<RankedAccessPoint> recommended;
  std::optional<ConnectResult> connection;
};

class Optimizer {
 public:
  explicit Optimizer(Storage& storage) : storage_(storage) {}
  std::vector<RankedAccessPoint> rank(WlanClient& wlan, const std::string& ssid);
  OptimizeResult optimize_once(WlanClient& wlan, const AutoConfig& config, bool force_evaluation = true);
 private:
  Storage& storage_;
};

bool process_is_running(std::uint32_t pid) noexcept;
bool start_daemon_process(const std::filesystem::path& executable, const std::string& ssid,
                          std::uint32_t& pid, std::string& error);
int run_daemon(const std::filesystem::path& executable, Storage& storage);

} // namespace wifiopt
