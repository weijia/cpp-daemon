#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Config.h"
#include "core/Plugin.h"
#include "core/PluginManager.h"

namespace cppdaemon {

// 程序中心状态：持配置、HTTP 服务、插件管理器、托盘、退出信号。
class App {
public:
  explicit App(const std::string& cfgPath);
  ~App();

  const Config& config() const;
  const Identity& identity() const { return identity_; }

  // 加载或生成配置，构建插件管理器。
  bool init();

  // 加载插件并启动本地 HTTP 服务（含托盘对象）。
  void start();

  // 阻塞运行：GUI 跑托盘消息循环；HEADLESS 跑信号循环。
  void run();

  // 校验、持久化新配置，广播给插件，必要时重启 HTTP。
  bool applyConfig(const Config& newCfg, std::string& err);

  // 运行期设置 HTTP 监听地址（不持久化），应在 start 前调用。
  void setListen(const std::string& addr);

  // 释放资源：关闭插件、HTTP 服务、托盘。
  void quit();

  bool quitting() const { return quitting_.load(); }

  // HTTP / Tray 转发辅助
  PluginManager& plugins() { return *plugins_; }
  std::string configPath() const { return cfgPath_; }

  // 插件声明的路由（存插件名而非指针）：转发时按名实时解析，
  // 插件热卸载后不会留下悬空指针。
  struct PluginRoute {
    std::string pluginName;
    Route route;
  };
  std::vector<PluginRoute> pluginRoutes() const {
    std::lock_guard<std::mutex> lk(routeMu_);
    return routeRegs_;
  }

  // 插件声明的托盘项：itemId -> 插件名，供点击转发。
  const std::map<std::string, std::string>& trayPlugins() const { return trayPlugins_; }

  // 托盘点击转发到对应插件。
  void forwardTrayClick(const std::string& itemId);

private:
  void applyAutostart();
  void restartHTTP();
  void listenHTTP();

  // 依据新旧配置桶的 enabled 差异，热加载/热卸载插件；返回插件集合是否变化。
  bool syncPlugins(const std::map<std::string, nlohmann::json>& oldP,
                   const std::map<std::string, nlohmann::json>& newP);
  // 清理某插件遗留的路由与托盘注册（卸载后调用）。
  void dropPluginRegs(const std::string& name);

  std::string cfgPath_;
  mutable std::mutex cfgMu_;
  Config cfg_;
  Identity identity_;

  std::unique_ptr<PluginManager> plugins_;
  std::unique_ptr<class HttpServer> http_;
  std::unique_ptr<class Tray> tray_;

  mutable std::mutex routeMu_;
  std::vector<PluginRoute> routeRegs_;
  std::map<std::string, std::string> trayPlugins_;

  std::mutex httpMu_;
  std::string httpAddr_;
  bool httpRunning_ = false;

  std::atomic<bool> quitting_{false};

  // 配置文件热加载：监听 config.json，检测到内容变化即重新应用（不回写文件，避免死循环）。
  void startConfigWatch();
  void stopConfigWatch();
  // Windows 下用 ReadDirectoryChangesW 阻塞等待目录变更；非 Windows/失败时回退轮询。
  void watchLoop();
  // 读取 config.json，与上次基准比较，内容变化则重新应用。
  void reloadIfChanged();
  std::thread watchThread_;
  std::atomic<bool> watchRunning_{false};
  std::string lastRaw_;  // 上一次已应用的配置原文，用于判断文件是否真的变化
};

}  // namespace cppdaemon
