#include "wifiopt/core.hpp"
#include "wifiopt/blackbox.hpp"
#include "wifiopt/cli.hpp"
#include "wifiopt/eventlog.hpp"
#include "wifiopt/gui_logic.hpp"
#include "wifiopt/storage.hpp"
#include "wifiopt/wlan_client.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace wifiopt;

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::cerr << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; ++failures; } } while (false)

void test_bss_load_valid() {
  const std::vector<std::uint8_t> ie{0, 1, 99, 11, 5, 0x34, 0x12, 128, 0x78, 0x56};
  const auto load = parse_bss_load(ie);
  CHECK(load.has_value());
  CHECK(load->station_count == 0x1234);
  CHECK(load->channel_utilization == 128);
  CHECK(load->available_admission_capacity == 0x5678);
}

void test_bss_load_missing() {
  const std::vector<std::uint8_t> ie{0, 3, 'a', 'b', 'c'};
  CHECK(!parse_bss_load(ie));
  CHECK(!parse_bss_load({}));
}

void test_bss_load_truncated() {
  const std::vector<std::uint8_t> bad_header{11};
  const std::vector<std::uint8_t> bad_body{11, 5, 1, 2, 3};
  const std::vector<std::uint8_t> too_short_load{11, 4, 1, 2, 3, 4};
  CHECK(!parse_bss_load(bad_header));
  CHECK(!parse_bss_load(bad_body));
  CHECK(!parse_bss_load(too_short_load));
}

void test_information_elements_valid_and_unknown() {
  const std::vector<std::uint8_t> ies{
      99, 2, 0xAA, 0xBB,
      45, 1, 0x00,
      191, 1, 0x00,
      127, 3, 0x00, 0x00, 0x08,
      221, 3, 0x00, 0x11, 0x22,
      221, 0};
  const auto parsed = parse_information_elements(ies);
  CHECK(parsed.has_value());
  CHECK(parsed->ht_capabilities);
  CHECK(parsed->vht_capabilities);
  CHECK(parsed->extended_capabilities);
  CHECK(parsed->bss_transition_management.has_value());
  CHECK(*parsed->bss_transition_management);
  CHECK(parsed->vendor_specific_count == 2);

  const std::vector<std::uint8_t> short_extended{127, 2, 0x00, 0x00};
  const auto short_result = parse_information_elements(short_extended);
  CHECK(short_result.has_value());
  CHECK(short_result->extended_capabilities);
  CHECK(!short_result->bss_transition_management.has_value());

  const std::vector<std::uint8_t> unknown_only{17, 1, 0x42};
  const auto unknown_result = parse_information_elements(unknown_only);
  CHECK(unknown_result.has_value());
  CHECK(!unknown_result->ht_capabilities);
  CHECK(!unknown_result->vht_capabilities);
  CHECK(!unknown_result->extended_capabilities);
  CHECK(unknown_result->vendor_specific_count == 0);
}

void test_information_elements_truncated() {
  const std::vector<std::uint8_t> bad_header{45};
  const std::vector<std::uint8_t> bad_body{191, 3, 1, 2};
  const std::vector<std::uint8_t> bad_after_valid{45, 0, 221, 4, 1};
  CHECK(!parse_information_elements(bad_header));
  CHECK(!parse_information_elements(bad_body));
  CHECK(!parse_information_elements(bad_after_valid));
  const auto empty = parse_information_elements({});
  CHECK(empty.has_value());
  CHECK(!empty->ht_capabilities);
}

AccessPoint base_ap(std::string bssid, int rssi = -60) {
  AccessPoint ap;
  ap.ssid = "AP-TUT5"; ap.bssid = std::move(bssid); ap.rssi_dbm = rssi; ap.signal_quality = 75;
  ap.frequency_khz = 5180000; ap.channel = 36;
  return ap;
}

void test_scoring_and_missing_values() {
  auto good = base_ap("00:00:00:00:00:01", -50);
  good.bss_load = BssLoad{8, 20, 100};
  good.history_attempts = 8; good.history_successes = 8;
  auto poor = base_ap("00:00:00:00:00:02", -85);
  poor.bss_load = BssLoad{1, 240, 100};
  poor.history_attempts = 8; poor.history_successes = 2; poor.recent_failures = 2;
  CHECK(score_access_point(good).score > score_access_point(poor).score);
  auto missing = base_ap("00:00:00:00:00:03");
  const auto score = score_access_point(missing);
  CHECK(score.score >= 0 && score.score <= 100);
  bool explained = false;
  for (const auto& reason : score.reasons) if (reason.find("not invented") != std::string::npos) explained = true;
  CHECK(explained);
}

