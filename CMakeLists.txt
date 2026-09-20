cmake_minimum_required(VERSION 3.20)
project(cpp-daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码为 UTF-8 无 BOM，显式以 UTF-8 编译，避免中文串在 GBK 代码页下报错（C2001/C4819）
if(MSVC)
  add_compile_options("/utf-8")
endif()

# 统一使用动态 CRT（/MD），保证宿主与插件跨 DLL 边界 ABI 稳定
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

option(HEADLESS "Build without system tray (signal-driven)" OFF)
if(HEADLESS)
  add_compile_definitions(CPPDAEMON_HEADLESS)
endif()

# 版本：可由 -DCPPD_VERSION=... 注入，默认 dev
if(NOT DEFINED CPPD_VERSION)
  set(CPPD_VERSION "dev")
endif()
add_compile_definitions(CPPD_VERSION="${CPPD_VERSION}")

# ---- 依赖（Conan 提供 openssl / nlohmann-json / httplib / paho-mqtt-cpp）----
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)        # 头文件库，启用 OpenSSL 支持
find_package(PahoMqttCpp CONFIG REQUIRED)    # 供 reporter 插件链接

# httplib 需要 OpenSSL 支持宏
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)

# ---- 头文件路径 ----
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- 宿主可执行 ----
add_executable(cpp-daemon
  src/main.cpp
  src/core/App.cpp
  src/core/Config.cpp
  src/core/PluginManager.cpp
  src/core/HttpServer.cpp
  src/core/Tray.cpp
  src/core/Autostart.cpp
)
target_link_libraries(cpp-daemon PRIVATE
  OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  httplib::httplib
)
# Windows: 默认不弹控制台（GUI 子系统）；headless 走控制台
if(WIN32 AND NOT HEADLESS)
  set_target_properties(cpp-daemon PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Windows: 嵌入托盘图标资源（appicon_tray.png），运行时由 Tray.cpp 包成 ICO
if(WIN32)
  target_sources(cpp-daemon PRIVATE resources.rc)
endif()

# 资源（图标 + 配置页）已全部内嵌进 exe，运行时不再依赖外部 assets 目录。

# ---- reporter 插件（动态库）----
add_subdirectory(src/plugins/reporter)
