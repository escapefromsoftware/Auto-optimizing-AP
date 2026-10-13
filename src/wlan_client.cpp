#include "wifiopt/wlan_client.hpp"

#include <objbase.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

namespace wifiopt {
namespace {

template <typename T> struct WlanDeleter { void operator()(T* p) const noexcept { if (p) WlanFreeMemory(p); } };
template <typename T> using wlan_ptr = std::unique_ptr<T, WlanDeleter<T>>;

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size) size = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size))
    MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string ssid_string(const DOT11_SSID& ssid) {
  return std::string(reinterpret_cast<const char*>(ssid.ucSSID),
                     reinterpret_cast<const char*>(ssid.ucSSID) + ssid.uSSIDLength);
}

std::string bssid_string(const DOT11_MAC_ADDRESS& mac) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (int i = 0; i < 6; ++i) {
    if (i) out << ':';
    out << std::setw(2) << static_cast<unsigned>(mac[i]);
  }
  return out.str();
}

bool parse_bssid(const std::string& text, DOT11_MAC_ADDRESS& mac) {
  if (!valid_bssid(text)) return false;
  for (int i = 0; i < 6; ++i) {
    try { mac[i] = static_cast<UCHAR>(std::stoul(text.substr(static_cast<std::size_t>(i) * 3, 2), nullptr, 16)); }
    catch (...) { return false; }
  }
  return true;
}

bool same_guid(const GUID& a, const GUID& b) noexcept { return InlineIsEqualGUID(a, b) != 0; }

std::optional<std::string> phy_standard(DOT11_PHY_TYPE type) {
  switch (type) {
    case dot11_phy_type_fhss: return "FHSS (legacy 802.11)";
    case dot11_phy_type_dsss: return "DSSS (legacy 802.11)";
    case dot11_phy_type_irbaseband: return "Infrared (legacy 802.11)";
    case dot11_phy_type_ofdm: return "802.11a";
    case dot11_phy_type_hrdsss: return "802.11b";
    case dot11_phy_type_erp: return "802.11g";
    case dot11_phy_type_ht: return "802.11n (HT)";
    case dot11_phy_type_vht: return "802.11ac (VHT)";
    case dot11_phy_type_dmg: return "802.11ad (DMG)";
    case dot11_phy_type_he: return "802.11ax (HE)";
    case dot11_phy_type_eht: return "802.11be (EHT)";
    default: return std::nullopt;
  }
}

std::wstring find_profile(HANDLE handle, const GUID& guid, const std::string& ssid) {
  PWLAN_PROFILE_INFO_LIST raw = nullptr;
  const DWORD rc = WlanGetProfileList(handle, &guid, nullptr, &raw);
  if (rc != ERROR_SUCCESS) throw WifiError("WlanGetProfileList", rc);
  wlan_ptr<WLAN_PROFILE_INFO_LIST> profiles(raw);
  const auto wanted = utf8_to_wide(ssid);
  for (DWORD i = 0; i < profiles->dwNumberOfItems; ++i) {
    const std::wstring name(profiles->ProfileInfo[i].strProfileName);
    if (_wcsicmp(name.c_str(), wanted.c_str()) == 0) return name;
  }
  // A profile name may differ from the SSID. Inspect read-only profile XML and
  // match the SSID section; never create or alter the enterprise profile.
  const std::wstring needle = L"<name>" + wanted + L"</name>";
  for (DWORD i = 0; i < profiles->dwNumberOfItems; ++i) {
    LPWSTR xml_raw = nullptr;
    DWORD flags = 0;
    DWORD access = 0;
    if (WlanGetProfile(handle, &guid, profiles->ProfileInfo[i].strProfileName, nullptr,
                       &xml_raw, &flags, &access) != ERROR_SUCCESS) continue;
    wlan_ptr<wchar_t> xml(xml_raw);
    const std::wstring text(xml_raw);
    const auto ssid_start = text.find(L"<SSID>");
    const auto ssid_end = ssid_start == std::wstring::npos ? std::wstring::npos : text.find(L"</SSID>", ssid_start);
    if (ssid_start != std::wstring::npos && ssid_end != std::wstring::npos &&
        text.find(needle, ssid_start) < ssid_end) return profiles->ProfileInfo[i].strProfileName;
  }
  return {};
}

} // namespace

