// reporter 插件：RemoteStorage 上报（实现 cppdaemon::Plugin）。
// MQTT 远程命令已拆分为独立的 mqtt 插件；本插件仅负责节点状态上报与 inbox 命令。
#include "core/Plugin.h"
#include "core/Config.h"

#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <httplib.h>
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/ssl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// 插件自包含：复刻宿主 Config.cpp 的配置解析与 URL 工具。
// 依据 Plugin.h 约定，宿主与插件互不 link，故插件需自带这些小工具。
namespace cppdaemon {

ReporterConfig reporterFromJson(const nlohmann::json& j) {
  ReporterConfig c;
  if (j.contains("remotestorage")) {
    auto& rs = j["remotestorage"];
    c.remotestorage.server = rs.value("server", "");
    c.remotestorage.user = rs.value("user", "");
    c.remotestorage.token = rs.value("token", "");
    c.remotestorage.scope = rs.value("scope", "");
    c.remotestorage.pathTemplate = rs.value("path_template", "");
  }
  if (j.contains("report")) {
    auto& rp = j["report"];
    c.report.intervalMinutes = rp.value("interval_minutes", 15);
    c.report.extraInfo = rp.value("extra_info", true);
  }
  return c;
}

std::string resolveURL(const RemoteStorageConfig& rs, const std::string& uuid) {
  std::string tmpl = rs.pathTemplate;
  if (tmpl.empty()) tmpl = "/ufs-nodes/{uuid}.json";
  std::string path = tmpl;
  auto replace = [&](const std::string& ph, const std::string& val) {
    size_t pos = 0;
    while ((pos = path.find(ph, pos)) != std::string::npos) {
      path.replace(pos, ph.size(), val);
      pos += val.size();
    }
  };
  replace("{user}", rs.user);
  replace("{uuid}", uuid);
  std::string base = rs.server;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + path;
}

}  // namespace cppdaemon

namespace {

std::string nowISO() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}

// 由 mqtt 插件的配置桶生成上报给服务端的 mqtt 字段。
// 服务端据此知道往哪个 topic、用哪个 secret 下发命令（沿用旧版 payload 结构）。
// mqtt 配置已迁出 reporter，这里通过 AppConfig::allPlugins 跨插件读取，避免重复配置。
nlohmann::json mqttPayloadFrom(const std::map<std::string, nlohmann::json>& all) {
  nlohmann::json out = nlohmann::json::object();
  auto it = all.find("mqtt");
  if (it != all.end() && it->second.is_object()) out = it->second;
  if (!out.contains("enabled")) out["enabled"] = false;
  if (!out.contains("broker")) out["broker"] = "";
  if (!out.contains("topic")) out["topic"] = "ufsd/cmd";
  if (!out.contains("username")) out["username"] = "";
  if (!out.contains("password")) out["password"] = "";
  if (!out.contains("secret")) out["secret"] = "";
  if (!out.contains("client_id")) out["client_id"] = "";
  return out;
}

// 上传错误分类
struct UploadError {
  enum Kind { Network, Auth, Server, Invalid } kind;
  std::string msg;
};
std::string uploadErrMsg(const UploadError& e) {
  static const char* names[] = {"network", "auth", "server", "invalid"};
  return std::string(names[e.kind]) + ": " + e.msg;
}

// 将完整 URL 拆成 httplib 需要的两段：base(https://host[:port]) + path(/x/y)
void splitURL(const std::string& url, std::string& base, std::string& path) {
  size_t start = 0;
  if (url.compare(0, 7, "http://") == 0) start = 7;
  else if (url.compare(0, 8, "https://") == 0) start = 8;
  size_t slash = url.find('/', start);
  if (slash == std::string::npos) {
    base = url;
    path = "/";
  } else {
    base = url.substr(0, slash);
    path = url.substr(slash);
  }
}

// httplib 错误枚举转可读字符串，便于排查 DNS/TLS/超时等网络问题
// （不同版本 httplib 不一定提供 to_string，故只输出错误码，用户可据此回传定位）
std::string httpErr(const httplib::Result& r) {
  if (r) return "ok";
  int code = static_cast<int>(r.error());
  std::string msg = "httplib错误码#" + std::to_string(code);
  if (code == 2)  // Error::Connection：TCP 建连失败
    msg += "(连接失败: 检查主机名/DNS/端口是否可达, 或是否需要代理)";
  return msg;
}

