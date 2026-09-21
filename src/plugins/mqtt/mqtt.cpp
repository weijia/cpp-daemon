// mqtt 插件：MQTT 远程命令控制（实现 cppdaemon::Plugin）。
// 职责：连接 Broker → 订阅主题 → AES 解密消息 → 执行 shell 命令 → 加密回执。
// 从 reporter 拆分而来；是否启用直接由宿主的插件加载开关（plugins.mqtt.enabled）控制，
// 故本插件不再自带 enabled 字段，避免与宿主开关同名冲突。
#include "core/Plugin.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <mqtt/async_client.h>

#ifdef _WIN32
#include <windows.h>
#define RUN_POPEN _popen
#define RUN_PCLOSE _pclose
#else
#define RUN_POPEN popen
#define RUN_PCLOSE pclose
#endif

#include "crypto.h"

namespace {

constexpr const char* kDefaultTopic = "ufsd/cmd";

// ---------- 持久化日志（同时写文件与 stderr） ----------
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
static void mlog(const char* fmt, ...) {
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
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fflush(g_logf);
  }
  fprintf(stderr, "[%s] ", ts);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

}  // namespace

namespace mqttplugin {

struct MqttCfg {
  std::string broker;
  std::string topic = kDefaultTopic;
  std::string username;
  std::string password;
  std::string secret;
  std::string clientId;
};

MqttCfg cfgFromJson(const nlohmann::json& j) {
  MqttCfg c;
  if (!j.is_object()) return c;
  c.broker = j.value("broker", "");
  c.topic = j.value("topic", kDefaultTopic);
  c.username = j.value("username", "");
  c.password = j.value("password", "");
  c.secret = j.value("secret", "");
  c.clientId = j.value("client_id", "");
  return c;
}

class MqttPlugin : public cppdaemon::Plugin {
public:
  const char* name() const override { return "mqtt"; }
  int protocolVersion() const override { return cppdaemon::kProtocolVersion; }

  bool configure(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      identity_ = cfg.identity;
      cfg_ = cfgFromJson(cfg.pluginConfig);
    }
    quit_ = false;
    thread_ = std::thread([this]() { loop(); });
    return true;
  }

