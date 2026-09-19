#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/Plugin.h"

namespace cppdaemon {

struct RemoteStorageConfig {
  std::string server;
  std::string user;
  std::string token;
  std::string scope;
  std::string pathTemplate;
};

struct ReportConfig {
  int intervalMinutes = 15;
  bool extraInfo = true;
};

struct MQTTConfig {
  bool enabled = false;
  std::string broker;
  std::string topic;
  std::string username;
  std::string password;
  std::string secret;
  std::string clientId;
};

struct ReporterConfig {
  RemoteStorageConfig remotestorage;
  ReportConfig report;
  MQTTConfig mqtt;
};

struct HTTPConfig {
  std::string listen = "127.0.0.1:9801";
};

// 程序完整配置，持久化于同目录 config.json。
struct Config {
  std::string uuid;
  std::string name;
  bool autostart = true;
  HTTPConfig http;
  // 每插件一个桶，按插件名索引（宿主只搬运不解析）。
  std::map<std::string, nlohmann::json> plugins;

  bool validate(std::string& err) const;
};

// 生成 UUID v4（依赖 OpenSSL RAND_bytes）。
std::string generateUUID();

// 默认配置（首次运行）。
Config defaultConfig();

// 从磁盘加载；不存在返回默认；旧版顶层 remotestorage/report/mqtt 自动迁移进 plugins.reporter。
Config ensureConfig(const std::string& path);

bool loadConfig(const std::string& path, Config& out);
bool saveConfig(const std::string& path, const Config& cfg);

// json 互转辅助（供 HTTP/reporter 复用）。
void toJson(const Config& c, nlohmann::json& j);
Config fromJson(const nlohmann::json& j);

void toJson(const ReporterConfig& c, nlohmann::json& j);
ReporterConfig reporterFromJson(const nlohmann::json& j);

// 各子配置序列化（reporter 插件复用）。
void toJson(const RemoteStorageConfig& c, nlohmann::json& j);
void toJson(const ReportConfig& c, nlohmann::json& j);
void toJson(const MQTTConfig& c, nlohmann::json& j);

// 计算 PUT 目标 URL（替换 {user}/{uuid}）。
std::string resolveURL(const RemoteStorageConfig& rs, const std::string& uuid);

}  // namespace cppdaemon