void test_quality_and_selection_scores() {
  auto ap = base_ap("00:00:00:00:00:04", -70);
  ap.signal_quality = 60;
  ap.bss_load = BssLoad{12, 64, 0};
  ap.cochannel_neighbors = 3;
  ap.history_attempts = 8;
  ap.history_successes = 6;
  ap.recent_failures = 1;
  ap.current = true;

  const auto scored = score_access_point(ap);
  const double signal = 32.0 * ((ap.rssi_dbm + 90.0) / 40.0) + 13.0 * 0.60;
  const double load = 20.0 * (1.0 - 64.0 / 255.0);
  const double interference = 8.0 / (1.0 + 0.35 * 3.0);
  const double history = 17.0 * ((6.0 + 1.0) / (8.0 + 2.0));
  const double expected_quality = signal + load + interference;
  const double expected_selection = expected_quality + history - 7.0 + 8.0;
  CHECK(std::abs(scored.quality_score - expected_quality) < 0.000001);
  CHECK(std::abs(scored.score - expected_selection) < 0.000001);

  auto different_selection_inputs = ap;
  different_selection_inputs.history_attempts = 0;
  different_selection_inputs.history_successes = 0;
  different_selection_inputs.recent_failures = 0;
  different_selection_inputs.current = false;
  const auto changed = score_access_point(different_selection_inputs);
  CHECK(std::abs(changed.quality_score - scored.quality_score) < 0.000001);
  CHECK(std::abs(changed.score - scored.score) > 0.000001);
}

void test_tie_breaking() {
  auto a = base_ap("00:00:00:00:00:02");
  auto b = base_ap("00:00:00:00:00:01");
  auto ranked = rank_access_points({a, b});
  CHECK(ranked.front().ap.bssid == b.bssid); // Stable final BSSID tie break.
  a.current = true;
  b.current = false;
  ScorePolicy no_bonus; no_bonus.current_ap_bonus = 0;
  ranked = rank_access_points({b, a}, no_bonus);
  CHECK(ranked.front().ap.current);
}

void test_failure_history_penalty() {
  auto clean = base_ap("00:00:00:00:00:01");
  auto failed = clean;
  failed.bssid = "00:00:00:00:00:02";
  failed.history_attempts = 3; failed.recent_failures = 3;
  CHECK(score_access_point(failed).score < score_access_point(clean).score);
}

void test_bssid_and_channels() {
  CHECK(valid_bssid("98:8F:00:41:5A:E4"));
  CHECK(!valid_bssid("98:8F:00:41:5A"));
  CHECK(requested_bssid_matched("98-8f-00-41-5a-e4", "98:8F:00:41:5A:E4"));
  CHECK(!requested_bssid_matched("98:8F:00:41:5A:E4", "98:8F:00:41:42:83"));
  CHECK(frequency_to_channel(2412000) == 1);
  CHECK(frequency_to_channel(5180000) == 36);
  CHECK(frequency_to_channel(5955000) == 1);
}

void test_retry_limiter() {
  RetryLimiter limiter(2, std::chrono::seconds(5));
  const auto t = std::chrono::steady_clock::now();
  CHECK(limiter.allow(t));
  CHECK(!limiter.allow(t + std::chrono::seconds(1)));
  CHECK(limiter.allow(t + std::chrono::seconds(5)));
  CHECK(!limiter.allow(t + std::chrono::seconds(20)));
  CHECK(limiter.attempts() == 2);
}

void test_usable_unicast_ipv4_classification() {
  CHECK(is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{192, 168, 10, 20}));
  CHECK(is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{10, 0, 0, 1}));
  CHECK(is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{169, 255, 1, 1}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{0, 0, 0, 0}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{127, 0, 0, 1}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{169, 254, 0, 1}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{169, 254, 255, 255}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{224, 0, 0, 1}));
  CHECK(!is_usable_unicast_ipv4(std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}

void test_old_history_format_remains_readable() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wifiopt-old-history-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  {
    Storage storage(root);
    {
      std::ofstream history(root / "history.tsv", std::ios::binary);
      history << "123\tAP-TUT5\t98:8F:00:41:42:83\t98:8F:00:41:42:83\t1\t0\tlegacy row\n";
    }
    auto rows = storage.load_history();
    CHECK(rows.size() == 1);
    CHECK(rows[0].detail == "legacy row");
    CHECK(!rows[0].ip_address_verified.has_value());

    storage.append_history({124, "AP-TUT5", "98:8F:00:41:42:84",
                            "98:8F:00:41:42:84", false, 0,
                            "IPv4 verification failed", std::optional<bool>{false}});
    rows = storage.load_history();
    CHECK(rows.size() == 2);
    CHECK(rows[1].ip_address_verified.has_value());
    CHECK(!*rows[1].ip_address_verified);
  }
  std::filesystem::remove_all(root);
}

