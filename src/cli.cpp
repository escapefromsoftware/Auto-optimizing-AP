#include "wifiopt/cli.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace wifiopt {
namespace {

struct Json {
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Object, Array> value;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  Json parse() {
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
      pos_ = 3; // Skip a leading UTF-8 BOM (e.g. from PowerShell's Out-File -Encoding utf8).
    }
    auto value = parse_value();
    whitespace();
    if (pos_ != text_.size()) fail("trailing content");
    return value;
  }

 private:
  [[noreturn]] void fail(const char* what) const {
    throw std::runtime_error(std::string("invalid JSON at byte ") + std::to_string(pos_) + ": " + what);
  }
  void whitespace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_;
  }
  bool consume(char c) {
    whitespace();
    if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
    return false;
  }
  Json parse_value() {
    whitespace();
    if (pos_ >= text_.size()) fail("expected value");
    if (text_[pos_] == '{') return Json{parse_object()};
    if (text_[pos_] == '[') return Json{parse_array()};
    if (text_[pos_] == '"') return Json{parse_string()};
    if (text_.substr(pos_, 4) == "true") { pos_ += 4; return Json{true}; }
    if (text_.substr(pos_, 5) == "false") { pos_ += 5; return Json{false}; }
    if (text_.substr(pos_, 4) == "null") { pos_ += 4; return Json{nullptr}; }
    return Json{parse_number()};
  }
  Json::Object parse_object() {
    if (!consume('{')) fail("expected object");
    Json::Object result;
    if (consume('}')) return result;
    while (true) {
      whitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key");
      auto key = parse_string();
      if (!consume(':')) fail("expected ':'");
      if (!result.emplace(std::move(key), parse_value()).second) fail("duplicate object key");
      if (consume('}')) return result;
      if (!consume(',')) fail("expected ','");
    }
  }
  Json::Array parse_array() {
    if (!consume('[')) fail("expected array");
    Json::Array result;
    if (consume(']')) return result;
    while (true) {
      result.push_back(parse_value());
      if (consume(']')) return result;
      if (!consume(',')) fail("expected ','");
    }
  }
  std::string parse_string() {
    if (!consume('"')) fail("expected string");
    std::string result;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return result;
      if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
      if (c != '\\') { result += c; continue; }
      if (pos_ >= text_.size()) fail("truncated escape");
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (pos_ + 4 > text_.size()) fail("truncated unicode escape");
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(h - 'A' + 10);
            else fail("invalid unicode escape");
          }
          if (code <= 0x7F) result += static_cast<char>(code);
          else if (code <= 0x7FF) {
            result += static_cast<char>(0xC0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: fail("invalid escape");
      }
    }
    fail("unterminated string");
  }
  double parse_number() {
    whitespace();
    const auto start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) fail("invalid number");
    if (text_[pos_] == '0') ++pos_;
    else {
      if (text_[pos_] < '1' || text_[pos_] > '9') fail("invalid number");
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail("invalid fraction");
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail("invalid exponent");
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    try { return std::stod(std::string(text_.substr(start, pos_ - start))); }
    catch (...) { fail("number out of range"); }
  }
  std::string_view text_;
  std::size_t pos_{};
};