// 解析代理配置 "http://host:port" 或 "host:port" -> (host, port)
bool parseProxy(const std::string& proxy, std::string& host, int& port) {
  std::string s = proxy;
  size_t d = s.find("://");
  if (d != std::string::npos) s = s.substr(d + 3);
  size_t c = s.rfind(':');
  if (c == std::string::npos || c + 1 >= s.size()) return false;
  try { port = std::stoi(s.substr(c + 1)); }
  catch (...) { return false; }
  host = s.substr(0, c);
  return !host.empty() && port > 0;
}

// 读取系统代理环境变量（httplib 默认不读，需要显式设置）
std::string envProxy() {
  const char* names[] = {"HTTPS_PROXY", "HTTP_PROXY", "https_proxy", "http_proxy"};
  for (auto n : names) {
    const char* v = std::getenv(n);
    if (v && *v) return v;
  }
  return "";
}

// ---------- 持久化日志（同时写文件与 stderr） ----------
// 日志文件位于 exe 同目录：cpp-daemon.log
static std::string logFilePath() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s(buf, n);
  size_t p = s.find_last_of("\\/");
  s = (p == std::string::npos) ? "." : s.substr(0, p);
  return s + "\\cpp-daemon.log";
#else
  return "cpp-daemon.log";
#endif
}
static FILE* g_logf = nullptr;
static void rlog(const char* fmt, ...) {
  char ts[32];
  time_t t = time(nullptr);
  struct tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
  va_list ap;
  if (!g_logf) g_logf = fopen(logFilePath().c_str(), "a");
  if (g_logf) {
    fprintf(g_logf, "[%s] ", ts);
    va_start(ap, fmt); vfprintf(g_logf, fmt, ap); va_end(ap);
    fflush(g_logf);
  }
  fprintf(stderr, "[%s] ", ts);
  va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

}  // namespace

namespace reporter {

// ---------- 节点结构 ----------
struct Capability {
  std::string id, type, label;
  nlohmann::json params;
};
struct Command {
  std::string cmdId, from, action;
  nlohmann::json params;
  std::string status;
  nlohmann::json result;
};
struct NodePayload {
  std::string uuid, name, status;
  int64_t lastAccess = 0;
  std::string hostname, os, version, externalIP;
  std::vector<std::string> internalIPs;
  std::vector<Capability> capabilities;
  std::vector<Command> inbox;
  nlohmann::json mqtt;  // 来自 mqtt 插件桶（跨插件只读）
};

// nlohmann JSON 适配（ADL，命名空间与结构体一致）
inline void to_json(nlohmann::json& j, const Capability& c) {
  j = {{"id", c.id}, {"type", c.type}, {"label", c.label}, {"params", c.params}};
}
inline void to_json(nlohmann::json& j, const Command& c) {
  j = {{"cmd_id", c.cmdId}, {"from", c.from}, {"action", c.action},
       {"params", c.params}, {"status", c.status}, {"result", c.result}};
}
inline void from_json(const nlohmann::json& j, Command& c) {
  c.cmdId = j.value("cmd_id", "");
  c.from = j.value("from", "");
  c.action = j.value("action", "");
  c.params = j.value("params", nlohmann::json::object());
  c.status = j.value("status", "");
  c.result = j.value("result", nlohmann::json::object());
}
inline void to_json(nlohmann::json& j, const NodePayload& p) {
  j = {{"uuid", p.uuid}, {"name", p.name}, {"status", p.status},
       {"last_access", p.lastAccess}, {"hostname", p.hostname}, {"os", p.os},
       {"version", p.version}, {"external_ip", p.externalIP},
       {"internal_ips", p.internalIPs}, {"capabilities", p.capabilities},
       {"inbox", p.inbox}, {"mqtt", p.mqtt}};
}

class Reporter : public cppdaemon::Plugin {
public:
  const char* name() const override { return "reporter"; }
  int protocolVersion() const override { return cppdaemon::kProtocolVersion; }

