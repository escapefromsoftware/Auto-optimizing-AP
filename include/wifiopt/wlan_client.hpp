#pragma once

#include "wifiopt/core.hpp"

#include <windows.h>
#include <wlanapi.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wifiopt {

class WifiError : public std::runtime_error {
 public:
  WifiError(std::string operation, DWORD code);
  DWORD code() const noexcept { return code_; }
 private:
  DWORD code_{};
};

struct InterfaceInfo {
  GUID guid{};
  std::wstring description;
  WLAN_INTERFACE_STATE state{wlan_interface_state_not_ready};
};

struct ConnectionStatus {
  bool connected{};
  std::string interface_name;
  std::string ssid;
  std::string bssid;
  std::string profile;
  int rssi_dbm{-100};
  unsigned signal_quality{};
};

struct ConnectResult {
  bool request_accepted{};
  bool completed{};
  bool success{};
  bool desired_bssid_matched{};
  bool timed_out{};
  DWORD api_error{};
  DWORD reason_code{};
  std::string requested_bssid;
  std::string actual_bssid;
  std::string detail;
};

class WlanClient {
 public:
  WlanClient();
  ~WlanClient();
  WlanClient(const WlanClient&) = delete;
  WlanClient& operator=(const WlanClient&) = delete;

  std::vector<InterfaceInfo> interfaces() const;
  ConnectionStatus status() const;
  std::vector<AccessPoint> scan(const std::optional<std::string>& ssid = std::nullopt,
                                std::chrono::milliseconds wait = std::chrono::milliseconds(2500));
  ConnectResult connect_bssid(const std::string& ssid, const std::string& bssid,
                              std::chrono::milliseconds timeout = std::chrono::seconds(25));

 private:
  struct NotificationWaiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{};
    bool success{};
    DWORD reason{};
    GUID interface_guid{};
  };
  static void WINAPI notification_callback(PWLAN_NOTIFICATION_DATA data, PVOID context);
  HANDLE handle_{nullptr};
  DWORD negotiated_version_{};
};

std::string windows_error_message(DWORD code);

} // namespace wifiopt