const Json::Object& object(const Json& value, const char* context) {
  const auto* result = std::get_if<Json::Object>(&value.value);
  if (!result) throw std::runtime_error(std::string(context) + " must be a JSON object");
  return *result;
}
const Json::Array& array(const Json& value, const char* context) {
  const auto* result = std::get_if<Json::Array>(&value.value);
  if (!result) throw std::runtime_error(std::string(context) + " must be a JSON array");
  return *result;
}
const Json* member(const Json::Object& value, const char* name) {
  const auto it = value.find(name);
  return it == value.end() ? nullptr : &it->second;
}
std::string string_member(const Json::Object& value, const char* name, bool required = true) {
  const auto* item = member(value, name);
  if (!item) {
    if (!required) return {};
    throw std::runtime_error(std::string("missing JSON string '") + name + "'");
  }
  const auto* result = std::get_if<std::string>(&item->value);
  if (!result) throw std::runtime_error(std::string("JSON member '") + name + "' must be a string");
  return *result;
}
double number_member(const Json::Object& value, const char* name, double fallback) {
  const auto* item = member(value, name);
  if (!item) return fallback;
  const auto* result = std::get_if<double>(&item->value);
  if (!result || !std::isfinite(*result))
    throw std::runtime_error(std::string("JSON member '") + name + "' must be a finite number");
  return *result;
}
bool bool_member(const Json::Object& value, const char* name, bool fallback) {
  const auto* item = member(value, name);
  if (!item) return fallback;
  const auto* result = std::get_if<bool>(&item->value);
  if (!result) throw std::runtime_error(std::string("JSON member '") + name + "' must be boolean");
  return *result;
}
void optional_bool(const Json::Object& value, const char* name, std::optional<bool>& out) {
  const auto* item = member(value, name);
  if (!item || std::holds_alternative<std::nullptr_t>(item->value)) return;
  const auto* result = std::get_if<bool>(&item->value);
  if (!result) throw std::runtime_error(std::string("JSON member '") + name + "' must be boolean or null");
  out = *result;
}
template <typename T> void optional_number(const Json::Object& value, const char* name, std::optional<T>& out) {
  const auto* item = member(value, name);
  if (!item || std::holds_alternative<std::nullptr_t>(item->value)) return;
  const auto* number = std::get_if<double>(&item->value);
  if (!number || !std::isfinite(*number)) throw std::runtime_error(std::string("invalid '") + name + "'");
  out = static_cast<T>(*number);
}
template <typename T> void json_optional(std::ostringstream& out, const std::optional<T>& value) {
  if (value) out << *value; else out << "null";
}
void json_optional_bool(std::ostringstream& out, const std::optional<bool>& value) {
  if (value) out << (*value ? "true" : "false"); else out << "null";
}

} // namespace

std::optional<OutputFormat> parse_output_format(std::string_view value) noexcept {
  if (value == "text") return OutputFormat::text;
  if (value == "json") return OutputFormat::json;
  if (value == "csv") return OutputFormat::csv;
  if (value == "ndjson") return OutputFormat::ndjson;
  return std::nullopt;
}

std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << static_cast<char>(c);
    }
  }
  return out.str();
}

std::string csv_field(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(value);
  std::string out = "\"";
  for (char c : value) out += c == '"' ? "\"\"" : std::string(1, c);
  return out + '"';
}

std::string band_name(std::uint32_t frequency_khz) {
  const auto mhz = frequency_khz / 1000;
  if (mhz >= 2400 && mhz < 2500) return "2.4 GHz";
  if (mhz >= 4900 && mhz < 5925) return "5 GHz";
  if (mhz >= 5925 && mhz <= 7125) return "6 GHz";
  return "unknown";
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i) out << ' ';
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return out.str();
}

std::string access_point_json(const AccessPoint& ap, const ScoreResult* score, bool include_reasons) {
  std::ostringstream out;
  out << "{\"ssid\":\"" << json_escape(ap.ssid) << "\",\"bssid\":\"" << json_escape(ap.bssid)
      << "\",\"rssi_dbm\":" << ap.rssi_dbm << ",\"signal_quality\":" << ap.signal_quality
      << ",\"frequency_khz\":" << ap.frequency_khz << ",\"channel\":" << ap.channel
      << ",\"band\":\"" << band_name(ap.frequency_khz) << "\",\"current\":" << (ap.current ? "true" : "false")
      << ",\"cochannel_neighbors\":" << ap.cochannel_neighbors << ",\"bss_load\":";
  if (ap.bss_load) {
    out << "{\"station_count\":" << ap.bss_load->station_count
        << ",\"channel_utilization\":" << static_cast<unsigned>(ap.bss_load->channel_utilization)
        << ",\"available_admission_capacity\":" << ap.bss_load->available_admission_capacity << '}';
  } else out << "null";
  out << ",\"phy_standard\":";
  if (ap.phy_standard) out << '"' << json_escape(*ap.phy_standard) << '"'; else out << "null";
  out << ",\"beacon_interval_tu\":"; json_optional(out, ap.beacon_interval_tu);
  out << ",\"ht_capabilities\":"; json_optional_bool(out, ap.ht_capabilities);
  out << ",\"vht_capabilities\":"; json_optional_bool(out, ap.vht_capabilities);
  out << ",\"extended_capabilities\":"; json_optional_bool(out, ap.extended_capabilities);
  out << ",\"bss_transition_management\":"; json_optional_bool(out, ap.bss_transition_management);
  out << ",\"vendor_specific_ie_count\":"; json_optional(out, ap.vendor_specific_ie_count);
  out << ",\"last_seen_unix\":"; json_optional(out, ap.last_seen_unix);
  out << ",\"receive_link_speed_bps\":"; json_optional(out, ap.receive_link_speed_bps);
  out << ",\"transmit_link_speed_bps\":"; json_optional(out, ap.transmit_link_speed_bps);
  out << ",\"history_attempts\":" << ap.history_attempts
      << ",\"history_successes\":" << ap.history_successes
      << ",\"recent_failures\":" << ap.recent_failures;
  if (score) {
    out << ",\"quality_score\":" << std::setprecision(15) << score->quality_score
        << ",\"score\":" << score->score;
    if (include_reasons) {
      out << ",\"reasons\":[";
      for (std::size_t i = 0; i < score->reasons.size(); ++i) {
        if (i) out << ',';
        out << '"' << json_escape(score->reasons[i]) << '"';
      }
      out << ']';
    }
  }
  out << '}';
  return out.str();
}

