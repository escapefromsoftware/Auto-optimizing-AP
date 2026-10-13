cmake_minimum_required(VERSION 3.24)
project(SmartWifiApOptimizer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Smart Wi-Fi AP Optimizer supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the MSVC runtime so the built exe runs on other PCs without
# requiring the Visual C++ Redistributable to be installed first.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wifiopt_core
  src/blackbox.cpp
  src/cli.cpp
  src/core.cpp
  src/eventlog.cpp
  src/storage.cpp
  src/wlan_client.cpp
  src/optimizer.cpp)
target_include_directories(wifiopt_core PUBLIC include)
target_compile_definitions(wifiopt_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wifiopt_core PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(wifiopt_core PUBLIC wlanapi ole32 iphlpapi wevtapi)

add_library(wifiopt_gui_logic src/gui_logic.cpp)
target_include_directories(wifiopt_gui_logic PUBLIC include)
target_compile_definitions(wifiopt_gui_logic PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wifiopt_gui_logic PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wifiopt_gui_logic PUBLIC wifiopt_core)

add_executable(wifiopt src/main.cpp)
target_compile_definitions(wifiopt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wifiopt PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(wifiopt PRIVATE wifiopt_core)

add_executable(wifiopt_gui WIN32 src/gui_main.cpp)
target_compile_definitions(wifiopt_gui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wifiopt_gui PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wifiopt_gui PRIVATE wifiopt_core wifiopt_gui_logic comctl32 shell32 gdi32 user32 comdlg32)

enable_testing()
add_executable(wifiopt_tests tests/test_main.cpp)
target_compile_definitions(wifiopt_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wifiopt_tests PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(wifiopt_tests PRIVATE wifiopt_core wifiopt_gui_logic)
add_test(NAME wifiopt_tests COMMAND wifiopt_tests)

install(TARGETS wifiopt RUNTIME DESTINATION bin)
