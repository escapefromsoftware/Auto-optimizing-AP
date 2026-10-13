#include "wifiopt/cli.hpp"
#include "wifiopt/blackbox.hpp"
#include "wifiopt/eventlog.hpp"
#include "wifiopt/optimizer.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using namespace wifiopt;

namespace {

std::atomic_bool stop_watch{false};

BOOL WINAPI console_handler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
    stop_watch = true;
    return TRUE;
  }
  return FALSE;
}

void help() {
  std::cout <<
      "Smart Wi-Fi AP Optimizer 1.0\n"
      "Usage:\n"
      "  wifiopt scan [--ssid SSID] [--format text|json|csv|ndjson]\n"
      "  wifiopt status [--format text|json|csv|ndjson]\n"
      "  wifiopt rank [--ssid SSID] [--format text|json|csv|ndjson]\n"
      "  wifiopt inspect <BSSID> [--ssid SSID] [--verbose] [--raw-ie] [--format text|json]\n"
      "  wifiopt connect <BSSID> [--ssid SSID]\n"
      "  wifiopt optimize [--ssid SSID] [--dry-run] [--explain] [--format text|json]\n"
      "  wifiopt history [--stats] [--format text|json|csv|ndjson]\n"
      "  wifiopt blackbox [--since 30m|24h|7d] [--format text|json]\n"
      "  wifiopt diagnose [--since 30m|24h|7d] [--report] [--json] [--format text|json|csv|ndjson]\n"
      "  wifiopt simulate --input FILE [--weights FILE] [--compare] [--explain] [--format text|json]\n"
      "  wifiopt watch --interval SECONDS|Ns|Nm [--duration Ns|Nm|Nh|Nd] [--ssid SSID] [--format text|ndjson]\n"
      "  wifiopt auto on|off|status [--ssid SSID]\n"
      "  wifiopt target [SSID]\n"
      "\nCommon output flags: --quiet --no-color\n"
      "The default SSID is AP-TUT5. Only saved Windows Wi-Fi profiles are used.\n";
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

std::optional<OutputFormat> requested_format(int argc, char** argv, OutputFormat fallback = OutputFormat::text) {
  if (has_option(argc, argv, "--json")) return OutputFormat::json;
  const auto value = option_value(argc, argv, "--format", "");
  if (value.empty()) return fallback;
  return parse_output_format(value);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void print_status(const ConnectionStatus& s) {
  if (!s.connected) { std::cout << "Wi-Fi: disconnected\n"; return; }
  std::cout << "Wi-Fi: connected\nSSID: " << s.ssid << "\nBSSID: " << s.bssid
            << "\nProfile: " << s.profile << "\nSignal: " << s.signal_quality << "%";
  if (s.rssi_dbm > -100) std::cout << " (" << s.rssi_dbm << " dBm)";
  std::cout << "\nInterface: " << s.interface_name << "\n";
}

std::string status_json(const ConnectionStatus& s) {
  std::ostringstream out;
  out << "{\"connected\":" << (s.connected ? "true" : "false")
      << ",\"ssid\":\"" << json_escape(s.ssid) << "\",\"bssid\":\"" << json_escape(s.bssid)
      << "\",\"profile\":\"" << json_escape(s.profile) << "\",\"rssi_dbm\":" << s.rssi_dbm
      << ",\"signal_quality\":" << s.signal_quality << ",\"interface\":\"" << json_escape(s.interface_name)
      << "\",\"receive_link_speed_bps\":";
  if (s.receive_link_speed_bps) out << *s.receive_link_speed_bps; else out << "null";
  out << ",\"transmit_link_speed_bps\":";
  if (s.transmit_link_speed_bps) out << *s.transmit_link_speed_bps; else out << "null";
  out << '}';
  return out.str();
}

void print_status_formatted(const ConnectionStatus& s, OutputFormat format) {
  if (format == OutputFormat::text) { print_status(s); return; }
  if (format == OutputFormat::json)
    std::cout << "{\"schema_version\":1,\"command\":\"status\",\"data\":" << status_json(s) << "}\n";
  else if (format == OutputFormat::ndjson)
    std::cout << "{\"schema_version\":1,\"command\":\"status\",\"data\":" << status_json(s) << "}\n";
  else {
    std::cout << "connected,ssid,bssid,profile,rssi_dbm,signal_quality,interface,receive_link_speed_bps,transmit_link_speed_bps\n"
              << (s.connected ? "true" : "false") << ',' << csv_field(s.ssid) << ',' << csv_field(s.bssid)
              << ',' << csv_field(s.profile) << ',' << s.rssi_dbm << ',' << s.signal_quality << ','
              << csv_field(s.interface_name) << ',';
    if (s.receive_link_speed_bps) std::cout << *s.receive_link_speed_bps;
    std::cout << ',';
    if (s.transmit_link_speed_bps) std::cout << *s.transmit_link_speed_bps;
    std::cout << '\n';
  }
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

void print_ap_csv_header(bool ranked) {
  std::cout << "ssid,bssid,rssi_dbm,signal_quality,frequency_khz,channel,band,current,cochannel_neighbors,"
               "station_count,channel_utilization,phy_standard,beacon_interval_tu";
  if (ranked) std::cout << ",quality_score,score,reasons";
  std::cout << '\n';
}

void print_ap_csv(const AccessPoint& ap, const ScoreResult* score = nullptr) {
  std::cout << csv_field(ap.ssid) << ',' << csv_field(ap.bssid) << ',' << ap.rssi_dbm << ','
            << ap.signal_quality << ',' << ap.frequency_khz << ',' << ap.channel << ','
            << csv_field(band_name(ap.frequency_khz)) << ',' << (ap.current ? "true" : "false") << ','
            << ap.cochannel_neighbors << ',';
  if (ap.bss_load) std::cout << ap.bss_load->station_count << ',' << static_cast<unsigned>(ap.bss_load->channel_utilization);
  else std::cout << ',';
  std::cout << ',' << csv_field(ap.phy_standard.value_or("")) << ',';
  if (ap.beacon_interval_tu) std::cout << *ap.beacon_interval_tu;
  if (score) {
    std::string reasons;
    for (std::size_t i = 0; i < score->reasons.size(); ++i) {
      if (i) reasons += " | ";
      reasons += score->reasons[i];
    }
    std::cout << ',' << std::setprecision(15) << score->quality_score << ',' << score->score << ',' << csv_field(reasons);
  }
  std::cout << '\n';
}

void print_ranked_data(const std::vector<RankedAccessPoint>& ranked, OutputFormat format,
                       const std::string& command, bool text_reasons = true, bool json_reasons = true) {
  if (format == OutputFormat::json) {
    std::cout << "{\"schema_version\":1,\"command\":\"" << command << "\",\"data\":[";
    for (std::size_t i = 0; i < ranked.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << access_point_json(ranked[i].ap, &ranked[i].scoring, json_reasons);
    }
    std::cout << "]}\n";
  } else if (format == OutputFormat::ndjson) {
    for (const auto& item : ranked)
      std::cout << "{\"schema_version\":1,\"command\":\"" << command << "\",\"data\":"
                << access_point_json(item.ap, &item.scoring, json_reasons) << "}\n";
  } else if (format == OutputFormat::csv) {
    print_ap_csv_header(true);
    for (const auto& item : ranked) print_ap_csv(item.ap, &item.scoring);
  } else {
    for (std::size_t i = 0; i < ranked.size(); ++i) {
      print_ap(ranked[i].ap, i + 1, ranked[i].scoring.score);
      if (text_reasons)
        for (const auto& reason : ranked[i].scoring.reasons) std::cout << "     - " << reason << "\n";
    }
  }
}

std::filesystem::path self_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(size);
  return buffer;
}

int invalid_format() {
  std::cerr << "Invalid --format value\n";
  return 2;
}

void print_history_stats_text(const HistoryStats& stats) {
  std::cout << "Observation period: ";
  if (stats.first_unix) std::cout << *stats.first_unix << " to " << *stats.last_unix;
  else std::cout << "not available";
  std::cout << "\nSamples: " << stats.samples << "\nAP switches: " << stats.ap_switches << "\n"
            << "Average connection time: unavailable (not recorded)\n";
  std::cout << "Success rate by BSSID:\n";
  if (stats.by_bssid.empty()) std::cout << "  (none)\n";
  for (const auto& [bssid, value] : stats.by_bssid) {
    const double percent = value.attempts ? value.successes * 100.0 / value.attempts : 0.0;
    std::cout << "  " << bssid << ": " << value.successes << '/' << value.attempts
              << " (" << std::fixed << std::setprecision(1) << percent << "%)\n";
  }
  std::cout << "Error code frequency:\n";
  if (stats.error_counts.empty()) std::cout << "  (none)\n";
  for (const auto& [code, count] : stats.error_counts) std::cout << "  " << code << ": " << count << "\n";
}

std::string history_stats_json(const HistoryStats& stats) {
  std::ostringstream out;
  out << "{\"observation_start_unix\":";
  if (stats.first_unix) out << *stats.first_unix; else out << "null";
  out << ",\"observation_end_unix\":";
  if (stats.last_unix) out << *stats.last_unix; else out << "null";
  out << ",\"samples\":" << stats.samples << ",\"ap_switches\":" << stats.ap_switches
      << ",\"average_connection_time_ms\":null,\"by_bssid\":[";
  bool first = true;
  for (const auto& [bssid, value] : stats.by_bssid) {
    if (!first) out << ','; first = false;
    out << "{\"bssid\":\"" << bssid << "\",\"attempts\":" << value.attempts
        << ",\"successes\":" << value.successes << ",\"success_rate\":"
        << (value.attempts ? value.successes * 1.0 / value.attempts : 0.0) << '}';
  }
  out << "],\"errors\":["; first = true;
  for (const auto& [code, count] : stats.error_counts) {
    if (!first) out << ','; first = false;
    out << "{\"code\":" << code << ",\"count\":" << count << '}';
  }
  out << "]}";
  return out.str();
}

std::string wlan_event_json(const WlanEvent& event) {
  std::ostringstream out;
  out << "{\"unix_time\":" << event.unix_time << ",\"event_id\":" << event.event_id
      << ",\"ssid\":\"" << json_escape(event.ssid) << "\",\"bssid\":\""
      << json_escape(event.bssid) << "\",\"error_code\":";
  if (event.error_code) out << *event.error_code; else out << "null";
  out << ",\"description\":\"" << json_escape(event.description) << "\"}";
  return out.str();
}

void print_blackbox_record_text(const BlackBoxRecord& record) {
  std::cout << record.unix_time << "  " << blackbox_event_type_name(record.event_type)
            << "  code=" << record.error_code;
  if (!record.ssid.empty()) std::cout << "  ssid=" << record.ssid;
  if (!record.related_bssid.empty()) std::cout << "  bssid=" << record.related_bssid;
  std::cout << "\n  Detail: " << record.detail
            << "\n  Possible cause: " << record.possible_cause
            << "\n  Recommended action: " << record.recommended_action
            << "\n  Prior observations: " << record.observations_before.size() << "\n";
  for (const auto& observation : record.observations_before) {
    std::cout << "    " << observation.unix_time << " "
              << (observation.connected ? "connected" : "disconnected");
    if (!observation.ssid.empty()) std::cout << " ssid=" << observation.ssid;
    if (!observation.bssid.empty()) std::cout << " bssid=" << observation.bssid;
    std::cout << " rssi=" << observation.rssi_dbm
              << " quality=" << observation.signal_quality << "%\n";
  }
}

int run_blackbox(int argc, char** argv, BlackBox& blackbox, bool quiet) {
  const auto format = requested_format(argc, argv);
  if (!format || (*format != OutputFormat::text && *format != OutputFormat::json))
    return invalid_format();
  auto records = blackbox.load_records();
  if (has_option(argc, argv, "--since")) {
    const auto duration = parse_duration_seconds(option_value(argc, argv, "--since", ""));
    if (!duration) {
      std::cerr << "Invalid --since duration; use values such as 30m, 24h, or 7d\n";
      return 2;
    }
    const auto cutoff = unix_now() - static_cast<std::int64_t>(*duration);
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const auto& record) { return record.unix_time < cutoff; }),
                  records.end());
  }
  if (quiet) return 0;
  if (*format == OutputFormat::json) {
    std::cout << "{\"schema_version\":1,\"command\":\"blackbox\",\"data\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << blackbox_record_json(records[i]);
    }
    std::cout << "]}\n";
  } else if (records.empty()) {
    std::cout << "No Black Box failure records have been recorded.\n";
  } else {
    std::cout << "Black Box records: " << records.size() << "\n";
    for (const auto& record : records) print_blackbox_record_text(record);
  }
  return 0;
}

