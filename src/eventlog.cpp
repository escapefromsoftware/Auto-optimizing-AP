#include "wifiopt/eventlog.hpp"

#include "wifiopt/core.hpp"

#include <windows.h>
#include <winevt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace wifiopt {
namespace {

class EventHandle {
 public:
  EventHandle() = default;
  explicit EventHandle(EVT_HANDLE handle) : handle_(handle) {}
  ~EventHandle() { if (handle_) EvtClose(handle_); }
  EventHandle(const EventHandle&) = delete;
  EventHandle& operator=(const EventHandle&) = delete;
  EventHandle(EventHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  EventHandle& operator=(EventHandle&& other) noexcept {
    if (this != &other) {
      if (handle_) EvtClose(handle_);
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  EVT_HANDLE get() const noexcept { return handle_; }

 private:
  EVT_HANDLE handle_{};
};

std::string utf8(const wchar_t* value, std::size_t length) {
  if (!value || length == 0) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string result(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), needed,
                      nullptr, nullptr);
  return result;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string xml_decode(std::string value) {
  const std::array<std::pair<const char*, const char*>, 5> entities{{
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}}};
  for (const auto& [encoded, decoded] : entities) {
    std::size_t position = 0;
    while ((position = value.find(encoded, position)) != std::string::npos) {
      value.replace(position, std::char_traits<char>::length(encoded), decoded);
      position += std::char_traits<char>::length(decoded);
    }
  }
  value = std::regex_replace(value, std::regex("<[^>]*>"), "");
  return value;
}

std::optional<std::uint32_t> unsigned_value(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  std::uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec == std::errc{} && parsed.ptr != value.data()) return result;
  return std::nullopt;
}

std::int64_t iso8601_unix(const std::string& text) {
  std::smatch match;
  const std::regex expression(
      R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?Z)",
      std::regex::icase);
  if (!std::regex_search(text, match, expression)) return 0;
  SYSTEMTIME system_time{};
  system_time.wYear = static_cast<WORD>(std::stoi(match[1].str()));
  system_time.wMonth = static_cast<WORD>(std::stoi(match[2].str()));
  system_time.wDay = static_cast<WORD>(std::stoi(match[3].str()));
  system_time.wHour = static_cast<WORD>(std::stoi(match[4].str()));
  system_time.wMinute = static_cast<WORD>(std::stoi(match[5].str()));
  system_time.wSecond = static_cast<WORD>(std::stoi(match[6].str()));
  FILETIME file_time{};
  if (!SystemTimeToFileTime(&system_time, &file_time)) return 0;
  ULARGE_INTEGER ticks{};
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  constexpr std::uint64_t epoch_ticks = 116444736000000000ULL;
  if (ticks.QuadPart < epoch_ticks) return 0;
  return static_cast<std::int64_t>((ticks.QuadPart - epoch_ticks) / 10000000ULL);
}

bool relevant(const WlanEvent& event) {
  switch (event.event_id) {
    case 8002:
    case 8003:
    case 11002:
    case 11004:
    case 12013:
    case 12014:
      return true;
    default:
      break;
  }
  if (event.error_code && *event.error_code != 0) return true;
  const auto description = lower(event.description);
  return description.find("fail") != std::string::npos ||
         description.find("disconnect") != std::string::npos ||
         description.find("authentication") != std::string::npos ||
         description.find("eap") != std::string::npos;
}

std::string default_description(std::uint32_t event_id) {
  switch (event_id) {
    case 8002: return "WLAN connection failed";
    case 8003: return "WLAN connection was disconnected";
    case 11002: return "WLAN security processing failed";
    case 11004: return "WLAN security processing stopped";
    case 12013: return "802.1X authentication failed";
    case 12014: return "802.1X authentication was restarted";
    default: return "WLAN AutoConfig event " + std::to_string(event_id);
  }
}

std::optional<std::string> render_xml(EVT_HANDLE event) {
  DWORD used = 0;
  DWORD properties = 0;
  if (EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &properties)) return std::string{};
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || used < sizeof(wchar_t)) return std::nullopt;
  std::vector<wchar_t> buffer(used / sizeof(wchar_t) + 1, L'\0');
  if (!EvtRender(nullptr, event, EvtRenderEventXml, used, buffer.data(), &used, &properties))
    return std::nullopt;
  const auto length = std::char_traits<wchar_t>::length(buffer.data());
  return utf8(buffer.data(), length);
}

} // namespace

