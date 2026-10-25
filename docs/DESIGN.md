# cpp-daemon — 设计文档

> 配套需求见 [REQUIREMENTS.md](./REQUIREMENTS.md)。

## 1. 架构总览

```
┌──────────────────────── 宿主进程 (cpp-daemon.exe) ────────────────────────┐
│  config.json (唯一真相源) │ 托盘(Win32) │ HTTP(httplib) │ App.Quit/ApplyConfig│
│                           │             │                                  │
│                    core::PluginManager ──LoadLibrary/dlopen──┐            │
└───────────────────────────┬──────────────────────────────────┼───────────┘
                            │ 进程内虚调用（无序列化）           │
        configure / onConfigChanged / handle / onTrayClick / shutdown /
        desiredUI / configSchema / protocolVersion
                            │                                      │
┌───────────────────────────┴──────────────────────┐  ┌──────────┴──────────┐
│  plugins/reporter.dll（同进程模块）                │  │ 未来任意 plugins/<n>.dll│
│  上报 + MQTT 命令通道（cppdaemon::Plugin 实现）     │  │ 同接口，宿主零改动      │
└──────────────────────────────────────────────────┘  └─────────────────────┘

core/Plugin.h：Plugin 抽象基类 + C-ABI 工厂声明 + 传输结构体（宿主与插件都只 include 它）
```

设计要点：**宿主只认识 `cppdaemon::Plugin` 抽象接口，不认识 reporter 的具体类型**。所有「宿主触发点」
（启动、退出、配置变更、点击、请求）都通过接口驱动插件。相比 go-daemon，本实现用
**动态库 + 进程内虚调用**替代「子进程 + net/rpc」，更简单、零序列化、零父进程看护。

## 2. 目录结构

```
cpp-daemon/
├─ CMakeLists.txt              # 宿主 + 插件统一构建（Conan 2 依赖）
├─ conanfile.txt               # 依赖清单：openssl / nlohmann-json / cpp-httplib / paho-mqttpp
├─ docs/{REQUIREMENTS,DESIGN}.md
├─ include/core/
│  ├─ Plugin.h                 # Plugin 接口、kProtocolVersion、传输结构体、C-ABI 工厂 typedef
│  ├─ App.h                    # 中心状态：配置/HTTP/插件管理/托盘/quit
│  ├─ Config.h                 # Config 结构 + 读写/默认/迁移/原子写盘 声明
│  ├─ PluginManager.h          # 插件管理器声明
│  ├─ HttpServer.h             # 本地 HTTP 服务 + 路由转发 声明
│  ├─ Tray.h                   # 系统托盘（Win32）/ headless 声明
│  └─ Autostart.h              # 开机自启 声明
├─ src/
│  ├─ main.cpp                 # 入口：解析 -http、加载配置、Start、跑托盘消息循环
│  └─ core/{Config,PluginManager,HttpServer,Tray,Autostart}.cpp
├─ src/plugins/reporter/
│  ├─ CMakeLists.txt           # 产出 reporter.dll（SHARED）
│  ├─ reporter.cpp             # Plugin 实现 + 上报循环 + MQTT 通道
│  └─ crypto.cpp/.h            # OpenSSL 兼容 AES-256-CBC (Salted__+PKCS7)
└─ assets/config_page.html     # 内联/离线配置页（schema 驱动渲染）
```

## 3. 插件接口（cppdaemon::Plugin）

```cpp
namespace cppdaemon {
class Plugin {
public:
  virtual ~Plugin() = default;
  virtual const char* name() const = 0;
  virtual int protocolVersion() const = 0;
  virtual bool configure(const AppConfig& cfg) = 0;        // 仅本插件桶
  virtual bool onConfigChanged(const AppConfig& cfg) = 0;
  virtual void shutdown() = 0;
  virtual DesiredUI desiredUI() const = 0;                  // 宿主拉取
  virtual ConfigSchema configSchema() const = 0;
  virtual Response handle(const std::string& route, const Request& req) = 0;
  virtual void onTrayClick(const std::string& itemId) = 0;
};
}
extern "C" { cppdaemon::Plugin* create_plugin(); void destroy_plugin(cppdaemon::Plugin*); }
```

调用方向**只有「宿主→插件」一种**（与 go-daemon 一致）。UI 注册采用「宿主拉取 `desiredUI()`」
而非「插件回调宿主」，彻底避开反向调用。`AppConfig.pluginConfig` 为 `nlohmann::json` 对象（仅本插件桶）。

## 4. 传输类型

