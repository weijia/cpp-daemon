# iot_report 插件：MQTT 遥测上报（主机+磁盘信息到 IoT 平台）
add_library(iot_report SHARED iot_report.cpp)

target_include_directories(iot_report PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(iot_report PRIVATE
  OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  httplib::httplib
  PahoMqttCpp::paho-mqttpp3-static
  $<$<PLATFORM_ID:Windows>:wbemuuid>
  $<$<PLATFORM_ID:Windows>:ole32>
  $<$<PLATFORM_ID:Windows>:oleaut32>
)

# 与宿主同动态 CRT，保证跨 DLL ABI 稳定
set_target_properties(iot_report PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# 输出到 exe 同级的 plugins/ 目录（<exedir>/plugins），与宿主加载路径一致
set_target_properties(iot_report PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
  LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
)
