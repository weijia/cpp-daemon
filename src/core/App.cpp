#include "core/App.h"
#include "core/HttpServer.h"
#include "core/Tray.h"
#include "core/Autostart.h"

#include <cstdio>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef CPPDAEMON_HEADLESS
#include <csignal>
#include <condition_variable>
#endif

namespace cppdaemon {

namespace {
// plugins/ 目录：与可执行文件同目录
std::string pluginsDirPath() {
  std::string exeDir;
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
    std::string s(buf);
    exeDir = s.substr(0, s.find_last_of("/\\"));
  }
#else
  char buf[1024] = {0};
  if (readlink("/proc/self/exe", buf, sizeof(buf)) > 0) {
    std::string s(buf);
    exeDir = s.substr(0, s.find_last_of("/\\"));
  }
#endif
  if (exeDir.empty()) return "plugins";
  return exeDir + "/plugins";
}

#ifdef CPPDAEMON_HEADLESS
// 信号驱动退出（Linux 后台 / CI）
struct SignalHandler {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  static void onSignal(int) {
    instance()->done = true;
    instance()->cv.notify_all();
  }
  static SignalHandler* instance() {
    static SignalHandler h;
    return &h;
  }
  void waitForQuit(std::function<void()> onQuit) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [this]() { return done; });
    }
    onQuit();
  }
};
#endif
}  // namespace

App::App(const std::string& cfgPath) : cfgPath_(cfgPath) {}

App::~App() { quit(); }

const Config& App::config() const {
  std::lock_guard<std::mutex> lk(cfgMu_);
  return cfg_;
}

bool App::init() {
  cfg_ = ensureConfig(cfgPath_);
  identity_ = Identity{cfg_.uuid, cfg_.name, CPPD_VERSION};
  applyAutostart();

  // 插件加载回调：收集路由与托盘项
  auto onTray = [this](const std::string& itemId, const TrayItem& item, Plugin* p) {
    trayPlugins_[itemId] = p;
    if (tray_) tray_->addPluginItem(itemId, item.label, item.tooltip);
  };
  auto onRoute = [this](const std::string& routeId, const Route& route, Plugin* p) {
    std::lock_guard<std::mutex> lk(routeMu_);
    routeRegs_.push_back({route, p});
  };
  plugins_ = std::make_unique<PluginManager>(pluginsDirPath(), onTray, onRoute);
  return true;
}

void App::start() {
  // 创建托盘对象（在加载插件前，便于即时挂载插件菜单项）
#ifndef CPPDAEMON_HEADLESS
  tray_ = std::make_unique<Tray>(this);
#endif
  plugins_->loadAll(cfg_.plugins, identity_);

  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    httpAddr_ = cfg_.http.listen;
  }
  http_ = std::make_unique<HttpServer>(this);
  http_->listen(httpAddr_);
  httpRunning_ = true;
}

void App::run() {
#ifdef CPPDAEMON_HEADLESS
  // 信号驱动退出
  SignalHandler wait;
  wait.waitForQuit([this]() { quit(); });
#else
  if (tray_) tray_->run();
  else quit();
#endif
}

void App::applyAutostart() {
  if (!setAutostart(cfg_.autostart)) {
    std::fprintf(stderr, "[app] 设置开机自启失败\n");
  }
}

bool App::applyConfig(const Config& newCfg, std::string& err) {
  if (!newCfg.validate(err)) return false;
  std::string oldListen;
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    oldListen = cfg_.http.listen;
    cfg_ = newCfg;
  }
  if (!saveConfig(cfgPath_, cfg_)) {
    err = "配置写盘失败";
    return false;
  }
  applyAutostart();
  if (newCfg.http.listen != oldListen) restartHTTP();
  plugins_->applyConfig(cfg_.plugins, identity_);
  return true;
}

void App::setListen(const std::string& addr) {
  std::lock_guard<std::mutex> lk(cfgMu_);
  cfg_.http.listen = addr;
}

void App::restartHTTP() {
  if (http_) http_->shutdown();
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    httpAddr_ = cfg_.http.listen;
  }
  http_ = std::make_unique<HttpServer>(this);
  http_->listen(httpAddr_);
}

void App::forwardTrayClick(const std::string& itemId) {
  auto it = trayPlugins_.find(itemId);
  if (it == trayPlugins_.end()) return;
  try {
    it->second->onTrayClick(itemId);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[app] 托盘点击处理异常: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[app] 托盘点击处理未知异常\n");
  }
}

void App::quit() {
  if (quitting_.exchange(true)) return;
  if (plugins_) plugins_->shutdownAll();
  if (http_) http_->shutdown();
  if (tray_) tray_->quit();
}

}  // namespace cppdaemon
