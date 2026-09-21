#include "core/HttpServer.h"
#include "core/App.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace cppdaemon {

namespace {

// 从嵌入资源（RCDATA）读取字符串，失败返回空串
std::string loadResourceString(const char* name) {
#ifdef _WIN32
  HMODULE hMod = GetModuleHandleA(nullptr);
  HRSRC hRes = FindResourceA(hMod, name, RT_RCDATA);
  if (!hRes) return {};
  HGLOBAL hGlob = LoadResource(hMod, hRes);
  if (!hGlob) return {};
  DWORD size = SizeofResource(hMod, hRes);
  const char* p = (const char*)LockResource(hGlob);
  if (!p || size == 0) return {};
  return std::string(p, size);
#else
  (void)name;
  return {};
#endif
}

// 配置页：优先从内嵌资源读取（exe 自包含，单文件分发）；
// 回退到 <exedir>/assets/config_page.html（开发期未重编资源时仍可用）。
std::string loadConfigPage() {
  std::string fromRes = loadResourceString("IDR_CONFIG_PAGE");
  if (!fromRes.empty()) return fromRes;

  // 候选目录：exe 同目录、exe 上级目录（适配 build/Release + build/assets 布局）、当前工作目录
  std::vector<std::string> dirs;
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
    std::string s(buf);
    std::string exeDir = s.substr(0, s.find_last_of("/\\"));
    dirs.push_back(exeDir);
    auto pos = exeDir.find_last_of("/\\");
    if (pos != std::string::npos) dirs.push_back(exeDir.substr(0, pos));
  }
#else
  char buf[1024] = {0};
  if (readlink("/proc/self/exe", buf, sizeof(buf)) > 0) {
    std::string s(buf);
    std::string exeDir = s.substr(0, s.find_last_of("/\\"));
    dirs.push_back(exeDir);
    auto pos = exeDir.find_last_of("/\\");
    if (pos != std::string::npos) dirs.push_back(exeDir.substr(0, pos));
  }
#endif
  dirs.push_back(".");

  std::vector<std::string> tried;
  for (auto& d : dirs) {
    bool dup = false;
    for (auto& t : tried) if (t == d) { dup = true; break; }
    if (dup) continue;
    tried.push_back(d);
    std::ifstream f(d + "/assets/config_page.html", std::ios::binary);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
    }
  }
  return "<!DOCTYPE html><html><head><meta charset='utf-8'><title>cpp-daemon</title></head>"
         "<body><h1>cpp-daemon</h1><p>配置页资源 assets/config_page.html 未找到。</p>"
         "<p>请访问 <a href='/api/config'>/api/config</a> 使用 REST API。</p></body></html>";
}

void toJson(const Field& f, nlohmann::json& j) {
  j = {{"key", f.key}, {"label", f.label}, {"type", f.type}};
  if (!f.hint.empty()) j["hint"] = f.hint;
  if (!f.options.empty()) j["options"] = f.options;
  if (!f.fields.empty()) {
    nlohmann::json fs = nlohmann::json::array();
    for (auto& sub : f.fields) {
      nlohmann::json sj;
      toJson(sub, sj);
      fs.push_back(sj);
    }
    j["fields"] = fs;
  }
}
void toJson(const ConfigSchema& s, nlohmann::json& j) {
  nlohmann::json fs = nlohmann::json::array();
  for (auto& f : s.fields) {
    nlohmann::json fj;
    toJson(f, fj);
    fs.push_back(fj);
  }
  j = {{"name", s.name}, {"title", s.title}, {"fields", fs}};
}
nlohmann::json configToJson(const Config& c) {
  nlohmann::json j;
  toJson(c, j);
  return j;
}

}  // namespace

class HttpServer::Impl {
public:
  explicit Impl(App* app) : app_(app) {}

