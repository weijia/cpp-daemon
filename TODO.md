# cpp-daemon — TODO

* Richard's TODO

## [x] 磁盘信息上报（reporter 插件扩展）

来源：需求文档 §2.7 / 设计文档 §11；数据格式 `my-data/docs/formats/host-disk-info-format.md` 第 3 节。

- [x] `Config.h`：`ReportConfig` 增加 `bool diskInfo = true;`
- [x] `reporter.cpp` `reporterFromJson`：读取 `report.disk_info`（默认 true）
- [x] `reporter.cpp` 新增 `collectHostDisks()`：WMI 采集物理盘（`Win32_DiskDrive`）、逻辑盘（`Win32_LogicalDisk` DriveType==3）、分区映射（关联 `Win32_DiskDriveToDiskPartition` + `Win32_LogicalDiskToPartition`）
- [x] `reporter.cpp` 新增 `buildDiskPayload()`：组装 `host_info + physical_disks + logical_disks + disk_partition_mappings + collection_time + last_updated`
- [x] `reporter.cpp` 新增 `uploadDisk()`：PUT 到 `ufs-nodes/{uuid}_disks.json`（复用现有 client/token/代理）
- [x] `reporter.cpp` `reportLoop`：新增 `lastDiskUpload` 时间戳，每天（24h）触发一次磁盘上报，独立于节点心跳；失败仅记日志不阻断心跳
- [x] `reporter.cpp` `configSchema()`：增加「上报磁盘信息」bool 配置项
- [x] `reporter.cpp` `desiredUI()` `GET /api/status`：返回上次磁盘上报时间 / 磁盘计数（便于验证）
- [x] `plugins/reporter/CMakeLists.txt`：链接 `Wbemuuid.lib`（WMI 必需）
- [x] 编译验证（Windows，Conan 2 + CMake），确认 `reporter.dll` 产出且 `Wbemuuid.lib` 解析通过
- [x] 运行时验证：RemoteStorage 出现 `ufs-nodes/{uuid}_disks.json`，每天更新一次，节点心跳不受影响

## [x] iot 设备信息上报（MQTT 插件）

来源：`iot_system/mqtt_push_example.py`（主题 `/PK/DEV/property/post`，payload `{"payload":{属性名:{"value":...}}}`，TLS 8883，client_id=PK@DEV，qos=1）；数据字段对齐 `host-disk-info-format.md` §3。

- [x] 新建独立插件 `src/plugins/iot_report/`（CMakeLists.txt + iot_report.cpp），不复用 `src/plugins/mqtt`
- [x] 顶层 `CMakeLists.txt` 增加 `add_subdirectory(src/plugins/iot_report)`
- [x] 独立配置段 `plugins.iot_report`：enabled/broker/port(8883)/tls(true)/verify_cert(false)/username/password/product_key/device_key/topic/client_id/interval_seconds(60)
- [x] WMI 采集（独立实现）：物理盘 `Win32_DiskDrive`、逻辑盘 `Win32_LogicalDisk WHERE DriveType=3`、host_info（hostname/platform_version/machine/serial_number）
- [x] 脱敏：device_name/hostname/serial_number/磁盘 SerialNumber/VolumeName 长度>4 保留后 4 位前段替换 xxx，<=4 整体替换 xxx；device_uuid 保留
- [x] payload 外壳 `{"payload":{host_info/physical_disks/logical_disks 各 {"value":...}}}`，不含任何凭据
- [x] 上报循环：独立线程按 interval_seconds publish qos=1；broker/凭据缺失时记日志并优雅禁用，不崩溃不阻塞
- [x] `configSchema()` 声明全部字段（password 类型）；`desiredUI()` 提供 `GET /api/iot`（connected/last_publish/last_result/last_error）
- [x] 编译验证：`build\Release\plugins\iot_report.dll` 产出，0 error；reporter.dll/mqtt.dll/scheduler.dll/cpp-daemon.exe 未破坏
- [x] 集成加载验证：临时加空凭据桶启动 daemon，确认插件加载、凭据缺失时优雅禁用、/api/iot 可查、其他插件正常；验证后已清理进程并恢复 config.json
- [x] 真实 MQTT 发布未验证：代码库无真实设备三元组（productKey/deviceNum/设备密钥），broker 凭据被脱敏，按要求不臆造、不做真实连接发布

## [x] iot 设备自助注册（方案 A：注册令牌）

来源：平台自助注册接口 `POST {platform}/prod-api/device/register`（body `{token,device_num,name}`，以响应 `code==200` 判定成功，data 返回 product_key/device_num/secret/topic，secret 仅当次返回）。

