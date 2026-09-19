# cpp-daemon — 需求文档

> 本文档描述 cpp-daemon 的功能性与非功能性需求。配套设计见 [DESIGN.md](./DESIGN.md)。
> 参考对象为同仓 `go-daemon`（Go 实现）；本实现用纯 C++ 重写，**不模仿其 go-plugin 子进程架构**，
> 改用 C++ 惯用的「运行时加载动态库 + C-ABI 工厂 + 进程内虚调用」插件模式。

## 1. 背景与目标

cpp-daemon 是一个常驻本机（主要面向 Windows）的上报代理：按配置间隔把节点状态上报到兼容
RemoteStorage 协议的服务器，并通过本地 HTTP 页面 + REST API 提供查看/配置。

核心目标：**把「上报到 RemoteStorage」以及「被远程命令控制（MQTT）」等业务能力，与宿主壳解耦**，
使其成为可插拔的动态库插件，从而支持后续在不改动宿主的前提下新增/替换插件（如日志采集、健康探针等）。

## 2. 功能需求

### 2.1 宿主壳

- 持有唯一配置真相源 `config.json`，提供 HTTP 配置页（`GET /`）与 REST API。
- 仅提供「壳」能力：系统托盘、本地 HTTP 服务、插件管理、开机自启；**自身不再实现上报/MQTT**。
- 启动、退出、配置变更、手动触发等宿主事件，通过插件接口（`Plugin` 抽象基类）驱动已加载的插件。

### 2.2 插件机制

- 插件为**动态库**（Windows `.dll` / Linux `.so`），宿主按 `plugins/` 目录加载。
- 任何插件必须实现 `cppdaemon::Plugin` 接口（见设计文档），并通过 C-ABI 工厂函数
  `create_plugin()` / `destroy_plugin()` 导出供宿主 `LoadLibrary`/`dlopen` 取用。
- 宿主与插件**只通过 `core/Plugin.h` 公共契约交互**，互不 link 对方（依赖倒置）。
- 宿主在加载时校验协议版本（`protocolVersion()`），不匹配直接拒绝。
- 进程内直接虚调用，无序列化层（与 go-daemon 的子进程 net/rpc 不同）。

### 2.3 reporter 插件（首个插件，含 MQTT）

- 接收宿主推送的配置（仅自己的桶 `plugins.reporter`），实现：
  - 周期性/手动上报节点状态到 RemoteStorage（读-合并-写回，含命令 inbox 执行）。
  - MQTT 远程命令通道（订阅主题、AES 解密、本地执行、加密回执，使用 paho.mqtt.cpp）。
- 通过 `desiredUI()` 声明托盘菜单项（如「立即上报」）与 HTTP 路由（`POST /api/update`、`GET /api/status`）。
- 通过 `configSchema()` 声明它需要的配置字段，供宿主配置页动态渲染。

### 2.4 配置

- 单一真相源：`config.json`。插件配置以「每插件一个桶」`plugins["<name>"]` 承载（`nlohmann::json` 对象），宿主只搬运不解析。
- 全局身份（`uuid`/`name`/`version`）由宿主统一下发（`AppConfig.identity`）。
- 配置页按插件自报的 `ConfigSchema` 通用渲染，宿主不硬编码任何插件字段语义。
- 旧版顶层 `remotestorage.*` / `report.*` / `mqtt.*` 字段在被读取时自动迁移进 `plugins.reporter` 桶（向后兼容）。

### 2.5 安全

- 远程命令默认禁用；启用需显式配置 broker 与 AES 密钥。
- MQTT 消息必须经 AES（OpenSSL / CryptoJS 兼容，AES-256-CBC + `Salted__` + PKCS7）解密；命令在本地以受控上下文（5 分钟超时，宿主退出即取消）执行。
- 插件配置桶隔离：宿主 `configure` 只传本插件自己的桶，插件看不到其他插件配置。
- 本地 HTTP 仅绑定 `127.0.0.1`，外部不可直接访问。

### 2.6 协议与版本

- 插件实现 `protocolVersion()`，宿主在取厂函数后比对 `kProtocolVersion`，不一致拒绝加载。
- 宿主与插件必须**使用同一工具链、同一动态 CRT（`/MD`）**构建，以保证 C++ 虚表与 `std::string` 跨 DLL 边界的 ABI 稳定。

## 3. 非功能需求

- **解耦**：宿主不依赖任何业务字段；新增插件零改动宿主代码（仅放动态库进 `plugins/`）。
- **资源**：空闲近零 CPU；仅在定时/手动/请求时发起网络。
- **跨平台**：宿主可在 Windows/Linux 构建（`HEADLESS` 宏用于无托盘环境）。
- **安全**：远程命令受密钥保护；插件为进程内模块，宿主退出即随进程卸载（无孤儿进程问题）。
- **自包含构建**：用 CMake + Conan 2 解析 OpenSSL / nlohmann-json / cpp-httplib / paho-mqttcpp 依赖。

## 4. 约束与假设

- 插件是可信本地二进制（与宿主同源构建），不做完整沙箱（进程内模式下的已知取舍）。
- 配置写盘采用「临时文件 + rename / `MoveFileEx`」原子写入。
- 所有网络 TLS 依赖系统/OpenSSL 根证书池。

## 5. 验收标准

- 宿主启动后能发现并加载 `plugins/reporter.dll`，插件上报成功、MQTT（若启用）可收发。
- 配置页正确渲染 reporter 的全部字段；修改保存后插件即时/下次生效。
- 协议版本不匹配的插件被拒绝加载并给出清晰错误。
- 宿主可在 Windows 下编译为单一宿主 exe + `plugins/reporter.dll`；`HEADLESS` 构建可在 Linux 后台运行。
