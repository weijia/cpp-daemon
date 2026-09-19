#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Plugin.h"

namespace cppdaemon {

// 宿主侧插件管理器：扫描 plugins/ 目录、加载动态库、版本校验、推送配置、
// 收集插件声明的 UI、广播配置变更、优雅卸载。
class PluginManager {
public:
  using TrayReg = std::pair<std::string, TrayItem>;  // (itemId, item)
  using RouteReg = std::pair<std::string, Route>;    // (routeId, route)

  PluginManager(const std::string& dir,
                std::function<void(const std::string&, const TrayItem&, Plugin*)> onTray,
                std::function<void(const std::string&, const Route&, Plugin*)> onRoute);
  ~PluginManager();

  // 扫描 dir 下所有动态库，对每个在 plugins 配置里有对应桶的插件进行加载。
  void loadAll(const std::map<std::string, nlohmann::json>& plugins,
               const Identity& identity);

  // 广播配置变更给所有已加载插件。
  void applyConfig(const std::map<std::string, nlohmann::json>& plugins,
                    const Identity& identity);

  // 优雅关闭所有插件并释放动态库。
  void shutdownAll();

  // 各插件的配置 schema（供配置页渲染）。
  std::map<std::string, ConfigSchema> schemas() const;

  // 按名取插件（用于转发点击/请求）。返回 nullptr 表示未找到。
  Plugin* get(const std::string& name) const;

  struct Entry {
    std::string name;
    void* handle = nullptr;        // HMODULE / void*
    create_plugin_fn createFn = nullptr;
    destroy_plugin_fn destroyFn = nullptr;
    Plugin* plugin = nullptr;
  };

private:
  void loadOne(const std::string& name, const std::string& path,
               const nlohmann::json& bucket, const Identity& identity);

  std::string dir_;
  std::function<void(const std::string&, const TrayItem&, Plugin*)> onTray_;
  std::function<void(const std::string&, const Route&, Plugin*)> onRoute_;
  std::map<std::string, Entry> entries_;
  mutable std::mutex mu_;
};

}  // namespace cppdaemon