void test_storage_initial_off_and_history() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wifiopt-test-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  {
    Storage storage(root);
    const auto initial = storage.load_config();
    CHECK(!initial.enabled);
    CHECK(std::filesystem::exists(root / "config.json"));
    storage.append_history({123, "AP-TUT5", "98:8F:00:41:42:83", "", false, 845, "EAP failure"});
    auto aps = std::vector<AccessPoint>{base_ap("98:8F:00:41:42:83")};
    storage.enrich_with_history(aps);
    CHECK(aps[0].history_attempts == 1);
    CHECK(aps[0].recent_failures == 1);
    const auto rows = storage.load_history();
    CHECK(rows.size() == 1 && rows[0].error_code == 845 && !rows[0].success);
    storage.record_observations(aps);
    CHECK(aps[0].last_seen_unix.has_value());
    CHECK(std::filesystem::exists(root / "last_seen.tsv"));
    std::ifstream last_seen(root / "last_seen.tsv", std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(last_seen)),
                            std::istreambuf_iterator<char>());
    CHECK(saved.find("98:8F:00:41:42:83") != std::string::npos);
  }
  std::filesystem::remove_all(root);
}

void test_storage_target_ssid_update_preserves_other_fields() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wifiopt-target-test-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  {
    Storage storage(root);
    AutoConfig original;
    original.enabled = true;
    original.interval_seconds = 137;
    original.min_improvement = 9.5;
    original.cooldown_seconds = 731;
    original.target_ssid = "OriginalNetwork";
    storage.save_config(original);

    auto target_only_update = storage.load_config();
    target_only_update.target_ssid = "TestNetwork123";
    storage.save_config(target_only_update);

    const auto saved = storage.load_config();
    CHECK(saved.target_ssid == "TestNetwork123");
    CHECK(saved.enabled == original.enabled);
    CHECK(saved.interval_seconds == original.interval_seconds);
    CHECK(std::abs(saved.min_improvement - original.min_improvement) < 0.001);
    CHECK(saved.cooldown_seconds == original.cooldown_seconds);
  }
  std::filesystem::remove_all(root);
}

void test_scan_json_round_trip_and_duration_parser() {
  auto first = base_ap("00:00:00:00:00:11", -51);
  first.ssid = "quoted \"network\"";
  first.bss_load = BssLoad{12, 44, 300};
  first.phy_standard = "802.11ax (HE)";
  first.beacon_interval_tu = static_cast<std::uint16_t>(100);
  first.last_seen_unix = 123456;
  auto second = base_ap("00:00:00:00:00:12", -72);
  const auto encoded = scan_json_document({first, second});
  const auto decoded = parse_scan_json_document(encoded);
  CHECK(decoded.size() == 2);
  CHECK(decoded[0].ssid == first.ssid);
  CHECK(decoded[0].bssid == first.bssid);
  CHECK(decoded[0].bss_load.has_value());
  CHECK(decoded[0].bss_load->channel_utilization == 44);
  CHECK(decoded[0].phy_standard == first.phy_standard);
  CHECK(decoded[0].last_seen_unix == first.last_seen_unix);
  CHECK(parse_duration_seconds("30m") == 1800);
  CHECK(parse_duration_seconds("7d") == 604800);
  CHECK(parse_duration_seconds("5", true) == 5);
  CHECK(!parse_duration_seconds("0s"));
  CHECK(!parse_duration_seconds("bad"));
}

