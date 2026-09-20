// reporter 插件：RemoteStorage 上报 + MQTT 远程命令（实现 cppdaemon::Plugin）。
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

#include <mqtt/async_client.h>

#ifdef _WIN32
#include <windows.h>
#include <iphlpapi.h>
#define RUN_POPEN _popen
#define RUN_PCLOSE _pclose
#else
#define RUN_POPEN popen
#define RUN_PCLOSE pclose
#endif

#include "crypto.h"

// 插件自包含：复刻宿主 Config.cpp 的配置解析与 URL 工具。
// 依据 Plugin.h 约定，宿主与插件互不 link，故插件需自带这些小工具。
namespace cppdaemon {

void toJson(const MQTTConfig& c, nlohmann::json& j) {
  j = {{"enabled", c.enabled},   {"broker", c.broker},   {"topic", c.topic},
       {"username", c.username}, {"password", c.password},
       {"secret", c.secret},     {"client_id", c.clientId}};
}

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
  if (j.contains("mqtt")) {
    auto& mq = j["mqtt"];
    c.mqtt.enabled = mq.value("enabled", false);
    c.mqtt.broker = mq.value("broker", "");
    c.mqtt.topic = mq.value("topic", "ufsd/cmd");
    c.mqtt.username = mq.value("username", "");
    c.mqtt.password = mq.value("password", "");
    c.mqtt.secret = mq.value("secret", "");
    c.mqtt.clientId = mq.value("client_id", "");
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

constexpr const char* kDefaultTopic = "ufsd/cmd";

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
  cppdaemon::MQTTConfig mqtt;
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
  nlohmann::json mj;
  cppdaemon::toJson(p.mqtt, mj);
  j = {{"uuid", p.uuid}, {"name", p.name}, {"status", p.status},
       {"last_access", p.lastAccess}, {"hostname", p.hostname}, {"os", p.os},
       {"version", p.version}, {"external_ip", p.externalIP},
       {"internal_ips", p.internalIPs}, {"capabilities", p.capabilities},
       {"inbox", p.inbox}, {"mqtt", mj}};
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
    client_.reset();
    quit_ = false;
    mqttQuit_ = false;
    reportThread_ = std::thread([this]() { reportLoop(); });
    mqttThread_ = std::thread([this]() { syncMQTT(); });
    return true;
  }

  bool onConfigChanged(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!cfg.pluginConfig.is_null()) cfg_ = cppdaemon::reporterFromJson(cfg.pluginConfig);
    }
    {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.notify_all();
    }
    // 干净地重启 MQTT 线程（旧线程退出后再起新线程，避免双客户端竞态）
    {
      std::lock_guard<std::mutex> lk(mqttMu_);
      mqttQuit_ = true;
    }
    if (mqttThread_.joinable()) mqttThread_.join();
    {
      std::lock_guard<std::mutex> lk(mqttMu_);
      mqttQuit_ = false;
    }
    mqttThread_ = std::thread([this]() { syncMQTT(); });
    return true;
  }

  void shutdown() override {
    quit_ = true;
    mqttQuit_ = true;
    {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.notify_all();
    }
    if (reportThread_.joinable()) reportThread_.join();
    if (mqttThread_.joinable()) mqttThread_.join();
    if (mqttCli_) {
      try { mqttCli_->disconnect()->wait(); } catch (...) {}
      mqttCli_.reset();
    }
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
        {"mqtt.enabled", "启用 MQTT", "bool"},
        {"mqtt.broker", "Broker 地址", "text"},
        {"mqtt.topic", "订阅主题 Topic", "text"},
        {"mqtt.username", "Broker 用户名", "text"},
        {"mqtt.password", "Broker 密码", "password"},
        {"mqtt.secret", "AES 解密密钥", "password"},
        {"mqtt.client_id", "ClientID", "text"},
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
  std::thread mqttThread_;
  std::mutex mqttMu_;
  bool mqttQuit_ = false;

  // 上报状态
  std::mutex stateMu_;
  cppdaemon::ReportResult state_;
  std::string lastError_;

  std::map<std::string, int64_t> executed_;  // cmdId -> 已执行时间戳

  std::string targetURL() {
    std::lock_guard<std::mutex> lk(mu_);
    return cppdaemon::resolveURL(cfg_.remotestorage, identity_.uuid);
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
      p.externalIP = externalIP();
    }
    p.capabilities = localCapabilities();
    if (c.mqtt.enabled) p.mqtt = c.mqtt;
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
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) return ips;
    std::vector<unsigned char> buf(size);
    if (GetAdaptersInfo((PIP_ADAPTER_INFO)buf.data(), &size) != NO_ERROR) return ips;
    for (PIP_ADAPTER_INFO a = (PIP_ADAPTER_INFO)buf.data(); a; a = a->Next) {
      for (PIP_ADDR_STRING s = &a->IpAddressList; s; s = s->Next) {
        std::string ip(s->IpAddress.String);
        if (ip == "0.0.0.0") continue;
        ips.push_back(ip);
      }
    }
