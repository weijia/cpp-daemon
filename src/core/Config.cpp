#include "core/Config.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <openssl/rand.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cppdaemon {

// ---------- UUID v4 ----------
std::string generateUUID() {
  unsigned char b[16];
  if (RAND_bytes(b, sizeof(b)) != 1) {
    return "00000000-0000-4000-8000-000000000000";
  }
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  char buf[40];
  snprintf(buf, sizeof(buf),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
  return std::string(buf);
}

// ---------- 默认配置 ----------
Config defaultConfig() {
  Config c;
  c.uuid = generateUUID();
#ifdef _WIN32
  char host[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = sizeof(host);
  c.name = (GetComputerNameA(host, &n) ? std::string(host) : "node");
#else
  char host[256] = {0};
  c.name = (gethostname(host, sizeof(host)) == 0 ? std::string(host) : "node");
#endif
  if (c.name.empty()) c.name = "node";
  c.autostart = true;
  c.http.listen = "127.0.0.1:9801";

  ReporterConfig rc;
  rc.remotestorage.server = "https://remotestorage.example.com";
  rc.remotestorage.user = "me";
  rc.remotestorage.token = "";
  rc.remotestorage.scope = "/ufs-nodes/";
  rc.remotestorage.pathTemplate = "/ufs-nodes/{uuid}.json";
  rc.report.intervalMinutes = 15;
  rc.report.extraInfo = true;
  rc.mqtt.enabled = false;
  rc.mqtt.topic = "ufsd/cmd";

  nlohmann::json bucket;
  toJson(rc, bucket);
  c.plugins["reporter"] = bucket;
  return c;
}

// ---------- json 互转 ----------
void toJson(const RemoteStorageConfig& c, nlohmann::json& j) {
  j = {{"server", c.server}, {"user", c.user}, {"token", c.token},
       {"scope", c.scope}, {"path_template", c.pathTemplate},
       {"proxy", c.proxy}};
}
void toJson(const ReportConfig& c, nlohmann::json& j) {
  j = {{"interval_minutes", c.intervalMinutes}, {"extra_info", c.extraInfo}};
}
void toJson(const MQTTConfig& c, nlohmann::json& j) {
  j = {{"enabled", c.enabled},   {"broker", c.broker},   {"topic", c.topic},
       {"username", c.username}, {"password", c.password},
       {"secret", c.secret},     {"client_id", c.clientId}};
}
void toJson(const ReporterConfig& c, nlohmann::json& j) {
  nlohmann::json rs, rp, mq;
  toJson(c.remotestorage, rs);
  toJson(c.report, rp);
  toJson(c.mqtt, mq);
  j = {{"remotestorage", rs}, {"report", rp}, {"mqtt", mq}};
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
    c.remotestorage.proxy = rs.value("proxy", "");
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

void toJson(const Config& c, nlohmann::json& j) {
  j = {{"uuid", c.uuid}, {"name", c.name}, {"autostart", c.autostart},
       {"http", {{"listen", c.http.listen}}}};
  nlohmann::json pl;
  for (auto& kv : c.plugins) pl[kv.first] = kv.second;
  j["plugins"] = pl;
}

Config fromJson(const nlohmann::json& j) {
  Config c;
  c.uuid = j.value("uuid", "");
  c.name = j.value("name", "");
  c.autostart = j.value("autostart", true);
  if (j.contains("http")) c.http.listen = j["http"].value("listen", "127.0.0.1:9801");
  if (j.contains("plugins") && j["plugins"].is_object()) {
    for (auto it = j["plugins"].begin(); it != j["plugins"].end(); ++it) {
      c.plugins[it.key()] = it.value();
    }
  }
  return c;
}

// ---------- URL 解析 ----------
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

// ---------- 校验 ----------
bool Config::validate(std::string& err) const {
  if (uuid.empty()) {
    err = "uuid 不能为空";
    return false;
  }
  if (http.listen.empty()) {
    err = "http.listen 不能为空";
    return false;
  }
  auto it = plugins.find("reporter");
  if (it != plugins.end() && !it->second.is_null()) {
    try {
      reporterFromJson(it->second);
    } catch (const std::exception& e) {
      err = std::string("plugins.reporter 配置非法: ") + e.what();
      return false;
    }
  }
  return true;
}

// ---------- 文件读写 ----------
bool loadConfig(const std::string& path, Config& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  nlohmann::json j;
  try {
    f >> j;
  } catch (...) {
    return false;
  }
  out = fromJson(j);
  return true;
}

bool saveConfig(const std::string& path, const Config& cfg) {
  nlohmann::json j;
  toJson(cfg, j);
  std::string data = j.dump(2);

  // 临时文件 + 原子 rename / MoveFileEx
#ifdef _WIN32
  char tmpl[MAX_PATH];
  snprintf(tmpl, sizeof(tmpl), "%s", path.c_str());
  std::string dir = path.substr(0, path.find_last_of("/\\"));
  if (dir.empty()) dir = ".";
  std::string tmp = dir + "\\config-" + generateUUID().substr(0, 8) + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), (std::streamsize)data.size());
    o.close();
    if (!o) return false;
  }
  if (!MoveFileExA(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp.c_str());
    return false;
  }
  return true;
#else
  std::string tmp = path + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), (std::streamsize)data.size());
    o.close();
    if (!o) return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