- `AppConfig{ Identity identity; nlohmann::json pluginConfig }`：宿主只给本插件自己的桶。
- `Identity{ uuid, name, version }`：全局身份，所有插件都需要。
- `ReporterConfig{ RemoteStorage, Report, MQTT }`：reporter 桶的反序列化结构（插件内定义）。
- `DesiredUI{ trayItems, routes }`、`ConfigSchema{ name, title, fields }`、`Field`、`Request`/`Response`、
  `ReportResult`（见 `Plugin.h`）。

## 5. 配置模型：单一真相源 + 每插件桶 + Schema 驱动

- `Config.plugins` 为每插件一个桶（`std::map<std::string, nlohmann::json>`），按插件名索引。
- 宿主 `configure` 推送 `AppConfig{ identity, plugins[name] }`（仅该插件的桶）。
- 配置页 `GET /api/config` 返回 `config + schemas`；保存时按 `ConfigSchema` 字段路径重建嵌套桶。
- 旧版顶层 `remotestorage.*`/`report.*`/`mqtt.*` 在 `ensureConfig` 中自动迁移进 `plugins.reporter`。

## 6. 生命周期与数据流

1. 启动：`App::start` → `PluginManager::loadAll(dir, identity)` → 对每个动态库 `LoadLibrary` + `GetProcAddress("create_plugin")`，
   比对 `protocolVersion`，调 `configure(bucket)`，再 `desiredUI()` 收集菜单/路由交给宿主注册。
2. 配置变更：`POST /api/config` → `App::applyConfig` → `PluginManager::applyConfig` → 各插件 `onConfigChanged`。
3. 菜单点击：宿主托盘点击 → `plugin->onTrayClick(itemId)`。
4. HTTP 请求：宿主把插件声明的路由转发 → `plugin->handle(route, req)` → 返回 `Response`。
5. 退出：`App::quit` → `plugin->shutdown()` → `destroy_plugin()` → `FreeLibrary`。

## 7. 关键设计决策与理由

| 决策 | 理由 |
|------|------|
| 动态库 + 进程内虚调用（非子进程 RPC） | C++ 惯用插件模式；简单、零序列化、零父进程看护；满足「不模仿 go-daemon 架构」 |
| 通过 `core/Plugin.h` 公共契约交互 | 依赖倒置；宿主不 link 插件、插件不 link 宿主 |
| `ConfigSchema` 驱动配置页 | 宿主不硬编码插件字段；通用渲染 + 统一 UX |
| `protocolVersion()` 校验 | 插件接口演进时老插件被清晰拒绝 |
| 桶隔离（只传本插件配置） | 插件间密钥不互见 |
| paho.mqtt.cpp 做 MQTT | 官方、稳；与 httplib 同需 OpenSSL，依赖收敛 |
| AES 用 OpenSSL EVP（Salted__+PKCS7） | 与 go-daemon（CryptoJS/OpenSSL）密文格式兼容，可互通 |
| 错误分类 `network`/`auth`/`server`/`invalid` | 网络未通时退避重试；其余立即放弃 |
| `/MD` 动态 CRT + 同工具链 | 保证 `std::string`/`nlohmann::json` 跨 DLL 边界 ABI 稳定 |
| 原子写盘（tmp + `MoveFileEx`） | 避免写盘中途崩溃导致配置损坏 |

## 8. 错误处理与上线约定

- 上报失败分 `network`/`auth`/`server`/`invalid` 四类；网络未通时指数退避（2s→60s）重试。
- 插件加载失败仅记日志并跳过该插件，**不阻塞宿主**。
- 虚调用外加异常边界（catch `std::exception`），单个插件抛 C++ 异常不扩散到宿主主流程。

## 9. 构建（Windows，Conan 2）

```powershell
# 1) 依赖由 conanfile.txt 解析（openssl / nlohmann-json / cpp-httplib / paho-mqttpp）
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=17
# 2) CMake 用 Conan 生成的 toolchain（find_package 目标名与 vcpkg 一致）
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# 产物：build/<gen>/cpp-daemon.exe + build/<gen>/plugins/reporter.dll（已同目录）
```

Linux headless：`conan install . ... -s compiler.libcxx=libstdc++11` 后加 `-DHEADLESS=ON`。
（也可用 vcpkg：CMake 的 `find_package` 写法兼容，仅把 `conan_toolchain.cmake` 换成 vcpkg 的 toolchain 即可。）

## 10. 可后续迭代

1. 嵌套/数组型配置字段（`Field.fields`/`Field.items` 已预留）。
2. 校验错误定位到字段（`onConfigChanged` 返回字段路径）。
3. 插件热加载：监听 `plugins/` 目录变化，`FreeLibrary` 后重新加载。
4. 插件崩溃隔离：未来若插件来源不可全信，可回退到子进程 + IPC（与 go-daemon 同思路）。
5. 插件签名/哈希校验：加载前校验二进制，防未授权插件。
6. 远程配置下发（插件主动 Pull）。

