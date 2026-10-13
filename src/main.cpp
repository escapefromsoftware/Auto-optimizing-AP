#include "wifiopt/optimizer.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

using namespace wifiopt;

namespace {

void help() {
  std::cout <<
      "Smart Wi-Fi AP Optimizer 1.0\n"
      "Usage:\n"
      "  wifiopt scan [--ssid SSID]\n"
      "  wifiopt status\n"
      "  wifiopt rank [--ssid SSID]\n"
      "  wifiopt connect <BSSID> [--ssid SSID]\n"
      "  wifiopt optimize [--ssid SSID]\n"
      "  wifiopt history\n"
      "  wifiopt diagnose\n"
      "  wifiopt auto on|off|status [--ssid SSID]\n"
      "  wifiopt target [SSID]\n"
      "\nThe default SSID is AP-TUT5. Only saved Windows Wi-Fi profiles are used.\n";
}

std::string trim(std::string value) {
  const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
  const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
  const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
  if (first >= last) return {};
  return {first, last};
}

std::string option_value(int argc, char** argv, const std::string& name, std::string fallback) {
  for (int i = 1; i + 1 < argc; ++i) if (argv[i] == name) return argv[i + 1];
  return fallback;
}

bool has_option(int argc, char** argv, const std::string& name) {
  for (int i = 1; i < argc; ++i) if (argv[i] == name) return true;
  return false;
}

void print_status(const ConnectionStatus& s) {
  if (!s.connected) { std::cout << "Wi-Fi: disconnected\n"; return; }
  std::cout << "Wi-Fi: connected\nSSID: " << s.ssid << "\nBSSID: " << s.bssid
            << "\nProfile: " << s.profile << "\nSignal: " << s.signal_quality << "%";
  if (s.rssi_dbm > -100) std::cout << " (" << s.rssi_dbm << " dBm)";
  std::cout << "\nInterface: " << s.interface_name << "\n";
}

void print_ap(const AccessPoint& ap, std::size_t index = 0, double score = -1) {
  if (index) std::cout << index << ". ";
  std::cout << ap.ssid << "  " << ap.bssid << (ap.current ? "  [CURRENT]" : "");
  if (score >= 0) std::cout << "  score=" << std::fixed << std::setprecision(1) << score;
  std::cout << "\n   RSSI=" << ap.rssi_dbm << " dBm quality=" << ap.signal_quality
            << "% frequency=" << ap.frequency_khz / 1000.0 << " MHz channel=" << ap.channel
            << " co-channel=" << ap.cochannel_neighbors << "\n";
  if (ap.bss_load) {
    std::cout << "   BSS Load: stations=" << ap.bss_load->station_count
              << " utilization=" << std::lround(ap.bss_load->channel_utilization * 100.0 / 255.0)
              << "% admission_capacity=" << ap.bss_load->available_admission_capacity << "\n";
  } else std::cout << "   BSS Load: not advertised (no utilization inferred)\n";
}

std::filesystem::path self_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(size);
  return buffer;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const std::string command = argc > 1 ? argv[1] : "--help";
    if (command == "--help" || command == "-h" || command == "help") { help(); return 0; }
    Storage storage;
    auto config = storage.load_config();

    if (command == "target") {
      if (argc == 2) {
        std::cout << config.target_ssid << "\n";
        return 0;
      }
      if (argc != 3) {
        std::cerr << "Usage: wifiopt target [SSID]\n";
        return 2;
      }
      const auto ssid = trim(argv[2]);
      if (ssid.empty()) {
        std::cerr << "Target SSID must not be empty or whitespace only\n";
        return 2;
      }
      config.target_ssid = ssid;
      storage.save_config(config);
      std::cout << "Target SSID saved: " << config.target_ssid << "\n";
      return 0;
    }

    config.target_ssid = option_value(argc, argv, "--ssid", config.target_ssid);
    if (command == "daemon") return run_daemon(self_path(), storage);

