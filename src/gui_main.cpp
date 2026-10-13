#include "wifiopt/optimizer.hpp"
#include "wifiopt/cli.hpp"
#include "wifiopt/eventlog.hpp"
#include "wifiopt/gui_logic.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
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
constexpr wchar_t kDetailsWindowClass[] = L"SmartWifiApOptimizerDetailsWindow-v1";
constexpr wchar_t kGraphWindowClass[] = L"SmartWifiApOptimizerGraphWindow-v1";
constexpr wchar_t kComparisonWindowClass[] = L"SmartWifiApOptimizerComparisonWindow-v1";
constexpr wchar_t kDiagnosticsWindowClass[] = L"SmartWifiApOptimizerDiagnosticsWindow-v1";
constexpr wchar_t kWindowTitle[] = L"Smart Wi-Fi AP Optimizer";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kAsyncComplete = WM_APP + 2;
constexpr UINT kGraphComplete = WM_APP + 3;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kGraphTimerId = 1;
constexpr UINT kGraphIntervalMs = 7000;

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
  id_details = 1011,
  id_details_text = 1012,
  id_graph = 1013,
  id_compare = 1014,
  id_diagnostics = 1015,
  id_graph_period = 1016,
  id_graph_export = 1017,
  id_comparison_text = 1018,
  id_diagnostics_text = 1019,
  id_diagnostics_refresh = 1020,
  id_diagnostics_export = 1021,
};

enum class AsyncKind { refresh, connect, automatic, diagnostics };

struct GraphSample {
  std::int64_t unix_time{};
  std::string ssid;
  std::string bssid;
  int rssi_dbm{-100};
  unsigned signal_quality{};
  double selection_score{};
};

struct GraphResult {
  std::optional<GraphSample> sample;
  std::wstring error;
};

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
  std::vector<BlackBoxRecord> blackbox_records;
  EventLogResult event_log;
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

std::wstring available_bool(const std::optional<bool>& value) {
  if (!value) return L"取得不可";
  return *value ? L"あり" : L"なし";
}