std::string windows_error_message(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD count = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string message = count ? wide_to_utf8(std::wstring(buffer, count)) : "Windows error " + std::to_string(code);
  if (buffer) LocalFree(buffer);
  while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) message.pop_back();
  return message;
}

WifiError::WifiError(std::string operation, DWORD code)
    : std::runtime_error(operation + " failed (" + std::to_string(code) + "): " + windows_error_message(code)), code_(code) {}

WlanClient::WlanClient() {
  const DWORD rc = WlanOpenHandle(2, nullptr, &negotiated_version_, &handle_);
  if (rc != ERROR_SUCCESS) throw WifiError("WlanOpenHandle", rc);
}

WlanClient::~WlanClient() { if (handle_) WlanCloseHandle(handle_, nullptr); }

std::vector<InterfaceInfo> WlanClient::interfaces() const {
  PWLAN_INTERFACE_INFO_LIST raw = nullptr;
  const DWORD rc = WlanEnumInterfaces(handle_, nullptr, &raw);
  if (rc != ERROR_SUCCESS) throw WifiError("WlanEnumInterfaces", rc);
  wlan_ptr<WLAN_INTERFACE_INFO_LIST> list(raw);
  std::vector<InterfaceInfo> result;
  result.reserve(list->dwNumberOfItems);
  for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
    const auto& item = list->InterfaceInfo[i];
    result.push_back({item.InterfaceGuid, item.strInterfaceDescription, item.isState});
  }
  return result;
}

ConnectionStatus WlanClient::status() const {
  for (const auto& iface : interfaces()) {
    if (iface.state != wlan_interface_state_connected) continue;
    DWORD size = 0;
    WLAN_OPCODE_VALUE_TYPE type{};
    PWLAN_CONNECTION_ATTRIBUTES raw = nullptr;
    const DWORD rc = WlanQueryInterface(handle_, &iface.guid, wlan_intf_opcode_current_connection,
                                        nullptr, &size, reinterpret_cast<PVOID*>(&raw), &type);
    if (rc != ERROR_SUCCESS) continue;
    wlan_ptr<WLAN_CONNECTION_ATTRIBUTES> attr(raw);
    ConnectionStatus result;
    result.connected = true;
    result.interface_name = wide_to_utf8(iface.description);
    result.ssid = ssid_string(attr->wlanAssociationAttributes.dot11Ssid);
    result.bssid = bssid_string(attr->wlanAssociationAttributes.dot11Bssid);
    result.profile = wide_to_utf8(attr->strProfileName);
    result.signal_quality = attr->wlanAssociationAttributes.wlanSignalQuality;
    PLONG rssi = nullptr;
    size = 0;
    if (WlanQueryInterface(handle_, &iface.guid, wlan_intf_opcode_rssi, nullptr, &size,
                           reinterpret_cast<PVOID*>(&rssi), &type) == ERROR_SUCCESS) {
      wlan_ptr<LONG> held(rssi);
      result.rssi_dbm = *held;
    }
    NET_LUID luid{};
    if (ConvertInterfaceGuidToLuid(&iface.guid, &luid) == NO_ERROR) {
      MIB_IF_ROW2 row{};
      row.InterfaceLuid = luid;
      if (GetIfEntry2(&row) == NO_ERROR) {
        result.receive_link_speed_bps = row.ReceiveLinkSpeed;
        result.transmit_link_speed_bps = row.TransmitLinkSpeed;
      }
    }
    return result;
  }
  return {};
}

