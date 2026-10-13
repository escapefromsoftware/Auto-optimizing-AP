#include "wifiopt/core.hpp"
#include "wifiopt/storage.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
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
} // namespace

int main() {
  test_bss_load_valid();
  test_bss_load_missing();
  test_bss_load_truncated();
  test_scoring_and_missing_values();
  test_tie_breaking();
  test_failure_history_penalty();
  test_bssid_and_channels();
  test_retry_limiter();
  test_storage_initial_off_and_history();
  test_storage_target_ssid_update_preserves_other_fields();
  if (failures) { std::cerr << failures << " test assertion(s) failed\n"; return 1; }
  std::cout << "All 10 test groups passed\n";
  return 0;
}