const char* optional_bool_text(const std::optional<bool>& value) {
  if (!value) return "unavailable";
  return *value ? "yes" : "no";
}

int run_simulate(int argc, char** argv, bool quiet) {
  const auto input = option_value(argc, argv, "--input", "");
  if (input.empty()) { std::cerr << "simulate requires --input <file>\n"; return 2; }
  const auto format = requested_format(argc, argv);
  if (!format || (*format != OutputFormat::text && *format != OutputFormat::json)) return invalid_format();
  try {
    const auto aps = parse_scan_json_document(read_file(input));
    const auto weights = option_value(argc, argv, "--weights", "");
    const auto policy = weights.empty() ? ScorePolicy{} : parse_score_policy_json(read_file(weights));
    const auto ranked = rank_access_points(aps, policy);
    const bool compare = has_option(argc, argv, "--compare");
    const bool explain = has_option(argc, argv, "--explain");
    const auto baseline = compare ? rank_access_points(aps) : std::vector<RankedAccessPoint>{};
    if (quiet) return 0;
    if (*format == OutputFormat::json) {
      std::cout << "{\"schema_version\":1,\"command\":\"simulate\",\"data\":{\"ranking\":[";
      for (std::size_t i = 0; i < ranked.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << access_point_json(ranked[i].ap, &ranked[i].scoring, explain);
      }
      std::cout << ']';
      if (compare) {
        std::cout << ",\"default_ranking\":[";
        for (std::size_t i = 0; i < baseline.size(); ++i) {
          if (i) std::cout << ',';
          std::cout << "{\"rank\":" << i + 1 << ",\"bssid\":\"" << baseline[i].ap.bssid << "\",\"score\":"
                    << baseline[i].scoring.score << '}';
        }
        std::cout << ']';
      }
      std::cout << "}}\n";
    } else {
      std::cout << "Simulated ranking of " << ranked.size() << " BSS entries\n";
      print_ranked_data(ranked, OutputFormat::text, "simulate", explain, explain);
      if (compare) {
        std::cout << "Comparison (default -> selected weights):\n";
        for (const auto& item : ranked) {
          const auto old = std::find_if(baseline.begin(), baseline.end(), [&](const auto& candidate) {
            return requested_bssid_matched(candidate.ap.bssid, item.ap.bssid) && candidate.ap.ssid == item.ap.ssid;
          });
          std::cout << "  " << item.ap.bssid << ": ";
          if (old != baseline.end()) std::cout << std::distance(baseline.begin(), old) + 1;
          else std::cout << '?';
          std::cout << " -> " << (&item - ranked.data()) + 1 << "\n";
        }
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "simulate input error: " << e.what() << "\n";
    return 2;
  }
}

int run_watch(int argc, char** argv, Storage& storage, BlackBox& blackbox,
              AutoConfig config, bool quiet) {
  const auto interval_text = option_value(argc, argv, "--interval", "");
  const auto interval = parse_duration_seconds(interval_text, true);
  if (!interval || *interval < 5) {
    std::cerr << "watch --interval must be at least 5 seconds (for example 5, 5s, or 1m)\n";
    return 2;
  }
  std::optional<std::uint64_t> duration;
  if (has_option(argc, argv, "--duration")) {
    duration = parse_duration_seconds(option_value(argc, argv, "--duration", ""), false);
    if (!duration) { std::cerr << "Invalid --duration; use values such as 15s, 30m, or 1h\n"; return 2; }
  }
  const auto format = requested_format(argc, argv, OutputFormat::text);
  if (!format || (*format != OutputFormat::text && *format != OutputFormat::ndjson)) return invalid_format();
  stop_watch = false;
  SetConsoleCtrlHandler(console_handler, TRUE);
  WlanClient wlan;
  Optimizer optimizer(storage, &blackbox);
  const auto started = std::chrono::steady_clock::now();
  while (!stop_watch) {
    const auto ranked = optimizer.rank(wlan, config.target_ssid);
    if (!quiet) {
      const auto observed = unix_now();
      if (*format == OutputFormat::ndjson) {
        std::cout << "{\"schema_version\":1,\"command\":\"watch\",\"observed_unix\":" << observed
                  << ",\"ssid\":\"" << json_escape(config.target_ssid) << "\",\"data\":[";
        for (std::size_t i = 0; i < ranked.size(); ++i) {
          if (i) std::cout << ',';
          std::cout << access_point_json(ranked[i].ap, &ranked[i].scoring, false);
        }
        std::cout << "]}\n" << std::flush;
      } else {
        std::cout << "[" << observed << "] " << config.target_ssid << ": " << ranked.size() << " BSS entries";
        if (!ranked.empty()) std::cout << ", best=" << ranked.front().ap.bssid << " score="
                                       << std::fixed << std::setprecision(1) << ranked.front().scoring.score;
        std::cout << "\n" << std::flush;
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    if (duration && elapsed.count() >= static_cast<long long>(*duration)) break;
    auto remaining = std::chrono::seconds(*interval);
    if (duration) {
      const auto until_end = std::chrono::seconds(*duration) - elapsed;
      if (until_end < remaining) remaining = until_end;
    }
    const auto wake = std::chrono::steady_clock::now() + remaining;
    while (!stop_watch && std::chrono::steady_clock::now() < wake)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (duration && std::chrono::steady_clock::now() - started >= std::chrono::seconds(*duration)) break;
  }
  SetConsoleCtrlHandler(console_handler, FALSE);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const std::string command = argc > 1 ? argv[1] : "--help";
    if (command == "--help" || command == "-h" || command == "help") { help(); return 0; }
    const bool quiet = has_option(argc, argv, "--quiet");
    (void)has_option(argc, argv, "--no-color"); // Reserved; current output is intentionally uncolored.
    Storage storage;
    auto config = storage.load_config();
    BlackBox blackbox(storage.root());

    if (command == "target") {
      if (argc == 2) { std::cout << config.target_ssid << "\n"; return 0; }
      if (argc != 3) { std::cerr << "Usage: wifiopt target [SSID]\n"; return 2; }
      const auto ssid = trim(argv[2]);
      if (ssid.empty()) { std::cerr << "Target SSID must not be empty or whitespace only\n"; return 2; }
      config.target_ssid = ssid;
      storage.save_config(config);
      std::cout << "Target SSID saved: " << config.target_ssid << "\n";
      return 0;
    }

    config.target_ssid = option_value(argc, argv, "--ssid", config.target_ssid);
    if (command == "daemon") return run_daemon(self_path(), storage);
    if (command == "simulate") return run_simulate(argc, argv, quiet);
    if (command == "watch") return run_watch(argc, argv, storage, blackbox, config, quiet);
    if (command == "blackbox") return run_blackbox(argc, argv, blackbox, quiet);

    if (command == "auto") {
      if (argc < 3) { std::cerr << "Expected auto on, off, or status\n"; return 2; }
      const std::string action = argv[2];
      if (action == "on") {
        config.enabled = true; storage.save_config(config);
        const auto existing = storage.load_daemon_state();
        if (process_is_running(existing.pid)) { std::cout << "Automatic optimization is already ON (PID " << existing.pid << ")\n"; return 0; }
        std::uint32_t pid = 0; std::string error;
        if (!start_daemon_process(self_path(), config.target_ssid, pid, error)) {
          config.enabled = false; storage.save_config(config);
          std::cerr << "Failed to start monitor: " << error << "\n"; return 1;
        }
        std::cout << "Automatic optimization ON; monitor PID " << pid << "\n"; return 0;
      }
      if (action == "off") {
        config.enabled = false; storage.save_config(config);
        for (int i = 0; i < 30; ++i) {
          const auto state = storage.load_daemon_state();
          if (!process_is_running(state.pid)) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "Automatic optimization OFF; the current Wi-Fi connection was left unchanged\n"; return 0;
      }
      if (action == "status") {
        const auto state = storage.load_daemon_state(); const bool running = process_is_running(state.pid);
        std::cout << "Configured: " << (config.enabled ? "ON" : "OFF")
                  << "\nMonitor process: " << (running ? "running" : "not running")
                  << "\nState: " << state.status << "\nSSID: " << state.ssid << "\nBSSID: " << state.bssid
                  << "\nLast detail: " << state.detail << "\n";
        if (config.enabled && !running) std::cout << "Warning: setting is ON but monitor is not running; run 'wifiopt auto on' to restart it.\n";
        return running == config.enabled ? 0 : 1;
      }
      std::cerr << "Unknown auto action\n"; return 2;
    }

    if (command == "history") {
      const auto format = requested_format(argc, argv);
      if (!format) return invalid_format();
      const bool stats_mode = has_option(argc, argv, "--stats");
      const auto rows = storage.load_history(stats_mode ? std::numeric_limits<std::size_t>::max() : 200);
      if (stats_mode) {
        const auto stats = compute_history_stats(rows);
        if (quiet) return 0;
        if (*format == OutputFormat::text) print_history_stats_text(stats);
        else if (*format == OutputFormat::json)
          std::cout << "{\"schema_version\":1,\"command\":\"history\",\"data\":" << history_stats_json(stats) << "}\n";
        else if (*format == OutputFormat::ndjson) {
          std::cout << "{\"schema_version\":1,\"command\":\"history\",\"type\":\"summary\",\"data\":"
                    << history_stats_json(stats) << "}\n";
          for (const auto& [bssid, value] : stats.by_bssid)
            std::cout << "{\"schema_version\":1,\"command\":\"history\",\"type\":\"bssid_stats\",\"bssid\":\""
                      << bssid << "\",\"attempts\":" << value.attempts << ",\"successes\":" << value.successes << "}\n";
        } else {
          std::cout << "record_type,key,attempts,successes,value\n"
                    << "summary,samples,,," << stats.samples << "\n"
                    << "summary,observation_start_unix,,,";
          if (stats.first_unix) std::cout << *stats.first_unix;
          std::cout << "\nsummary,observation_end_unix,,,";
          if (stats.last_unix) std::cout << *stats.last_unix;
          std::cout << "\nsummary,ap_switches,,," << stats.ap_switches << "\n";
          for (const auto& [bssid, value] : stats.by_bssid)
            std::cout << "bssid," << bssid << ',' << value.attempts << ',' << value.successes << ','
                      << (value.attempts ? value.successes * 1.0 / value.attempts : 0.0) << '\n';
          for (const auto& [code, count] : stats.error_counts)
            std::cout << "error," << code << ",,," << count << '\n';
        }
        return 0;
      }
      if (quiet) return 0;
      if (*format == OutputFormat::text) {
        if (rows.empty()) std::cout << "No connection attempts have been recorded.\n";
        for (const auto& r : rows) std::cout << r.unix_time << "  " << (r.success ? "SUCCESS" : "FAIL")
            << "  requested=" << r.requested_bssid << " actual=" << r.actual_bssid
            << " code=" << r.error_code << "  " << r.detail << "\n";
      } else if (*format == OutputFormat::csv) {
        std::cout << "unix_time,ssid,requested_bssid,actual_bssid,success,error_code,detail\n";
        for (const auto& r : rows) std::cout << r.unix_time << ',' << csv_field(r.ssid) << ',' << r.requested_bssid
            << ',' << r.actual_bssid << ',' << (r.success ? "true" : "false") << ',' << r.error_code << ',' << csv_field(r.detail) << '\n';
      } else {
        const bool document = *format == OutputFormat::json;
        if (document) std::cout << "{\"schema_version\":1,\"command\":\"history\",\"data\":[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
          const auto& r = rows[i];
          std::ostringstream item;
          item << "{\"unix_time\":" << r.unix_time << ",\"ssid\":\"" << json_escape(r.ssid)
               << "\",\"requested_bssid\":\"" << r.requested_bssid << "\",\"actual_bssid\":\"" << r.actual_bssid
               << "\",\"success\":" << (r.success ? "true" : "false") << ",\"error_code\":" << r.error_code
               << ",\"detail\":\"" << json_escape(r.detail) << "\"}";
          if (document) { if (i) std::cout << ','; std::cout << item.str(); }
          else std::cout << "{\"schema_version\":1,\"command\":\"history\",\"data\":" << item.str() << "}\n";
        }
        if (document) std::cout << "]}\n";
      }
      return 0;
    }

    WlanClient wlan;
    Optimizer optimizer(storage, &blackbox);
    if (command == "status") {
      const auto format = requested_format(argc, argv); if (!format) return invalid_format();
      const auto value = wlan.status(); if (!quiet) print_status_formatted(value, *format); return 0;
    }
    if (command == "scan") {
      const auto format = requested_format(argc, argv); if (!format) return invalid_format();
      const bool filtered = has_option(argc, argv, "--ssid");
      auto aps = wlan.scan(filtered ? std::optional<std::string>(config.target_ssid) : std::nullopt);
      storage.record_observations(aps);
      if (quiet) return 0;
      if (*format == OutputFormat::text) {
        std::cout << "Detected " << aps.size() << " BSS entries" << (filtered ? " for " + config.target_ssid : "") << "\n";
        for (const auto& ap : aps) print_ap(ap);
      } else if (*format == OutputFormat::json) std::cout << scan_json_document(aps) << '\n';
      else if (*format == OutputFormat::ndjson) {
        for (const auto& ap : aps) std::cout << "{\"schema_version\":1,\"command\":\"scan\",\"data\":" << access_point_json(ap) << "}\n";
      } else { print_ap_csv_header(false); for (const auto& ap : aps) print_ap_csv(ap); }
      return 0;
    }
    if (command == "rank") {
      const auto format = requested_format(argc, argv); if (!format) return invalid_format();
      const auto ranked = optimizer.rank(wlan, config.target_ssid);
      if (quiet) return 0;
      if (*format == OutputFormat::text) std::cout << "Ranked " << ranked.size() << " BSS entries for " << config.target_ssid << "\n";
      print_ranked_data(ranked, *format, "rank");
      return 0;
    }
    if (command == "inspect") {
      if (argc < 3 || !valid_bssid(argv[2])) { std::cerr << "A valid BSSID is required\n"; return 2; }
      const auto format = requested_format(argc, argv); if (!format || (*format != OutputFormat::text && *format != OutputFormat::json)) return invalid_format();
      const bool filtered = has_option(argc, argv, "--ssid");
      auto aps = wlan.scan(filtered ? std::optional<std::string>(config.target_ssid) : std::nullopt);
      storage.record_observations(aps); storage.enrich_with_history(aps);
      const auto wanted = normalize_bssid(argv[2]);
      const auto found = std::find_if(aps.begin(), aps.end(), [&](const auto& ap) { return normalize_bssid(ap.bssid) == wanted; });
      if (found == aps.end()) { std::cerr << "BSSID " << wanted << " was not found in the current scan" << (filtered ? " for SSID '" + config.target_ssid + "'" : "") << "\n"; return 2; }
      const auto score = score_access_point(*found);
      const bool verbose = has_option(argc, argv, "--verbose"); const bool raw = has_option(argc, argv, "--raw-ie");
      if (quiet) return 0;
      if (*format == OutputFormat::json) {
        std::cout << "{\"schema_version\":1,\"command\":\"inspect\",\"data\":" << access_point_json(*found, &score, verbose);
        if (raw) std::cout << ",\"raw_ie_hex\":\"" << hex_bytes(found->raw_information_elements) << "\"";
        std::cout << "}\n";
      } else {
        std::cout << "BSSID: " << found->bssid << "\nSSID: " << found->ssid << "\nBand: " << band_name(found->frequency_khz)
                  << "\nPHY: " << found->phy_standard.value_or("unavailable") << "\nBeacon interval: ";
        if (found->beacon_interval_tu) std::cout << *found->beacon_interval_tu << " TU"; else std::cout << "unavailable";
        std::cout << "\nLast seen (Unix): "; if (found->last_seen_unix) std::cout << *found->last_seen_unix; else std::cout << "unavailable";
        std::cout << "\nReceive link speed: "; if (found->receive_link_speed_bps) std::cout << *found->receive_link_speed_bps << " bps"; else std::cout << "unavailable (not current AP)";
        std::cout << "\nTransmit link speed: "; if (found->transmit_link_speed_bps) std::cout << *found->transmit_link_speed_bps << " bps"; else std::cout << "unavailable (not current AP)";
        std::cout << "\nQuality score: " << std::fixed << std::setprecision(1) << score.quality_score
                  << "\nSelection score: " << score.score << "\n";
        print_ap(*found);
        if (verbose) {
          std::cout << "IE detection: HT=" << optional_bool_text(found->ht_capabilities)
                    << " VHT=" << optional_bool_text(found->vht_capabilities)
                    << " Extended Capabilities=" << optional_bool_text(found->extended_capabilities)
                    << " Vendor IE count=";
          if (found->vendor_specific_ie_count) std::cout << *found->vendor_specific_ie_count;
          else std::cout << "unavailable";
          std::cout << "\nScore reasons:\n";
          for (const auto& reason : score.reasons) std::cout << "  - " << reason << "\n";
        }
        if (raw) std::cout << "Raw IE (" << found->raw_information_elements.size() << " bytes): " << hex_bytes(found->raw_information_elements) << "\n";
      }
      return 0;
    }
    if (command == "connect") {
      if (argc < 3 || !valid_bssid(argv[2])) { std::cerr << "A valid BSSID is required\n"; return 2; }
      const auto before = wlan.status();
      blackbox.observe(make_wifi_observation(before));
      const auto r = wlan.connect_bssid(config.target_ssid, argv[2]); const bool success = r.success && r.desired_bssid_matched;
      const auto after = wlan.status();
      const ConnectionRecord connection_record{
          unix_now(), config.target_ssid, r.requested_bssid, r.actual_bssid,
          success, r.api_error ? r.api_error : r.reason_code, r.detail};
      storage.append_history(connection_record);
      blackbox.record_connection(connection_record, make_wifi_observation(after));
      blackbox.observe(make_wifi_observation(after), true);
      std::cout << r.detail << "\nRequested: " << r.requested_bssid << "\nActual: " << r.actual_bssid << "\n";
      return success ? 0 : 1;
    }
    if (command == "optimize") {
      const auto format = requested_format(argc, argv); if (!format || (*format != OutputFormat::text && *format != OutputFormat::json)) return invalid_format();
      const bool dry_run = has_option(argc, argv, "--dry-run"); const bool explain = has_option(argc, argv, "--explain");
      const auto r = optimizer.optimize_once(wlan, config, true, dry_run);
      if (!quiet) {
        if (*format == OutputFormat::json) {
          std::cout << "{\"schema_version\":1,\"command\":\"optimize\",\"data\":{\"dry_run\":" << (dry_run ? "true" : "false")
                    << ",\"action_taken\":" << (r.action_taken ? "true" : "false") << ",\"would_switch\":" << (r.would_switch ? "true" : "false")
                    << ",\"success\":" << (r.success ? "true" : "false") << ",\"message\":\"" << json_escape(r.message)
                    << "\",\"threshold\":" << r.threshold << ",\"improvement\":";
          if (r.improvement) std::cout << *r.improvement; else std::cout << "null";
          std::cout << ",\"cooldown_active\":false,\"recommended\":";
          if (r.recommended) std::cout << access_point_json(r.recommended->ap, &r.recommended->scoring, explain); else std::cout << "null";
          if (r.action_taken) {
            std::cout << ",\"ip_verification\":";
            if (r.ip_verification) {
              std::cout << "{\"verified\":" << (r.ip_verification->verified ? "true" : "false")
                        << ",\"timed_out\":" << (r.ip_verification->timed_out ? "true" : "false")
                        << ",\"api_error\":" << r.ip_verification->api_error
                        << ",\"detail\":\"" << json_escape(r.ip_verification->detail) << "\"}";
            } else std::cout << "null";
            const auto print_quality_json = [](const std::optional<ConnectionQuality>& quality) {
              if (quality) std::cout << "{\"rssi_dbm\":" << quality->rssi_dbm
                                     << ",\"signal_quality\":" << quality->signal_quality << '}';
              else std::cout << "null";
            };
            std::cout << ",\"before_quality\":";
            print_quality_json(r.before_quality);
            std::cout << ",\"after_quality\":";
            print_quality_json(r.after_quality);
          }
          std::cout << "}}\n";
        } else {
          std::cout << r.message << "\n";
          if (r.recommended) std::cout << "Recommended: " << r.recommended->ap.bssid << " score=" << std::fixed << std::setprecision(1) << r.recommended->scoring.score << "\n";
          if (r.action_taken) {
            std::cout << "IP verification: ";
            if (r.ip_verification) std::cout << (r.ip_verification->verified ? "passed" : "failed")
                                             << " (" << r.ip_verification->detail << ")";
            else std::cout << "not performed (BSSID connection was not verified)";
            std::cout << "\nBefore quality: ";
            if (r.before_quality) std::cout << r.before_quality->rssi_dbm << " dBm, "
                                            << r.before_quality->signal_quality << "%";
            else std::cout << "unavailable";
            std::cout << "\nAfter quality: ";
            if (r.after_quality) std::cout << r.after_quality->rssi_dbm << " dBm, "
                                           << r.after_quality->signal_quality << "%";
            else std::cout << "unavailable";
            std::cout << "\n";
          }
          if (explain) {
            std::cout << "Threshold: " << config.min_improvement << "\nImprovement: ";
            if (r.improvement) std::cout << *r.improvement; else std::cout << "not applicable";
            std::cout << "\nCooldown: inactive (one-shot evaluation has no persisted switch timer)\n";
            if (r.recommended) for (const auto& reason : r.recommended->scoring.reasons) std::cout << "  - " << reason << "\n";
          }
        }
      }
      return r.success ? 0 : 1;
    }
    if (command == "diagnose") {
      const auto format = requested_format(argc, argv); if (!format) return invalid_format();
      const bool report = has_option(argc, argv, "--report");
      if (report && *format != OutputFormat::text && *format != OutputFormat::json)
        return invalid_format();
      auto rows = storage.load_history(std::numeric_limits<std::size_t>::max());
      std::optional<std::uint64_t> requested_since;
      if (has_option(argc, argv, "--since")) {
        requested_since = parse_duration_seconds(option_value(argc, argv, "--since", ""));
        if (!requested_since) { std::cerr << "Invalid --since duration; use values such as 30m, 24h, or 7d\n"; return 2; }
        const auto cutoff = unix_now() - static_cast<std::int64_t>(*requested_since);
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) { return row.unix_time < cutoff; }), rows.end());
      } else if (!report && rows.size() > 20) rows.erase(rows.begin(), rows.end() - 20);
      const auto status = wlan.status(); unsigned failures = 0, eap845 = 0;
      for (const auto& r : rows) if (!r.success) { ++failures; if (r.error_code == 845) ++eap845; }
      if (quiet) return 0;
      if (report) {
        auto blackbox_records = blackbox.load_records(20);
        if (requested_since) {
          const auto cutoff = unix_now() - static_cast<std::int64_t>(*requested_since);
          blackbox_records.erase(
              std::remove_if(blackbox_records.begin(), blackbox_records.end(),
                             [&](const auto& record) { return record.unix_time < cutoff; }),
              blackbox_records.end());
        }
        const auto stats = compute_history_stats(rows);
        const auto event_log = query_wlan_events(requested_since.value_or(86400), 100);
        if (*format == OutputFormat::text) {
          std::cout << "Wi-Fi diagnostic report\n\nCurrent connection\n";
          print_status(status);
          std::cout << "\nHistory statistics\n";
          print_history_stats_text(stats);
          std::cout << "\nBlack Box records\n";
          if (blackbox_records.empty()) std::cout << "No Black Box failure records have been recorded.\n";
          for (const auto& record : blackbox_records) print_blackbox_record_text(record);
          std::cout << "\nWindows WLAN AutoConfig events\n";
          if (!event_log.available) {
            std::cout << event_log.message << " (error " << event_log.error_code << ")\n";
          } else if (event_log.events.empty()) {
            std::cout << "No related WLAN events were found in the requested period.\n";
          } else {
            for (const auto& event : event_log.events) {
              std::cout << event.unix_time << "  event=" << event.event_id;
              if (event.error_code) std::cout << " code=" << *event.error_code;
              if (!event.ssid.empty()) std::cout << " ssid=" << event.ssid;
              if (!event.bssid.empty()) std::cout << " bssid=" << event.bssid;
              std::cout << "  " << event.description << "\n";
              if (event.error_code == 845U)
                std::cout << "  Error 845 is authentication evidence, not proof that an AP is defective or congested.\n";
            }
          }
          std::cout << "\nConfig/history directory: " << storage.root().string() << "\n";
        } else {
          std::cout << "{\"schema_version\":1,\"command\":\"diagnose\",\"report\":true,\"data\":{"
                    << "\"status\":" << status_json(status)
                    << ",\"history_statistics\":" << history_stats_json(stats)
                    << ",\"blackbox\":[";
          for (std::size_t i = 0; i < blackbox_records.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << blackbox_record_json(blackbox_records[i]);
          }
          std::cout << "],\"event_log\":{\"available\":" << (event_log.available ? "true" : "false")
                    << ",\"error_code\":" << event_log.error_code
                    << ",\"message\":\"" << json_escape(event_log.message) << "\",\"events\":[";
          for (std::size_t i = 0; i < event_log.events.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << wlan_event_json(event_log.events[i]);
          }
          std::cout << "]},\"config_directory\":\"" << json_escape(storage.root().string()) << "\"}}\n";
        }
      } else if (*format == OutputFormat::text) {
        print_status(status);
        std::cout << "Recent recorded attempts: " << rows.size() << ", failures: " << failures << "\n";
        if (eap845) std::cout << "Recorded error 845 count: " << eap845 << ". This is authentication evidence, not proof that an AP is defective or congested.\n";
        std::cout << "Config/history directory: " << storage.root().string() << "\n";
      } else if (*format == OutputFormat::json) {
        std::cout << "{\"schema_version\":1,\"command\":\"diagnose\",\"data\":{\"status\":" << status_json(status)
                  << ",\"attempts\":" << rows.size() << ",\"failures\":" << failures << ",\"error_845\":" << eap845
                  << ",\"config_directory\":\"" << json_escape(storage.root().string()) << "\"}}\n";
      } else if (*format == OutputFormat::csv) {
        std::cout << "attempts,failures,error_845,connected,ssid,bssid,config_directory\n" << rows.size() << ',' << failures << ',' << eap845 << ','
                  << (status.connected ? "true" : "false") << ',' << csv_field(status.ssid) << ',' << status.bssid << ',' << csv_field(storage.root().string()) << '\n';
      } else {
        std::cout << "{\"schema_version\":1,\"command\":\"diagnose\",\"attempts\":" << rows.size() << ",\"failures\":" << failures
                  << ",\"error_845\":" << eap845 << ",\"status\":" << status_json(status) << "}\n";
      }
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