## 11. 磁盘信息上报（reporter 插件扩展）

### 11.1 目标与边界

在 `reporter` 插件内新增「主机磁盘信息上报」能力。**与节点状态心跳分文件、分频率**，互不干扰：

| 数据 | 文件 | 频率 | 现状 |
|------|------|------|------|
| 节点状态心跳（格式文档 #1） | `ufs-nodes/{uuid}.json` | `interval_minutes`（默认 15 分钟） | 已有，不变 |
| 主机磁盘信息（格式文档 #3） | `ufs-nodes/{uuid}_disks.json` | **每天一次** | 新增 |

- 磁盘文件与节点状态文件**文件名后缀不同**（`_disks`），共存于 `ufs-nodes/`，互不覆盖（符合格式文档「两者共存」约定）。
- 磁盘上报失败**仅记日志**，不阻断节点心跳上报（磁盘为附属信息）。

### 11.2 数据源：纯 WMI，不依赖 SMART

磁盘主信息（SN、型号、介质、大小）均来自 WMI，**无需 SMART、无需 smartctl**：

- `Win32_DiskDrive` → 物理盘：`SerialNumber`(SN)、`Model`(型号)、`MediaType`(介质：SSD/HDD/Fixed hard disk media)、`Size`(大小，字节→GB)、`InterfaceType`、`FirmwareRevision`、`Partitions`、`Status`。
- `Win32_LogicalDisk`（仅 `DriveType==3` 本地磁盘）→ 逻辑盘：`DeviceID`(盘符)、`FileSystem`、`Size`、`FreeSpace`、`UsagePercent`。
- 分区映射：通过 `Win32_DiskDriveToDiskPartition` + `Win32_LogicalDiskToPartition` 关联得到「物理盘↔分区↔逻辑盘」。

> 温度/健康等 SMART 遥测**不在本次范围**（用户明确只需要 SN/型号/介质/大小等主信息），故不引入 `MSFT_PhysicalDisk` 与 smartctl 依赖。

### 11.3 上报结构与存储

单文件包含本机全部盘，结构对齐格式文档 `host-disk-info-format.md` 第 3 节：

```json
{
  "host_info": { "device_uuid": "...", "device_name": "...", "hostname": "...",
                 "platform": "Windows", "platform_version": "...", "machine": "...", "serial_number": "..." },
  "collection_time": "2026-10-24T...",
  "last_updated": "2026-10-24T...",
  "physical_disks": [ { "DeviceID": "...", "Model": "...", "SerialNumber": "...",
                        "SizeGB": 931.51, "InterfaceType": "SCSI", "MediaType": "SSD",
                        "Partitions": 4, "Status": "OK", "FirmwareRevision": "..." } ],
  "logical_disks": [ { "DeviceID": "C:", "FileSystem": "NTFS", "SizeGB": 930.5,
                       "FreeSpaceGB": 420.3, "UsedSpaceGB": 510.2, "UsagePercent": 54.83, "DriveType": 3 } ],
  "disk_partition_mappings": [ { "PhysicalDisk": "...", "LogicalDisk": "C:", "VolumeName": "..." } ]
}
```

- `host_info.device_uuid` = `identity_.uuid`，`device_name` = `identity_.name`；`hostname`/`platform`/`machine` 等同心跳采集逻辑。
- 路径默认 `/ufs-nodes/{uuid}_disks.json`，复用 `resolveURL` 的 `{uuid}` 占位（或新增磁盘路径模板），复用现有 `cli`/`token`/代理逻辑。

### 11.4 实现要点

- **定时器**：在 `reportLoop` 内另记 `lastDiskUpload` 时间戳，每天（24h）触发一次磁盘 PUT；与心跳循环共用一个线程，独立时间判断。
- **采集**：新增 `collectHostDisks()`（COM 初始化 + WMI 查询，返回聚合结构）、`buildDiskPayload()`（组装上述 JSON）、`uploadDisk()`（PUT 到磁盘文件）。
- **配置**：`ReportConfig` 加 `bool diskInfo = true;`（开关）；`reporterFromJson` 读取 `report.disk_info`；`configSchema()` 加「上报磁盘信息」bool 项。
- **可观测**：`desiredUI()` 的 `GET /api/status` 增加返回「上次磁盘上报时间 / 磁盘计数」，便于验证。
- **构建**：`plugins/reporter/CMakeLists.txt` 需链接 `Wbemuuid.lib`（WMI 必需）；源文件已含 `<windows.h>` 等 Windows 头。

### 11.5 参考

- 数据格式：`my-data/docs/formats/host-disk-info-format.md`（第 1、3 节）。
- 节点状态上报与 inbox 逻辑见 §6 及 `reporter.cpp` 的 `sync()`。
