# mqtt 插件：MQTT 远程命令（连接 Broker / 订阅 / AES 加解密 / 执行命令 / 回执）
add_library(mqtt SHARED mqtt.cpp crypto.cpp)

target_include_directories(mqtt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(mqtt PRIVATE
  OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  PahoMqttCpp::paho-mqttpp3-static
)

# 与宿主同动态 CRT，保证跨 DLL ABI 稳定
set_target_properties(mqtt PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# 输出到 exe 同级的 plugins/ 目录（<exedir>/plugins），与宿主加载路径一致
set_target_properties(mqtt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
  LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
)
