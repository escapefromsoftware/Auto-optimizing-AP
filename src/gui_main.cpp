#include "wifiopt/optimizer.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace wifiopt;

constexpr wchar_t kMainWindowClass[] = L"SmartWifiApOptimizerGuiWindow-v1";
constexpr wchar_t kHistoryWindowClass[] = L"SmartWifiApOptimizerHistoryWindow-v1";
constexpr wchar_t kWindowTitle[] = L"Smart Wi-Fi AP Optimizer";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kAsyncComplete = WM_APP + 2;
constexpr UINT kTrayIconId = 1;

enum ControlId : int {
  id_refresh = 1001,
  id_ranking = 1002,
  id_connect = 1003,
  id_auto = 1004,
  id_history = 1005,
  id_exit = 1006,
  id_history_list = 1007,
  id_target_combo = 1008,
  id_target_apply = 1009,
};

enum class AsyncKind { refresh, connect, automatic };

struct AsyncResult {
  AsyncKind kind{AsyncKind::refresh};
  ConnectionStatus status;
  bool has_status{};
  std::vector<RankedAccessPoint> ranked;
  std::vector<std::string> ssids;
  std::string rank_error;
  ConnectResult connection;
  bool connection_success{};
  std::wstring message;
  std::wstring fatal_error;
};

std::wstring to_wide(const std::string& value) {
  if (value.empty()) return {};
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size == 0) {
    code_page = CP_ACP;
    flags = 0;
    size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
  }
  if (size == 0) return L"(text conversion failed)";
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string to_utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size == 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring trim(std::wstring value) {
  const auto whitespace = [](wchar_t c) { return std::iswspace(c) != 0; };
  const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
  const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
  if (first >= last) return {};
  return {first, last};
}

std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) return {};
  buffer.resize(size);
  return std::filesystem::path(buffer);
}