  bool configure(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      identity_ = cfg.identity;
      if (!cfg.pluginConfig.is_null()) cfg_ = cppdaemon::reporterFromJson(cfg.pluginConfig);
      else cfg_ = cppdaemon::ReporterConfig{};
    }
    {
      std::lock_guard<std::mutex> lk(allMu_);
      allPlugins_ = cfg.allPlugins;
    }
    client_.reset();
    quit_ = false;
    reportThread_ = std::thread([this]() { reportLoop(); });
    return true;
  }

  bool onConfigChanged(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!cfg.pluginConfig.is_null()) cfg_ = cppdaemon::reporterFromJson(cfg.pluginConfig);
    }
    {
      std::lock_guard<std::mutex> lk(allMu_);
      allPlugins_ = cfg.allPlugins;
    }
    {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.notify_all();
    }
    return true;
  }

  void shutdown() override {
    quit_ = true;
    {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.notify_all();
    }
    if (reportThread_.joinable()) reportThread_.join();
  }

  cppdaemon::DesiredUI desiredUI() const override {
    cppdaemon::DesiredUI ui;
    ui.trayItems.push_back({"立即上报", "立即向服务器上报状态"});
    ui.routes.push_back({"POST", "/api/update"});
    ui.routes.push_back({"GET", "/api/status"});
    return ui;
  }

  cppdaemon::ConfigSchema configSchema() const override {
    cppdaemon::ConfigSchema s;
    s.name = "reporter";
    s.title = "RemoteStorage 上报";
    s.fields = {
        {"remotestorage.server", "服务器地址 Server", "text", "存储根，含用户名路径"},
        {"remotestorage.user", "用户 User", "text"},
        {"remotestorage.token", "Bearer Token", "password"},
        {"remotestorage.path_template", "路径模板 PathTemplate", "text", "支持 {user} {uuid}"},
        {"remotestorage.proxy", "代理 Proxy（可选）", "text", "公司网络需填写，如 http://proxy:8080；留空则自动读取系统代理环境变量"},
        {"report.interval_minutes", "上报间隔（分钟）", "number"},
        {"report.extra_info", "附带本机基础信息", "bool"},
    };
    return s;
  }

  cppdaemon::Response handle(const std::string& /*route*/, const cppdaemon::Request& req) override {
    cppdaemon::Response resp;
    if (req.path == "/api/update") {
      bool ok = uploadNow();
      nlohmann::json j = {{"ok", ok}};
      if (!ok) j["error"] = lastError_;
      resp.body = j.dump();
    } else if (req.path == "/api/status") {
      nlohmann::json j = {{"status", stateSnapshot()}};
      resp.body = j.dump();
    } else {
      resp.statusCode = 404;
      resp.body = "not found";
      return resp;
    }
    resp.statusCode = 200;
    resp.header["Content-Type"] = "application/json; charset=utf-8";
    return resp;
  }

  void onTrayClick(const std::string& /*itemId*/) override { uploadNow(); }