std::string scan_json_document(const std::vector<AccessPoint>& aps) {
  std::ostringstream out;
  out << "{\"schema_version\":1,\"command\":\"scan\",\"data\":[";
  for (std::size_t i = 0; i < aps.size(); ++i) {
    if (i) out << ',';
    out << access_point_json(aps[i]);
  }
  out << "]}";
  return out.str();
}

std::vector<AccessPoint> parse_scan_json_document(std::string_view text) {
  const auto root = object(JsonParser(text).parse(), "scan document");
  if (number_member(root, "schema_version", -1) != 1) throw std::runtime_error("unsupported scan schema_version");
  if (string_member(root, "command") != "scan") throw std::runtime_error("input JSON is not a scan document");
  const auto* data = member(root, "data");
  if (!data) throw std::runtime_error("scan document has no data array");
  std::vector<AccessPoint> result;
  for (const auto& value : array(*data, "scan data")) {
    const auto& item = object(value, "access point");
    AccessPoint ap;
    ap.ssid = string_member(item, "ssid");
    ap.bssid = string_member(item, "bssid");
    if (!valid_bssid(ap.bssid)) throw std::runtime_error("scan document contains invalid BSSID");
    ap.rssi_dbm = static_cast<int>(number_member(item, "rssi_dbm", -100));
    ap.signal_quality = static_cast<unsigned>(number_member(item, "signal_quality", 0));
    ap.frequency_khz = static_cast<std::uint32_t>(number_member(item, "frequency_khz", 0));
    ap.channel = static_cast<int>(number_member(item, "channel", frequency_to_channel(ap.frequency_khz)));
    ap.current = bool_member(item, "current", false);
    ap.cochannel_neighbors = static_cast<unsigned>(number_member(item, "cochannel_neighbors", 0));
    ap.history_attempts = static_cast<unsigned>(number_member(item, "history_attempts", 0));
    ap.history_successes = static_cast<unsigned>(number_member(item, "history_successes", 0));
    ap.recent_failures = static_cast<unsigned>(number_member(item, "recent_failures", 0));
    if (const auto* load = member(item, "bss_load"); load && !std::holds_alternative<std::nullptr_t>(load->value)) {
      const auto& bss = object(*load, "bss_load");
      ap.bss_load = BssLoad{
          static_cast<std::uint16_t>(number_member(bss, "station_count", 0)),
          static_cast<std::uint8_t>(number_member(bss, "channel_utilization", 0)),
          static_cast<std::uint16_t>(number_member(bss, "available_admission_capacity", 0))};
    }
    if (const auto* phy = member(item, "phy_standard")) {
      if (const auto* string = std::get_if<std::string>(&phy->value)) ap.phy_standard = *string;
    }
    optional_number(item, "beacon_interval_tu", ap.beacon_interval_tu);
    optional_bool(item, "ht_capabilities", ap.ht_capabilities);
    optional_bool(item, "vht_capabilities", ap.vht_capabilities);
    optional_bool(item, "extended_capabilities", ap.extended_capabilities);
    optional_bool(item, "bss_transition_management", ap.bss_transition_management);
    optional_number(item, "vendor_specific_ie_count", ap.vendor_specific_ie_count);
    optional_number(item, "last_seen_unix", ap.last_seen_unix);
    optional_number(item, "receive_link_speed_bps", ap.receive_link_speed_bps);
    optional_number(item, "transmit_link_speed_bps", ap.transmit_link_speed_bps);
    result.push_back(std::move(ap));
  }
  return result;
}

