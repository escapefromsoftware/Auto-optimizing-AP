#include "wifiopt/optimizer.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace wifiopt {
namespace {

std::optional<ConnectionQuality> connection_quality(const ConnectionStatus& status) {
  if (!status.connected) return std::nullopt;
  return ConnectionQuality{status.rssi_dbm, status.signal_quality};
}

std::string verification_detail(const IpVerificationResult& verification) {
  return std::string("IPv4 verification ") +
         (verification.verified ? "passed: " : "failed: ") + verification.detail;
}

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                           static_cast<int>(value.size()), result.data(), size)) return {};
  return result;
}

} // namespace

std::optional<OptimizationMode> parse_optimization_mode(std::string_view value) noexcept {
  if (value == "normal") return OptimizationMode::normal;
  if (value == "gaming") return OptimizationMode::gaming;
  if (value == "call") return OptimizationMode::call;
  if (value == "manual") return OptimizationMode::manual;
  return std::nullopt;
}

std::string_view optimization_mode_name(OptimizationMode mode) noexcept {
  switch (mode) {
    case OptimizationMode::normal: return "normal";
    case OptimizationMode::gaming: return "gaming";
    case OptimizationMode::call: return "call";
    case OptimizationMode::manual: return "manual";
  }
  return "normal";
}

double mode_hysteresis_threshold(const AutoConfig& config, OptimizationMode mode) noexcept {
  if (mode == OptimizationMode::gaming)
    return config.min_improvement + config.gaming_additional_margin;
  if (mode == OptimizationMode::call)
    return config.min_improvement + config.call_additional_margin;
  return config.min_improvement;
}

bool mode_allows_automatic_switch(OptimizationMode mode) noexcept {
  return mode != OptimizationMode::manual;
}

ProcessDetectionResult detect_monitored_process(
    const std::vector<std::string>& process_names) noexcept {
  ProcessDetectionResult result;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    result.error_code = GetLastError();
    return result;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    result.error_code = GetLastError();
    CloseHandle(snapshot);
    return result;
  }
  result.success = true;
  do {
    for (const auto& configured : process_names) {
      const auto wide = utf8_to_wide(configured);
      if (!wide.empty() && _wcsicmp(entry.szExeFile, wide.c_str()) == 0) {
        result.matched = true;
        result.matched_name = configured;
        CloseHandle(snapshot);
        return result;
      }
    }
  } while (Process32NextW(snapshot, &entry));
  const DWORD last_error = GetLastError();
  CloseHandle(snapshot);
  if (last_error != ERROR_NO_MORE_FILES) {
    result.success = false;
    result.error_code = last_error;
  }
  return result;
}

std::vector<RankedAccessPoint> Optimizer::rank(WlanClient& wlan, const std::string& ssid) {
  // Scan every SSID so co-channel neighbor counts include other networks too.
  ConnectionStatus observed_status;
  auto aps = wlan.scan(std::nullopt, std::chrono::milliseconds(2500), &observed_status);
  if (blackbox_) blackbox_->observe(make_wifi_observation(observed_status));
  storage_.record_observations(aps);
  aps.erase(std::remove_if(aps.begin(), aps.end(), [&](const auto& ap) { return ap.ssid != ssid; }), aps.end());
  storage_.enrich_with_history(aps);
  return rank_access_points(std::move(aps));
}

