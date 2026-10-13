#pragma once

#include "wifiopt/core.hpp"

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wifiopt {

struct BlackBoxRecord;

enum class OutputFormat { text, json, csv, ndjson };

std::optional<OutputFormat> parse_output_format(std::string_view value) noexcept;
std::string json_escape(std::string_view value);
std::string csv_field(std::string_view value);
std::string access_point_json(const AccessPoint& ap,
                              const ScoreResult* score = nullptr,
                              bool include_reasons = false);
std::string scan_json_document(const std::vector<AccessPoint>& aps);
std::vector<AccessPoint> parse_scan_json_document(std::string_view text);
ScorePolicy parse_score_policy_json(std::string_view text);
std::optional<std::uint64_t> parse_duration_seconds(std::string_view value,
                                                    bool plain_seconds = false) noexcept;
std::string band_name(std::uint32_t frequency_khz);
std::string hex_bytes(const std::vector<std::uint8_t>& bytes);

struct ApHistoryStats {
  unsigned attempts{};
  unsigned successes{};
};

struct HistoryStats {
  std::size_t samples{};
  std::optional<std::int64_t> first_unix;
  std::optional<std::int64_t> last_unix;
  unsigned ap_switches{};
  std::map<std::string, ApHistoryStats> by_bssid;
  std::map<std::uint32_t, unsigned> error_counts;
  std::array<ApHistoryStats, 4> by_time_band{};
  std::optional<double> average_connection_time_ms;
};

HistoryStats compute_history_stats(const std::vector<ConnectionRecord>& rows);
std::string blackbox_record_json(const BlackBoxRecord& record);
BlackBoxRecord parse_blackbox_record_json(std::string_view text);

} // namespace wifiopt
