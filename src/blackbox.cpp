#include "wifiopt/blackbox.hpp"

#include "wifiopt/cli.hpp"
#include "wifiopt/storage.hpp"
#include "wifiopt/wlan_client.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace wifiopt {
namespace {

std::string known_bssid(const std::string& value) {
  return valid_bssid(value) ? normalize_bssid(value) : std::string{};
}

} // namespace

const char* blackbox_event_type_name(BlackBoxEventType type) noexcept {
  switch (type) {
    case BlackBoxEventType::connection_failure: return "connection_failure";
    case BlackBoxEventType::unexpected_disconnect: return "unexpected_disconnect";
    case BlackBoxEventType::ap_switch_failure: return "ap_switch_failure";
  }
  return "connection_failure";
}

std::optional<BlackBoxEventType> parse_blackbox_event_type(std::string_view value) noexcept {
  if (value == "connection_failure") return BlackBoxEventType::connection_failure;
  if (value == "unexpected_disconnect") return BlackBoxEventType::unexpected_disconnect;
  if (value == "ap_switch_failure") return BlackBoxEventType::ap_switch_failure;
  return std::nullopt;
}

WifiObservation make_wifi_observation(const ConnectionStatus& status,
                                      std::int64_t observed_at) {
  WifiObservation observation;
  observation.unix_time = observed_at ? observed_at : unix_now();
  observation.ssid = status.ssid;
  observation.bssid = known_bssid(status.bssid);
  observation.rssi_dbm = status.rssi_dbm;
  observation.signal_quality = status.signal_quality;
  observation.connected = status.connected;
  return observation;
}

BlackBox::BlackBox(std::filesystem::path storage_root, std::size_t observation_capacity,
                   std::size_t record_limit)
    : path_(std::move(storage_root) / "blackbox.jsonl"),
      observation_capacity_(std::max<std::size_t>(1, observation_capacity)),
      record_limit_(std::max<std::size_t>(1, record_limit)) {}

std::optional<BlackBoxRecord> BlackBox::observe(const WifiObservation& observation,
                                                bool expected_disconnect) {
  std::optional<BlackBoxRecord> detected;
  if (!observations_.empty() && observations_.back().connected && !observation.connected &&
      !expected_disconnect) {
    const auto& previous = observations_.back();
    detected = make_record(BlackBoxEventType::unexpected_disconnect,
                           observation.unix_time, 0, previous.ssid,
                           known_bssid(previous.bssid), {}, {},
                           "Connected state changed to disconnected between consecutive observations",
                           observation);
    persist(*detected);
  }

  observations_.push_back(observation);
  while (observations_.size() > observation_capacity_) observations_.pop_front();
  return detected;
}

std::optional<BlackBoxRecord> BlackBox::record_connection(
    const ConnectionRecord& connection,
    const std::optional<WifiObservation>& supplied_observation) {
  const auto requested = known_bssid(connection.requested_bssid);
  const auto actual = known_bssid(connection.actual_bssid);
  const bool switch_mismatch = !requested.empty() && !actual.empty() &&
                               !requested_bssid_matched(requested, actual);
  if (connection.success && !switch_mismatch) return std::nullopt;

  const auto type = switch_mismatch ? BlackBoxEventType::ap_switch_failure
                                    : BlackBoxEventType::connection_failure;
  WifiObservation event = supplied_observation.value_or(WifiObservation{});
  if (event.unix_time == 0) event.unix_time = connection.unix_time;
  if (event.ssid.empty()) event.ssid = connection.ssid;
  if (event.bssid.empty() && !actual.empty()) event.bssid = actual;
  if (!supplied_observation) event.connected = connection.success && !actual.empty();
  const auto related = !requested.empty() ? requested : actual;
  auto record = make_record(type, connection.unix_time, connection.error_code,
                            connection.ssid, related, requested, actual,
                            connection.detail, std::move(event));
  persist(record);
  return record;
}

std::vector<WifiObservation> BlackBox::observations() const {
  return {observations_.begin(), observations_.end()};
}

std::vector<BlackBoxRecord> BlackBox::load_records(std::size_t limit) const {
  std::vector<BlackBoxRecord> records;
  std::ifstream input(path_, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    try {
      records.push_back(parse_blackbox_record_json(line));
    } catch (...) {
      // One damaged line must not make the remaining diagnostic record unavailable.
    }
  }
  if (records.size() > limit)
    records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(limit));
  return records;
}

BlackBoxRecord BlackBox::make_record(BlackBoxEventType type, std::int64_t unix_time,
                                     std::uint32_t error_code, std::string ssid,
                                     std::string related_bssid, std::string requested_bssid,
                                     std::string actual_bssid, std::string detail,
                                     WifiObservation event_observation) const {
  BlackBoxRecord record;
  record.unix_time = unix_time;
  record.event_type = type;
  record.error_code = error_code;
  record.ssid = std::move(ssid);
  record.related_bssid = std::move(related_bssid);
  record.requested_bssid = std::move(requested_bssid);
  record.actual_bssid = std::move(actual_bssid);
  record.detail = std::move(detail);
  record.observations_before.assign(observations_.begin(), observations_.end());
  record.event_observation = std::move(event_observation);

  if (error_code == 845) {
    record.possible_cause =
        "Error 845 is evidence of an 802.1X/EAP authentication failure; it does not by itself prove that a specific AP is defective or congested.";
    record.recommended_action =
        "Check the saved enterprise profile, identity/certificate validity, system time, and authentication-server logs before attributing the failure to an AP.";
  } else if (type == BlackBoxEventType::ap_switch_failure) {
    record.possible_cause =
        "Windows or the Wi-Fi driver selected a different BSSID, or the requested BSSID did not complete association; this is not conclusive evidence of AP failure.";
    record.recommended_action =
        "Compare signal and authentication events for both requested and actual BSSIDs, then retry only if policy and cooldown permit.";
  } else if (type == BlackBoxEventType::unexpected_disconnect) {
    record.possible_cause =
        "The connection disappeared between observations; radio conditions, roaming, driver behavior, or authentication may be involved.";
    record.recommended_action =
        "Review nearby WLAN AutoConfig events and the preceding signal observations; avoid assigning a cause without corroborating evidence.";
  } else {
    record.possible_cause =
        "The connection attempt failed, but the available result alone may not identify whether authentication, radio conditions, policy, or the driver caused it.";
    record.recommended_action =
        "Review the WLAN AutoConfig events and saved-profile status, and compare repeated failures before taking corrective action.";
  }
  return record;
}

void BlackBox::persist(const BlackBoxRecord& record) const {
  auto records = load_records(record_limit_);
  records.push_back(record);
  if (records.size() > record_limit_)
    records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(record_limit_));
  std::ostringstream output;
  for (const auto& item : records) output << blackbox_record_json(item) << '\n';
  atomic_write_file(path_, output.str());
}

} // namespace wifiopt
