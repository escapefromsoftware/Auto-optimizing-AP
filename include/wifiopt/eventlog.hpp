#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wifiopt {

struct WlanEvent {
  std::int64_t unix_time{};
  std::uint32_t event_id{};
  std::string ssid;
  std::string bssid;
  std::optional<std::uint32_t> error_code;
  std::string description;
};

struct EventLogResult {
  bool available{};
  std::uint32_t error_code{};
  std::string message;
  std::vector<WlanEvent> events;
};

std::optional<WlanEvent> parse_wlan_event_xml(std::string_view xml);
EventLogResult query_wlan_events(std::uint64_t since_seconds,
                                 std::size_t maximum_events = 200) noexcept;

} // namespace wifiopt
