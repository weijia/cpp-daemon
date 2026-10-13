#include "core/App.h"
#include "core/HttpServer.h"
#include "core/Tray.h"
#include "core/Autostart.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef CPPDAEMON_HEADLESS
#include <csignal>
#include <condition_variable>
#endif

namespace cppdaemon {

namespace {
// routeId / itemId 形如 "<插件名>#r0"、"<插件名>#t0"，取 '#' 之前即插件名。
std::string pluginNameOf(const std::string& id) {
  auto p = id.find('#');
  return p == std::string::npos ? id : id.substr(0, p);
}

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
  auto onTray = [this](const std::string& itemId, const TrayItem& item, Plugin*) {
    trayPlugins_[itemId] = pluginNameOf(itemId);
    if (tray_) tray_->addPluginItem(itemId, item.label, item.tooltip);
  };
  auto onRoute = [this](const std::string& routeId, const Route& route, Plugin*) {
    std::lock_guard<std::mutex> lk(routeMu_);
    routeRegs_.push_back({pluginNameOf(routeId), route});
  };
  plugins_ = std::make_unique<PluginManager>(pluginsDirPath(), onTray, onRoute);
  startConfigWatch();  // 启动配置文件热加载（轮询，不阻塞）
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
  std::map<std::string, nlohmann::json> oldPlugins;
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    oldListen = cfg_.http.listen;
    oldPlugins = cfg_.plugins;
    cfg_ = newCfg;
  }
  if (!saveConfig(cfgPath_, cfg_)) {
    err = "配置写盘失败";
    return false;
  }
  // 记录已落盘的配置原文，避免被热加载轮询误判为“外部修改”。
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    nlohmann::json j;
    toJson(cfg_, j);
    lastRaw_ = j.dump();
  }
  applyAutostart();
  // 启用状态变化 → 热加载/热卸载；路由由 HTTP 层按插件名实时分发，故无需重启服务。
  syncPlugins(oldPlugins, cfg_.plugins);
  if (newCfg.http.listen != oldListen) restartHTTP();
  plugins_->applyConfig(cfg_.plugins, identity_);
  return true;
}

bool App::syncPlugins(const std::map<std::string, nlohmann::json>& oldP,
                      const std::map<std::string, nlohmann::json>& newP) {
  if (!plugins_) return false;
  // 无桶视为未启用；桶内无 enabled 字段视为启用（兼容旧配置）。
  auto enabledOf = [](const std::map<std::string, nlohmann::json>& m,
                      const std::string& n) -> bool {
    auto it = m.find(n);
    if (it == m.end()) return false;
    if (!it->second.is_object()) return true;
    auto e = it->second.find("enabled");
    if (e == it->second.end() || !e->is_boolean()) return true;
    return e->get<bool>();
  };

  bool changed = false;
  for (auto& name : plugins_->availablePlugins()) {
    bool wasOn = enabledOf(oldP, name);
    bool nowOn = enabledOf(newP, name);
    if (wasOn == nowOn) continue;
    changed = true;
    if (nowOn) {
      auto it = newP.find(name);
      nlohmann::json bucket = (it == newP.end()) ? nlohmann::json::object() : it->second;
      plugins_->load(name, bucket, identity_);
      std::fprintf(stdout, "[app] 插件 %s 已启用（热加载）\n", name.c_str());
    } else {
      plugins_->unload(name);
      dropPluginRegs(name);
      std::fprintf(stdout, "[app] 插件 %s 已禁用（热卸载）\n", name.c_str());
    }
  }
  return changed;
}

void App::dropPluginRegs(const std::string& name) {
  std::lock_guard<std::mutex> lk(routeMu_);
  routeRegs_.erase(
      std::remove_if(routeRegs_.begin(), routeRegs_.end(),
                     [&](const PluginRoute& r) { return r.pluginName == name; }),
      routeRegs_.end());
  for (auto it = trayPlugins_.begin(); it != trayPlugins_.end();) {
    if (pluginNameOf(it->first) == name) it = trayPlugins_.erase(it);
    else ++it;
  }
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
  Plugin* p = plugins_ ? plugins_->get(it->second) : nullptr;
  if (!p) return;  // 插件已卸载/禁用，忽略点击
  try {
    p->onTrayClick(itemId);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[app] 托盘点击处理异常: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[app] 托盘点击处理未知异常\n");
  }
}