OptimizeResult Optimizer::optimize_once(WlanClient& wlan, const AutoConfig& config,
                                        bool automatic, bool dry_run) {
  OptimizeResult out;
  auto mode = parse_optimization_mode(config.mode).value_or(OptimizationMode::normal);
  // Manual mode constrains only daemon automation. An explicit optimize command
  // remains an intentional user action and uses the normal threshold.
  if (!automatic && mode == OptimizationMode::manual) mode = OptimizationMode::normal;
  if (automatic && !mode_allows_automatic_switch(mode)) dry_run = true;
  out.dry_run = dry_run;
  out.threshold = mode_hysteresis_threshold(config, mode);
  auto ranked = rank(wlan, config.target_ssid);
  if (ranked.empty()) { out.message = "No BSS found for SSID '" + config.target_ssid + "'"; return out; }
  out.recommended = ranked.front();
  const auto before = wlan.status();
  out.before_quality = connection_quality(before);
  if (blackbox_) blackbox_->observe(make_wifi_observation(before));
  const auto current = std::find_if(ranked.begin(), ranked.end(), [](const auto& item) { return item.ap.current; });
  if (ranked.front().ap.current) {
    out.success = true;
    out.message = "Current AP is already the highest-ranked choice";
    return out;
  }
  if (current != ranked.end()) {
    const double improvement = ranked.front().scoring.score - current->scoring.score;
    out.improvement = improvement;
    if (improvement < out.threshold) {
      std::ostringstream s;
      s << "No switch: expected improvement " << improvement << " is below hysteresis threshold "
        << out.threshold;
      out.success = true;
      out.message = s.str();
      return out;
    }
  }

  out.would_switch = true;
  if (dry_run) {
    out.success = true;
    out.message = "Dry run: would request connection to " + ranked.front().ap.bssid;
    return out;
  }

  out.action_taken = true;
  const auto connect_started = std::chrono::steady_clock::now();
  out.connection = wlan.connect_bssid(config.target_ssid, ranked.front().ap.bssid);
  const auto connect_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - connect_started);
  const auto& c = *out.connection;
  auto after = wlan.status();
  if (c.success && c.desired_bssid_matched && after.connected) {
    out.ip_verification = wait_for_interface_ipv4(after.interface_guid);
    after = wlan.status();
  }
  out.after_quality = connection_quality(after);
  out.success = c.success && c.desired_bssid_matched && out.ip_verification &&
                out.ip_verification->verified;
  std::string connection_detail = c.detail;
  if (out.ip_verification) connection_detail += "; " + verification_detail(*out.ip_verification);
  std::uint32_t error_code = c.api_error ? c.api_error : c.reason_code;
  if (!error_code && out.ip_verification && out.ip_verification->api_error)
    error_code = out.ip_verification->api_error;
  const ConnectionRecord connection_record{
      unix_now(), config.target_ssid, c.requested_bssid, c.actual_bssid, out.success,
      error_code, connection_detail,
      out.ip_verification ? std::optional<bool>(out.ip_verification->verified) : std::nullopt,
      static_cast<std::uint64_t>(connect_duration.count())};
  storage_.append_history(connection_record);
  if (blackbox_) {
    blackbox_->record_connection(connection_record, make_wifi_observation(after));
    blackbox_->observe(make_wifi_observation(after), true);
  }
  out.message = connection_detail;

  if (!out.success && before.connected && before.ssid == config.target_ssid && valid_bssid(before.bssid)) {
    // One bounded rollback attempt; never modify the saved enterprise profile.
    const auto rollback_started = std::chrono::steady_clock::now();
    auto rollback = wlan.connect_bssid(config.target_ssid, before.bssid, std::chrono::seconds(20));
    const auto rollback_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - rollback_started);
    auto after_rollback = wlan.status();
    std::optional<IpVerificationResult> rollback_ip;
    if (rollback.success && rollback.desired_bssid_matched && after_rollback.connected) {
      rollback_ip = wait_for_interface_ipv4(after_rollback.interface_guid);
      after_rollback = wlan.status();
    }
    const bool rollback_verified = rollback.success && rollback.desired_bssid_matched &&
                                   rollback_ip && rollback_ip->verified;
    std::string rollback_detail = "rollback: " + rollback.detail;
    if (rollback_ip) rollback_detail += "; " + verification_detail(*rollback_ip);
    std::uint32_t rollback_error = rollback.api_error ? rollback.api_error : rollback.reason_code;
    if (!rollback_error && rollback_ip && rollback_ip->api_error)
      rollback_error = rollback_ip->api_error;
    const ConnectionRecord rollback_record{
        unix_now(), config.target_ssid, before.bssid, rollback.actual_bssid,
        rollback_verified, rollback_error, rollback_detail,
        rollback_ip ? std::optional<bool>(rollback_ip->verified) : std::nullopt,
        static_cast<std::uint64_t>(rollback_duration.count())};
    storage_.append_history(rollback_record);
    if (blackbox_) {
      blackbox_->record_connection(rollback_record, make_wifi_observation(after_rollback));
      blackbox_->observe(make_wifi_observation(after_rollback), true);
    }
    out.message += rollback_verified
                       ? "; original BSSID restored"
                       : "; rollback did not restore verified IPv4 connectivity";
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
    BlackBox blackbox(storage.root());
    Optimizer optimizer(storage, &blackbox);
    while (true) {
      auto config = storage.load_config();
      if (!config.enabled) break;
      state.status = "monitoring";
      state.heartbeat_unix = unix_now();
      const auto current = wlan.status();
      blackbox.observe(make_wifi_observation(current));
      state.ssid = current.ssid;
      state.bssid = current.bssid;
      state.detail = "evaluating";
      storage.save_daemon_state(state);
      if (unix_now() - last_switch >= static_cast<std::int64_t>(config.cooldown_seconds)) {
        try {
          auto evaluation_config = config;
          bool safety_dry_run = config.mode == "manual";
          std::string context;
          if (config.process_detection_enabled) {
            const auto detection = detect_monitored_process(config.call_process_names);
            if (!detection.success) {
              evaluation_config.mode = "manual";
              safety_dry_run = true;
              context = "process detection failed (error " +
                        std::to_string(detection.error_code) + "); manual safety fallback; ";
            } else if (detection.matched && config.mode != "manual") {
              evaluation_config.mode = "call";
              context = "call process detected (" + detection.matched_name + "); ";
            }
          }
          const auto outcome = optimizer.optimize_once(wlan, evaluation_config, true, safety_dry_run);
          state.detail = context + outcome.message;
          if (outcome.action_taken) last_switch = unix_now();
        } catch (const std::exception& e) { state.detail = e.what(); }
      } else state.detail = "cooldown active";
      state.heartbeat_unix = unix_now();
      const auto after = wlan.status();
      blackbox.observe(make_wifi_observation(after));
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
