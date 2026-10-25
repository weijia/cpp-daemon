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

## [x] 托盘右键退出修复（图标 NIM_DELETE + 进程真正退出 + 实测）

现象：托盘右键「退出」后图标残留、`Get-Process cpp-daemon` 仍在。退出链路为 托盘 WM_COMMAND kIdmQuit → `App::requestQuit()`（独立线程跑 `quit()`）→ stopConfigWatch → `PluginManager::shutdownAll`（字典序 iot_report→mqtt→reporter）→ http_->shutdown → `tray_->quit()`（PostMessage WM_APP+1）→ 消息循环退出后 `Shell_NotifyIcon(NIM_DELETE)`。

- [x] 根因定位（临时 stderr/文件打点 quit_trace.log 实测）：卡死在 `iot_report::shutdown()` 的 `thread_.join()`。iot_report 原 loop 收尾在**持有 `cliMu_`** 的状态下调用 `disconnect()->wait()`，与 paho 内部网络线程争锁，使 shutdown 的 join 长时间不返回；`tray_->quit()` 因此永不执行 → WM_APP+1 不投递 → NIM_DELETE 不调用 → 图标残留 + 进程存活。仅在 iot_report 连 TLS broker 并完成至少一次真实发布后触发（时序相关），故 8s 内退出干净、跑满一个发布周期后必现
- [x] 对齐 mqtt 可靠退出路径：`src/plugins/iot_report/iot_report.cpp` loop 收尾改为与 mqtt 插件一致——`disconnect()->wait()` 移到 `cliMu_` 锁外执行，再在锁内 `cli_.reset()`；shutdown() 本身已与 mqtt 同构（fire disconnect → join → 防御性 reset），未动。未回退 iot_report 任何上报行为，仅改拆毁段的加锁范围
- [x] 编译验证：单 cmd vcvarsall x64 + uvx conan install + cmake preset 流程，0 error；cpp-daemon.exe 与 iot_report/mqtt/reporter/scheduler 各 dll 正常产出，既有插件行为不变
- [x] 实测：启动最终二进制 → 等待 ≥70s 让 iot_report 真实发布一次 → 触发退出，连跑 3 轮均在 ~2–4s 内进程消失（Get-Process 为空）。触发途径为临时 `/api/quit`，其调用与托盘右键「退出」**同一条** `App::requestQuit()`（托盘 WM_COMMAND kIdmQuit → `app_->requestQuit()`）；验证后已移除该临时路由，最终产物不含调试接口
- [x] 托盘图标断言：进程退出即证明 `tray_->quit()`（WM_APP+1）已执行并走到消息循环后的 `Shell_NotifyIcon(NIM_DELETE)`（该调用在 Tray::run() 消息循环 break 后无条件执行，无竞态）。Win11 托盘 flyout/P-Invoke 右键自动化未稳定跑通（提升区/溢出区/UIPI），以「同路径 requestQuit 实测 3/3 + NIM_DELETE 代码级论证」交付
