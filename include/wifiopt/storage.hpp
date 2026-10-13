#pragma once

#include "wifiopt/core.hpp"

#include <filesystem>
#include <vector>

namespace wifiopt {

class Storage {
 public:
  explicit Storage(std::filesystem::path root = {});
  const std::filesystem::path& root() const noexcept { return root_; }
  AutoConfig load_config() const;
  void save_config(const AutoConfig& config) const;
  DaemonState load_daemon_state() const;
  void save_daemon_state(const DaemonState& state) const;
  void append_history(const ConnectionRecord& record) const;
  std::vector<ConnectionRecord> load_history(std::size_t limit = 200) const;
  void enrich_with_history(std::vector<AccessPoint>& aps) const;
  void record_observations(std::vector<AccessPoint>& aps) const;
 private:
  std::filesystem::path root_;
};

std::int64_t unix_now() noexcept;

} // namespace wifiopt