std::vector<AccessPoint> WlanClient::scan(const std::optional<std::string>& filter,
                                          std::chrono::milliseconds wait) {
  const auto ifaces = interfaces();
  if (ifaces.empty()) throw std::runtime_error("No wireless LAN interface was found");
  DOT11_SSID wanted{};
  DOT11_SSID* wanted_ptr = nullptr;
  if (filter) {
    if (filter->size() > DOT11_SSID_MAX_LENGTH) throw std::invalid_argument("SSID is longer than 32 bytes");
    wanted.uSSIDLength = static_cast<ULONG>(filter->size());
    std::memcpy(wanted.ucSSID, filter->data(), filter->size());
    wanted_ptr = &wanted;
  }

  bool requested = false;
  DWORD last_error = ERROR_SUCCESS;
  for (const auto& iface : ifaces) {
    const DWORD rc = WlanScan(handle_, &iface.guid, wanted_ptr, nullptr, nullptr);
    if (rc == ERROR_SUCCESS) requested = true;
    else last_error = rc;
  }
  if (!requested && last_error != ERROR_SUCCESS) throw WifiError("WlanScan", last_error);
  std::this_thread::sleep_for(wait);

  const auto connected = status();
  std::vector<AccessPoint> result;
  std::set<std::string> seen;
  for (const auto& iface : ifaces) {
    PWLAN_BSS_LIST raw = nullptr;
    const DWORD rc = WlanGetNetworkBssList(handle_, &iface.guid, wanted_ptr, dot11_BSS_type_any,
                                           FALSE, nullptr, &raw);
    if (rc == ERROR_ACCESS_DENIED) {
      throw std::runtime_error("Wi-Fi BSS access was denied. Enable Windows Settings > Privacy & security > Location > Location services and allow desktop apps, then retry.");
    }
    if (rc != ERROR_SUCCESS) { last_error = rc; continue; }
    wlan_ptr<WLAN_BSS_LIST> list(raw);
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
      const auto& entry = list->wlanBssEntries[i];
      AccessPoint ap;
      ap.ssid = ssid_string(entry.dot11Ssid);
      if (filter && ap.ssid != *filter) continue;
      ap.bssid = bssid_string(entry.dot11Bssid);
      const std::string uniqueness = ap.bssid + "|" + ap.ssid;
      if (!seen.insert(uniqueness).second) continue;
      ap.rssi_dbm = entry.lRssi;
      ap.signal_quality = entry.uLinkQuality;
      ap.frequency_khz = entry.ulChCenterFrequency;
      ap.channel = frequency_to_channel(ap.frequency_khz);
      ap.current = connected.connected && requested_bssid_matched(ap.bssid, connected.bssid);
      if (ap.current) {
        ap.receive_link_speed_bps = connected.receive_link_speed_bps;
        ap.transmit_link_speed_bps = connected.transmit_link_speed_bps;
      }
      ap.phy_standard = phy_standard(entry.dot11BssPhyType);
      ap.beacon_interval_tu = entry.usBeaconPeriod;
      const auto* list_base = reinterpret_cast<const std::uint8_t*>(list.get());
      const auto* base = reinterpret_cast<const std::uint8_t*>(&entry);
      const auto entry_offset = static_cast<std::size_t>(base - list_base);
      const auto total_size = static_cast<std::size_t>(list->dwTotalSize);
      const auto ie_offset = static_cast<std::size_t>(entry.ulIeOffset);
      const auto ie_size = static_cast<std::size_t>(entry.ulIeSize);
      if (ie_size > 0 && ie_offset >= sizeof(WLAN_BSS_ENTRY) &&
          entry_offset <= total_size && ie_offset <= total_size - entry_offset &&
          ie_size <= total_size - entry_offset - ie_offset) {
        const std::span<const std::uint8_t> ies{base + ie_offset, ie_size};
        ap.raw_information_elements.assign(ies.begin(), ies.end());
        ap.bss_load = parse_bss_load(ies);
        if (const auto parsed = parse_information_elements(ies)) {
          ap.ht_capabilities = parsed->ht_capabilities;
          ap.vht_capabilities = parsed->vht_capabilities;
          ap.extended_capabilities = parsed->extended_capabilities;
          ap.bss_transition_management = parsed->bss_transition_management;
          ap.vendor_specific_ie_count = parsed->vendor_specific_count;
        }
      }
      result.push_back(std::move(ap));
    }
  }
  if (result.empty() && last_error != ERROR_SUCCESS) throw WifiError("WlanGetNetworkBssList", last_error);
  for (auto& ap : result) {
    ap.cochannel_neighbors = static_cast<unsigned>(std::count_if(result.begin(), result.end(), [&](const auto& other) {
      return other.channel != 0 && other.channel == ap.channel && other.bssid != ap.bssid;
    }));
  }
  return result;
}