int scaled(HWND window, int value) {
  return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

std::wstring history_time(std::int64_t unix_time) {
  const std::time_t raw = static_cast<std::time_t>(unix_time);
  std::tm local{};
  if (localtime_s(&local, &raw) != 0) return std::to_wstring(unix_time);
  wchar_t buffer[32]{};
  if (wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M:%S", &local) == 0) {
    return std::to_wstring(unix_time);
  }
  return buffer;
}

class GuiApp {
 public:
  explicit GuiApp(HINSTANCE instance)
      : instance_(instance),
        taskbar_created_message_(RegisterWindowMessageW(L"TaskbarCreated")),
        storage_(),
        config_(storage_.load_config()) {}
  ~GuiApp() {
    shutting_down_.store(true);
    if (worker_.joinable()) worker_.join();
  }

  bool create(int show_command) {
    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = &GuiApp::main_window_proc;
    main_class.hInstance = instance_;
    main_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    main_class.lpszClassName = kMainWindowClass;
    main_class.hIconSm = main_class.hIcon;
    if (!RegisterClassExW(&main_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW history_class = main_class;
    history_class.lpfnWndProc = &GuiApp::history_window_proc;
    history_class.lpszClassName = kHistoryWindowClass;
    if (!RegisterClassExW(&history_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window_ = CreateWindowExW(0, kMainWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650, nullptr, nullptr,
                              instance_, this);
    if (!window_) return false;
    ShowWindow(window_, show_command);
    UpdateWindow(window_);
    request_refresh();
    return true;
  }

  int run() {
    MSG message{};
    while (true) {
      const BOOL result = GetMessageW(&message, nullptr, 0, 0);
      if (result == 0) return static_cast<int>(message.wParam);
      if (result == -1) return 1;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

 private:
  static GuiApp* app_from(HWND window) {
    return reinterpret_cast<GuiApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }

  static LRESULT CALLBACK main_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
      app->window_ = window;
    }
    auto* app = app_from(window);
    return app ? app->handle_main_message(message, wparam, lparam)
               : DefWindowProcW(window, message, wparam, lparam);
  }

  static LRESULT CALLBACK history_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = app_from(window);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_CREATE:
        app->history_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, nullptr,
                                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                                 LBS_NOINTEGRALHEIGHT,
                                             0, 0, 0, 0, window,
                                             reinterpret_cast<HMENU>(id_history_list), app->instance_, nullptr);
        app->apply_font(app->history_list_);
        return 0;
      case WM_SIZE:
        MoveWindow(app->history_list_, scaled(window, 10), scaled(window, 10),
                   LOWORD(lparam) - scaled(window, 20), HIWORD(lparam) - scaled(window, 20), TRUE);
        return 0;
      case WM_CLOSE:
        DestroyWindow(window);
        return 0;
      case WM_NCDESTROY:
        app->history_window_ = nullptr;
        app->history_list_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  LRESULT handle_main_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbar_created_message_ != 0 && message == taskbar_created_message_) {
      tray_added_ = false;
      add_tray_icon();
      update_tray_tooltip();
      return 0;
    }
    switch (message) {
      case WM_CREATE:
        create_controls();
        add_tray_icon();
        update_auto_label();
        return 0;
      case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;
        layout_controls(LOWORD(lparam), HIWORD(lparam));
        return 0;
      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        recreate_font();
        return 0;
      }
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = scaled(window_, 760);
        info->ptMinTrackSize.y = scaled(window_, 500);
        return 0;
      }
      case WM_COMMAND:
        handle_command(LOWORD(wparam));
        return 0;
      case WM_CLOSE:
        ShowWindow(window_, tray_added_ ? SW_HIDE : SW_MINIMIZE);
        return 0;
      case WM_ENDSESSION:
        if (wparam != 0) begin_exit();
        return 0;
      case kTrayCallback:
        handle_tray_event(static_cast<UINT>(lparam));
        return 0;
      case kAsyncComplete:
        finish_worker();
        return 0;
      case WM_DESTROY:
        shutting_down_.store(true);
        if (worker_.joinable()) worker_.join();
        if (history_window_) DestroyWindow(history_window_);
        remove_tray_icon();
        if (font_) {
          DeleteObject(font_);
          font_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
  }

  void create_controls() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);

    status_label_ = make_control(0, L"STATIC", L"Wi-Fi status: checking...", SS_LEFT,
                                 0, 0, 0, 0, 0);
    refresh_button_ = make_control(0, L"BUTTON", L"Refresh", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_refresh);
    const auto target = target_text();
    target_label_ = make_control(0, L"STATIC", target.c_str(), SS_LEFT,
                                 0, 0, 0, 0, 0);
    target_combo_ = make_control(0, WC_COMBOBOXW, to_wide(config_.target_ssid).c_str(),
                                 CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                                 0, 0, 0, 0, id_target_combo);
    SetWindowTextW(target_combo_, to_wide(config_.target_ssid).c_str());
    target_apply_button_ = make_control(0, L"BUTTON", L"Apply", BS_PUSHBUTTON,
                                        0, 0, 0, 0, id_target_apply);
    ranking_ = make_control(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, id_ranking);
    ListView_SetExtendedListViewStyle(ranking_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    add_ranking_columns();
    connect_button_ = make_control(0, L"BUTTON", L"Connect to selected AP", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_connect);
    auto_button_ = make_control(0, L"BUTTON", L"Automatic optimization: ...", BS_PUSHBUTTON,
                                0, 0, 0, 0, id_auto);
    history_button_ = make_control(0, L"BUTTON", L"History", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_history);
    result_label_ = make_control(0, L"STATIC", L"Ready", SS_LEFT,
                                 0, 0, 0, 0, 0);
    recreate_font();
  }

  HWND make_control(DWORD extended_style, const wchar_t* class_name, const wchar_t* text,
                    DWORD style, int x, int y, int width, int height, int id) const {
    return CreateWindowExW(extended_style, class_name, text,
                           WS_CHILD | WS_VISIBLE | style, x, y, width, height, window_,
                           id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           instance_, nullptr);
  }

  void add_ranking_columns() const {
    struct Column { const wchar_t* name; int width; };
    const Column columns[]{{L"BSSID", 150}, {L"SSID", 170}, {L"RSSI (dBm)", 90},
                           {L"Quality (%)", 90}, {L"Channel", 75}, {L"Score", 75},
                           {L"Current", 75}};
    for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
      column.pszText = const_cast<wchar_t*>(columns[index].name);
      column.cx = columns[index].width;
      column.iSubItem = index;
      ListView_InsertColumn(ranking_, index, &column);
    }
  }

  void layout_controls(int width, int height) const {
    const int margin = scaled(window_, 16);
    const int gap = scaled(window_, 8);
    const int button_height = scaled(window_, 30);
    const int refresh_width = scaled(window_, 100);
    const int apply_width = scaled(window_, 90);
    MoveWindow(status_label_, margin, scaled(window_, 16),
               width - margin * 2 - refresh_width - gap, scaled(window_, 24), TRUE);
    MoveWindow(refresh_button_, width - margin - refresh_width, scaled(window_, 12),
               refresh_width, button_height, TRUE);
    MoveWindow(target_label_, margin, scaled(window_, 49), width - margin * 2,
               scaled(window_, 22), TRUE);
    MoveWindow(target_combo_, margin, scaled(window_, 73),
               width - margin * 2 - apply_width - gap, scaled(window_, 240), TRUE);
    MoveWindow(target_apply_button_, width - margin - apply_width, scaled(window_, 72),
               apply_width, button_height, TRUE);
    const int buttons_y = height - scaled(window_, 86);
    const int list_top = scaled(window_, 112);
    MoveWindow(ranking_, margin, list_top, width - margin * 2,
               (buttons_y - gap) - list_top, TRUE);
    MoveWindow(connect_button_, margin, buttons_y, scaled(window_, 190), button_height, TRUE);
    MoveWindow(auto_button_, margin + scaled(window_, 198), buttons_y,
               scaled(window_, 210), button_height, TRUE);
    MoveWindow(history_button_, margin + scaled(window_, 416), buttons_y,
               scaled(window_, 100), button_height, TRUE);
    MoveWindow(result_label_, margin, height - scaled(window_, 48), width - margin * 2,
               scaled(window_, 38), TRUE);

    const int available = width - margin * 2;
    ListView_SetColumnWidth(ranking_, 0, scaled(window_, 145));
    ListView_SetColumnWidth(ranking_, 1, (std::max)(scaled(window_, 120), available - scaled(window_, 550)));
  }

  void recreate_font() {
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-scaled(window_, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HWND controls[]{status_label_, refresh_button_, target_label_, target_combo_,
                          target_apply_button_, ranking_, connect_button_, auto_button_,
                          history_button_, result_label_, history_list_};
    for (HWND control : controls) apply_font(control);
  }

  void apply_font(HWND control) const {
    if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }

  std::wstring target_text() const {
    return L"Target SSID: " + to_wide(config_.target_ssid) + L"  (ranked access points)";
  }

  void handle_command(WORD id) {
    switch (id) {
      case id_refresh:
        request_refresh();
        break;
      case id_target_apply:
        request_target_apply();
        break;
      case id_connect:
        request_connect();
        break;
      case id_auto:
        request_auto_toggle();
        break;
      case id_history:
        show_history();
        break;
      case id_exit:
        begin_exit();
        break;
      default:
        break;
    }
  }

  void request_refresh() {
    const std::string target_ssid = config_.target_ssid;
    start_worker([this, target_ssid] {
      AsyncResult result;
      result.kind = AsyncKind::refresh;
      WlanClient wlan;
      result.status = wlan.status();
      result.has_status = true;
      try {
        auto aps = wlan.scan(std::nullopt);
        std::set<std::string> unique_ssids;
        for (const auto& ap : aps) {
          if (!ap.ssid.empty()) unique_ssids.insert(ap.ssid);
        }
        result.ssids.assign(unique_ssids.begin(), unique_ssids.end());
        aps.erase(std::remove_if(aps.begin(), aps.end(), [&](const auto& ap) {
                    return ap.ssid != target_ssid;
                  }),
                  aps.end());
        storage_.enrich_with_history(aps);
        result.ranked = rank_access_points(std::move(aps));
      } catch (const std::exception& error) {
        result.rank_error = error.what();
      }
      return result;
    });
  }

  void request_target_apply() {
    const int length = GetWindowTextLengthW(target_combo_);
    std::wstring text(static_cast<std::size_t>((std::max)(length, 0)) + 1, L'\0');
    const int copied = GetWindowTextW(target_combo_, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>((std::max)(copied, 0)));
    text = trim(std::move(text));
    if (text.empty()) {
      SetWindowTextW(result_label_, L"Target SSID must not be empty or whitespace only.");
      return;
    }
    try {
      auto latest = storage_.load_config();
      latest.target_ssid = to_utf8(text);
      if (latest.target_ssid.empty()) {
        SetWindowTextW(result_label_, L"Target SSID could not be converted to UTF-8.");
        return;
      }
      storage_.save_config(latest);
      config_ = std::move(latest);
      SetWindowTextW(target_combo_, to_wide(config_.target_ssid).c_str());
      SetWindowTextW(target_label_, target_text().c_str());
      SetWindowTextW(result_label_, L"Target SSID saved; refreshing ranking...");
      request_refresh();
    } catch (const std::exception& error) {
      SetWindowTextW(result_label_, (L"Could not save target SSID: " + to_wide(error.what())).c_str());
    }
  }

  void request_connect() {
    const int selected = ListView_GetNextItem(ranking_, -1, LVNI_SELECTED);
    if (selected < 0) {
      SetWindowTextW(result_label_, L"Select an access point in the ranking first.");
      return;
    }
    wchar_t bssid_buffer[64]{};
    ListView_GetItemText(ranking_, selected, 0, bssid_buffer, static_cast<int>(std::size(bssid_buffer)));
    const std::wstring selected_bssid(bssid_buffer);
    const std::string bssid = ranked_.at(static_cast<std::size_t>(selected)).ap.bssid;
    const std::string ssid = config_.target_ssid;
    SetWindowTextW(result_label_, (L"Connecting to " + selected_bssid + L" ...").c_str());
    start_worker([this, ssid, bssid] {
      AsyncResult result;
      result.kind = AsyncKind::connect;
      WlanClient wlan;
      result.connection = wlan.connect_bssid(ssid, bssid);
      result.connection_success = result.connection.success && result.connection.desired_bssid_matched;
      storage_.append_history({unix_now(), ssid, result.connection.requested_bssid,
                               result.connection.actual_bssid, result.connection_success,
                               result.connection.api_error ? result.connection.api_error
                                                           : result.connection.reason_code,
                               result.connection.detail});
      return result;
    });
  }

  void request_auto_toggle() {
    const bool turn_off = automatic_is_on();
    start_worker([this, turn_off] {
      AsyncResult result;
      result.kind = AsyncKind::automatic;
      auto config = storage_.load_config();
      if (turn_off) {
        config.enabled = false;
        storage_.save_config(config);
        for (int index = 0; index < 30; ++index) {
          const auto state = storage_.load_daemon_state();
          if (!process_is_running(state.pid)) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        result.message = L"Automatic optimization OFF; the current Wi-Fi connection was left unchanged.";
        return result;
      }

      config.enabled = true;
      storage_.save_config(config);
      const auto existing = storage_.load_daemon_state();
      if (process_is_running(existing.pid)) {
        result.message = L"Automatic optimization is already ON (PID " +
                         std::to_wstring(existing.pid) + L").";
        return result;
      }
      std::uint32_t pid = 0;
      std::string error;
      const auto cli = executable_path().parent_path() / L"wifiopt.exe";
      if (!start_daemon_process(cli, config.target_ssid, pid, error)) {
        config.enabled = false;
        storage_.save_config(config);
        result.fatal_error = L"Failed to start monitor: " + to_wide(error);
        return result;
      }
      result.message = L"Automatic optimization ON; monitor PID " + std::to_wstring(pid) + L".";
      return result;
    });
  }

  void start_worker(std::function<AsyncResult()> job) {
    if (busy_.exchange(true)) {
      SetWindowTextW(result_label_, L"Another operation is still running.");
      return;
    }
    enable_operation_controls(false);
    try {
      if (worker_.joinable()) worker_.join();
      worker_ = std::thread([this, job = std::move(job)]() mutable {
        AsyncResult result;
        try {
          result = job();
        } catch (const std::exception& error) {
          result.fatal_error = to_wide(error.what());
        } catch (...) {
          result.fatal_error = L"Unknown error";
        }
        {
          std::lock_guard lock(result_mutex_);
          pending_result_ = std::move(result);
        }
        if (!shutting_down_.load()) PostMessageW(window_, kAsyncComplete, 0, 0);
      });
    } catch (const std::exception& error) {
      busy_.store(false);
      enable_operation_controls(true);
      SetWindowTextW(result_label_, to_wide(error.what()).c_str());
    }
  }

  void finish_worker() {
    if (worker_.joinable()) worker_.join();
    std::optional<AsyncResult> result;
    {
      std::lock_guard lock(result_mutex_);
      result = std::move(pending_result_);
      pending_result_.reset();
    }
    busy_.store(false);
    enable_operation_controls(true);
    if (!result) return;
    if (!result->fatal_error.empty()) {
      SetWindowTextW(result_label_, (L"Error: " + result->fatal_error).c_str());
      update_auto_label();
      return;
    }
    switch (result->kind) {
      case AsyncKind::refresh:
        apply_refresh(*result);
        break;
      case AsyncKind::connect:
        apply_connect(*result);
        request_refresh();
        break;
      case AsyncKind::automatic:
        SetWindowTextW(result_label_, result->message.c_str());
        config_ = storage_.load_config();
        SetWindowTextW(target_label_, target_text().c_str());
        update_auto_label();
        break;
    }
  }

  void apply_refresh(const AsyncResult& result) {
    if (result.has_status) {
      current_status_ = result.status;
      if (result.status.connected) {
        std::wostringstream text;
        text << L"Connected  |  SSID: " << to_wide(result.status.ssid)
             << L"  |  BSSID: " << to_wide(result.status.bssid)
             << L"  |  Signal: " << result.status.signal_quality << L"%  |  ";
        if (result.status.rssi_dbm > -100) text << result.status.rssi_dbm << L" dBm";
        else text << L"RSSI unavailable";
        SetWindowTextW(status_label_, text.str().c_str());
      } else {
        SetWindowTextW(status_label_, L"Wi-Fi: disconnected");
      }
      update_tray_tooltip();
    }
    ranked_ = result.ranked;
    populate_ranking();
    if (!result.rank_error.empty()) {
      SetWindowTextW(result_label_, (L"Ranking failed: " + to_wide(result.rank_error)).c_str());
    } else {
      SetWindowTextW(result_label_, (L"Ranking updated: " + std::to_wstring(ranked_.size()) +
                                     L" access point(s).").c_str());
    }
    config_ = storage_.load_config();
    SetWindowTextW(target_label_, target_text().c_str());
    populate_target_ssids(result.ssids);
    update_auto_label();
  }

  void populate_target_ssids(const std::vector<std::string>& ssids) const {
    SendMessageW(target_combo_, CB_RESETCONTENT, 0, 0);
    for (const auto& ssid : ssids) {
      const auto text = to_wide(ssid);
      SendMessageW(target_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SetWindowTextW(target_combo_, to_wide(config_.target_ssid).c_str());
  }

  void apply_connect(const AsyncResult& result) const {
    const auto& connection = result.connection;
    std::wstring text = result.connection_success ? L"SUCCESS" : L"FAILED";
    text += L"  |  Requested: " + to_wide(connection.requested_bssid);
    text += L"  |  Actual: " + (connection.actual_bssid.empty() ? L"(none)" : to_wide(connection.actual_bssid));
    if (!connection.detail.empty()) text += L"  |  " + to_wide(connection.detail);
    SetWindowTextW(result_label_, text.c_str());
  }

  void populate_ranking() const {
    ListView_DeleteAllItems(ranking_);
    for (std::size_t index = 0; index < ranked_.size(); ++index) {
      const auto& item = ranked_[index];
      std::wstring bssid = to_wide(item.ap.bssid);
      LVITEMW row{};
      row.mask = LVIF_TEXT;
      row.iItem = static_cast<int>(index);
      row.pszText = bssid.data();
      const int inserted = ListView_InsertItem(ranking_, &row);
      if (inserted < 0) continue;
      set_list_text(inserted, 1, to_wide(item.ap.ssid));
      set_list_text(inserted, 2, std::to_wstring(item.ap.rssi_dbm));
      set_list_text(inserted, 3, std::to_wstring(item.ap.signal_quality));
      set_list_text(inserted, 4, std::to_wstring(item.ap.channel));
      wchar_t score[32]{};
      swprintf_s(score, L"%.1f", item.scoring.score);
      set_list_text(inserted, 5, score);
      set_list_text(inserted, 6, item.ap.current ? L"Yes" : L"");
    }
  }

  void set_list_text(int row, int column, std::wstring value) const {
    ListView_SetItemText(ranking_, row, column, value.data());
  }

  bool automatic_is_on() const {
    try {
      const auto config = storage_.load_config();
      const auto state = storage_.load_daemon_state();
      return config.enabled && process_is_running(state.pid);
    } catch (...) {
      return false;
    }
  }

  void update_auto_label() const {
    try {
      const auto config = storage_.load_config();
      const auto state = storage_.load_daemon_state();
      const bool running = process_is_running(state.pid);
      std::wstring label = L"Automatic optimization: ";
      if (config.enabled && running) label += L"ON";
      else if (config.enabled) label += L"ON (monitor stopped)";
      else label += L"OFF";
      SetWindowTextW(auto_button_, label.c_str());
    } catch (const std::exception& error) {
      SetWindowTextW(auto_button_, (L"Automatic optimization: error (" + to_wide(error.what()) + L")").c_str());
    }
  }

  void enable_operation_controls(bool enabled) const {
    EnableWindow(refresh_button_, enabled);
    EnableWindow(target_combo_, enabled);
    EnableWindow(target_apply_button_, enabled);
    EnableWindow(connect_button_, enabled && !ranked_.empty());
    EnableWindow(auto_button_, enabled);
  }

  void show_history() {
    if (history_window_) {
      ShowWindow(history_window_, SW_RESTORE);
      SetForegroundWindow(history_window_);
      return;
    }
    history_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHistoryWindowClass, L"Connection history",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                      scaled(window_, 900), scaled(window_, 430), window_, nullptr,
                                      instance_, this);
    if (!history_window_) {
      SetWindowTextW(result_label_, L"Could not create the history window.");
      return;
    }
    try {
      const auto rows = storage_.load_history();
      if (rows.empty()) {
        SendMessageW(history_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"No connection attempts have been recorded."));
      }
      for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        std::wostringstream line;
        line << history_time(it->unix_time) << L"  " << (it->success ? L"SUCCESS" : L"FAIL")
             << L"  requested=" << to_wide(it->requested_bssid)
             << L"  actual=" << to_wide(it->actual_bssid)
             << L"  code=" << it->error_code << L"  " << to_wide(it->detail);
        const auto value = line.str();
        SendMessageW(history_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
      }
      SendMessageW(history_list_, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(scaled(window_, 1800)), 0);
    } catch (const std::exception& error) {
      const auto text = L"Could not load history: " + to_wide(error.what());
      SendMessageW(history_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
  }

  void add_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallback;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, kWindowTitle);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
  }

  void remove_tray_icon() const {
    if (!tray_added_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
  }

  void update_tray_tooltip() const {
    if (!tray_added_) return;
    std::wstring tooltip = kWindowTitle;
    if (current_status_.connected) {
      tooltip = to_wide(current_status_.ssid) + L" | " + to_wide(current_status_.bssid) + L" | " +
                std::to_wstring(current_status_.signal_quality) + L"%";
    } else {
      tooltip += L" | disconnected";
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
  }

  void handle_tray_event(UINT event) {
    if (event == WM_LBUTTONDBLCLK) {
      open_window();
      return;
    }
    if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, id_refresh, L"Open");
    AppendMenuW(menu, MF_STRING, id_auto,
                automatic_is_on() ? L"Automatic optimization OFF" : L"Automatic optimization ON");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, id_exit, L"Exit");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected == id_refresh) open_window();
    else if (selected != 0) handle_command(static_cast<WORD>(selected));
    PostMessageW(window_, WM_NULL, 0, 0);
  }

  void open_window() const {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
  }

  void begin_exit() {
    if (exiting_) return;
    exiting_ = true;
    DestroyWindow(window_);
  }

  HINSTANCE instance_{};
  UINT taskbar_created_message_{};
  HWND window_{};
  HWND status_label_{};
  HWND refresh_button_{};
  HWND target_label_{};
  HWND target_combo_{};
  HWND target_apply_button_{};
  HWND ranking_{};
  HWND connect_button_{};
  HWND auto_button_{};
  HWND history_button_{};
  HWND result_label_{};
  HWND history_window_{};
  HWND history_list_{};
  HFONT font_{};
  bool tray_added_{};
  bool exiting_{};
  Storage storage_;
  AutoConfig config_;
  ConnectionStatus current_status_;
  std::vector<RankedAccessPoint> ranked_;
  std::thread worker_;
  std::atomic_bool busy_{};
  std::atomic_bool shutting_down_{};
  std::mutex result_mutex_;
  std::optional<AsyncResult> pending_result_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    GuiApp app(instance);
    if (!app.create(show_command)) {
      MessageBoxW(nullptr, L"The main window could not be created.", kWindowTitle,
                  MB_OK | MB_ICONERROR);
      return 1;
    }
    return app.run();
  } catch (const std::exception& error) {
    const auto message = L"Startup failed: " + to_wide(error.what());
    MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }
}