std::wstring frequency_band(std::uint32_t frequency_khz) {
  const auto mhz = frequency_khz / 1000U;
  if (mhz < 3000U) return L"2.4 GHz";
  if (mhz < 5925U) return L"5 GHz";
  return L"6 GHz";
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
    if (graph_worker_.joinable()) graph_worker_.join();
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

    WNDCLASSEXW details_class = main_class;
    details_class.lpfnWndProc = &GuiApp::details_window_proc;
    details_class.lpszClassName = kDetailsWindowClass;
    if (!RegisterClassExW(&details_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW graph_class = main_class;
    graph_class.lpfnWndProc = &GuiApp::graph_window_proc;
    graph_class.lpszClassName = kGraphWindowClass;
    if (!RegisterClassExW(&graph_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW comparison_class = main_class;
    comparison_class.lpfnWndProc = &GuiApp::comparison_window_proc;
    comparison_class.lpszClassName = kComparisonWindowClass;
    if (!RegisterClassExW(&comparison_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW diagnostics_class = main_class;
    diagnostics_class.lpfnWndProc = &GuiApp::diagnostics_window_proc;
    diagnostics_class.lpszClassName = kDiagnosticsWindowClass;
    if (!RegisterClassExW(&diagnostics_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window_ = CreateWindowExW(0, kMainWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1120, 680, nullptr, nullptr,
                              instance_, this);
    if (!window_) return false;
    if (!ensure_details_window() || !ensure_graph_window() ||
        !ensure_comparison_window() || !ensure_diagnostics_window()) return false;
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
      case WM_DPICHANGED:
        app->apply_dpi_change(window, lparam);
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

  static LRESULT CALLBACK details_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = app_from(window);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_CREATE:
        app->details_text_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_details_text), app->instance_, nullptr);
        app->apply_font(app->details_text_);
        return app->details_text_ ? 0 : -1;
      case WM_SIZE:
        MoveWindow(app->details_text_, scaled(window, 10), scaled(window, 10),
                   (std::max)(0, static_cast<int>(LOWORD(lparam)) - scaled(window, 20)),
                   (std::max)(0, static_cast<int>(HIWORD(lparam)) - scaled(window, 20)), TRUE);
        return 0;
      case WM_DPICHANGED:
        app->apply_dpi_change(window, lparam);
        return 0;
      case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
      case WM_NCDESTROY:
        app->details_window_ = nullptr;
        app->details_text_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  static LRESULT CALLBACK graph_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = app_from(window);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_CREATE:
        app->graph_period_ = CreateWindowExW(
            0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_graph_period), app->instance_, nullptr);
        for (const wchar_t* period : {L"直近1分", L"直近5分", L"直近30分", L"直近1時間"})
          SendMessageW(app->graph_period_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(period));
        SendMessageW(app->graph_period_, CB_SETCURSEL, 1, 0);
        app->graph_export_button_ = CreateWindowExW(
            0, L"BUTTON", L"CSV/JSONへ出力", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_graph_export), app->instance_, nullptr);
        app->apply_font(app->graph_period_);
        app->apply_font(app->graph_export_button_);
        return app->graph_period_ && app->graph_export_button_ ? 0 : -1;
      case WM_SIZE:
        MoveWindow(app->graph_period_, scaled(window, 12), scaled(window, 10),
                   scaled(window, 150), scaled(window, 220), TRUE);
        MoveWindow(app->graph_export_button_,
                   (std::max)(scaled(window, 174), static_cast<int>(LOWORD(lparam)) - scaled(window, 172)),
                   scaled(window, 9), scaled(window, 160), scaled(window, 30), TRUE);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
      case WM_SHOWWINDOW:
        if (wparam) {
          app->graph_visible_.store(true);
          SetTimer(window, kGraphTimerId, kGraphIntervalMs, nullptr);
          app->request_graph_sample();
        } else {
          app->graph_visible_.store(false);
          KillTimer(window, kGraphTimerId);
        }
        return 0;
      case WM_TIMER:
        if (wparam == kGraphTimerId) app->request_graph_sample();
        return 0;
      case WM_COMMAND:
        if (LOWORD(wparam) == id_graph_period && HIWORD(wparam) == CBN_SELCHANGE)
          InvalidateRect(window, nullptr, TRUE);
        else if (LOWORD(wparam) == id_graph_export)
          app->export_graph_samples();
        return 0;
      case kGraphComplete:
        app->finish_graph_worker();
        return 0;
      case WM_PAINT:
        app->paint_graph(window);
        return 0;
      case WM_DPICHANGED:
        app->apply_dpi_change(window, lparam);
        return 0;
      case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
      case WM_NCDESTROY:
        KillTimer(window, kGraphTimerId);
        app->graph_visible_.store(false);
        app->graph_window_ = nullptr;
        app->graph_period_ = nullptr;
        app->graph_export_button_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  static LRESULT CALLBACK comparison_window_proc(HWND window, UINT message, WPARAM wparam,
                                                  LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = app_from(window);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_CREATE:
        app->comparison_text_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_comparison_text), app->instance_, nullptr);
        app->apply_font(app->comparison_text_);
        return app->comparison_text_ ? 0 : -1;
      case WM_SIZE:
        MoveWindow(app->comparison_text_, scaled(window, 10), scaled(window, 10),
                   (std::max)(0, static_cast<int>(LOWORD(lparam)) - scaled(window, 20)),
                   (std::max)(0, static_cast<int>(HIWORD(lparam)) - scaled(window, 20)), TRUE);
        return 0;
      case WM_DPICHANGED:
        app->apply_dpi_change(window, lparam);
        return 0;
      case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
      case WM_NCDESTROY:
        app->comparison_window_ = nullptr;
        app->comparison_text_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  static LRESULT CALLBACK diagnostics_window_proc(HWND window, UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      auto* app = static_cast<GuiApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = app_from(window);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_CREATE:
        app->diagnostics_refresh_button_ = CreateWindowExW(
            0, L"BUTTON", L"再読込", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_diagnostics_refresh), app->instance_, nullptr);
        app->diagnostics_export_button_ = CreateWindowExW(
            0, L"BUTTON", L"JSONへ出力", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_diagnostics_export), app->instance_, nullptr);
        app->diagnostics_text_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"診断データは未読込です。", WS_CHILD | WS_VISIBLE |
                WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id_diagnostics_text), app->instance_, nullptr);
        app->apply_font(app->diagnostics_refresh_button_);
        app->apply_font(app->diagnostics_export_button_);
        app->apply_font(app->diagnostics_text_);
        return app->diagnostics_text_ && app->diagnostics_refresh_button_ &&
                       app->diagnostics_export_button_ ? 0 : -1;
      case WM_SIZE:
        MoveWindow(app->diagnostics_refresh_button_, scaled(window, 10), scaled(window, 10),
                   scaled(window, 100), scaled(window, 30), TRUE);
        MoveWindow(app->diagnostics_export_button_, scaled(window, 118), scaled(window, 10),
                   scaled(window, 130), scaled(window, 30), TRUE);
        MoveWindow(app->diagnostics_text_, scaled(window, 10), scaled(window, 50),
                   (std::max)(0, static_cast<int>(LOWORD(lparam)) - scaled(window, 20)),
                   (std::max)(0, static_cast<int>(HIWORD(lparam)) - scaled(window, 60)), TRUE);
        return 0;
      case WM_COMMAND:
        if (LOWORD(wparam) == id_diagnostics_refresh) app->request_diagnostics();
        else if (LOWORD(wparam) == id_diagnostics_export) app->export_diagnostics();
        return 0;
      case WM_DPICHANGED:
        app->apply_dpi_change(window, lparam);
        return 0;
      case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
      case WM_NCDESTROY:
        app->diagnostics_window_ = nullptr;
        app->diagnostics_text_ = nullptr;
        app->diagnostics_refresh_button_ = nullptr;
        app->diagnostics_export_button_ = nullptr;
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
        info->ptMinTrackSize.x = scaled(window_, 920);
        info->ptMinTrackSize.y = scaled(window_, 500);
        return 0;
      }
      case WM_COMMAND:
        handle_command(LOWORD(wparam));
        return 0;
      case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->idFrom == id_ranking && header->code == NM_DBLCLK) {
          show_selected_details();
          return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
      }
      case WM_CLOSE:
        if (details_window_) ShowWindow(details_window_, SW_HIDE);
        if (graph_window_) ShowWindow(graph_window_, SW_HIDE);
        if (comparison_window_) ShowWindow(comparison_window_, SW_HIDE);
        if (diagnostics_window_) ShowWindow(diagnostics_window_, SW_HIDE);
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
      case kGraphComplete:
        finish_graph_worker();
        return 0;
      case WM_DESTROY:
        shutting_down_.store(true);
        graph_visible_.store(false);
        if (worker_.joinable()) worker_.join();
        if (graph_worker_.joinable()) graph_worker_.join();
        if (history_window_) DestroyWindow(history_window_);
        if (details_window_) DestroyWindow(details_window_);
        if (graph_window_) DestroyWindow(graph_window_);
        if (comparison_window_) DestroyWindow(comparison_window_);
        if (diagnostics_window_) DestroyWindow(diagnostics_window_);
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
                            LVS_REPORT | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, id_ranking);
    ListView_SetExtendedListViewStyle(ranking_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    add_ranking_columns();
    connect_button_ = make_control(0, L"BUTTON", L"Connect to selected AP", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_connect);
    details_button_ = make_control(0, L"BUTTON", L"Details", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_details);
    compare_button_ = make_control(0, L"BUTTON", L"比較", BS_PUSHBUTTON,
                                   0, 0, 0, 0, id_compare);
    graph_button_ = make_control(0, L"BUTTON", L"グラフ", BS_PUSHBUTTON,
                                 0, 0, 0, 0, id_graph);
    diagnostics_button_ = make_control(0, L"BUTTON", L"診断", BS_PUSHBUTTON,
                                       0, 0, 0, 0, id_diagnostics);
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
                           {L"Quality (%)", 90}, {L"Channel", 75},
                           {L"Quality Score", 100}, {L"Selection Score", 110},
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
    MoveWindow(connect_button_, margin, buttons_y, scaled(window_, 170), button_height, TRUE);
    MoveWindow(details_button_, margin + scaled(window_, 178), buttons_y,
               scaled(window_, 82), button_height, TRUE);
    MoveWindow(compare_button_, margin + scaled(window_, 268), buttons_y,
               scaled(window_, 82), button_height, TRUE);
    MoveWindow(graph_button_, margin + scaled(window_, 358), buttons_y,
               scaled(window_, 82), button_height, TRUE);
    MoveWindow(diagnostics_button_, margin + scaled(window_, 448), buttons_y,
               scaled(window_, 82), button_height, TRUE);
    MoveWindow(history_button_, margin + scaled(window_, 538), buttons_y,
               scaled(window_, 82), button_height, TRUE);
    MoveWindow(auto_button_, margin + scaled(window_, 628), buttons_y,
               (std::max)(scaled(window_, 230), width - margin * 2 - scaled(window_, 628)),
               button_height, TRUE);
    MoveWindow(result_label_, margin, height - scaled(window_, 48), width - margin * 2,
               scaled(window_, 38), TRUE);

    const int available = width - margin * 2;
    ListView_SetColumnWidth(ranking_, 0, scaled(window_, 145));
    ListView_SetColumnWidth(ranking_, 1,
                            (std::max)(scaled(window_, 130), available - scaled(window_, 735)));
    ListView_SetColumnWidth(ranking_, 2, scaled(window_, 95));
    ListView_SetColumnWidth(ranking_, 3, scaled(window_, 100));
    ListView_SetColumnWidth(ranking_, 4, scaled(window_, 80));
    ListView_SetColumnWidth(ranking_, 5, scaled(window_, 115));
    ListView_SetColumnWidth(ranking_, 6, scaled(window_, 120));
    ListView_SetColumnWidth(ranking_, 7, scaled(window_, 80));
  }

  void recreate_font(UINT dpi = 0) {
    if (font_) DeleteObject(font_);
    if (dpi == 0) dpi = GetDpiForWindow(window_);
    font_ = CreateFontW(-MulDiv(15, static_cast<int>(dpi), 96), 0, 0, 0,
                        FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const HWND controls[]{status_label_, refresh_button_, target_label_, target_combo_,
                          target_apply_button_, ranking_, connect_button_, auto_button_,
                          details_button_, compare_button_, graph_button_, diagnostics_button_,
                          history_button_, result_label_, history_list_, details_text_, graph_period_,
                          graph_export_button_, comparison_text_, diagnostics_text_,
                          diagnostics_refresh_button_, diagnostics_export_button_};
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
      case id_details:
        show_selected_details();
        break;
      case id_compare:
        show_comparison();
        break;
      case id_graph:
        show_graph();
        break;
      case id_diagnostics:
        show_diagnostics();
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
      std::lock_guard storage_lock(storage_operation_mutex_);
      AsyncResult result;
      result.kind = AsyncKind::refresh;
      WlanClient wlan;
      result.status = wlan.status();
      result.has_status = true;
      try {
        auto aps = wlan.scan(std::nullopt);
        storage_.record_observations(aps);
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
      std::lock_guard storage_lock(storage_operation_mutex_);
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
      std::lock_guard storage_lock(storage_operation_mutex_);
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

  void request_diagnostics() {
    if (diagnostics_text_) SetWindowTextW(diagnostics_text_, L"診断データを読み込んでいます...");
    start_worker([this] {
      AsyncResult result;
      result.kind = AsyncKind::diagnostics;
      {
        std::lock_guard storage_lock(storage_operation_mutex_);
        BlackBox blackbox(storage_.root());
        result.blackbox_records = blackbox.load_records(20);
      }
      result.event_log = query_wlan_events(86400, 100);
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
    if (!result) {
      enable_operation_controls(true);
      return;
    }
    if (!result->fatal_error.empty()) {
      SetWindowTextW(result_label_, (L"Error: " + result->fatal_error).c_str());
      update_auto_label();
      enable_operation_controls(true);
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
      case AsyncKind::diagnostics:
        diagnostic_records_ = std::move(result->blackbox_records);
        diagnostic_event_log_ = std::move(result->event_log);
        populate_diagnostics();
        SetWindowTextW(result_label_, L"診断データを更新しました。");
        break;
    }
    if (!busy_.load()) enable_operation_controls(true);
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
      swprintf_s(score, L"%.1f", item.scoring.quality_score);
      set_list_text(inserted, 5, score);
      swprintf_s(score, L"%.1f", item.scoring.score);
      set_list_text(inserted, 6, score);
      set_list_text(inserted, 7, item.ap.current ? L"Yes" : L"");
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
    EnableWindow(details_button_, enabled && !ranked_.empty());
    EnableWindow(compare_button_, enabled && ranked_.size() >= 2);
    EnableWindow(diagnostics_button_, enabled);
    EnableWindow(diagnostics_refresh_button_, enabled);
    EnableWindow(auto_button_, enabled);
  }

  bool ensure_details_window() {
    if (details_window_) return true;
    details_window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kDetailsWindowClass, L"Access Point Details",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, scaled(window_, 720),
        scaled(window_, 690), window_, nullptr, instance_, this);
    return details_window_ != nullptr;
  }

  bool ensure_graph_window() {
    if (graph_window_) return true;
    graph_window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kGraphWindowClass, L"リアルタイム AP グラフ",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, scaled(window_, 900),
        scaled(window_, 560), window_, nullptr, instance_, this);
    return graph_window_ != nullptr;
  }

  bool ensure_comparison_window() {
    if (comparison_window_) return true;
    comparison_window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kComparisonWindowClass, L"アクセスポイント比較",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, scaled(window_, 900),
        scaled(window_, 650), window_, nullptr, instance_, this);
    return comparison_window_ != nullptr;
  }

  bool ensure_diagnostics_window() {
    if (diagnostics_window_) return true;
    diagnostics_window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kDiagnosticsWindowClass, L"診断 - Black Box / WLANイベント",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, scaled(window_, 980),
        scaled(window_, 680), window_, nullptr, instance_, this);
    return diagnostics_window_ != nullptr;
  }

  void apply_dpi_change(HWND window, LPARAM lparam) {
    const auto* suggested = reinterpret_cast<RECT*>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left, suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    recreate_font(GetDpiForWindow(window));
  }

  void show_graph() {
    if (!ensure_graph_window()) {
      SetWindowTextW(result_label_, L"グラフウィンドウを作成できませんでした。");
      return;
    }
    ShowWindow(graph_window_, SW_RESTORE);
    SetForegroundWindow(graph_window_);
  }

  unsigned graph_period_seconds() const {
    const LRESULT selected = graph_period_ ? SendMessageW(graph_period_, CB_GETCURSEL, 0, 0) : 1;
    constexpr unsigned periods[]{60, 300, 1800, 3600};
    if (selected < 0 || selected >= static_cast<LRESULT>(std::size(periods))) return 300;
    return periods[static_cast<std::size_t>(selected)];
  }

  std::vector<GraphSample> visible_graph_samples() const {
    const auto cutoff = unix_now() - static_cast<std::int64_t>(graph_period_seconds());
    std::vector<GraphSample> visible;
    std::copy_if(graph_samples_.begin(), graph_samples_.end(), std::back_inserter(visible),
                 [&](const auto& sample) { return sample.unix_time >= cutoff; });
    return visible;
  }

  void request_graph_sample() {
    if (!graph_visible_.load() || graph_busy_.exchange(true)) return;
    if (graph_worker_.joinable()) graph_worker_.join();
    const std::string target_ssid = config_.target_ssid;
    try {
      graph_worker_ = std::thread([this, target_ssid] {
        GraphResult result;
        try {
          std::lock_guard storage_lock(storage_operation_mutex_);
          WlanClient wlan;
          Optimizer optimizer(storage_);
          const auto ranked = optimizer.rank(wlan, target_ssid);
          const auto current = std::find_if(ranked.begin(), ranked.end(),
                                            [](const auto& item) { return item.ap.current; });
          if (current == ranked.end()) {
            result.error = L"対象SSIDの現在接続中APを取得できませんでした。";
          } else {
            result.sample = GraphSample{unix_now(), current->ap.ssid, current->ap.bssid,
                                        current->ap.rssi_dbm, current->ap.signal_quality,
                                        current->scoring.score};
          }
        } catch (const std::exception& error) {
          result.error = to_wide(error.what());
        } catch (...) {
          result.error = L"不明なエラー";
        }
        {
          std::lock_guard lock(graph_result_mutex_);
          pending_graph_result_ = std::move(result);
        }
        if (!shutting_down_.load()) PostMessageW(window_, kGraphComplete, 0, 0);
      });
    } catch (const std::exception& error) {
      graph_busy_.store(false);
      graph_status_ = to_wide(error.what());
      if (graph_window_) InvalidateRect(graph_window_, nullptr, TRUE);
    }
  }

  void finish_graph_worker() {
    if (graph_worker_.joinable()) graph_worker_.join();
    std::optional<GraphResult> result;
    {
      std::lock_guard lock(graph_result_mutex_);
      result = std::move(pending_graph_result_);
      pending_graph_result_.reset();
    }
    graph_busy_.store(false);
    if (!result || !graph_visible_.load()) return;
    if (result->sample) {
      graph_samples_.push_back(std::move(*result->sample));
      const auto cutoff = unix_now() - 3600;
      graph_samples_.erase(std::remove_if(graph_samples_.begin(), graph_samples_.end(),
                                          [&](const auto& sample) {
                                            return sample.unix_time < cutoff;
                                          }),
                           graph_samples_.end());
      if (graph_samples_.size() > 600)
        graph_samples_.erase(graph_samples_.begin(), graph_samples_.end() - 600);
      graph_status_ = L"最終更新: " + history_time(graph_samples_.back().unix_time) + L"  /  " +
                      to_wide(graph_samples_.back().bssid);
    } else {
      graph_status_ = L"取得不可: " + result->error;
    }
    if (graph_window_) InvalidateRect(graph_window_, nullptr, TRUE);
  }

  void paint_graph(HWND window) const {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    if (font_) SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);

    RECT plot{scaled(window, 64), scaled(window, 78),
              (std::max)(static_cast<LONG>(scaled(window, 180)),
                         client.right - scaled(window, 64)),
              (std::max)(static_cast<LONG>(scaled(window, 150)),
                         client.bottom - scaled(window, 58))};
    const auto samples = visible_graph_samples();
    std::wstring heading = L"RSSI (青, 左軸 -100～-30 dBm) / Signal Quality (緑, 右軸 0～100%) / 選択スコア (赤, 右軸 0～100)";
    TextOutW(dc, scaled(window, 12), scaled(window, 48), heading.c_str(),
             static_cast<int>(heading.size()));
    const std::wstring status = graph_status_.empty() ? L"サンプル取得待ち..." : graph_status_;
    TextOutW(dc, scaled(window, 12), client.bottom - scaled(window, 30), status.c_str(),
             static_cast<int>(status.size()));

    HPEN grid_pen = CreatePen(PS_DOT, 1, RGB(190, 190, 190));
    HGDIOBJ old_pen = SelectObject(dc, grid_pen);
    for (int tick = 0; tick <= 4; ++tick) {
      const int y = plot.bottom - MulDiv(plot.bottom - plot.top, tick, 4);
      MoveToEx(dc, plot.left, y, nullptr);
      LineTo(dc, plot.right, y);
      const std::wstring rssi = std::to_wstring(-100 + tick * 70 / 4);
      TextOutW(dc, scaled(window, 12), y - scaled(window, 8), rssi.c_str(),
               static_cast<int>(rssi.size()));
      const std::wstring percent = std::to_wstring(tick * 25);
      TextOutW(dc, plot.right + scaled(window, 8), y - scaled(window, 8), percent.c_str(),
               static_cast<int>(percent.size()));
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);
    Rectangle(dc, plot.left, plot.top, plot.right, plot.bottom);

    if (!samples.empty()) {
      const auto end = unix_now();
      const auto start = end - static_cast<std::int64_t>(graph_period_seconds());
      const auto draw_series = [&](COLORREF color, const auto& normalized_value) {
        std::vector<POINT> points;
        points.reserve(samples.size());
        for (const auto& sample : samples) {
          const double x_fraction = std::clamp(
              static_cast<double>(sample.unix_time - start) / (end - start), 0.0, 1.0);
          const double y_fraction = std::clamp(normalized_value(sample), 0.0, 1.0);
          points.push_back({plot.left + static_cast<LONG>((plot.right - plot.left) * x_fraction),
                            plot.bottom - static_cast<LONG>((plot.bottom - plot.top) * y_fraction)});
        }
        HPEN pen = CreatePen(PS_SOLID, scaled(window, 2), color);
        HGDIOBJ previous = SelectObject(dc, pen);
        if (points.size() > 1) Polyline(dc, points.data(), static_cast<int>(points.size()));
        else Ellipse(dc, points.front().x - 2, points.front().y - 2,
                     points.front().x + 3, points.front().y + 3);
        SelectObject(dc, previous);
        DeleteObject(pen);
      };
      draw_series(RGB(40, 100, 210), [](const auto& sample) {
        return (sample.rssi_dbm + 100.0) / 70.0;
      });
      draw_series(RGB(20, 150, 70), [](const auto& sample) {
        return sample.signal_quality / 100.0;
      });
      draw_series(RGB(210, 55, 50), [](const auto& sample) {
        return sample.selection_score / 100.0;
      });
    } else {
      const std::wstring empty = L"表示期間内のサンプルはありません。";
      TextOutW(dc, plot.left + scaled(window, 20), plot.top + scaled(window, 20),
               empty.c_str(), static_cast<int>(empty.size()));
    }
    EndPaint(window, &paint);
  }

  std::optional<std::pair<std::filesystem::path, bool>> choose_graph_export_path() const {
    wchar_t filename[MAX_PATH] = L"wifiopt-graph.csv";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = graph_window_;
    dialog.lpstrFilter = L"CSV ファイル (*.csv)\0*.csv\0JSON ファイル (*.json)\0*.json\0\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    dialog.nFilterIndex = 1;
    dialog.lpstrDefExt = L"csv";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return std::nullopt;
    std::filesystem::path path(filename);
    const bool json = dialog.nFilterIndex == 2 || path.extension() == L".json";
    if (path.extension().empty()) path.replace_extension(json ? L".json" : L".csv");
    return std::pair{std::move(path), json};
  }

  void export_graph_samples() {
    const auto samples = visible_graph_samples();
    if (samples.empty()) {
      MessageBoxW(graph_window_, L"表示期間内に出力できるサンプルがありません。", kWindowTitle,
                  MB_OK | MB_ICONINFORMATION);
      return;
    }
    const auto selected = choose_graph_export_path();
    if (!selected) return;
    std::ostringstream out;
    if (selected->second) {
      out << "{\"schema_version\":1,\"view\":\"realtime_graph\",\"period_seconds\":"
          << graph_period_seconds() << ",\"samples\":[";
      for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i) out << ',';
        const auto& sample = samples[i];
        out << "{\"unix_time\":" << sample.unix_time << ",\"ssid\":\""
            << json_escape(sample.ssid) << "\",\"bssid\":\"" << json_escape(sample.bssid)
            << "\",\"rssi_dbm\":" << sample.rssi_dbm << ",\"signal_quality\":"
            << sample.signal_quality << ",\"selection_score\":" << std::setprecision(15)
            << sample.selection_score << '}';
      }
      out << "]}\n";
    } else {
      out << "unix_time,ssid,bssid,rssi_dbm,signal_quality,selection_score\n";
      for (const auto& sample : samples)
        out << sample.unix_time << ',' << csv_field(sample.ssid) << ',' << sample.bssid << ','
            << sample.rssi_dbm << ',' << sample.signal_quality << ',' << std::setprecision(15)
            << sample.selection_score << '\n';
    }
    try {
      atomic_write_file(selected->first, out.str());
      MessageBoxW(graph_window_, L"表示中のサンプルを保存しました。", kWindowTitle,
                  MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& error) {
      MessageBoxW(graph_window_, (L"保存できませんでした: " + to_wide(error.what())).c_str(),
                  kWindowTitle, MB_OK | MB_ICONERROR);
    }
  }

  void show_comparison() {
    std::vector<RankedAccessPoint> selected;
    int row = -1;
    while ((row = ListView_GetNextItem(ranking_, row, LVNI_SELECTED)) >= 0) {
      if (static_cast<std::size_t>(row) < ranked_.size())
        selected.push_back(ranked_[static_cast<std::size_t>(row)]);
    }
    if (selected.size() < 2) {
      SetWindowTextW(result_label_, L"比較するアクセスポイントを2件以上選択してください。");
      return;
    }
    if (!ensure_comparison_window()) {
      SetWindowTextW(result_label_, L"比較ウィンドウを作成できませんでした。");
      return;
    }

    std::wostringstream text;
    text << L"複数AP比較（選択 " << selected.size() << L" 件）\r\n\r\n";
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& item = selected[i];
      const auto& ap = item.ap;
      const wchar_t name = i < 26 ? static_cast<wchar_t>(L'A' + i) : L'?';
      text << L"AP " << name << L"  " << to_wide(ap.bssid) << L"\r\n"
           << L"  信号強度: " << ap.rssi_dbm << L" dBm / " << ap.signal_quality << L"%\r\n"
           << L"  周波数帯/チャネル: " << frequency_band(ap.frequency_khz) << L" / "
           << ap.channel << L"\r\n  BSS Load: ";
      if (ap.bss_load) {
        text << L"端末 " << ap.bss_load->station_count << L"、利用率 "
             << std::lround(ap.bss_load->channel_utilization * 100.0 / 255.0) << L"%";
      } else text << L"取得不可";
      text << L"\r\n  接続履歴: " << ap.history_successes << L"成功 / "
           << ap.history_attempts << L"試行";
      if (ap.history_attempts)
        text << L" (" << std::lround(ap.history_successes * 100.0 / ap.history_attempts) << L"%)";
      else text << L" (取得不可)";
      text << L"\r\n  品質スコア: " << std::fixed << std::setprecision(1)
           << item.scoring.quality_score << L" / 73\r\n  選択スコア: "
           << item.scoring.score << L" / 100\r\n\r\n";
    }
    text << L"推奨理由\r\n" << to_wide(build_ap_recommendation(selected)) << L"\r\n\r\n"
         << L"同一物理APの可能性（推定）\r\n";
    for (std::size_t i = 0; i < selected.size(); ++i) {
      for (std::size_t j = i + 1; j < selected.size(); ++j) {
        const auto assessment = assess_same_physical_ap(selected[i].ap, selected[j].ap);
        text << L"AP " << static_cast<wchar_t>(L'A' + i) << L" / AP "
             << static_cast<wchar_t>(L'A' + j) << L": "
             << (assessment.possibly_same ? L"可能性あり" : L"根拠不足") << L"\r\n  根拠: ";
        if (assessment.evidence.empty()) text << L"一致する決定的でない指標なし";
        for (std::size_t k = 0; k < assessment.evidence.size(); ++k) {
          if (k) text << L"、";
          text << to_wide(assessment.evidence[k]);
        }
        text << L"\r\n  " << to_wide(assessment.explanation) << L"\r\n";
      }
    }
    const auto contents = text.str();
    SetWindowTextW(comparison_text_, contents.c_str());
    ShowWindow(comparison_window_, SW_RESTORE);
    SetForegroundWindow(comparison_window_);
  }

  void show_diagnostics() {
    if (!ensure_diagnostics_window()) {
      SetWindowTextW(result_label_, L"診断ウィンドウを作成できませんでした。");
      return;
    }
    ShowWindow(diagnostics_window_, SW_RESTORE);
    SetForegroundWindow(diagnostics_window_);
    request_diagnostics();
  }

  void populate_diagnostics() {
    if (!diagnostics_text_) return;
    std::wostringstream text;
    text << L"Black Box（直近 " << diagnostic_records_.size() << L" 件）\r\n";
    if (diagnostic_records_.empty()) text << L"障害レコードはありません。\r\n";
    for (const auto& record : diagnostic_records_) {
      text << L"\r\n" << history_time(record.unix_time) << L"  "
           << to_wide(blackbox_event_type_name(record.event_type)) << L"  code="
           << record.error_code << L"\r\n  障害詳細: " << to_wide(record.detail)
           << L"\r\n  原因候補: " << to_wide(record.possible_cause)
           << L"\r\n  推奨対処: " << to_wide(record.recommended_action) << L"\r\n";
    }
    text << L"\r\nWindows WLAN AutoConfigイベント（直近24時間）\r\n";
    if (!diagnostic_event_log_.available) {
      text << L"取得不可: " << to_wide(diagnostic_event_log_.message) << L" (error "
           << diagnostic_event_log_.error_code << L")\r\n";
    } else if (diagnostic_event_log_.events.empty()) {
      text << L"関連イベントは見つかりませんでした。\r\n";
    } else {
      for (const auto& event : diagnostic_event_log_.events) {
        text << history_time(event.unix_time) << L"  event=" << event.event_id;
        if (event.error_code) text << L" code=" << *event.error_code;
        if (!event.ssid.empty()) text << L" ssid=" << to_wide(event.ssid);
        if (!event.bssid.empty()) text << L" bssid=" << to_wide(event.bssid);
        text << L"\r\n  " << to_wide(event.description) << L"\r\n";
      }
    }
    const auto contents = text.str();
    SetWindowTextW(diagnostics_text_, contents.c_str());
    diagnostic_loaded_ = true;
  }

  void export_diagnostics() {
    if (!diagnostic_loaded_) {
      MessageBoxW(diagnostics_window_, L"先に診断データを読み込んでください。", kWindowTitle,
                  MB_OK | MB_ICONINFORMATION);
      return;
    }
    wchar_t filename[MAX_PATH] = L"wifiopt-diagnostics.json";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = diagnostics_window_;
    dialog.lpstrFilter = L"JSON ファイル (*.json)\0*.json\0\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;

    std::ostringstream out;
    out << "{\"schema_version\":1,\"command\":\"diagnose\",\"report\":true,\"data\":{\"blackbox\":[";
    for (std::size_t i = 0; i < diagnostic_records_.size(); ++i) {
      if (i) out << ',';
      out << blackbox_record_json(diagnostic_records_[i]);
    }
    out << "],\"event_log\":{\"available\":"
        << (diagnostic_event_log_.available ? "true" : "false")
        << ",\"error_code\":" << diagnostic_event_log_.error_code << ",\"message\":\""
        << json_escape(diagnostic_event_log_.message) << "\",\"events\":[";
    for (std::size_t i = 0; i < diagnostic_event_log_.events.size(); ++i) {
      if (i) out << ',';
      const auto& event = diagnostic_event_log_.events[i];
      out << "{\"unix_time\":" << event.unix_time << ",\"event_id\":" << event.event_id
          << ",\"ssid\":\"" << json_escape(event.ssid) << "\",\"bssid\":\""
          << json_escape(event.bssid) << "\",\"error_code\":";
      if (event.error_code) out << *event.error_code; else out << "null";
      out << ",\"description\":\"" << json_escape(event.description) << "\"}";
    }
    out << "]}}}\n";
    try {
      atomic_write_file(std::filesystem::path(filename), out.str());
      MessageBoxW(diagnostics_window_, L"診断レポートを保存しました。", kWindowTitle,
                  MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& error) {
      MessageBoxW(diagnostics_window_, (L"保存できませんでした: " + to_wide(error.what())).c_str(),
                  kWindowTitle, MB_OK | MB_ICONERROR);
    }
  }

  void show_selected_details() {
    const int selected = ListView_GetNextItem(ranking_, -1, LVNI_SELECTED);
    if (selected < 0 || static_cast<std::size_t>(selected) >= ranked_.size()) {
      SetWindowTextW(result_label_, L"Select an access point in the ranking first.");
      return;
    }
    if (!ensure_details_window()) {
      SetWindowTextW(result_label_, L"Could not create the AP details window.");
      return;
    }

    const auto& item = ranked_[static_cast<std::size_t>(selected)];
    const auto& ap = item.ap;
    std::wostringstream text;
    text << L"SSID: " << to_wide(ap.ssid) << L"\r\n"
         << L"BSSID: " << to_wide(ap.bssid) << L"\r\n"
         << L"RSSI: " << ap.rssi_dbm << L" dBm\r\n"
         << L"Signal Quality: " << ap.signal_quality << L"%\r\n"
         << L"周波数帯: " << frequency_band(ap.frequency_khz) << L"\r\n"
         << L"周波数: " << std::fixed << std::setprecision(3)
         << ap.frequency_khz / 1000.0 << L" MHz\r\n"
         << L"チャネル番号: " << ap.channel << L"\r\n"
         << L"PHY規格: " << (ap.phy_standard ? to_wide(*ap.phy_standard) : L"取得不可") << L"\r\n"
         << L"ビーコン間隔: ";
    if (ap.beacon_interval_tu) text << *ap.beacon_interval_tu << L" TU";
    else text << L"取得不可";
    text << L"\r\n"
         << L"セキュリティ方式: 取得不可 (Windows APIはSSID単位でのみ提供)\r\n"
         << L"認証方式: 取得不可 (Windows APIはSSID単位でのみ提供)\r\n"
         << L"接続状態: " << (ap.current ? L"現在接続中" : L"未接続") << L"\r\n"
         << L"リンク速度: ";
    if (!ap.current) {
      text << L"取得不可 (未接続のため)";
    } else if (ap.receive_link_speed_bps && ap.transmit_link_speed_bps) {
      text << std::setprecision(1)
           << L"受信 " << *ap.receive_link_speed_bps / 1000000.0 << L" Mbps / 送信 "
           << *ap.transmit_link_speed_bps / 1000000.0 << L" Mbps";
    } else {
      text << L"取得不可 (GetIfEntry2で取得できませんでした)";
    }
    text << L"\r\n最終検出日時: ";
    if (ap.last_seen_unix) text << history_time(*ap.last_seen_unix) << L" (本ツールのスキャン観測時刻)";
    else text << L"取得不可";

    text << L"\r\n\r\nBSS Load\r\n";
    if (ap.bss_load) {
      text << L"  Station Count: " << ap.bss_load->station_count << L"\r\n"
           << L"  Channel Utilization: "
           << std::lround(ap.bss_load->channel_utilization * 100.0 / 255.0) << L"% (raw "
           << static_cast<unsigned>(ap.bss_load->channel_utilization) << L"/255)\r\n"
           << L"  Available Admission Capacity: "
           << ap.bss_load->available_admission_capacity << L"";
    } else {
      text << L"  取得不可 (not advertised)";
    }

    text << L"\r\n\r\n802.11 Information Elements\r\n"
         << L"  HT Capabilities (ID 45): " << available_bool(ap.ht_capabilities) << L"\r\n"
         << L"  VHT Capabilities (ID 191): " << available_bool(ap.vht_capabilities) << L"\r\n"
         << L"  Extended Capabilities (ID 127): " << available_bool(ap.extended_capabilities) << L"\r\n"
         << L"  BSS Transition Management: ";
    if (ap.bss_transition_management) {
      text << (*ap.bss_transition_management ? L"対応" : L"非対応");
    } else {
      text << L"取得不可 (情報なし、またはオクテット不足)";
    }
    text << L"\r\n  HE Capabilities: 未対応\r\n"
         << L"  EHT Capabilities: 未対応\r\n"
         << L"  Vendor Specific IE (ID 221): ";
    if (ap.vendor_specific_ie_count) text << *ap.vendor_specific_ie_count << L" 個";
    else text << L"取得不可";

    text << L"\r\n\r\nスコア\r\n"
         << L"  品質スコア: " << std::setprecision(1) << item.scoring.quality_score << L" / 73\r\n"
         << L"  選択スコア: " << item.scoring.score << L" / 100\r\n"
         << L"  内訳:\r\n";
    for (const auto& reason : item.scoring.reasons) {
      text << L"    - " << to_wide(reason) << L"\r\n";
    }

    const auto details = text.str();
    SetWindowTextW(details_text_, details.c_str());
    const auto title = L"AP Details - " + to_wide(ap.bssid);
    SetWindowTextW(details_window_, title.c_str());
    ShowWindow(details_window_, SW_RESTORE);
    SetForegroundWindow(details_window_);
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
  HWND details_button_{};
  HWND compare_button_{};
  HWND graph_button_{};
  HWND diagnostics_button_{};
  HWND auto_button_{};
  HWND history_button_{};
  HWND result_label_{};
  HWND history_window_{};
  HWND history_list_{};
  HWND details_window_{};
  HWND details_text_{};
  HWND graph_window_{};
  HWND graph_period_{};
  HWND graph_export_button_{};
  HWND comparison_window_{};
  HWND comparison_text_{};
  HWND diagnostics_window_{};
  HWND diagnostics_text_{};
  HWND diagnostics_refresh_button_{};
  HWND diagnostics_export_button_{};
  HFONT font_{};
  bool tray_added_{};
  bool exiting_{};
  Storage storage_;
  AutoConfig config_;
  ConnectionStatus current_status_;
  std::vector<RankedAccessPoint> ranked_;
  std::vector<GraphSample> graph_samples_;
  std::wstring graph_status_;
  std::vector<BlackBoxRecord> diagnostic_records_;
  EventLogResult diagnostic_event_log_;
  bool diagnostic_loaded_{};
  std::thread worker_;
  std::thread graph_worker_;
  std::atomic_bool busy_{};
  std::atomic_bool graph_busy_{};
  std::atomic_bool graph_visible_{};
  std::atomic_bool shutting_down_{};
  std::mutex storage_operation_mutex_;
  std::mutex result_mutex_;
  std::mutex graph_result_mutex_;
  std::optional<AsyncResult> pending_result_;
  std::optional<GraphResult> pending_graph_result_;
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