void WINAPI WlanClient::notification_callback(PWLAN_NOTIFICATION_DATA data, PVOID context) {
  if (!data || !context || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
  if (data->NotificationCode != wlan_notification_acm_connection_complete &&
      data->NotificationCode != wlan_notification_acm_connection_attempt_fail) return;
  auto& waiter = *static_cast<NotificationWaiter*>(context);
  if (!same_guid(data->InterfaceGuid, waiter.interface_guid)) return;
  std::lock_guard lock(waiter.mutex);
  waiter.done = true;
  waiter.success = data->NotificationCode == wlan_notification_acm_connection_complete;
  if (data->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA) && data->pData) {
    waiter.reason = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData)->wlanReasonCode;
  }
  waiter.cv.notify_all();
}

ConnectResult WlanClient::connect_bssid(const std::string& ssid, const std::string& bssid,
                                        std::chrono::milliseconds timeout) {
  ConnectResult result;
  result.requested_bssid = normalize_bssid(bssid);
  DOT11_MAC_ADDRESS mac{};
  if (!parse_bssid(bssid, mac)) {
    result.api_error = ERROR_INVALID_PARAMETER;
    result.detail = "Invalid BSSID syntax";
    return result;
  }
  const auto ifaces = interfaces();
  if (ifaces.empty()) { result.detail = "No wireless interface"; return result; }
  auto selected = std::find_if(ifaces.begin(), ifaces.end(), [](const auto& i) { return i.state == wlan_interface_state_connected; });
  if (selected == ifaces.end()) selected = ifaces.begin();
  const auto profile = find_profile(handle_, selected->guid, ssid);
  if (profile.empty()) {
    result.api_error = ERROR_NOT_FOUND;
    result.detail = "No saved Wi-Fi profile named '" + ssid + "'; no profile or credentials were modified";
    return result;
  }

  DOT11_BSSID_LIST desired{};
  desired.Header.Type = 0x80; // NDIS_OBJECT_TYPE_DEFAULT
  desired.Header.Revision = 1; // DOT11_BSSID_LIST_REVISION_1
  desired.Header.Size = sizeof(DOT11_BSSID_LIST);
  desired.uNumOfEntries = 1;
  desired.uTotalNumOfEntries = 1;
  std::memcpy(desired.BSSIDs[0], mac, sizeof(mac));

  NotificationWaiter waiter;
  waiter.interface_guid = selected->guid;
  DWORD previous = 0;
  DWORD rc = WlanRegisterNotification(handle_, WLAN_NOTIFICATION_SOURCE_ACM, TRUE,
                                      &WlanClient::notification_callback, &waiter, nullptr, &previous);
  if (rc != ERROR_SUCCESS) { result.api_error = rc; result.detail = "WlanRegisterNotification: " + windows_error_message(rc); return result; }

  WLAN_CONNECTION_PARAMETERS params{};
  params.wlanConnectionMode = wlan_connection_mode_profile;
  params.strProfile = profile.c_str();
  params.dot11BssType = dot11_BSS_type_any;
  params.dwFlags = 0;
  params.pDesiredBssidList = &desired;
  rc = WlanConnect(handle_, &selected->guid, &params, nullptr);
  result.request_accepted = rc == ERROR_SUCCESS;
  if (rc == ERROR_SUCCESS) {
    std::unique_lock lock(waiter.mutex);
    result.completed = waiter.cv.wait_for(lock, timeout, [&] { return waiter.done; });
    result.timed_out = !result.completed;
    result.reason_code = waiter.reason;
    result.success = result.completed && waiter.success && waiter.reason == WLAN_REASON_CODE_SUCCESS;
  } else {
    result.api_error = rc;
    result.detail = "WlanConnect: " + windows_error_message(rc);
  }
  WlanRegisterNotification(handle_, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);

  const auto actual = status();
  result.actual_bssid = actual.bssid;
  result.desired_bssid_matched = actual.connected && requested_bssid_matched(bssid, actual.bssid);
  if (result.success && !result.desired_bssid_matched) result.detail = "Connection completed, but Windows/driver selected a different BSSID";
  else if (result.success) result.detail = "Connected to requested BSSID";
  else if (result.timed_out) result.detail = "Connection notification timed out";
  else if (result.detail.empty()) result.detail = "Connection failed; WLAN reason code " + std::to_string(result.reason_code);
  return result;
}

} // namespace wifiopt