ScorePolicy parse_score_policy_json(std::string_view text) {
  const auto root = object(JsonParser(text).parse(), "weights document");
  ScorePolicy policy;
  policy.rssi_weight = number_member(root, "signal_rssi", policy.rssi_weight);
  policy.signal_quality_weight = number_member(root, "signal_quality", policy.signal_quality_weight);
  policy.bss_load_weight = number_member(root, "bss_load", policy.bss_load_weight);
  policy.bss_load_neutral_score = number_member(root, "bss_load_neutral", policy.bss_load_neutral_score);
  policy.channel_interference_weight = number_member(root, "channel_interference", policy.channel_interference_weight);
  policy.channel_interference_decay = number_member(root, "channel_interference_decay", policy.channel_interference_decay);
  policy.connection_history_weight = number_member(root, "connection_history", policy.connection_history_weight);
  policy.connection_history_neutral_score = number_member(root, "connection_history_neutral", policy.connection_history_neutral_score);
  policy.current_ap_bonus = number_member(root, "current_ap_bonus", policy.current_ap_bonus);
  policy.recent_failure_penalty = number_member(root, "recent_failure_penalty", policy.recent_failure_penalty);
  const double values[]{policy.rssi_weight, policy.signal_quality_weight, policy.bss_load_weight,
      policy.bss_load_neutral_score, policy.channel_interference_weight, policy.channel_interference_decay,
      policy.connection_history_weight, policy.connection_history_neutral_score,
      policy.current_ap_bonus, policy.recent_failure_penalty};
  if (std::any_of(std::begin(values), std::end(values), [](double value) { return value < 0.0 || value > 1000.0; }))
    throw std::runtime_error("weights must be between 0 and 1000");
  return policy;
}

std::optional<std::uint64_t> parse_duration_seconds(std::string_view value, bool plain_seconds) noexcept {
  if (value.empty()) return std::nullopt;
  std::uint64_t multiplier = 1;
  const char suffix = value.back();
  if (suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd') {
    value.remove_suffix(1);
    multiplier = suffix == 's' ? 1 : suffix == 'm' ? 60 : suffix == 'h' ? 3600 : 86400;
  } else if (!plain_seconds) return std::nullopt;
  if (value.empty() || value.front() == '-') return std::nullopt;
  std::uint64_t number = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number == 0 ||
      number > std::numeric_limits<std::uint64_t>::max() / multiplier) return std::nullopt;
  return number * multiplier;
}

HistoryStats compute_history_stats(const std::vector<ConnectionRecord>& rows) {
  HistoryStats stats;
  stats.samples = rows.size();
  std::string previous;
  for (const auto& row : rows) {
    if (!stats.first_unix || row.unix_time < *stats.first_unix) stats.first_unix = row.unix_time;
    if (!stats.last_unix || row.unix_time > *stats.last_unix) stats.last_unix = row.unix_time;
    const auto bssid = valid_bssid(row.actual_bssid) ? normalize_bssid(row.actual_bssid)
                                                     : normalize_bssid(row.requested_bssid);
    if (!bssid.empty()) {
      auto& ap = stats.by_bssid[bssid];
      ++ap.attempts;
      if (row.success) ++ap.successes;
    }
    if (!row.success) ++stats.error_counts[row.error_code];
    if (valid_bssid(row.actual_bssid)) {
      const auto actual = normalize_bssid(row.actual_bssid);
      if (!previous.empty() && actual != previous) ++stats.ap_switches;
      previous = actual;
    }
  }
  return stats;
}

} // namespace wifiopt
