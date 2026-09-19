#pragma once

#include <memory>
#include <string>

#include "core/Plugin.h"

namespace cppdaemon {

class App;  // 前向声明

// 本地 HTTP 服务：基础路由（配置页 + REST API）+ 插件动态声明路由转发。
class HttpServer {
public:
  explicit HttpServer(App* app);
  ~HttpServer();

  // 在 addr 上启动（后台线程）。addr 形如 127.0.0.1:9801。
  void listen(const std::string& addr);
  // 优雅关闭。
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cppdaemon