private:
  cppdaemon::ReporterConfig cfg_;
  cppdaemon::Identity identity_;
  std::unique_ptr<httplib::Client> client_;
  std::mutex mu_;

  bool quit_ = false;
  std::mutex cvMu_;
  std::condition_variable cv_;
  std::thread reportThread_;

  // 跨插件只读配置视图（AppConfig::allPlugins）
  std::mutex allMu_;
  std::map<std::string, nlohmann::json> allPlugins_;

  // 上报状态
  std::mutex stateMu_;
  cppdaemon::ReportResult state_;
  std::string lastError_;

  std::map<std::string, int64_t> executed_;  // cmdId -> 已执行时间戳

  std::string targetURL() {
    std::lock_guard<std::mutex> lk(mu_);
    return cppdaemon::resolveURL(cfg_.remotestorage, identity_.uuid);
  }

  std::map<std::string, nlohmann::json> allPluginsSnapshot() {
    std::lock_guard<std::mutex> lk(allMu_);
    return allPlugins_;
  }

  int intervalMin() {
    std::lock_guard<std::mutex> lk(mu_);
    int m = cfg_.report.intervalMinutes;
    return m <= 0 ? 15 : m;
  }

  void setState(bool ok, const std::string& err) {
    std::lock_guard<std::mutex> lk(stateMu_);
    state_.lastAttempt = nowISO();
    state_.lastSuccess = ok;
    state_.lastError = err;
    if (!ok) lastError_ = err;
  }
  cppdaemon::ReportResult stateSnapshot() {
    std::lock_guard<std::mutex> lk(stateMu_);
    return state_;
  }

  bool uploadNow() {
    cppdaemon::ReporterConfig c;
    {
      std::lock_guard<std::mutex> lk(mu_);
      c = cfg_;
    }
    std::string err;
    bool ok = sync(c, err);
    setState(ok, err);
    if (!ok) rlog("[reporter] 上报失败: %s\n", err.c_str());
    return ok;
  }

  void reportLoop() {
    uploadUntilOK();
    auto next = std::chrono::steady_clock::now() + std::chrono::minutes(intervalMin());
    while (!quit_) {
      {
        std::unique_lock<std::mutex> lk(cvMu_);
        cv_.wait_until(lk, next, [this]() { return quit_; });
      }
      if (quit_) break;
      uploadUntilOK();
      next = std::chrono::steady_clock::now() + std::chrono::minutes(intervalMin());
    }
  }

  void uploadUntilOK() {
    auto backoff = std::chrono::seconds(2);
    const auto maxBackoff = std::chrono::seconds(60);
    while (!quit_) {
      std::string err;
      cppdaemon::ReporterConfig c;
      {
        std::lock_guard<std::mutex> lk(mu_);
        c = cfg_;
      }
      bool ok = sync(c, err);
      setState(ok, err);
      if (ok) return;
      // 仅网络错误重试
      if (err.find("network") != 0) return;
      {
        std::unique_lock<std::mutex> lk(cvMu_);
        cv_.wait_for(lk, backoff, [this]() { return quit_; });
      }
      if (quit_) return;
      backoff = std::min(backoff * 2, maxBackoff);
    }
  }

  // RemoteStorage 同步（读-合并-写回）
  bool sync(const cppdaemon::ReporterConfig& c, std::string& err) {
    std::string url = cppdaemon::resolveURL(c.remotestorage, identity_.uuid);
    if (url.empty()) {
      err = uploadErrMsg({UploadError::Invalid, "目标 URL 为空"});
      return false;
    }
    // 每次同步用本地客户端，避免 host 绑定陈旧
    std::string base, path;
    splitURL(url, base, path);
    if (base.empty()) {
      err = uploadErrMsg({UploadError::Invalid,
        "remotestorage.server 未配置（请在配置页填写服务器地址，需含 http(s)://）"});
      rlog("[reporter] 上报失败: %s\n", err.c_str());
      return false;
    }
    rlog("[reporter] 同步 GET base=%s path=%s\n", base.c_str(), path.c_str());
    httplib::Client cli(base);
    cli.enable_server_certificate_verification(true);
    // 可选代理（公司网络常见需要）
    std::string ph; int pp = 0;
    std::string proxy = c.remotestorage.proxy;
    if (proxy.empty()) proxy = envProxy();
    if (!proxy.empty() && parseProxy(proxy, ph, pp)) {
      cli.set_proxy(ph, pp);
      rlog("[reporter] 使用代理 %s:%d\n", ph.c_str(), pp);
    } else if (!c.remotestorage.proxy.empty()) {
      rlog("[reporter] 代理配置无效: %s\n", c.remotestorage.proxy.c_str());
    }

    // GET 现有节点
    httplib::Headers hdr;
    if (!c.remotestorage.token.empty())
      hdr.emplace("Authorization", "Bearer " + c.remotestorage.token);
    auto gres = cli.Get(path, hdr);
    nlohmann::json existing;
    bool exists = false;
    if (gres) {
      if (gres->status == 404) {
        exists = false;
      } else if (gres->status == 401 || gres->status == 403) {
        err = uploadErrMsg({UploadError::Auth, "服务器返回 " + std::to_string(gres->status)});
        return false;
      } else if (gres->status < 200 || gres->status >= 300) {
        err = uploadErrMsg({UploadError::Server, "服务器返回 " + std::to_string(gres->status)});
        return false;
      } else {
        try {
          existing = nlohmann::json::parse(gres->body);
          exists = true;
        } catch (...) {
          err = uploadErrMsg({UploadError::Server, "节点数据解析失败"});
          return false;
        }
      }
    } else {
      err = uploadErrMsg({UploadError::Network, "GET 失败: " + httpErr(gres)});
      return false;
    }

    NodePayload payload = buildPayload(c);
    if (exists && existing.contains("inbox") && existing["inbox"].is_array()) {
      for (auto& ci : existing["inbox"]) payload.inbox.push_back(ci.get<Command>());
    }
    // 执行 inbox 中 pending 命令
    std::vector<Command> toExec = execInbox(payload);
    std::string body = nlohmann::json(payload).dump();

    auto pres = cli.Put(path, hdr, body, "application/json");
    if (!pres) {
      err = uploadErrMsg({UploadError::Network, "PUT 失败: " + httpErr(pres)});
      return false;
    }
    if (pres->status < 200 || pres->status >= 300) {
      if (pres->status == 401 || pres->status == 403)
        err = uploadErrMsg({UploadError::Auth, "服务器返回 " + std::to_string(pres->status)});
      else
        err = uploadErrMsg({UploadError::Server, "服务器返回 " + std::to_string(pres->status)});
      return false;
    }
    // 执行本地电源命令
    for (auto& cmd : toExec) {
      std::string mode;
      if (cmd.params.contains("mode")) mode = cmd.params["mode"].get<std::string>();
      if (!execPower(mode)) {
        rlog("[reporter] 执行电源命令失败 mode=%s\n", mode.c_str());
        markFailed(payload, cmd.cmdId, "执行失败: " + mode);
        auto ub = nlohmann::json(payload).dump();
        cli.Put(path, hdr, ub, "application/json");
      }
    }
    rlog("[reporter] 上报成功\n");
    return true;
  }

  NodePayload buildPayload(const cppdaemon::ReporterConfig& c) {
    NodePayload p;
    p.uuid = identity_.uuid;
    p.name = identity_.name;
    p.status = "online";
    p.lastAccess = (int64_t)std::time(nullptr) * 1000;
    if (c.report.extraInfo) {
      char h[256] = {0};
#ifdef _WIN32
      DWORD n = sizeof(h);
      GetComputerNameA(h, &n);
#else
      gethostname(h, sizeof(h));
#endif
      p.hostname = h;
#ifdef _WIN32
      p.os = "windows";
#else
      p.os = "linux";
#endif
      p.version = identity_.version;
      p.internalIPs = internalIPs();
      p.externalIP = externalIP(c.remotestorage.proxy);
    }
    p.capabilities = localCapabilities();
    p.mqtt = mqttPayloadFrom(allPluginsSnapshot());
    return p;
  }

  std::vector<Command> execInbox(NodePayload& node) {
    std::vector<Command> toExec;
    for (auto& cmd : node.inbox) {
      if (cmd.status != "pending") continue;
      if (executed_.count(cmd.cmdId)) continue;
      bool supported = false;
      for (auto& cap : node.capabilities)
        if (cap.id == cmd.action) supported = true;
      if (!supported) {
        cmd.status = "rejected";
        cmd.result = {{"error", "不支持的动作: " + cmd.action}};
        continue;
      }
      if (cmd.action == "power") {
        cmd.status = "done";
        cmd.result = {{"mode", cmd.params.value("mode", "")}, {"ok", true}};
        executed_[cmd.cmdId] = (int64_t)std::time(nullptr) * 1000;
        toExec.push_back(cmd);
      } else {
        cmd.status = "rejected";
        cmd.result = {{"error", "未实现动作"}};
      }
    }
    return toExec;
  }

  void markFailed(NodePayload& node, const std::string& cmdId, const std::string& e) {
    for (auto& cmd : node.inbox)
      if (cmd.cmdId == cmdId) {
        cmd.status = "failed";
        cmd.result = {{"error", e}};
        return;
      }
  }

  bool execPower(const std::string& mode) {
#ifdef _WIN32
    std::string cmd;
    if (mode == "hibernate") cmd = "shutdown.exe /h";
    else if (mode == "sleep") cmd = "rundll32.exe powrprof.dll,SetSuspendState 0,1,0";
    else if (mode == "shutdown") cmd = "shutdown.exe /s /t 0";
    else if (mode == "restart") cmd = "shutdown.exe /r /t 0";
    else return false;
    return std::system(cmd.c_str()) == 0;
#else
    (void)mode;
    return false;
#endif
  }

  std::vector<std::string> internalIPs() {
    std::vector<std::string> ips;
#ifdef _WIN32
    // 用 GetAdaptersAddresses 替代废弃的 GetAdaptersInfo：可拿到 IPv6，并过滤回环/链路本地
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
      return ips;  // size 仍为 0 -> 本机无法枚举，直接返回
    std::vector<unsigned char> buf(size);
    auto* adas = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adas, &size) != NO_ERROR) return ips;
    for (auto* a = adas; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) continue;  // 跳过未连接的网卡
      for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
        auto fam = ua->Address.lpSockaddr->sa_family;
        char str[INET6_ADDRSTRLEN] = {0};
        if (fam == AF_INET) {
          auto* sin = reinterpret_cast<SOCKADDR_IN*>(ua->Address.lpSockaddr);
          inet_ntop(AF_INET, &sin->sin_addr, str, sizeof(str));
        } else if (fam == AF_INET6) {
          auto* sin6 = reinterpret_cast<SOCKADDR_IN6*>(ua->Address.lpSockaddr);
          inet_ntop(AF_INET6, &sin6->sin6_addr, str, sizeof(str));
        } else {
          continue;
        }
        std::string ip(str);
        if (fam == AF_INET) {
          if (ip == "127.0.0.1" || ip.compare(0, 8, "169.254.") == 0) continue;  // 回环/链路本地
        } else {
          if (ip == "::1" || ip.compare(0, 4, "fe80") == 0) continue;            // 回环/链路本地
        }
        ips.push_back(ip);
      }
    }