void test_configurable_scoring_is_default_compatible_and_changes_ranking() {
  auto strong = base_ap("00:00:00:00:00:21", -45);
  strong.bss_load = BssLoad{4, 240, 0};
  auto clear = base_ap("00:00:00:00:00:22", -80);
  clear.bss_load = BssLoad{4, 0, 0};

  const auto implicit = rank_access_points({strong, clear});
  const ScorePolicy defaults;
  const auto explicit_defaults = rank_access_points({strong, clear}, defaults);
  CHECK(implicit.size() == explicit_defaults.size());
  CHECK(implicit[0].ap.bssid == explicit_defaults[0].ap.bssid);
  CHECK(std::abs(implicit[0].scoring.score - explicit_defaults[0].scoring.score) < 0.000001);
  CHECK(implicit[0].ap.bssid == strong.bssid);

  const auto custom = parse_score_policy_json(
      R"({"signal_rssi":0,"signal_quality":0,"bss_load":100,"bss_load_neutral":50})");
  const auto reranked = rank_access_points({strong, clear}, custom);
  CHECK(reranked[0].ap.bssid == clear.bssid);
  const auto repeated = rank_access_points({strong, clear}, custom);
  CHECK(repeated[0].ap.bssid == reranked[0].ap.bssid);
  CHECK(std::abs(repeated[0].scoring.score - reranked[0].scoring.score) < 0.000001);
}

void test_history_statistics() {
  const std::vector<ConnectionRecord> rows{
      {100, "x", "00:00:00:00:00:01", "00:00:00:00:00:01", true, 0, "ok"},
      {200, "x", "00:00:00:00:00:02", "00:00:00:00:00:02", false, 845, "fail"},
      {300, "x", "00:00:00:00:00:02", "00:00:00:00:00:02", true, 0, "ok"}};
  const auto stats = compute_history_stats(rows);
  CHECK(stats.samples == 3);
  CHECK(stats.first_unix == 100);
  CHECK(stats.last_unix == 300);
  CHECK(stats.ap_switches == 1);
  CHECK(stats.by_bssid.at("00:00:00:00:00:02").attempts == 2);
  CHECK(stats.by_bssid.at("00:00:00:00:00:02").successes == 1);
  CHECK(stats.error_counts.at(845) == 1);
}

void test_blackbox_ring_and_disconnect_detection() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wifiopt-blackbox-ring-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  {
    BlackBox blackbox(root, 3, 10);
    for (std::int64_t timestamp = 1; timestamp <= 4; ++timestamp) {
      WifiObservation observation{timestamp, "test", "00:00:00:00:00:01", -50,
                                  80, true};
      CHECK(!blackbox.observe(observation));
    }
    const auto ring = blackbox.observations();
    CHECK(ring.size() == 3);
    CHECK(ring.front().unix_time == 2);

    const WifiObservation disconnected{5, {}, {}, -100, 0, false};
    const auto failure = blackbox.observe(disconnected);
    CHECK(failure.has_value());
    CHECK(failure->event_type == BlackBoxEventType::unexpected_disconnect);
    CHECK(failure->observations_before.size() == 3);
    CHECK(failure->related_bssid == "00:00:00:00:00:01");
    CHECK(blackbox.load_records().size() == 1);

    CHECK(!blackbox.observe({6, "test", "00:00:00:00:00:01", -55, 70, true}));
    CHECK(!blackbox.observe({7, {}, {}, -100, 0, false}, true));
    CHECK(blackbox.load_records().size() == 1);
  }
  std::filesystem::remove_all(root);
}

void test_blackbox_rotation_and_bssid_attribution() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wifiopt-blackbox-rotation-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  {
    BlackBox blackbox(root, 2, 3);
    for (std::int64_t timestamp = 100; timestamp < 105; ++timestamp) {
      const auto record = blackbox.record_connection(
          {timestamp, "test", "00:00:00:00:00:10", {}, false, 845, "EAP failure"});
      CHECK(record.has_value());
      CHECK(record->related_bssid == "00:00:00:00:00:10");
      CHECK(record->possible_cause.find("does not by itself prove") != std::string::npos);
    }
    const auto records = blackbox.load_records();
    CHECK(records.size() == 3);
    CHECK(records.front().unix_time == 102);
    CHECK(records.back().unix_time == 104);

    const auto unknown = blackbox.record_connection(
        {200, "test", {}, {}, false, 1, "unknown target"});
    CHECK(unknown.has_value());
    CHECK(unknown->related_bssid.empty());
    CHECK(unknown->requested_bssid.empty());
    CHECK(unknown->actual_bssid.empty());

    const auto mismatch = blackbox.record_connection(
        {201, "test", "00:00:00:00:00:20", "00:00:00:00:00:21", true, 0,
         "different AP"});
    CHECK(mismatch.has_value());
    CHECK(mismatch->event_type == BlackBoxEventType::ap_switch_failure);
    CHECK(mismatch->related_bssid == "00:00:00:00:00:20");

    const auto no_ip = blackbox.record_connection(
        {202, "test", "00:00:00:00:00:30", "00:00:00:00:00:30", false, 0,
         "IPv4 verification failed", std::optional<bool>{false}});
    CHECK(no_ip.has_value());
    CHECK(no_ip->ip_address_verified.has_value() && !*no_ip->ip_address_verified);
    CHECK(no_ip->possible_cause.find("non-APIPA IPv4") != std::string::npos);
    const auto reloaded = blackbox.load_records();
    CHECK(!reloaded.empty());
    CHECK(reloaded.back().ip_address_verified.has_value());
    CHECK(!*reloaded.back().ip_address_verified);
  }
  std::filesystem::remove_all(root);
}