- [x] config 新增字段：`platform`（平台地址）、`register_token`（password 类型）、`device_num`（可选，缺省 `DEV`+uuid前10位大写）；cfgFromJson/configSchema 同步
- [x] 本地凭据持久化：exe 同目录 `iot_report_cred.json`（`{product_key,device_num,secret}`），存在则直接用、不再注册
- [x] 自助注册流程：无凭据文件但 register_token 非空 → httplib POST 注册，成功后写入凭据文件再连 MQTT；注册失败按 interval 周期重试
- [x] 两皆无 → 记日志「凭据与注册令牌均缺失，不连接」并优雅禁用，不崩溃不阻塞
- [x] 连接失败重试改进：连接失败不再退出线程，按 interval 周期重连直到成功或 quit_
- [x] 凭据装载后 username=device_num、password=secret、client_id=PK@DEV、topic=/PK/DEV/property/post；secret/令牌不进日志与 payload，脱敏规则不变
- [x] 编译验证：`build\Release\plugins\iot_report.dll` 产出，0 error；reporter.dll/mqtt.dll/scheduler.dll/cpp-daemon.exe 未破坏
- [x] 集成验证：空凭据+空令牌启动 daemon，确认走「凭据与注册令牌均缺失」优雅禁用分支、/api/iot 可查、其他插件正常；验证后已清理进程并恢复 config.json
- [x] 真实注册未做：禁止真实调用线上平台注册/创建设备（外部动作需用户确认）；待用户填入真实 platform+register_token 后自动完成注册与上报
- [x] TLS 自签证书修复：connectMqtt SSL 分支补 `ssl.set_enable_server_cert_auth(c.verifyCert)`（与 set_verify 同步），关闭自签 EMQX 证书链校验
- [x] 端到端验证通过：日志「已连接 MQTT topic=/richard/8694cae7-.../property/post」「发布成功 len=678」；/api/iot 返回 connected=true、last_result=ok；日志不含 secret/register_token 明文；验证后已清理 daemon 进程

## [x] 配置页必填标记（required schema + 前端红* + 保存校验）

- [x] 框架级：`include/core/Plugin.h` 的 `Field` 增加 `bool required=false`；`src/core/HttpServer.cpp` toJson 始终输出 `"required"` 字段
- [x] iot_report configSchema 标注必填：enabled/broker/port/product_key/register_token/platform；username/password hint 注明「自助注册模式可空」；tls/verify_cert 不标
- [x] 其它插件保守标注：mqtt 的 topic/secret、reporter 的 remotestorage.server/token；scheduler 为数组字段不标
- [x] config_page.html：renderField 在 label 后渲染红色「*」；submit 前 validateRequired 校验 text/number/password 非空，为空 toast 提示并阻止 POST（bool/array/select 跳过；已禁用插件跳过）
- [x] 编译验证：0 error；iot_report.dll/mqtt.dll/reporter.dll/scheduler.dll/cpp-daemon.exe 均正常产出
- [x] 实测：/api/config 返回 required=true 字段清单正确（iot_report 6 项、mqtt 2 项、reporter 2 项）；配置页 200；服务端下发 HTML 含红星渲染与校验代码；验证后已清理 daemon 进程
- [x] 空必填拦截：前端 JS 静态断言（validateRequired 在 fetch POST 前 return），未做浏览器交互实测

## [x] 托盘右键退出修复（NIM_DELETE + 进程真正退出 + 压测验证）

现象：托盘右键「退出」后图标残留、`Get-Process cpp-daemon` 仍在、9801 仍监听（http_->shutdown() 未执行到）。退出链路：托盘 WM_COMMAND kIdmQuit → `App::requestQuit()`（detached 线程跑 `quit()`）→ stopConfigWatch → `PluginManager::shutdownAll()`（字典序 iot_report→mqtt→reporter）→ http_->shutdown → `tray_->quit()`（PostMessage WM_APP+1）→ 消息循环 break 后 `Shell_NotifyIcon(NIM_DELETE)`。

