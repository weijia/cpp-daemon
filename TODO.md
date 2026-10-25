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
