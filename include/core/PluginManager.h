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

  // 扫描 dir 下所有动态库，对每个在 plugins 配置里有对应桶且 enabled!=false 的插件进行加载。
  void loadAll(const std::map<std::string, nlohmann::json>& plugins,
               const Identity& identity);

  // 热启用：加载单个插件（按库基名）。已加载则直接返回。
  void load(const std::string& name, const nlohmann::json& bucket,
            const Identity& identity);

  // 热禁用：卸载单个插件（shutdown + destroy + 关闭动态库）。
  void unload(const std::string& name);

  // 目录下发现的所有插件库基名（无论是否已加载/是否启用），供配置页列出开关。
  std::vector<std::string> availablePlugins() const;

  bool isLoaded(const std::string& name) const;

  // 广播配置变更给所有已加载插件。
  void applyConfig(const std::map<std::string, nlohmann::json>& plugins,
                    const Identity& identity);

  // 优雅关闭所有插件并释放动态库。
  void shutdownAll();

  // 各插件的配置 schema（供配置页渲染）。只包含已加载的插件。
  std::map<std::string, ConfigSchema> schemas() const;

  // 配置页专用：已加载插件 + 未加载插件的 schema。
  // 未加载的会临时载入动态库读取 configSchema() 后立刻卸载（不调 configure()，不运行它），
  // 这样禁用中的插件在配置页也能展示完整选项，勾选启用即可见，无需先保存。
  std::map<std::string, ConfigSchema> allSchemas() const;

  // 当前已加载并运行的插件名。
  std::vector<std::string> loadedPlugins() const;

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
  // 临时载入动态库读取 schema（不 configure、不启动），读完即卸载。
  ConfigSchema probeSchema(const std::string& name) const;
  // 最近一次全量插件桶快照，用于填充 AppConfig::allPlugins（跨插件只读视图）。
  std::map<std::string, nlohmann::json> allBuckets() const;

  std::string dir_;
  std::function<void(const std::string&, const TrayItem&, Plugin*)> onTray_;
  std::function<void(const std::string&, const Route&, Plugin*)> onRoute_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, nlohmann::json> buckets_;
  mutable std::mutex mu_;
};

}  // namespace cppdaemon
