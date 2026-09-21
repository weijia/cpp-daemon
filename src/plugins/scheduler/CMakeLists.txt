# scheduler 插件：类 crontab 的定时任务（支持 cron 表达式与固定间隔两种调度）
add_library(scheduler SHARED scheduler.cpp)

target_include_directories(scheduler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(scheduler PRIVATE
  nlohmann_json::nlohmann_json
)

# 与宿主同动态 CRT，保证跨 DLL ABI 稳定
set_target_properties(scheduler PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# 输出到 exe 同级的 plugins/ 目录（<exedir>/plugins），与宿主加载路径一致
set_target_properties(scheduler PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
  LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cpp-daemon>/plugins
)
