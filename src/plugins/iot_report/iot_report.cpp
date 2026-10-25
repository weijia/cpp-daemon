// iot_report 插件：把节点主机+磁盘信息通过 MQTT 遥测上报到 IoT 平台（PK/DEV/property/post）。
// 独立于 reporter（RemoteStorage 心跳）与 mqtt（远程命令）插件；不复用其配置桶。
// payload 外壳：{"payload":{属性名:{"value":...}}}；身份字段脱敏；不含任何凭据。
#include "core/Plugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <httplib.h>
#include <mqtt/async_client.h>

#ifdef _WIN32
#include <windows.h>
#include <wbemidl.h>
#endif

namespace {

// ---------- 持久化日志（与其它插件同文件 cpp-daemon.log） ----------
std::string logFilePath() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s(buf, n);
  size_t p = s.find_last_of("\\/");
  return (p == std::string::npos) ? "cpp-daemon.log" : s.substr(0, p) + "\\cpp-daemon.log";
#else
  return "cpp-daemon.log";
#endif
}
static FILE* g_logf = nullptr;
static void ilog(const char* fmt, ...) {
  char ts[32];
  time_t t = time(nullptr);
  struct tm tb;
#ifdef _WIN32
  localtime_s(&tb, &t);
#else
  localtime_r(&t, &tb);
#endif
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tb);
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

// ---------- 脱敏：序列号+名字类身份字段 ----------
// 长度>4：前段替换为 "xxx"，保留后 4 位；长度<=4：整体替换为 "xxx"。
std::string maskId(const std::string& raw) {
  size_t a = raw.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = raw.find_last_not_of(" \t\r\n");
  std::string s = raw.substr(a, b - a + 1);
  if (s.size() > 4) return "xxx" + s.substr(s.size() - 4);
  if (s.empty()) return "";
  return "xxx";
}

#ifdef _WIN32
std::string toUtf8W(const wchar_t* w, int len = -1) {
  if (!w) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
  if (len < 0 && !s.empty() && s.back() == '\0') s.pop_back();
  return s;
}
std::wstring toW(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
double round2(double v) { return std::round(v * 100.0) / 100.0; }

struct PhysDisk {
  std::string deviceID, model, serialNumber, interfaceType, mediaType, status, firmwareRevision;
  double sizeGB = 0.0;
  long partitions = 0;
};
struct LogDisk {
  std::string deviceID, volumeName, fileSystem;
  double sizeGB = 0.0, freeSpaceGB = 0.0;
  long driveType = 0;
};
struct HostInfo {
  std::string hostname, platformVersion, machine, serialNumber;
};

struct WmiSession {
  IWbemLocator* loc = nullptr;
  IWbemServices* svc = nullptr;
  bool comInited = false;
  bool ok = false;
  WmiSession() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) comInited = false;
    else if (FAILED(hr)) return;
    else comInited = true;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr, EOAC_NONE, nullptr);
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<LPVOID*>(&loc));
    if (FAILED(hr) || !loc) return;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(ns);
    if (FAILED(hr) || !svc) return;
    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    ok = true;
  }
  ~WmiSession() {
    if (svc) svc->Release();
    if (loc) loc->Release();
    if (comInited) CoUninitialize();
  }
  WmiSession(const WmiSession&) = delete;
  WmiSession& operator=(const WmiSession&) = delete;
};