#endif
    return ips;
  }

  std::string externalIP() {
    try {
      httplib::Client cli("https://api.ipify.org");
      auto r = cli.Get("/?format=text");
      if (r && r->status == 200) return r->body;
    } catch (...) {
    }
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

  // ---------- MQTT ----------
  std::unique_ptr<mqtt::async_client> mqttCli_;

  void syncMQTT() {
    cppdaemon::ReporterConfig c;
    {
      std::lock_guard<std::mutex> lk(mu_);
      c = cfg_;
    }
    if (mqttCli_) {
      try { mqttCli_->disconnect()->wait(); } catch (...) {}
      mqttCli_.reset();
    }
    if (!c.mqtt.enabled || c.mqtt.broker.empty()) {
      rlog("[reporter] MQTT 未启用或未配置 Broker\n");
      return;
    }
    std::string topic = c.mqtt.topic.empty() ? kDefaultTopic : c.mqtt.topic;
    std::string cid = c.mqtt.clientId.empty()
                          ? ("ufsd-" + (identity_.uuid.size() > 8 ? identity_.uuid.substr(0, 8) : identity_.uuid))
                          : c.mqtt.clientId;

    try {
      auto cli = std::make_unique<mqtt::async_client>(c.mqtt.broker, cid);
      auto secret = c.mqtt.secret;
      auto broker = c.mqtt.broker;
      auto t = topic;
      auto self = this;
      struct Cb : public mqtt::callback {
        Reporter* r;
        std::string secret, topic;
        Cb(Reporter* r, const std::string& s, const std::string& tp)
            : r(r), secret(s), topic(tp) {}
        void connected(const std::string&) override {
          try {
            r->mqttCli_->subscribe(topic, 1)->wait();
            rlog("[reporter] MQTT 已订阅 %s\n", topic.c_str());
          } catch (const std::exception& e) {
            rlog("[reporter] MQTT 订阅失败: %s\n", e.what());
          }
        }
        void connection_lost(const std::string& cause) override {
          rlog("[reporter] MQTT 连接丢失: %s\n", cause.c_str());
        }
        void message_arrived(mqtt::const_message_ptr m) override {
          std::string plain;
          if (!reporter::decryptAES(secret, m->to_string(), plain)) {
            rlog("[reporter] MQTT 解密失败\n");
            return;
          }
          r->handleMQTT(plain);
        }
      };
      Cb cb(this, secret, topic);
      cli->set_callback(cb);
      mqtt::connect_options conn;
      conn.set_clean_session(true);
      if (!c.mqtt.username.empty()) {
        conn.set_user_name(c.mqtt.username);
        conn.set_password(c.mqtt.password);
      }
      mqttCli_ = std::move(cli);
      mqttCli_->connect(conn)->wait();
      // 阻塞直到退出
      while (!mqttQuit_ && !quit_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      try { mqttCli_->disconnect()->wait(); } catch (...) {}
      mqttCli_.reset();
    } catch (const std::exception& e) {
      rlog("[reporter] MQTT 初始化失败: %s\n", e.what());
    }
  }

  void handleMQTT(const std::string& plain) {
    nlohmann::json src = nlohmann::json::parse(plain, nullptr, false);
    std::string cmd = src.value("msg", std::string());
    if (cmd.empty()) return;
    std::string out;
    bool ok = true;
    std::string e;
    if (mqttCli_) {
      // 执行命令（cmd /C 或 sh -c），5 分钟超时
      std::string shell = cmd;
#ifdef _WIN32
      shell = "cmd /C " + cmd;
#else
      shell = "sh -c " + cmd;
#endif
      FILE* pipe = RUN_POPEN(shell.c_str(), "r");
      if (pipe) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) out += buf;
        int code = RUN_PCLOSE(pipe);
        ok = (code == 0);
        if (!ok) e = "exit code " + std::to_string(code);
      } else {
        ok = false;
        e = "无法启动命令";
      }
    }
    if (!mqttCli_) return;
    // 加密回执到 topic+"/reply"
    nlohmann::json rep = {{"id", src.value("id", "")},
                          {"msgId", src.value("msgId", "")},
                          {"user", src.value("user", "")},
                          {"ok", ok},
                          {"output", out},
                          {"error", e},
                          {"time", (int64_t)std::time(nullptr) * 1000}};
    std::string cipher = reporter::encryptAES(cfg_.mqtt.secret, rep.dump());
    try {
      mqttCli_->publish(mqtt::make_message(cfg_.mqtt.topic + "/reply", cipher, 1, false));
    } catch (const std::exception& ex) {
      rlog("[reporter] MQTT 回执失败: %s\n", ex.what());
    }
  }
};

}  // namespace reporter

// ---------- C-ABI 工厂 ----------
extern "C" CPPD_PLUGIN_API cppdaemon::Plugin* create_plugin() { return new reporter::Reporter(); }
extern "C" CPPD_PLUGIN_API void destroy_plugin(cppdaemon::Plugin* p) { delete p; }