- [x] 根因（打点 quit_trace.log + 卡死现场 minidump 交叉确认）：卡在 `plugins_->shutdownAll()`。iot_report 连真实 TLS broker（120.48.27.188:8883），paho 的 `connect(conn)->wait()` 与 loop 收尾 `disconnect()->wait()` 均为**无超时**无限等待；且 loop 收尾 disconnect 与 shutdown 的 disconnect **双重调用**，在 quit 与发布循环竞态窗口内（打点 fopen 改变时序即可绕过，故第一轮"锁外 disconnect"在带打点构建里 3/3、真实干净二进制仍挂），`thread_.join()` 长卡 → tray_->quit() 永不执行 → NIM_DELETE 不调用 → 图标残留 + 进程存活。reporter 的 httplib GET/PUT 未设超时，quit 时 reportThread 可能阻塞在请求上
- [x] 修复（对齐 mqtt 可靠拆毁 + 有限超时，消除所有无限等待）：
  - `iot_report.cpp`：connect()->wait() → wait_for(15s)；loop 收尾 disconnect()->wait() → wait_for(5s)；shutdown 防御性 disconnect()->wait() → wait_for(5s)。保留第一轮"disconnect wait 在 cliMu_ 锁外"改动
  - `mqtt.cpp`：connect wait_for(15s)、subscribe wait_for(10s)、loop 与 shutdown 的 disconnect wait_for(5s)（同模式加固，TCP broker 原本快但同隐患）
  - `reporter.cpp`：主上传 httplib::Client 显式 set_connection_timeout(10s)/set_read_timeout(15s)/set_write_timeout(10s)，避免 reportThread 阻塞在 GET/PUT 上导致 join 不返回
  - scheduler 在 config 中 disabled 不加载，未改
- [x] 编译：单 cmd vcvarsall x64 + uvx conan install + cmake preset 流程，0 error；cpp-daemon.exe 与四插件 dll 正常产出
- [x] 压测验证（带打点构建）：启动→等 75s 让 iot_report 真实发布→触发同路径 requestQuit，连跑 3 轮均 2–3s 内进程消失（Get-Process 为空）；打点 quit_trace.log 显示 quit 全链路（iot_report→mqtt→reporter→http→tray）逐段完成
- [x] 最终产物：移除全部 tr()/rtr() 打点与临时 `/api/quit` 路由（git diff 仅剩 iot_report/mqtt/reporter 三个插件文件，App.cpp/PluginManager.cpp/HttpServer.cpp 已还原）
- [x] 托盘图标断言：进程退出即证明 `tray_->quit()`（WM_APP+1）已执行并走到消息循环后的 `Shell_NotifyIcon(NIM_DELETE)`（该调用在 Tray::run() 消息循环 break 后无条件执行，无竞态）。Win11 托盘 flyout/提升区/UIPI 隔离下 P/Invoke 真实右键自动化未稳定跑通（中文 tooltip 编码 + 通知区窗口嵌套），以「同路径 requestQuit 压测 3/3 + NIM_DELETE 代码级论证」交付；托盘右键 WM_COMMAND kIdmQuit 与 /api/quit 调用同一 `App::requestQuit()`，路径一致
- [x] 收尾轮 WM_COMMAND 同路径压测：最终二进制（exe 22:10 构建）上，按 pid 枚举定位 CppDaemonTrayClass 窗口 → PostMessage(WM_COMMAND, wparam=2/*kIdmQuit*/, 0)，即跳过鼠标菜单交互、但与「退出」菜单项**共用同一条 WndProc→requestQuit 分支**。连测 3 轮（启动→等 75s 真实发布→投递），退出耗时 7s/6s/7s，9801 均释放，无残留进程。**如实标注：WM_COMMAND 投递（非鼠标点击）**。真实鼠标右键自动化受 Win11 flyout/UIPI 限制未跑通；已沉淀 `tray_quit_verify.ps1` 供用户一键手动复核（启动→提示手动右键退出→自动轮询 60s 报耗时/兜底清理）

## [x] iot_report 断线自动重连修复

现象：运行中 publish 抛 `MQTT error [-3]: Disconnected`，catch 仅记 lastResult/lastError，不跳出回外层重连，connected_ 假 true，平台 120s 后判 offline。

- [x] loop 重构为「外层连接重试 + 内层发布循环」：publish 异常（含 -3）→ connected_=false、lastResult=error、跳出内层 → teardownMqtt 清理旧连接 → 外层重连
- [x] 抽出 `teardownMqtt()`：disconnect()->wait_for(5s) 不持 cliMu_ + 锁内 cli_.reset()，复用至 connectMqtt 失败/loop 退出/重连前
- [x] connected_ 状态同步：断开置 false、重连成功置 true；连接建立即清 lastError_（/api/iot 快速反映恢复）
- [x] quit_ 兼容：所有 wait_for 带 quit_ 谓词；connect wait 15s / disconnect wait 5s 有限超时；shutdown 路径不变
- [x] 编译验证：0 error；iot_report.dll/mqtt.dll/reporter.dll/scheduler.dll/cpp-daemon.exe 正常产出
- [x] 正常周期上报验证：/api/iot 三次采样 connected=true、last_result=ok、last_publish 从 10:03:00 → 10:04:00 连续刷新、无连续失败
- [x] 断线模拟（手 1 改 broker 到不可达地址）未执行：用户确认服务暂不可用；以正常周期验证 + 代码级论证（发布失败 break→teardown→外层 connectMqtt 重试→恢复）交付
