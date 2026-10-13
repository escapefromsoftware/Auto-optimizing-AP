#include "wifiopt/storage.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace wifiopt {
namespace {

std::string json_escape(const std::string& value) {
  std::string out;
  for (const unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: if (c >= 0x20) out += static_cast<char>(c); break;
    }
  }
  return out;
}

std::string unescape_field(const std::string& value) {
  std::string out;
  bool escaped = false;
  for (char c : value) {
    if (escaped) {
      out += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
      escaped = false;
    } else if (c == '\\') escaped = true;
    else out += c;
  }
  if (escaped) out += '\\';
  return out;
}

std::string escape_field(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
    const auto pos = line.find('\t', start);
    result.push_back(unescape_field(line.substr(start, pos == std::string::npos ? pos : pos - start)));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return result;
}

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<std::string> json_string(const std::string& text, const std::string& key) {
  std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (std::regex_search(text, match, re)) return match[1].str();
  return std::nullopt;
}

std::optional<double> json_number(const std::string& text, const std::string& key) {
  std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  if (std::regex_search(text, match, re)) {
    try { return std::stod(match[1].str()); } catch (...) { return std::nullopt; }
  }
  return std::nullopt;
}

std::optional<bool> json_bool(const std::string& text, const std::string& key) {
  std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (std::regex_search(text, match, re)) return match[1].str() == "true";
  return std::nullopt;
}

void atomic_write(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + tmp.string());
    out << text;
    out.flush();
    if (!out) throw std::runtime_error("cannot finish writing " + tmp.string());
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto code = GetLastError();
    std::filesystem::remove(tmp);
    throw std::runtime_error("cannot replace " + path.string() + ": " + std::to_string(code));
  }
}

} // namespace

std::int64_t unix_now() noexcept {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

Storage::Storage(std::filesystem::path root) {
  if (!root.empty()) root_ = std::move(root);
  else {
    wchar_t* override_path = nullptr;
    std::size_t override_size = 0;
    if (_wdupenv_s(&override_path, &override_size, L"WIFIOPT_DATA_DIR") == 0 &&
        override_path && *override_path) {
      root_ = override_path;
      std::free(override_path);
      std::filesystem::create_directories(root_);
      return;
    }
    if (override_path) std::free(override_path);
    wchar_t* local = nullptr;
    std::size_t ignored = 0;
    if (_wdupenv_s(&local, &ignored, L"LOCALAPPDATA") == 0 && local) {
      root_ = std::filesystem::path(local) / L"SmartWifiApOptimizer";
      std::free(local);
    } else root_ = std::filesystem::temp_directory_path() / L"SmartWifiApOptimizer";
  }
  std::filesystem::create_directories(root_);
}

AutoConfig Storage::load_config() const {
  AutoConfig c;
  const auto path = root_ / "config.json";
  if (!std::filesystem::exists(path)) {
    save_config(c); // First launch is explicitly OFF.
    return c;
  }
  const auto text = read_all(path);
  if (auto v = json_bool(text, "enabled")) c.enabled = *v;
  if (auto v = json_number(text, "interval_seconds")) c.interval_seconds = static_cast<unsigned>(std::clamp(*v, 10.0, 3600.0));
  if (auto v = json_number(text, "min_improvement")) c.min_improvement = std::clamp(*v, 1.0, 50.0);
  if (auto v = json_number(text, "cooldown_seconds")) c.cooldown_seconds = static_cast<unsigned>(std::clamp(*v, 30.0, 86400.0));
  if (auto v = json_string(text, "target_ssid")) c.target_ssid = *v;
  return c;
}

void Storage::save_config(const AutoConfig& c) const {
  std::ostringstream out;
  out << "{\n  \"enabled\": " << (c.enabled ? "true" : "false")
      << ",\n  \"interval_seconds\": " << c.interval_seconds
      << ",\n  \"min_improvement\": " << c.min_improvement
      << ",\n  \"cooldown_seconds\": " << c.cooldown_seconds
      << ",\n  \"target_ssid\": \"" << json_escape(c.target_ssid) << "\"\n}\n";
  atomic_write(root_ / "config.json", out.str());
}

DaemonState Storage::load_daemon_state() const {
  DaemonState s;
  const auto path = root_ / "daemon_state.json";
  if (!std::filesystem::exists(path)) return s;
  const auto text = read_all(path);
  if (auto v = json_number(text, "pid")) s.pid = static_cast<std::uint32_t>(*v);
  if (auto v = json_number(text, "heartbeat_unix")) s.heartbeat_unix = static_cast<std::int64_t>(*v);
  if (auto v = json_string(text, "status")) s.status = *v;
  if (auto v = json_string(text, "ssid")) s.ssid = *v;
  if (auto v = json_string(text, "bssid")) s.bssid = *v;
  if (auto v = json_string(text, "detail")) s.detail = *v;
  return s;
}

void Storage::save_daemon_state(const DaemonState& s) const {
  std::ostringstream out;
  out << "{\n  \"pid\": " << s.pid << ",\n  \"heartbeat_unix\": " << s.heartbeat_unix
      << ",\n  \"status\": \"" << json_escape(s.status) << "\""
      << ",\n  \"ssid\": \"" << json_escape(s.ssid) << "\""
      << ",\n  \"bssid\": \"" << json_escape(s.bssid) << "\""
      << ",\n  \"detail\": \"" << json_escape(s.detail) << "\"\n}\n";
  atomic_write(root_ / "daemon_state.json", out.str());
}

void Storage::append_history(const ConnectionRecord& r) const {
  std::filesystem::create_directories(root_);
  std::ofstream out(root_ / "history.tsv", std::ios::binary | std::ios::app);
  if (!out) throw std::runtime_error("cannot append connection history");
  out << r.unix_time << '\t' << escape_field(r.ssid) << '\t' << escape_field(normalize_bssid(r.requested_bssid))
      << '\t' << escape_field(normalize_bssid(r.actual_bssid)) << '\t' << (r.success ? 1 : 0)
      << '\t' << r.error_code << '\t' << escape_field(r.detail) << '\n';
}

std::vector<ConnectionRecord> Storage::load_history(std::size_t limit) const {
  std::vector<ConnectionRecord> rows;
  std::ifstream in(root_ / "history.tsv", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = split_tabs(line);
    if (f.size() != 7) continue;
    try {
      rows.push_back({std::stoll(f[0]), f[1], f[2], f[3], f[4] == "1",
                      static_cast<std::uint32_t>(std::stoul(f[5])), f[6]});
    } catch (...) { /* Ignore damaged individual history rows. */ }
  }
  if (rows.size() > limit) rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(limit));
  return rows;
}

void Storage::enrich_with_history(std::vector<AccessPoint>& aps) const {
  const auto rows = load_history(500);
  for (auto& ap : aps) {
    const auto key = normalize_bssid(ap.bssid);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      if (normalize_bssid(it->requested_bssid) != key && normalize_bssid(it->actual_bssid) != key) continue;
      ++ap.history_attempts;
      if (it->success) ++ap.history_successes;
      else if (ap.recent_failures < 3) ++ap.recent_failures;
    }
  }
}

} // namespace wifiopt