  void listen(const std::string& addr) {
    addr_ = addr;
    server_.Get("/", [this](const httplib::Request&, httplib::Response& res) {
      res.set_header("Content-Type", "text/html; charset=utf-8");
      res.set_content(loadConfigPage(), "text/html; charset=utf-8");
    });
    server_.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
      nlohmann::json out;
      out["config"] = configToJson(app_->config());
      out["schemas"] = schemasJson();
      out["available_plugins"] = availableJson();
      out["loaded_plugins"] = loadedJson();
      res.set_header("Content-Type", "application/json; charset=utf-8");
      res.set_content(out.dump(), "application/json");
    });
    server_.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
      nlohmann::json j;
      try {
        j = nlohmann::json::parse(req.body);
      } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("{\"error\":\"配置 JSON 解析失败: \"") + e.what() + "\"}", "application/json");
        return;
      }
      Config newCfg = fromJson(j);
      std::string err;
      if (!newCfg.validate(err)) {
        res.status = 400;
        res.set_content(std::string("{\"error\":\"") + err + "\"}", "application/json");
        return;
      }
      if (!app_->applyConfig(newCfg, err)) {
        res.status = 400;
        res.set_content(std::string("{\"error\":\"") + err + "\"}", "application/json");
        return;
      }
      // 插件可能被热加载/热卸载，回传最新 schema 与可用插件列表，便于页面即时重绘。
      nlohmann::json out = {{"ok", true}, {"config", configToJson(app_->config())}};
      out["schemas"] = schemasJson();
      out["available_plugins"] = availableJson();
      out["loaded_plugins"] = loadedJson();
      res.set_content(out.dump(), "application/json");
    });

    // 插件路由：httplib 的路由表是运行期无保护的 vector，增删不安全；
    // 而插件需要支持热加载/热卸载，故启动时只注册一次通配入口，
    // 真正的分发在请求时按插件名实时解析（插件已卸载 → 404，不会碰到已析构对象）。
    registerPluginDispatchers();

    thread_ = std::thread([this]() {
      // addr_ 形如 host:port，httplib 的 listen 需拆开
      auto pos = addr_.rfind(':');
      if (pos == std::string::npos) return;
      std::string host = addr_.substr(0, pos);
      int port = std::stoi(addr_.substr(pos + 1));
      server_.listen(host, port);  // 阻塞直到 stop()
    });
  }

  static bool sameMethod(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::toupper(static_cast<unsigned char>(a[i])) !=
          std::toupper(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  }

  // 插件路由只注册一次通配入口（httplib 按注册顺序首个命中即返回，
  // 内置的 "/"、"/api/config" 先注册，因此优先命中）。
  void registerPluginDispatchers() {
    auto dispatcher = [this](const httplib::Request& req, httplib::Response& res) {
      handlePluginRoute(req, res);
    };
    server_.Get(".*", dispatcher);
    server_.Post(".*", dispatcher);
    server_.Put(".*", dispatcher);
    server_.Delete(".*", dispatcher);
  }

  void handlePluginRoute(const httplib::Request& req, httplib::Response& res) {
    Plugin* plugin = nullptr;
    std::string path;
    for (auto& rr : app_->pluginRoutes()) {
      if (rr.route.path != req.path) continue;
      if (!sameMethod(rr.route.method, req.method)) continue;
      plugin = app_->plugins().get(rr.pluginName);  // 热卸载后为 nullptr
      path = rr.route.path;
      break;
    }
    if (!plugin) {
      res.status = 404;
      res.set_content(R"({"error":"插件未加载或该路由不存在"})", "application/json");
      return;
    }
    Request r;
    r.method = req.method;
    r.path = req.path;
    for (auto& h : req.headers) r.header[h.first] = h.second;
    r.body = req.body;
    Response resp;
    try {
      resp = plugin->handle(path, r);
    } catch (const std::exception& e) {
      res.status = 500;
      res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
      return;
    } catch (...) {
      res.status = 500;
      res.set_content(R"({"error":"插件处理未知异常"})", "application/json");
      return;
    }
    for (auto& h : resp.header) res.set_header(h.first, h.second);
    res.status = resp.statusCode;
    std::string ctype = "application/octet-stream";
    auto ct = resp.header.find("Content-Type");
    if (ct != resp.header.end()) ctype = ct->second;
    res.set_content(resp.body, ctype);
  }

  // 含未加载插件（仅探测 schema，不运行），保证配置页总能展示完整选项
  nlohmann::json schemasJson() {
    nlohmann::json schemas = nlohmann::json::object();
    for (auto& kv : app_->plugins().allSchemas()) {
      nlohmann::json sj;
      toJson(kv.second, sj);
      schemas[kv.first] = sj;
    }
    return schemas;
  }

  nlohmann::json availableJson() {
    nlohmann::json avail = nlohmann::json::array();
    for (auto& n : app_->plugins().availablePlugins()) avail.push_back(n);
    return avail;
  }

  nlohmann::json loadedJson() {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& n : app_->plugins().loadedPlugins()) arr.push_back(n);
    return arr;
  }

  void shutdown() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

private:
  App* app_;
  httplib::Server server_;
  std::string addr_;
  std::thread thread_;
};

HttpServer::HttpServer(App* app) : impl_(std::make_unique<Impl>(app)) {}
HttpServer::~HttpServer() = default;

void HttpServer::listen(const std::string& addr) { impl_->listen(addr); }
void HttpServer::shutdown() { impl_->shutdown(); }

}  // namespace cppdaemon