    if (command == "auto") {
      if (argc < 3) { std::cerr << "Expected auto on, off, or status\n"; return 2; }
      const std::string action = argv[2];
      if (action == "on") {
        config.enabled = true;
        storage.save_config(config);
        const auto existing = storage.load_daemon_state();
        if (process_is_running(existing.pid)) {
          std::cout << "Automatic optimization is already ON (PID " << existing.pid << ")\n";
          return 0;
        }
        std::uint32_t pid = 0; std::string error;
        if (!start_daemon_process(self_path(), config.target_ssid, pid, error)) {
          config.enabled = false; storage.save_config(config);
          std::cerr << "Failed to start monitor: " << error << "\n"; return 1;
        }
        std::cout << "Automatic optimization ON; monitor PID " << pid << "\n";
        return 0;
      }
      if (action == "off") {
        config.enabled = false;
        storage.save_config(config);
        for (int i = 0; i < 30; ++i) {
          const auto state = storage.load_daemon_state();
          if (!process_is_running(state.pid)) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "Automatic optimization OFF; the current Wi-Fi connection was left unchanged\n";
        return 0;
      }
      if (action == "status") {
        const auto state = storage.load_daemon_state();
        const bool running = process_is_running(state.pid);
        std::cout << "Configured: " << (config.enabled ? "ON" : "OFF")
                  << "\nMonitor process: " << (running ? "running" : "not running")
                  << "\nState: " << state.status << "\nSSID: " << state.ssid << "\nBSSID: " << state.bssid
                  << "\nLast detail: " << state.detail << "\n";
        if (config.enabled && !running) std::cout << "Warning: setting is ON but monitor is not running; run 'wifiopt auto on' to restart it.\n";
        return running == config.enabled ? 0 : 1;
      }
      std::cerr << "Unknown auto action\n"; return 2;
    }

    WlanClient wlan;
    Optimizer optimizer(storage);
    if (command == "status") { print_status(wlan.status()); return 0; }
    if (command == "scan") {
      const bool filtered = has_option(argc, argv, "--ssid");
      const auto aps = wlan.scan(filtered ? std::optional<std::string>(config.target_ssid) : std::nullopt);
      std::cout << "Detected " << aps.size() << " BSS entries"
                << (filtered ? " for " + config.target_ssid : "") << "\n";
      for (const auto& ap : aps) print_ap(ap);
      return 0;
    }
    if (command == "rank") {
      const auto ranked = optimizer.rank(wlan, config.target_ssid);
      std::cout << "Ranked " << ranked.size() << " BSS entries for " << config.target_ssid << "\n";
      for (std::size_t i = 0; i < ranked.size(); ++i) {
        print_ap(ranked[i].ap, i + 1, ranked[i].scoring.score);
        for (const auto& reason : ranked[i].scoring.reasons) std::cout << "     - " << reason << "\n";
      }
      return 0;
    }
    if (command == "connect") {
      if (argc < 3 || !valid_bssid(argv[2])) { std::cerr << "A valid BSSID is required\n"; return 2; }
      const auto r = wlan.connect_bssid(config.target_ssid, argv[2]);
      const bool success = r.success && r.desired_bssid_matched;
      storage.append_history({unix_now(), config.target_ssid, r.requested_bssid, r.actual_bssid,
                              success, r.api_error ? r.api_error : r.reason_code, r.detail});
      std::cout << r.detail << "\nRequested: " << r.requested_bssid << "\nActual: " << r.actual_bssid << "\n";
      return success ? 0 : 1;
    }
    if (command == "optimize") {
      const auto r = optimizer.optimize_once(wlan, config);
      std::cout << r.message << "\n";
      if (r.recommended) std::cout << "Recommended: " << r.recommended->ap.bssid << " score="
                                    << std::fixed << std::setprecision(1) << r.recommended->scoring.score << "\n";
      return r.success ? 0 : 1;
    }
    if (command == "history") {
      const auto rows = storage.load_history();
      if (rows.empty()) std::cout << "No connection attempts have been recorded.\n";
      for (const auto& r : rows) std::cout << r.unix_time << "  " << (r.success ? "SUCCESS" : "FAIL")
          << "  requested=" << r.requested_bssid << " actual=" << r.actual_bssid
          << " code=" << r.error_code << "  " << r.detail << "\n";
      return 0;
    }
    if (command == "diagnose") {
      print_status(wlan.status());
      const auto rows = storage.load_history(20);
      unsigned failures = 0, eap845 = 0;
      for (const auto& r : rows) if (!r.success) { ++failures; if (r.error_code == 845) ++eap845; }
      std::cout << "Recent recorded attempts: " << rows.size() << ", failures: " << failures << "\n";
      if (eap845) std::cout << "Recorded error 845 count: " << eap845 << ". This is authentication evidence, not proof that an AP is defective or congested.\n";
      std::cout << "Config/history directory: " << storage.root().string() << "\n";
      return 0;
    }
    std::cerr << "Unknown command: " << command << "\n"; help(); return 2;
  } catch (const WifiError& e) {
    std::cerr << "Wi-Fi API error: " << e.what() << "\n";
    if (e.code() == ERROR_ACCESS_DENIED) std::cerr << "Check Windows Location services and desktop app location access.\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