#endif
    return ips;
  }

  // 外网 IP：沿用 sync() 的代理逻辑（配置 proxy -> 环境变量兜底），并用多服务兜底；
  // 失败会打日志而非静默返回空，便于定位（公司网络通常需配置 remotestorage.proxy）。
  std::string externalIP(const std::string& cfgProxy) {
    struct Prov { std::string host, path; };
    static const Prov kProviders[] = {
        {"https://api.ipify.org", "/?format=text"},
        {"https://ifconfig.me", "/ip"},
        {"https://icanhazip.com", "/"},
    };
    std::string proxy = cfgProxy.empty() ? envProxy() : cfgProxy;
    std::string ph; int pp = 0;
    bool haveProxy = !proxy.empty() && parseProxy(proxy, ph, pp);
    for (const auto& pr : kProviders) {
      try {
        httplib::Client cli(pr.host);
        cli.enable_server_certificate_verification(true);
        if (haveProxy) cli.set_proxy(ph, pp);
        auto r = cli.Get(pr.path);
        if (r && r->status == 200 && !r->body.empty()) {
          std::string s = r->body;
          s.erase(0, s.find_first_not_of(" \t\r\n"));
          s.erase(s.find_last_not_of(" \t\r\n") + 1);
          if (!s.empty()) {
            rlog("[reporter] 外网 IP 获取成功 (%s): %s\n", pr.host.c_str(), s.c_str());
            return s;
          }
        }
      } catch (...) {
      }
      rlog("[reporter] 外网 IP 获取失败，尝试下一个服务: %s\n", pr.host.c_str());
    }
    rlog("[reporter] 外网 IP 获取失败（如处公司网络，请在配置页填写 remotestorage.proxy）\n");
    return "";
  }

  std::vector<Capability> localCapabilities() {
#ifdef _WIN32
    return {{"power", "action", "电源控制",
             {{"mode", std::vector<std::string>{"sleep", "hibernate", "shutdown", "restart"}}}}};
#else
    return {};
#endif
  }

};

}  // namespace reporter

// ---------- C-ABI 工厂 ----------
extern "C" CPPD_PLUGIN_API cppdaemon::Plugin* create_plugin() { return new reporter::Reporter(); }
extern "C" CPPD_PLUGIN_API void destroy_plugin(cppdaemon::Plugin* p) { delete p; }