std::optional<WlanEvent> parse_wlan_event_xml(std::string_view xml_view) {
  try {
    const std::string xml(xml_view);
    std::smatch match;
    WlanEvent event;
    if (!std::regex_search(xml, match, std::regex(R"(<EventID(?:\s+[^>]*)?>(\d+)</EventID>)",
                                                  std::regex::icase)))
      return std::nullopt;
    event.event_id = static_cast<std::uint32_t>(std::stoul(match[1].str()));
    if (std::regex_search(xml, match, std::regex(R"(SystemTime\s*=\s*["']([^"']+)["'])",
                                                  std::regex::icase)))
      event.unix_time = iso8601_unix(match[1].str());

    std::map<std::string, std::string> fields;
    const std::regex data_expression(R"(<Data\b([^>]*)>([\s\S]*?)</Data>)", std::regex::icase);
    for (std::sregex_iterator it(xml.begin(), xml.end(), data_expression), end; it != end; ++it) {
      std::smatch name_match;
      const auto attributes = (*it)[1].str();
      if (!std::regex_search(attributes, name_match,
                             std::regex(R"(Name\s*=\s*["']([^"']+)["'])", std::regex::icase)))
        continue;
      fields[lower(name_match[1].str())] = xml_decode((*it)[2].str());
    }

    const auto field = [&](std::initializer_list<const char*> names) -> std::string {
      for (const auto* name : names) {
        const auto found = fields.find(name);
        if (found != fields.end() && !found->second.empty()) return found->second;
      }
      return {};
    };
    event.ssid = field({"ssid", "networkname", "network name"});
    auto bssid = field({"bssid", "accesspoint", "access point"});
    if (valid_bssid(bssid)) event.bssid = normalize_bssid(bssid);
    if (event.bssid.empty()) {
      const std::regex bssid_expression(R"(([0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}))");
      if (std::regex_search(xml, match, bssid_expression)) event.bssid = normalize_bssid(match[1].str());
    }

    for (const auto* name : {"reasoncode", "errorcode", "error code", "reason code"}) {
      const auto found = fields.find(name);
      if (found != fields.end()) {
        event.error_code = unsigned_value(found->second);
        if (event.error_code) break;
      }
    }
    if (!event.error_code && std::regex_search(xml, match, std::regex(R"(\b845\b)")))
      event.error_code = 845;

    event.description = field({"failurereason", "failure reason", "reason", "message", "error"});
    if (event.description.empty()) event.description = default_description(event.event_id);
    return event;
  } catch (...) {
    return std::nullopt;
  }
}

EventLogResult query_wlan_events(std::uint64_t since_seconds,
                                 std::size_t maximum_events) noexcept {
  EventLogResult result;
  try {
    constexpr wchar_t channel[] = L"Microsoft-Windows-WLAN-AutoConfig/Operational";
    const auto bounded_seconds = std::min<std::uint64_t>(
        since_seconds, std::numeric_limits<std::uint64_t>::max() / 1000ULL);
    const std::wstring query = L"*[System[TimeCreated[timediff(@SystemTime) <= " +
                               std::to_wstring(bounded_seconds * 1000ULL) + L"]]]";
    EventHandle query_handle(EvtQuery(nullptr, channel, query.c_str(),
                                      EvtQueryChannelPath | EvtQueryReverseDirection));
    if (!query_handle.get()) {
      result.error_code = GetLastError();
      result.message = "Windows event log could not be read (insufficient permissions or unsupported environment).";
      return result;
    }

    while (result.events.size() < maximum_events) {
      EVT_HANDLE raw_events[16]{};
      DWORD returned = 0;
      if (!EvtNext(query_handle.get(), static_cast<DWORD>(std::size(raw_events)), raw_events,
                   0, 0, &returned)) {
        const auto error = GetLastError();
        if (error == ERROR_NO_MORE_ITEMS) break;
        result.error_code = error;
        result.message = "Windows event log could not be read (insufficient permissions or unsupported environment).";
        return result;
      }
      for (DWORD i = 0; i < returned; ++i) {
        EventHandle event(raw_events[i]);
        if (result.events.size() < maximum_events) {
          const auto xml = render_xml(event.get());
          if (!xml) continue;
          if (auto parsed = parse_wlan_event_xml(*xml); parsed && relevant(*parsed))
            result.events.push_back(std::move(*parsed));
        }
      }
    }
    result.available = true;
    result.message = "Windows WLAN event log read successfully.";
  } catch (...) {
    result.available = false;
    result.message = "Windows event log could not be read (insufficient permissions or unsupported environment).";
  }
  return result;
}

} // namespace wifiopt