void App::quit() {
  if (quitting_.exchange(true)) return;
  stopConfigWatch();
  if (plugins_) plugins_->shutdownAll();
  if (http_) http_->shutdown();
  if (tray_) tray_->quit();
}

void App::requestQuit() {
  // 退出清理可能耗时（插件关闭需等待连接/线程），若在托盘 UI 线程同步执行会冻结
  // 消息循环，表现为“右键菜单点不出来”。放到独立线程，UI 线程立即返回并保持响应，
  // 待清理完成后由 quit() 内的 tray_->quit() 投递 WM_APP+1 结束消息循环。
  std::thread([this]() { quit(); }).detach();
}

void App::startConfigWatch() {
  // 以当前内存配置作为基准，避免启动后立即触发一次“变更”。
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    nlohmann::json j;
    toJson(cfg_, j);
    lastRaw_ = j.dump();
  }
  watchRunning_ = true;
  watchThread_ = std::thread([this]() { watchLoop(); });
}

// 读取 config.json，与上次已应用的基准比较；若内容确实变化则重新应用（不回写文件，避免死循环）。
void App::reloadIfChanged() {
  Config nc;
  if (!loadConfig(cfgPath_, nc)) return;  // 文件暂不存在/读取失败，下一轮再试
  nlohmann::json j;
  toJson(nc, j);
  std::string raw = j.dump();
  if (raw == lastRaw_) return;  // 内容无变化
  std::string err;
  if (!nc.validate(err)) {
    std::fprintf(stderr, "[app] 配置文件已变更但校验失败，已忽略: %s\n", err.c_str());
    lastRaw_ = raw;  // 记录以免反复报错
    return;
  }
  if (!plugins_) {  // 插件管理器尚未就绪（极少情况），仅更新基准。
    lastRaw_ = raw;
    return;
  }
  // 外部修改：重新应用，但不回写文件（否则会陷入 mtime 死循环）。
  std::string oldListen;
  std::map<std::string, nlohmann::json> oldPlugins;
  {
    std::lock_guard<std::mutex> lk(cfgMu_);
    oldListen = cfg_.http.listen;
    oldPlugins = cfg_.plugins;
    cfg_ = nc;
    lastRaw_ = raw;
  }
  syncPlugins(oldPlugins, cfg_.plugins);
  if (nc.http.listen != oldListen) restartHTTP();
  plugins_->applyConfig(cfg_.plugins, identity_);
  std::fprintf(stderr, "[app] 检测到配置文件变更，已热加载\n");
}

void App::watchLoop() {
#ifdef _WIN32
  // Windows：用目录变更通知（ReadDirectoryChangesW）等待 config.json 变化，事件驱动、无空转。
  namespace fs = std::filesystem;
  fs::path p(cfgPath_);
  std::wstring dir = p.parent_path().wstring();
  HANDLE hDir = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (hDir != INVALID_HANDLE_VALUE) {
    char buf[8192];
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    while (watchRunning_.load()) {
      DWORD bytes = 0;
      ResetEvent(ov.hEvent);
      if (!ReadDirectoryChangesW(hDir, buf, sizeof(buf), FALSE,
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
                                 &bytes, &ov, nullptr)) {
        break;
      }
      // 500ms 超时以便退出时能及时唤醒（避免 quit 时 join 死锁）
      DWORD r = WaitForSingleObject(ov.hEvent, 500);
      if (r == WAIT_TIMEOUT) continue;
      if (r != WAIT_OBJECT_0) break;
      GetOverlappedResult(hDir, &ov, &bytes, FALSE);
      // 写入可能尚未结束，稍作去抖后再读，避免读半截文件
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      if (watchRunning_.load()) reloadIfChanged();
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(hDir);
    return;
  }
  std::fprintf(stderr, "[app] 无法打开配置目录监听，回退为轮询\n");
#endif
  // 回退：定时轮询（兼容非 Windows 或目录句柄获取失败）
  while (watchRunning_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!watchRunning_.load()) break;
    reloadIfChanged();
  }
}

void App::stopConfigWatch() {
  watchRunning_ = false;
  if (watchThread_.joinable()) watchThread_.join();
}

}  // namespace cppdaemon