std::string wmiStr(IWbemClassObject* o, const wchar_t* prop) {
  VARIANT v; VariantInit(&v);
  HRESULT hr = o->Get(prop, 0, &v, nullptr, nullptr);
  std::string out;
  if (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal) out = toUtf8W(v.bstrVal);
  VariantClear(&v);
  return out;
}
long wmiLong(IWbemClassObject* o, const wchar_t* prop) {
  VARIANT v; VariantInit(&v);
  HRESULT hr = o->Get(prop, 0, &v, nullptr, nullptr);
  long out = 0;
  if (SUCCEEDED(hr)) {
    if (v.vt == VT_I4) out = v.lVal;
    else if (v.vt == VT_BSTR && v.bstrVal) { try { out = std::stol(toUtf8W(v.bstrVal)); } catch (...) {} }
  }
  VariantClear(&v);
  return out;
}
double wmiBytesGB(IWbemClassObject* o, const wchar_t* prop) {
  VARIANT v; VariantInit(&v);
  HRESULT hr = o->Get(prop, 0, &v, nullptr, nullptr);
  double bytes = 0.0;
  if (SUCCEEDED(hr)) {
    if (v.vt == VT_BSTR && v.bstrVal) { try { bytes = std::stod(toUtf8W(v.bstrVal)); } catch (...) {} }
    else if (v.vt == VT_I4) bytes = (double)v.lVal;
  }
  VariantClear(&v);
  return bytes / (1024.0 * 1024.0 * 1024.0);
}
std::vector<IWbemClassObject*> wmiQuery(IWbemServices* svc, const wchar_t* wql) {
  std::vector<IWbemClassObject*> out;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(wql);
  IEnumWbemClassObject* en = nullptr;
  HRESULT hr = svc->ExecQuery(lang, q,
                              WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                              nullptr, &en);
  SysFreeString(lang); SysFreeString(q);
  if (FAILED(hr) || !en) return out;
  IWbemClassObject* obj = nullptr;
  ULONG ret = 0;
  while (en->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret == 1 && obj) {
    out.push_back(obj);
    obj = nullptr;
  }
  en->Release();
  return out;
}
#endif  // _WIN32

}  // namespace

namespace iotreport {

struct IotCfg {
  bool enabled = true;
  std::string broker;
  int port = 8883;
  bool tls = true;
  bool verifyCert = false;
  std::string username;
  std::string password;
  std::string productKey;
  std::string deviceKey;  // = deviceNum（client_id=PK@DEV，username=DEV）
  std::string topic;
  std::string clientId;
  int intervalSeconds = 60;
  // 方案 A：自助注册
  std::string platform;       // 如 http://deyutech.top:2223
  std::string registerToken;   // 注册令牌（password 类型配置，不进日志）
};

IotCfg cfgFromJson(const nlohmann::json& j) {
  IotCfg c;
  if (!j.is_object()) return c;
  c.enabled = j.value("enabled", true);
  c.broker = j.value("broker", "");
  c.port = j.value("port", 8883);
  c.tls = j.value("tls", true);
  c.verifyCert = j.value("verify_cert", false);
  c.username = j.value("username", "");
  c.password = j.value("password", "");
  c.productKey = j.value("product_key", "");
  // device_num 为主；兼容旧 device_key
  c.deviceKey = j.contains("device_num") ? j.value("device_num", "") : j.value("device_key", "");
  c.topic = j.value("topic", "");
  c.clientId = j.value("client_id", "");
  c.intervalSeconds = j.value("interval_seconds", 60);
  c.platform = j.value("platform", "");
  c.registerToken = j.value("register_token", "");
  return c;
}

class IotReportPlugin : public cppdaemon::Plugin {
public:
  const char* name() const override { return "iot_report"; }
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
    return true;
  }