  bool onConfigChanged(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      cfg_ = cfgFromJson(cfg.pluginConfig);
    }
    // 干净重启连接线程（旧线程退出后再起新的，避免双客户端竞态）
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    quit_ = false;
    thread_ = std::thread([this]() { loop(); });
    return true;
  }

  void shutdown() override {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(cliMu_);
    if (cli_) {
      try {
        cli_->disconnect()->wait();
      } catch (...) {
      }
      cli_.reset();
    }
  }

  cppdaemon::DesiredUI desiredUI() const override {
    cppdaemon::DesiredUI ui;
    ui.routes.push_back({"GET", "/api/mqtt"});
    return ui;
  }

  cppdaemon::ConfigSchema configSchema() const override {
    cppdaemon::ConfigSchema s;
    s.name = "mqtt";
    s.title = "MQTT 远程命令";
    // 注意：不含 enabled 字段——启停由宿主的插件加载开关统一控制。
    s.fields = {
        {"broker", "Broker 地址", "text", "如 tcp://broker.example.com:1883"},
        {"topic", "订阅主题 Topic", "text", "回执发往 <topic>/reply"},
        {"username", "Broker 用户名", "text"},
        {"password", "Broker 密码", "password"},
        {"secret", "AES 密钥", "password", "与服务端一致：解密收到的命令、加密回执"},
        {"client_id", "ClientID", "text", "留空则用 ufsd-<uuid 前 8 位>"},
    };
    return s;
  }

  cppdaemon::Response handle(const std::string& /*route*/,
                             const cppdaemon::Request& req) override {
    cppdaemon::Response resp;
    if (req.path == "/api/mqtt") {
      MqttCfg c;
      bool connected = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        c = cfg_;
      }
      {
        std::lock_guard<std::mutex> lk(cliMu_);
        connected = cli_ && cli_->is_connected();
      }
      nlohmann::json j = {{"broker", c.broker},
                          {"topic", c.topic},
                          {"connected", connected},
                          {"last_message", lastMsg_},
                          {"last_result", lastResult_}};
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

  void onTrayClick(const std::string& /*itemId*/) override {}

private:
  MqttCfg cfg_;
  cppdaemon::Identity identity_;
  std::mutex mu_;

  std::mutex cliMu_;
  std::unique_ptr<mqtt::async_client> cli_;

  std::thread thread_;
  std::atomic<bool> quit_{false};

  std::string lastMsg_;
  std::string lastResult_;

  void loop() {
    MqttCfg c;
    {
      std::lock_guard<std::mutex> lk(mu_);
      c = cfg_;
    }
    {
      std::lock_guard<std::mutex> lk(cliMu_);
      if (cli_) {
        try {
          cli_->disconnect()->wait();
        } catch (...) {
        }
        cli_.reset();
      }
    }
    if (c.broker.empty()) {
      mlog("[mqtt] 未配置 Broker，插件不连接\n");
      return;
    }
    std::string topic = c.topic.empty() ? kDefaultTopic : c.topic;
    std::string cid =
        c.clientId.empty()
            ? ("ufsd-" + (identity_.uuid.size() > 8 ? identity_.uuid.substr(0, 8)
                                                    : identity_.uuid))
            : c.clientId;

    try {
      auto cli = std::make_unique<mqtt::async_client>(c.broker, cid);
      auto secret = c.secret;
      struct Cb : public mqtt::callback {
        MqttPlugin* p;
        MqttCfg cfg;
        std::string topic;
        Cb(MqttPlugin* p, MqttCfg cfg, const std::string& tp)
            : p(p), cfg(cfg), topic(tp) {}
        void connected(const std::string&) override {
          try {
            std::lock_guard<std::mutex> lk(p->cliMu_);
            if (p->cli_) p->cli_->subscribe(topic, 1)->wait();
            mlog("[mqtt] 已连接并订阅 %s\n", topic.c_str());
          } catch (const std::exception& e) {
            mlog("[mqtt] 订阅失败: %s\n", e.what());
          }
        }
        void connection_lost(const std::string& cause) override {
          mlog("[mqtt] 连接丢失: %s\n", cause.c_str());
        }
        void message_arrived(mqtt::const_message_ptr m) override {
          std::string plain;
          if (!daemoncrypto::decryptAES(cfg.secret, m->to_string(), plain)) {
            mlog("[mqtt] 解密失败（请检查 AES 密钥是否与服务端一致）\n");
            return;
          }
          p->executeCommand(cfg, plain);
        }
      };
      Cb cb(this, c, topic);
      cli->set_callback(cb);
      mqtt::connect_options conn;
      conn.set_clean_session(true);
      if (!c.username.empty()) {
        conn.set_user_name(c.username);
        conn.set_password(c.password);
      }
      {
        std::lock_guard<std::mutex> lk(cliMu_);
        cli_ = std::move(cli);
      }
      {
        std::lock_guard<std::mutex> lk(cliMu_);
        if (cli_) cli_->connect(conn)->wait();
      }
      while (!quit_) std::this_thread::sleep_for(std::chrono::seconds(2));
      std::lock_guard<std::mutex> lk(cliMu_);
      if (cli_) {
        try {
          cli_->disconnect()->wait();
        } catch (...) {
        }
        cli_.reset();
      }
    } catch (const std::exception& e) {
      mlog("[mqtt] 初始化失败: %s\n", e.what());
    }
  }

  // 收到明文消息：取 msg 字段当命令执行，结果加密回执到 <topic>/reply
  void executeCommand(const MqttCfg& c, const std::string& plain) {
    nlohmann::json src = nlohmann::json::parse(plain, nullptr, false);
    if (src.is_discarded()) {
      mlog("[mqtt] 消息不是合法 JSON\n");
      return;
    }
    std::string cmd = src.value("msg", std::string());
    if (cmd.empty()) return;
    lastMsg_ = cmd;
    mlog("[mqtt] 收到命令: %s\n", cmd.c_str());

    std::string out;
    bool ok = false;
    std::string err;
#ifdef _WIN32
    std::string shell = "cmd /C " + cmd;
#else
    std::string shell = "sh -c " + cmd;
#endif
    FILE* pipe = RUN_POPEN(shell.c_str(), "r");
    if (pipe) {
      char buf[512];
      while (fgets(buf, sizeof(buf), pipe)) out += buf;
      int code = RUN_PCLOSE(pipe);
      ok = (code == 0);
      if (!ok) err = "exit code " + std::to_string(code);
    } else {
      err = "无法启动命令";
    }
    mlog("[mqtt] 命令执行结束 ok=%d %s\n", ok ? 1 : 0, err.c_str());

    nlohmann::json rep = {{"id", src.value("id", "")},
                          {"msgId", src.value("msgId", "")},
                          {"user", src.value("user", "")},
                          {"ok", ok},
                          {"output", out},
                          {"error", err},
                          {"time", (int64_t)std::time(nullptr) * 1000}};
    lastResult_ = ok ? "ok" : err;
    std::string cipher = daemoncrypto::encryptAES(c.secret, rep.dump());
    try {
      std::lock_guard<std::mutex> lk(cliMu_);
      if (cli_) cli_->publish(mqtt::make_message(c.topic + "/reply", cipher, 1, false));
    } catch (const std::exception& ex) {
      mlog("[mqtt] 回执失败: %s\n", ex.what());
    }
  }
};

}  // namespace mqttplugin

// ---------- C-ABI 工厂 ----------
extern "C" CPPD_PLUGIN_API cppdaemon::Plugin* create_plugin() { return new mqttplugin::MqttPlugin(); }
extern "C" CPPD_PLUGIN_API void destroy_plugin(cppdaemon::Plugin* p) { delete p; }