#endif
}

Config ensureConfig(const std::string& path) {
  Config c;
  if (!loadConfig(path, c)) {
    return defaultConfig();  // 不存在 → 默认（调用方负责写盘）
  }
  if (c.uuid.empty()) c.uuid = generateUUID();
  if (c.http.listen.empty()) c.http.listen = "127.0.0.1:9801";
  if (c.plugins.empty()) c.plugins["reporter"] = nlohmann::json::object();

  // 旧版顶层 remotestorage/report/mqtt 迁移进 plugins.reporter
  if (!c.plugins.count("reporter")) {
    nlohmann::json legacy;
    // 旧字段可能直接挂在顶层（这里仅当明确存在时迁移）
    bool hasLegacy = false;
    RemoteStorageConfig rs;
    ReportConfig rp;
    MQTTConfig mq;
    // 通过重新读取原始文件判断是否含旧顶层字段
    {
      std::ifstream f(path, std::ios::binary);
      if (f) {
        nlohmann::json raw;
        try {
          f >> raw;
          if (raw.contains("remotestorage")) {
            auto& o = raw["remotestorage"];
            rs.server = o.value("server", "");
            rs.user = o.value("user", "");
            rs.token = o.value("token", "");
            rs.scope = o.value("scope", "");
            rs.pathTemplate = o.value("path_template", "");
            hasLegacy = true;
          }
          if (raw.contains("report")) {
            auto& o = raw["report"];
            rp.intervalMinutes = o.value("interval_minutes", 0);
            rp.extraInfo = o.value("extra_info", false);
            hasLegacy = true;
          }
          if (raw.contains("mqtt")) {
            auto& o = raw["mqtt"];
            mq.enabled = o.value("enabled", false);
            mq.broker = o.value("broker", "");
            mq.topic = o.value("topic", "ufsd/cmd");
            mq.username = o.value("username", "");
            mq.password = o.value("password", "");
            mq.secret = o.value("secret", "");
            mq.clientId = o.value("client_id", "");
            hasLegacy = true;
          }
        } catch (...) {
        }
      }
    }
    if (hasLegacy) {
      if (rp.intervalMinutes == 0) rp.intervalMinutes = 15;
      ReporterConfig rc;
      rc.remotestorage = rs;
      rc.report = rp;
      rc.mqtt = mq;
      nlohmann::json bucket;
      toJson(rc, bucket);
      c.plugins["reporter"] = bucket;
    }
  }
  return c;
}

}  // namespace cppdaemon