  void shutdown() override {
    quit_ = true;
    cv_.notify_all();
    {
      std::lock_guard<std::mutex> lk(cliMu_);
      if (cli_) { try { cli_->disconnect(); } catch (...) {} }
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(cliMu_);
    if (cli_) { try { cli_->disconnect()->wait(); } catch (...) {} cli_.reset(); }
  }

  cppdaemon::DesiredUI desiredUI() const override {
    cppdaemon::DesiredUI ui;
    ui.routes.push_back({"GET", "/api/iot"});
    return ui;
  }

  cppdaemon::ConfigSchema configSchema() const override {
    cppdaemon::ConfigSchema s;
    s.name = "iot_report";
    s.title = "IoT 设备信息上报（MQTT 遥测）";
    s.fields = {
        {"enabled", "启用上报", "bool"},
        {"broker", "Broker 地址", "text", "如 120.48.27.188（不带协议）"},
        {"port", "端口", "number", "默认 8883"},
        {"tls", "启用 TLS", "bool", "默认 true"},
        {"verify_cert", "校验证书", "bool", "默认 false（自签/不安全）"},
        {"username", "Username（deviceNum）", "text", "自助注册模式可空（注册成功后自动填充）"},
        {"password", "设备密钥", "password", "自助注册模式可空（注册成功后自动填充）"},
        {"product_key", "ProductKey", "text"},
        {"device_key", "DeviceNum", "text"},
        {"topic", "上报主题", "text", "缺省 /{product_key}/{device_key}/property/post"},
        {"client_id", "ClientID", "text", "缺省 {product_key}@{device_key}"},
        {"interval_seconds", "上报间隔（秒）", "number", "默认 60"},
        {"platform", "平台地址", "text", "如 http://deyutech.top:2223（自助注册用）"},
        {"register_token", "注册令牌", "password", "平台生成后填入；首次注册成功后自动取得三元组"},
        {"device_num", "设备编号（可选）", "text", "缺省自动生成 DEV+uuid前10位大写；重复注册走重置分支"},
    };
    // 必填：自助注册模式下这些字段为空则无法连 MQTT/注册
    for (auto& f : s.fields) {
      if (f.key == "enabled" || f.key == "broker" || f.key == "port" ||
          f.key == "product_key" || f.key == "register_token" || f.key == "platform") {
        f.required = true;
      }
    }
    return s;
  }

  cppdaemon::Response handle(const std::string& /*route*/,
                             const cppdaemon::Request& req) override {
    cppdaemon::Response resp;
    if (req.path != "/api/iot") {
      resp.statusCode = 404;
      resp.body = "not found";
      return resp;
    }
    std::lock_guard<std::mutex> lk(stMu_);
    nlohmann::json j = {
        {"connected", (bool)connected_},
        {"last_publish", lastPublish_},
        {"last_result", lastResult_},
        {"last_error", lastError_}
    };
    resp.body = j.dump();
    resp.statusCode = 200;
    resp.header["Content-Type"] = "application/json; charset=utf-8";
    return resp;
  }

  void onTrayClick(const std::string& /*itemId*/) override {}

private:
  IotCfg cfg_;
  cppdaemon::Identity identity_;
  std::mutex mu_;

  std::mutex cliMu_;
  std::unique_ptr<mqtt::async_client> cli_;
  std::atomic<bool> connected_{false};

  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::mutex cvMu_;
  std::condition_variable cv_;

  std::mutex stMu_;
  std::string lastPublish_, lastResult_, lastError_;

  // 采集主机+磁盘（WMI，独立实现）。失败返回 false，调用方只记日志。
  bool collect(HostInfo& hi, std::vector<PhysDisk>& phys, std::vector<LogDisk>& logs) {
#ifdef _WIN32
    WmiSession wmi;
    if (!wmi.ok) return false;
    char h[256] = {0};
    DWORD hn = sizeof(h);
    GetComputerNameA(h, &hn);
    hi.hostname = h;
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    hi.machine = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? "AMD64"
               : (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) ? "ARM64" : "Unknown";
    for (auto* o : wmiQuery(wmi.svc, L"SELECT Version FROM Win32_OperatingSystem")) {
      hi.platformVersion = wmiStr(o, L"Version"); o->Release(); break;
    }
    for (auto* o : wmiQuery(wmi.svc, L"SELECT SerialNumber FROM Win32_BIOS")) {
      hi.serialNumber = wmiStr(o, L"SerialNumber"); o->Release(); break;
    }
    for (auto* o : wmiQuery(wmi.svc,
          L"SELECT DeviceID,Model,SerialNumber,Size,InterfaceType,MediaType,Partitions,Status,FirmwareRevision FROM Win32_DiskDrive")) {
      PhysDisk d;
      d.deviceID = wmiStr(o, L"DeviceID");
      d.model = wmiStr(o, L"Model");
      d.serialNumber = wmiStr(o, L"SerialNumber");
      d.sizeGB = wmiBytesGB(o, L"Size");
      d.interfaceType = wmiStr(o, L"InterfaceType");
      d.mediaType = wmiStr(o, L"MediaType");
      d.partitions = wmiLong(o, L"Partitions");
      d.status = wmiStr(o, L"Status");
      d.firmwareRevision = wmiStr(o, L"FirmwareRevision");
      phys.push_back(std::move(d));
      o->Release();
    }
    for (auto* o : wmiQuery(wmi.svc,
          L"SELECT DeviceID,VolumeName,FileSystem,Size,FreeSpace,DriveType FROM Win32_LogicalDisk WHERE DriveType=3")) {
      LogDisk d;
      d.deviceID = wmiStr(o, L"DeviceID");
      d.volumeName = wmiStr(o, L"VolumeName");
      d.fileSystem = wmiStr(o, L"FileSystem");
      d.sizeGB = wmiBytesGB(o, L"Size");
      d.freeSpaceGB = wmiBytesGB(o, L"FreeSpace");
      d.driveType = wmiLong(o, L"DriveType");
      logs.push_back(std::move(d));
      o->Release();
    }
    return true;
#else
    (void)hi; (void)phys; (void)logs;
    return false;
#endif
  }

  // 组装 {"payload":{属性名:{"value":...}}}；身份字段脱敏；不含凭据。
  std::string buildPayload() {
    HostInfo hi;
    std::vector<PhysDisk> phys;
    std::vector<LogDisk> logs;
    if (!collect(hi, phys, logs)) {
      std::lock_guard<std::mutex> lk(stMu_);
      lastError_ = "WMI 采集失败";
      return "";
    }
    nlohmann::json host = {
      {"device_uuid", identity_.uuid},            // uuid 保留不脱敏
      {"device_name", maskId(identity_.name)},
      {"hostname", maskId(hi.hostname)},
      {"platform", "Windows"},
      {"platform_version", hi.platformVersion},
      {"machine", hi.machine},
      {"serial_number", maskId(hi.serialNumber)}
    };
    nlohmann::json pa = nlohmann::json::array();
    for (const auto& d : phys) {
      pa.push_back({
        {"DeviceID", d.deviceID},
        {"Model", d.model},
        {"SerialNumber", maskId(d.serialNumber)},
        {"SizeGB", round2(d.sizeGB)},
        {"InterfaceType", d.interfaceType},
        {"MediaType", d.mediaType},
        {"Partitions", d.partitions},
        {"Status", d.status},
        {"FirmwareRevision", d.firmwareRevision}
      });
    }
    nlohmann::json la = nlohmann::json::array();
    for (const auto& d : logs) {
      double used = d.sizeGB - d.freeSpaceGB;
      double pct = d.sizeGB > 0 ? used / d.sizeGB * 100.0 : 0.0;
      la.push_back({
        {"DeviceID", d.deviceID},
        {"VolumeName", maskId(d.volumeName)},
        {"FileSystem", d.fileSystem},
        {"SizeGB", round2(d.sizeGB)},
        {"FreeSpaceGB", round2(d.freeSpaceGB)},
        {"UsedSpaceGB", round2(used)},
        {"UsagePercent", round2(pct)},
        {"DriveType", d.driveType}
      });
    }
    nlohmann::json root = {
      {"payload", {
        {"host_info", {{"value", host}}},
        {"physical_disks", {{"value", pa}}},
        {"logical_disks", {{"value", la}}}
      }}
    };
    return root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  std::string resolveTopic(const IotCfg& c) {
    if (!c.topic.empty()) return c.topic;
    return "/" + c.productKey + "/" + c.deviceKey + "/property/post";
  }
  std::string resolveClientId(const IotCfg& c) {
    if (!c.clientId.empty()) return c.clientId;
    return c.productKey + "@" + c.deviceKey;
  }

  // exe 同目录
  std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "." : s.substr(0, p);
#else
    return ".";
#endif
  }
  std::string credFilePath() { return exeDir() + sep() + "iot_report_cred.json"; }
  static std::string sep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
  }

