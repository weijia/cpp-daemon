# reporter 插件：产出 reporter.dll / libreporter.so
add_library(reporter SHARED reporter.cpp crypto.cpp)

target_include_directories(reporter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(reporter PRIVATE
  OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  httplib::httplib
  PahoMqttCpp::paho-mqttpp3-static
  $<$<PLATFORM_ID:Windows>:iphlpapi>
)

# 与宿主同动态 CRT，保证跨 DLL ABI 稳定
set_target_properties(reporter PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# 输出到 <build>/plugins/，与宿主 exe 同目录
set_target_properties(reporter PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)
