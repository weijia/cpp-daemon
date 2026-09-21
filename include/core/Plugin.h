#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// 宿主与插件公共契约：双方只 include 本文件，互不 link 对方。
// 进程内虚调用，无序列化层。宿主与插件须同工具链 + 同动态 CRT 构建。

namespace cppdaemon {

// 协议版本：宿主与插件必须一致，否则拒绝加载。
// v2：AppConfig 增加 allPlugins（跨插件只读配置视图）。
constexpr int kProtocolVersion = 2;

struct Identity {
  std::string uuid;
  std::string name;
  std::string version;
};

// 宿主推给插件的配置：全局身份 + 仅本插件自己的桶（json 对象）。
struct AppConfig {
  Identity identity;
  nlohmann::json pluginConfig;  // 仅本插件桶，如 config.plugins["reporter"]
  // 全部插件的配置桶（只读视图）：插件可据此读取其它插件的配置，
  // 避免同一份参数（如服务器地址/令牌）在多处重复配置。
  std::map<std::string, nlohmann::json> allPlugins;
};

struct TrayItem {
  std::string label;
  std::string tooltip;
};

struct Route {
  std::string method;  // GET/POST...
  std::string path;    // 如 /api/update
};

struct DesiredUI {
  std::vector<TrayItem> trayItems;
  std::vector<Route> routes;
};

struct Field {
  std::string key;      // 嵌套用点路径，如 "remotestorage.server"
  std::string label;
  std::string type;     // text|password|number|bool|select
  std::string hint;
  std::vector<std::string> options;  // select 用
  std::vector<Field> fields;         // 嵌套对象（预留）
};

struct ConfigSchema {
  std::string name;
  std::string title;
  std::vector<Field> fields;
};

struct Request {
  std::string method;
  std::string path;
  std::map<std::string, std::string> header;
  std::string body;
};

struct Response {
  int statusCode = 200;
  std::map<std::string, std::string> header;
  std::string body;
};

struct ReportResult {
  std::string lastAttempt;  // ISO-8601 时间字符串
  bool lastSuccess = false;
  std::string lastError;
};

inline void to_json(nlohmann::json& j, const ReportResult& r) {
  j = {{"last_attempt", r.lastAttempt},
       {"last_success", r.lastSuccess},
       {"last_error", r.lastError}};
}

// 插件抽象接口：宿主唯一认识的插件类型。
class Plugin {
public:
  virtual ~Plugin() = default;

  virtual const char* name() const = 0;
  virtual int protocolVersion() const = 0;

  // 加载时调用一次，推送本插件配置桶。返回 false 表示加载失败。
  virtual bool configure(const AppConfig& cfg) = 0;
  // 配置变更时调用（仅本插件桶）。
  virtual bool onConfigChanged(const AppConfig& cfg) = 0;
  // 宿主退出时调用。
  virtual void shutdown() = 0;

  // 宿主拉取：插件想挂的托盘菜单与 HTTP 路由。
  virtual DesiredUI desiredUI() const = 0;
  // 宿主拉取：配置页字段 schema。
  virtual ConfigSchema configSchema() const = 0;

  // 宿主转发插件声明的 HTTP 路由请求。
  virtual Response handle(const std::string& route, const Request& req) = 0;
  // 宿主转发插件托盘菜单点击。
  virtual void onTrayClick(const std::string& itemId) = 0;
};

}  // namespace cppdaemon

// C-ABI 工厂：每个插件动态库导出这两个符号。
// Windows 必须用 __declspec(dllexport) 才会把符号导出到 DLL 导出表；
// GCC/Clang 在 -fvisibility=hidden 下需用 visibility("default") 覆盖。
#if defined(_WIN32)
#define CPPD_PLUGIN_API __declspec(dllexport)
#elif defined(__GNUC__)
#define CPPD_PLUGIN_API __attribute__((visibility("default")))
#else
#define CPPD_PLUGIN_API
#endif

extern "C" {
using create_plugin_fn = cppdaemon::Plugin* (*)();
using destroy_plugin_fn = void (*)(cppdaemon::Plugin*);
}
