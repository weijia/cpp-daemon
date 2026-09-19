#include "core/PluginManager.h"

#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

namespace cppdaemon {

namespace {

void logErr(const std::string& msg) { std::fprintf(stderr, "[plugin] %s\n", msg.c_str()); }
void logInfo(const std::string& msg) { std::fprintf(stdout, "[plugin] %s\n", msg.c_str()); }

// 跨平台动态库句柄
struct LibHandle {
#ifdef _WIN32
  HMODULE h = nullptr;
#else
  void* h = nullptr;
#endif
  bool ok() const { return h != nullptr; }
};

LibHandle loadLib(const std::string& path) {
  LibHandle lh;
#ifdef _WIN32
  lh.h = LoadLibraryA(path.c_str());
#else
  lh.h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  return lh;
}

void closeLib(LibHandle& lh) {
  if (!lh.h) return;
#ifdef _WIN32
  FreeLibrary(lh.h);
#else
  dlclose(lh.h);
#endif
  lh.h = nullptr;
}

void* sym(LibHandle& lh, const char* name) {
#ifdef _WIN32
  return (void*)GetProcAddress(lh.h, name);
#else
  return dlsym(lh.h, name);
#endif
}

// 扫描 dir 下所有动态库文件名（含扩展名）。
std::vector<std::string> listLibs(const std::string& dir) {
  std::vector<std::string> out;
#ifdef _WIN32
  std::string pat = dir + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string name = fd.cFileName;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
      out.push_back(name);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".so") == 0)
      out.push_back(name);
  }
  closedir(d);
#endif
  return out;
}

std::string suffix() {
#ifdef _WIN32
  return ".dll";
#else
  return ".so";
#endif
}

// 虚调用异常边界：单个插件抛 C++ 异常不扩散到宿主。
#define SAFE_CALL(ret, expr, dflt) \
  try { ret = (expr); } catch (const std::exception& ex) { logErr(std::string("调用插件异常: ") + ex.what()); ret = (dflt); } catch (...) { logErr("调用插件未知异常"); ret = (dflt); }

}  // namespace

PluginManager::PluginManager(
    const std::string& dir,
    std::function<void(const std::string&, const TrayItem&, Plugin*)> onTray,
    std::function<void(const std::string&, const Route&, Plugin*)> onRoute)
    : dir_(dir), onTray_(std::move(onTray)), onRoute_(std::move(onRoute)) {}

PluginManager::~PluginManager() { shutdownAll(); }

void PluginManager::loadAll(const std::map<std::string, nlohmann::json>& plugins,
                            const Identity& identity) {
  auto libs = listLibs(dir_);
  std::string suf = suffix();
  for (auto& fname : libs) {
    std::string base = fname.substr(0, fname.size() - suf.size());
    auto it = plugins.find(base);
    if (it == plugins.end()) {
      logInfo("插件 " + base + " 无对应配置桶，跳过");
      continue;
    }
    loadOne(base, dir_ + "/" + fname, it->second, identity);
  }
}

void PluginManager::loadOne(const std::string& name, const std::string& path,
                            const nlohmann::json& bucket, const Identity& identity) {
  LibHandle lh = loadLib(path);
  if (!lh.ok()) {
    logErr("插件 " + name + " 加载失败（动态库无法打开）");
    return;
  }
  auto createFn = reinterpret_cast<create_plugin_fn>(sym(lh, "create_plugin"));
  auto destroyFn = reinterpret_cast<destroy_plugin_fn>(sym(lh, "destroy_plugin"));
  if (!createFn || !destroyFn) {
    logErr("插件 " + name + " 缺少 create_plugin/destroy_plugin 导出");
    closeLib(lh);
    return;
  }
  Plugin* p = nullptr;
  try {
    p = createFn();
  } catch (...) {
    p = nullptr;
  }
  if (!p) {
    logErr("插件 " + name + " create_plugin 返回空");
    closeLib(lh);
    return;
  }
  if (p->protocolVersion() != kProtocolVersion) {
    logErr("插件 " + name + " 协议版本不匹配（插件 " +
           std::to_string(p->protocolVersion()) + " / 宿主 " +
           std::to_string(kProtocolVersion) + "）");
    destroyFn(p);
    closeLib(lh);
    return;
  }
  AppConfig ac{identity, bucket};
  bool ok = false;
  SAFE_CALL(ok, p->configure(ac), false);
  if (!ok) {
    logErr("插件 " + name + " configure 失败");
    destroyFn(p);
    closeLib(lh);
    return;
  }
  DesiredUI ui;
  SAFE_CALL(ui, p->desiredUI(), DesiredUI{});
  for (size_t i = 0; i < ui.trayItems.size(); ++i) {
    if (onTray_) onTray_(name + "#t" + std::to_string(i), ui.trayItems[i], p);
  }
  for (size_t i = 0; i < ui.routes.size(); ++i) {
    if (onRoute_) onRoute_(name + "#r" + std::to_string(i), ui.routes[i], p);
  }
  Entry e;
  e.name = name;
  e.handle = lh.h;
  e.createFn = createFn;
  e.destroyFn = destroyFn;
  e.plugin = p;
  {
    std::lock_guard<std::mutex> lk(mu_);
    entries_[name] = std::move(e);
  }
  logInfo("插件 " + name + " 加载成功");
  // 注意：lh.h 已移入 entry，勿再关闭
}

void PluginManager::applyConfig(const std::map<std::string, nlohmann::json>& plugins,
                                const Identity& identity) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& kv : entries_) {
    auto it = plugins.find(kv.first);
    if (it == plugins.end()) continue;
    AppConfig ac{identity, it->second};
    bool ok = false;
    SAFE_CALL(ok, kv.second.plugin->onConfigChanged(ac), false);
    if (!ok) logErr("插件 " + kv.first + " onConfigChanged 失败");
  }
}

void PluginManager::shutdownAll() {
  std::map<std::string, Entry> snapshot;
  {
    std::lock_guard<std::mutex> lk(mu_);
    snapshot.swap(entries_);
  }
  for (auto& kv : snapshot) {
    try { kv.second.plugin->shutdown(); } catch (...) {}
    if (kv.second.destroyFn) kv.second.destroyFn(kv.second.plugin);
    LibHandle lh;
    lh.h = reinterpret_cast<HMODULE>(kv.second.handle);
    closeLib(lh);
  }
}

std::map<std::string, ConfigSchema> PluginManager::schemas() const {
  std::map<std::string, ConfigSchema> out;
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& kv : entries_) {
    ConfigSchema s;
    SAFE_CALL(s, kv.second.plugin->configSchema(), ConfigSchema{});
    if (!s.name.empty()) out[kv.first] = s;
  }
  return out;
}

Plugin* PluginManager::get(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(name);
  return it == entries_.end() ? nullptr : it->second.plugin;
}

}  // namespace cppdaemon
