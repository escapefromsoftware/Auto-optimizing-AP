#include "wifiopt/optimizer.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace wifiopt {

std::vector<RankedAccessPoint> Optimizer::rank(WlanClient& wlan, const std::string& ssid) {
  // Scan every SSID so co-channel neighbor counts include other networks too.
  auto aps = wlan.scan();
  aps.erase(std::remove_if(aps.begin(), aps.end(), [&](const auto& ap) { return ap.ssid != ssid; }), aps.end());
  storage_.enrich_with_history(aps);
  return rank_access_points(std::move(aps));
}

OptimizeResult Optimizer::optimize_once(WlanClient& wlan, const AutoConfig& config, bool) {
  OptimizeResult out;
  auto ranked = rank(wlan, config.target_ssid);
  if (ranked.empty()) { out.message = "No BSS found for SSID '" + config.target_ssid + "'"; return out; }
  out.recommended = ranked.front();
  const auto before = wlan.status();
  const auto current = std::find_if(ranked.begin(), ranked.end(), [](const auto& item) { return item.ap.current; });
  if (ranked.front().ap.current) {
    out.success = true;
    out.message = "Current AP is already the highest-ranked choice";
    return out;
  }
  if (current != ranked.end()) {
    const double improvement = ranked.front().scoring.score - current->scoring.score;
    if (improvement < config.min_improvement) {
      std::ostringstream s;
      s << "No switch: expected improvement " << improvement << " is below hysteresis threshold "
        << config.min_improvement;
      out.success = true;
      out.message = s.str();
      return out;
    }
  }

  out.action_taken = true;
  out.connection = wlan.connect_bssid(config.target_ssid, ranked.front().ap.bssid);
  const auto& c = *out.connection;
  out.success = c.success && c.desired_bssid_matched;
  storage_.append_history({unix_now(), config.target_ssid, c.requested_bssid, c.actual_bssid,
                           out.success, c.api_error ? c.api_error : c.reason_code, c.detail});
  out.message = c.detail;

  if (!out.success && before.connected && before.ssid == config.target_ssid && valid_bssid(before.bssid)) {
    // One bounded rollback attempt; never modify the saved enterprise profile.
    auto rollback = wlan.connect_bssid(config.target_ssid, before.bssid, std::chrono::seconds(20));
    storage_.append_history({unix_now(), config.target_ssid, before.bssid, rollback.actual_bssid,
                             rollback.success && rollback.desired_bssid_matched,
                             rollback.api_error ? rollback.api_error : rollback.reason_code,
                             "rollback: " + rollback.detail});
    out.message += rollback.success && rollback.desired_bssid_matched
                       ? "; original BSSID restored"
                       : "; rollback did not restore the original BSSID";
  }
  return out;
}

bool process_is_running(std::uint32_t pid) noexcept {
  if (!pid) return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  DWORD exit_code = 0;
  const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return running;
}

bool start_daemon_process(const std::filesystem::path& executable, const std::string& ssid,
                          std::uint32_t& pid, std::string& error) {
  std::wstring command = L"\"" + executable.wstring() + L"\" daemon --ssid \"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, ssid.data(), static_cast<int>(ssid.size()), nullptr, 0);
  std::wstring wide_ssid(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, ssid.data(), static_cast<int>(ssid.size()), wide_ssid.data(), n);
  command += wide_ssid + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, executable.parent_path().c_str(),
                      &startup, &info)) {
    error = windows_error_message(GetLastError());
    return false;
  }
  pid = info.dwProcessId;
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return true;
}

int run_daemon(const std::filesystem::path&, Storage& storage) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SmartWifiApOptimizerDaemon-v1");
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mutex) CloseHandle(mutex);
    return 2;
  }
  DaemonState state;
  state.pid = GetCurrentProcessId();
  state.status = "starting";
  state.heartbeat_unix = unix_now();
  storage.save_daemon_state(state);
  std::int64_t last_switch = 0;
  int exit_code = 0;
  try {
    WlanClient wlan;
    Optimizer optimizer(storage);
    while (true) {
      auto config = storage.load_config();
      if (!config.enabled) break;
      state.status = "monitoring";
      state.heartbeat_unix = unix_now();
      const auto current = wlan.status();
      state.ssid = current.ssid;
      state.bssid = current.bssid;
      state.detail = "evaluating";
      storage.save_daemon_state(state);
      if (unix_now() - last_switch >= static_cast<std::int64_t>(config.cooldown_seconds)) {
        try {
          const auto outcome = optimizer.optimize_once(wlan, config);
          state.detail = outcome.message;
          if (outcome.action_taken) last_switch = unix_now();
        } catch (const std::exception& e) { state.detail = e.what(); }
      } else state.detail = "cooldown active";
      state.heartbeat_unix = unix_now();
      const auto after = wlan.status();
      state.ssid = after.ssid;
      state.bssid = after.bssid;
      storage.save_daemon_state(state);
      const unsigned half_seconds = config.interval_seconds * 2;
      for (unsigned i = 0; i < half_seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!storage.load_config().enabled) break;
      }
    }
  } catch (const std::exception& e) {
    state.detail = e.what();
    exit_code = 1;
  }
  state.status = "stopped";
  state.pid = 0;
  state.heartbeat_unix = unix_now();
  storage.save_daemon_state(state);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return exit_code;
}

} // namespace wifiopt