  // 缺省稳定 deviceNum：DEV + uuid 前 10 位大写（重复注册时平台按同 deviceNum 查重走重置）
  std::string defaultDeviceNum() {
    std::string u = identity_.uuid;
    if (u.size() > 10) u = u.substr(0, 10);
    for (auto& ch : u) ch = (char)toupper((unsigned char)ch);
    return "DEV" + u;
  }

  // 从本地凭据文件装载 product_key/device_num/secret。成功返回 true。
  bool loadCreds(IotCfg& c) {
    std::string path = credFilePath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    std::ifstream in(path);
    if (!in) return false;
    nlohmann::json j;
    try { in >> j; } catch (...) { return false; }
    if (!j.is_object()) return false;
    std::string pk = j.value("product_key", "");
    std::string dn = j.value("device_num", "");
    std::string secret = j.value("secret", "");
    if (pk.empty() || dn.empty() || secret.empty()) return false;
    c.productKey = pk; c.deviceKey = dn; c.password = secret;
    return true;
  }

  // 写入本地凭据文件（仅该文件含 secret；不进日志）。
  void saveCreds(const std::string& pk, const std::string& dn, const std::string& secret) {
    nlohmann::json j = {{"product_key", pk}, {"device_num", dn}, {"secret", secret}};
    std::ofstream out(credFilePath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump();
  }

  // 自助注册：POST {platform}/prod-api/device/register。成功返回 true 并填充 c 的三元组。
  // 以 body 内 code==200 判定成功（后端 fail() 可能 HTTP 200）。日志不含 token/secret 明文。
  bool doRegister(IotCfg& c, const std::string& deviceNum) {
    if (c.platform.empty() || c.registerToken.empty()) return false;
    std::string url = c.platform + "/prod-api/device/register";
    std::string base, path;
    {
      size_t start = 0;
      if (url.compare(0, 7, "http://") == 0) start = 7;
      else if (url.compare(0, 8, "https://") == 0) start = 8;
      size_t slash = url.find('/', start);
      if (slash == std::string::npos) { base = url; path = "/"; }
      else { base = url.substr(0, slash); path = url.substr(slash); }
    }
    nlohmann::json req = {
      {"token", c.registerToken},
      {"device_num", deviceNum},
      {"name", identity_.name}
    };
    try {
      httplib::Client cli(base);
      cli.set_connection_timeout(10, 0);
      cli.set_read_timeout(10, 0);
      auto res = cli.Post(path, req.dump(), "application/json");
      if (!res) {
        ilog("[iot_report] 注册请求失败（网络错误）\n");
        return false;
      }
      nlohmann::json j;
      try { j = nlohmann::json::parse(res->body, nullptr, false); } catch (...) {}
      if (j.is_discarded()) {
        ilog("[iot_report] 注册响应解析失败 HTTP=%d\n", (int)res->status);
        return false;
      }
      int code = j.value("code", 0);
      if (code != 200) {
        ilog("[iot_report] 注册被拒 code=%d msg=%s\n", code, j.value("msg", "").c_str());
        return false;
      }
      auto d = j.value("data", nlohmann::json::object());
      std::string pk = d.value("product_key", "");
      std::string dn = d.value("device_num", deviceNum);
      std::string secret = d.value("secret", "");
      if (pk.empty() || secret.empty()) {
        ilog("[iot_report] 注册返回缺少 product_key/secret\n");
        return false;
      }
      saveCreds(pk, dn, secret);
      c.productKey = pk; c.deviceKey = dn; c.password = secret;
      ilog("[iot_report] 注册成功 device_num=%s\n", dn.c_str());
      return true;
    } catch (const std::exception& e) {
      ilog("[iot_report] 注册异常: %s\n", e.what());
      return false;
    }
  }

  // 连接 MQTT。成功返回 true；失败记日志返回 false（调用方负责重试）。
  bool connectMqtt(const IotCfg& c) {
    std::string uri = (c.tls ? "ssl://" : "tcp://") + c.broker + ":" + std::to_string(c.port);
    std::string topic = resolveTopic(c);
    std::string cid = resolveClientId(c);
    try {
      auto cli = std::make_unique<mqtt::async_client>(uri, cid);
      { std::lock_guard<std::mutex> lk(cliMu_); cli_ = std::move(cli); }
      mqtt::connect_options conn;
      conn.set_clean_session(true);
      conn.set_connect_timeout(10);
      conn.set_keep_alive_interval(20);
      conn.set_user_name(c.deviceKey);       // username = device_num
      conn.set_password(c.password);         // password = secret
      if (c.tls) {
        mqtt::ssl_options ssl;
        ssl.set_verify(c.verifyCert);                 // post-connect 主机名校验
        ssl.set_enable_server_cert_auth(c.verifyCert); // 服务器证书链校验（自签 EMQX 需关闭）
        conn.set_ssl(ssl);
      }
      mqtt::async_client* raw = nullptr;
      { std::lock_guard<std::mutex> lk(cliMu_); raw = cli_.get(); }
      raw->connect(conn)->wait();
      connected_ = true;
      ilog("[iot_report] 已连接 MQTT topic=%s\n", topic.c_str());
      return true;
    } catch (const std::exception& e) {
      connected_ = false;
      ilog("[iot_report] 连接失败: %s\n", e.what());
      std::lock_guard<std::mutex> lk(stMu_);
      lastError_ = std::string("连接失败: ") + e.what();
      return false;
    }
  }

  void loop() {
    IotCfg c;
    { std::lock_guard<std::mutex> lk(mu_); c = cfg_; }
    if (!c.enabled) { ilog("[iot_report] 已禁用（enabled=false）\n"); return; }

    int interval = c.intervalSeconds > 0 ? c.intervalSeconds : 60;
    std::string deviceNum = c.deviceKey;
    if (deviceNum.empty()) deviceNum = defaultDeviceNum();

    // 1) 本地凭据文件优先
    bool haveCreds = loadCreds(c);
    if (!haveCreds && !c.registerToken.empty()) {
      // 2) 有注册令牌 → 注册（失败按 interval 周期重试，直到成功或 quit）
      while (!quit_ && !haveCreds) {
        haveCreds = doRegister(c, deviceNum);
        if (haveCreds) break;
        std::unique_lock<std::mutex> lk(cvMu_);
        cv_.wait_for(lk, std::chrono::seconds(interval), [this]() { return quit_.load(); });
      }
    }
    if (!haveCreds) {
      ilog("[iot_report] 凭据与注册令牌均缺失，不连接\n");
      std::lock_guard<std::mutex> lk(stMu_);
      lastError_ = "凭据与注册令牌均缺失，未连接";
      return;
    }

    // 3) 连 MQTT（失败按 interval 重试，直到成功或 quit）
    while (!quit_ && !connectMqtt(c)) {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.wait_for(lk, std::chrono::seconds(interval), [this]() { return quit_.load(); });
    }
    if (quit_) return;

    std::string topic = resolveTopic(c);
    while (!quit_) {
      {
        std::unique_lock<std::mutex> lk(cvMu_);
        cv_.wait_for(lk, std::chrono::seconds(interval), [this]() { return quit_.load(); });
      }
      if (quit_) break;
      std::string body = buildPayload();
      if (body.empty()) continue;
      try {
        std::lock_guard<std::mutex> lk(cliMu_);
        if (!cli_) break;
        cli_->publish(mqtt::make_message(topic, body, 1, false));
        std::lock_guard<std::mutex> slk(stMu_);
        lastPublish_ = nowISO();
        lastResult_ = "ok";
        lastError_.clear();
        ilog("[iot_report] 发布成功 len=%zu\n", body.size());
      } catch (const std::exception& e) {
        ilog("[iot_report] 发布失败: %s\n", e.what());
        std::lock_guard<std::mutex> slk(stMu_);
        lastResult_ = "error";
        lastError_ = e.what();
      }
    }
    // 断开连接：wait 不持 cliMu_（与 mqtt 插件一致）。
    // 若在持锁状态下调用 disconnect()->wait()，会与 paho 内部网络线程争锁，
    // 导致 shutdown() 的 thread_.join() 长时间卡死（表现为托盘退出后进程不退出）。
    if (cli_) {
      try { cli_->disconnect()->wait(); } catch (...) {}
    }
    {
      std::lock_guard<std::mutex> lk(cliMu_);
      cli_.reset();
    }
    connected_ = false;
  }

  static std::string nowISO() {
    time_t t = time(nullptr);
    struct tm tb;
#ifdef _WIN32
    localtime_s(&tb, &t);
#else
    localtime_r(&t, &tb);
#endif
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tb);
    return b;
  }
};

}  // namespace iotreport

extern "C" CPPD_PLUGIN_API cppdaemon::Plugin* create_plugin() { return new iotreport::IotReportPlugin(); }
extern "C" CPPD_PLUGIN_API void destroy_plugin(cppdaemon::Plugin* p) { delete p; }