void test_eventlog_xml_parser() {
  const auto parsed = parse_wlan_event_xml(R"xml(
<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
  <System><EventID>12013</EventID><TimeCreated SystemTime="2026-10-13T01:02:03.456Z"/></System>
  <EventData>
    <Data Name="SSID">Corp&amp;Lab</Data>
    <Data Name="BSSID">98-8f-00-41-5a-e4</Data>
    <Data Name="ReasonCode">845</Data>
    <Data Name="FailureReason">802.1X/EAP authentication failed</Data>
  </EventData>
</Event>)xml");
  CHECK(parsed.has_value());
  CHECK(parsed->event_id == 12013);
  CHECK(parsed->unix_time > 0);
  CHECK(parsed->ssid == "Corp&Lab");
  CHECK(parsed->bssid == "98:8F:00:41:5A:E4");
  CHECK(parsed->error_code == 845U);
  CHECK(parsed->description == "802.1X/EAP authentication failed");
  CHECK(!parse_wlan_event_xml("<Event><System/></Event>"));
}

void test_gui_comparison_logic() {
  AccessPoint first;
  first.bssid = "00:11:22:33:44:50";
  first.frequency_khz = 5180000;
  first.channel = 36;
  first.rssi_dbm = -48;
  first.history_attempts = 8;
  first.history_successes = 7;
  AccessPoint second = first;
  second.bssid = "00:11:22:33:44:58";
  second.rssi_dbm = -65;
  second.history_successes = 4;

  const auto possible = assess_same_physical_ap(first, second);
  CHECK(possible.possibly_same);
  CHECK(possible.evidence.size() == 3);
  CHECK(possible.explanation.find("not conclusive proof") != std::string::npos);

  second.channel = 40;
  second.frequency_khz = 5200000;
  const auto rejected = assess_same_physical_ap(first, second);
  CHECK(!rejected.possibly_same);
  CHECK(rejected.explanation.find("BSSID similarity alone") != std::string::npos);

  RankedAccessPoint ranked_first{first, ScoreResult{82.0, 61.0, {}}};
  RankedAccessPoint ranked_second{second, ScoreResult{70.0, 55.0, {}}};
  const std::array selected{ranked_first, ranked_second};
  const auto recommendation = build_ap_recommendation(selected);
  CHECK(recommendation.find("AP A is recommended") != std::string::npos);
  CHECK(recommendation.find("strongest RSSI") != std::string::npos);
  CHECK(recommendation.find("BSS Load information is unavailable") != std::string::npos);
}
} // namespace

int main() {
  test_bss_load_valid();
  test_bss_load_missing();
  test_bss_load_truncated();
  test_information_elements_valid_and_unknown();
  test_information_elements_truncated();
  test_scoring_and_missing_values();
  test_quality_and_selection_scores();
  test_tie_breaking();
  test_failure_history_penalty();
  test_bssid_and_channels();
  test_retry_limiter();
  test_usable_unicast_ipv4_classification();
  test_old_history_format_remains_readable();
  test_storage_initial_off_and_history();
  test_storage_target_ssid_update_preserves_other_fields();
  test_scan_json_round_trip_and_duration_parser();
  test_configurable_scoring_is_default_compatible_and_changes_ranking();
  test_history_statistics();
  test_blackbox_ring_and_disconnect_detection();
  test_blackbox_rotation_and_bssid_attribution();
  test_eventlog_xml_parser();
  test_gui_comparison_logic();
  if (failures) { std::cerr << failures << " test assertion(s) failed\n"; return 1; }
  std::cout << "All 22 test groups passed\n";
  return 0;
}
