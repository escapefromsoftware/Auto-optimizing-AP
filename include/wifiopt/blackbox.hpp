#pragma once

#include "wifiopt/core.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wifiopt {

struct ConnectionStatus;

struct WifiObservation {
  std::int64_t unix_time{};
  std::string ssid;
  std::string bssid;
  int rssi_dbm{-100};
  unsigned signal_quality{};
  bool connected{};
};

enum class BlackBoxEventType {
  connection_failure,
  unexpected_disconnect,
  ap_switch_failure
};

struct BlackBoxRecord {
  std::int64_t unix_time{};
  BlackBoxEventType event_type{BlackBoxEventType::connection_failure};
  std::uint32_t error_code{};
  std::string ssid;
  std::string related_bssid;
  std::string requested_bssid;
  std::string actual_bssid;
  std::string detail;
  std::string possible_cause;
  std::string recommended_action;
  std::vector<WifiObservation> observations_before;
  WifiObservation event_observation;
  std::optional<bool> ip_address_verified;
};

const char* blackbox_event_type_name(BlackBoxEventType type) noexcept;
std::optional<BlackBoxEventType> parse_blackbox_event_type(std::string_view value) noexcept;
WifiObservation make_wifi_observation(const ConnectionStatus& status,
                                      std::int64_t unix_time = 0);

class BlackBox {
 public:
  explicit BlackBox(std::filesystem::path storage_root,
                    std::size_t observation_capacity = 30,
                    std::size_t record_limit = 500);

  std::optional<BlackBoxRecord> observe(const WifiObservation& observation,
                                        bool expected_disconnect = false);
  std::optional<BlackBoxRecord> record_connection(
      const ConnectionRecord& connection,
      const std::optional<WifiObservation>& event_observation = std::nullopt);
  std::vector<WifiObservation> observations() const;
  std::vector<BlackBoxRecord> load_records(std::size_t limit = 500) const;

 private:
  BlackBoxRecord make_record(BlackBoxEventType type, std::int64_t unix_time,
                             std::uint32_t error_code, std::string ssid,
                             std::string related_bssid, std::string requested_bssid,
                             std::string actual_bssid, std::string detail,
                             WifiObservation event_observation) const;
  void persist(const BlackBoxRecord& record) const;

  std::filesystem::path path_;
  std::size_t observation_capacity_{};
  std::size_t record_limit_{};
  std::deque<WifiObservation> observations_;
};

} // namespace wifiopt
