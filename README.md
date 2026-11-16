# 头环 MCAP 上传服务（C / ARM64 Linux）

发布目录只需一个 `head-cam-service`，内置监控默认配置、版本、构建信息、部署和更新工具。上传、监控由守护进程独立管理。

首次部署时，将程序上传到 `/mnt/cdata/head-cam-service`，然后以 root 执行：

```sh
cd /mnt/cdata
chmod 700 head-cam-service
./head-cam-service install
```

安装后自动启动并在设备的 `/config/diy.sh` 注册开机自启。日常管理使用 `./head-cam-service` 的 `restart`、`stop`、`start`、`status` 子命令。手动恢复默认网关可运行 `./head-cam-service dhcp`。
自更新默认关闭，安装、开机和重启不检查远端更新；手动更新先 stop，再 update，最后 start。
查询当前文件版本：`./head-cam-service --version`；查询后台运行版本：`./head-cam-service status`；构建详情：`./head-cam-service --build-info`。
`EgoQueryDeviceInfo()` 返回的完整 `deviceCode` 用作 MQTT Client ID。MQTT Topic 中的 `{sn}` 和 MQTT 报文里的 `device_id`、`device_sn` 取 SN 后四位。OTA 请求体里的 `device_id` 同样使用后四位。固件版本取 `version`。SDK 查询失败时监控状态为 `sdk_unavailable`，上传服务独立运行。

手机 App 浏览头环目录、选择目录后提交任务。C 服务递归扫描 MCAP，解析头环文件名，校验 SHA256，上传 OSS 并写入 metadata。接口和任务管理参考 teleop，头环命名及 metadata 参考 develop 的 head_cam。

**没有附件处理，没有 MNS 通知，不自动修复本地 MCAP；支持 App 选择上传成功后删除本地文件。**

## 单独启动上传组件

运行产物是 `dist/head-cam-service`，一个 ARM64 Linux 静态可执行文件，内嵌 ossutil、CA 证书与加密凭证。设备无需 Python、额外 ossutil 或明文 AK/SK 配置；不再发布独立 JSON、版本、校验清单或脚本。

```sh
cd /mnt/cdata
chmod 700 head-cam-service
./head-cam-service upload --self-check
./head-cam-service upload --host 0.0.0.0 --port 38127
```

使用 upload 子命令启动 HTTP 服务，App 连接 `http://头环IP:38127`。开机自启动按部署文档安装。`--self-check` 检查内嵌凭证初始化和 ossutil 启动，不访问 OSS，也不打印凭证。

默认运行目录为可执行文件旁的 `runtime/`，可通过 `--runtime-dir /可写路径/runtime` 指定（父目录必须存在）。运行目录需要可写且允许执行，用于释放内嵌依赖。设备还需可用的 `/proc` 和可写 `/tmp`。静态 ELF 的 Linux ABI 基线为 3.7，仍需在实际头环系统验证兼容性。

可选 `--allowed-root /mnt/sd/sd0` 限制浏览与上传范围。默认不限制目录根路径，初始浏览页列出自动识别的 TF 卡；配置后初始页列出该允许根目录。

设置环境变量 `HEAD_CAM_UPLOAD_TOKEN` 后，所有接口需 `Authorization: Bearer <令牌>`；未设置则不启用鉴权。

## App 接口

App 保存提交返回的任务 ID，页面恢复后可继续轮询对应任务。

| 接口 | 用途 |
| --- | --- |
| `GET /health` | 服务就绪检查，不验证 OSS 连通性 |
| `GET /directories` | TF 卡或允许根目录列表 |
| `GET /directories?path=...&offset=0` | 浏览子目录，每页最多 200 项 |
| `GET /files?path=...&offset=0` | 列出当前目录完整 MCAP 文件，每页最多 200 项 |
| `POST /upload` | 提交 `{"path":"/mnt/sd/sd0/采集目录"}` |
| `GET /upload/{task_id}` | 查询进度和逐文件结果 |
| `POST /files/delete` | 删除单个本地 MCAP，提交目录 `path`、文件名 `name` 和列表返回的 `fileId` |

App 点击目录进入下一层，点击“上传当前目录”时提交返回的 path，无需用户输入路径。路径属于头环，不是手机。仓库提供设备接口，App 页面需在 App 工程中接入。

POST 立即返回任务 ID，后台逐个上传；`accepted=true` 不表示上传完成。提交 TF 卡根目录递归处理卡内数据，提交子目录只处理该目录。无终端输入或 YES 确认。

目录内单选或多选文件：POST `/upload` 提交 `{"path":"/mnt/sd/sd0/采集目录","files":["文件1.mcap","文件2.mcap"]}`。只处理选中的当前层文件，不递归；files 限 1–100 个不重复名称，请求总大小仍最多 16 KiB。App 用 `/files` 展示文件并勾选，切换目录清空勾选。添加 `upload_state=UPLOADED` 可筛选已上传且仍在设备上的文件。

## 文件和业务规则

- 支持 `.mcap` / `.mcap.incomplete`，后缀不区分大小写。跳过符号链接、系统索引目录及 runtime。
- 识别 `SSIEGO_Cam_<设备号>_<YYYYMMDD>_*` 和 `SSI_IRIS2_EGO_<型号变体>_<设备号>_<YYYYMMDD>_*`。设备号为英文字母或数字；日期需有效。前缀大小写遵循 develop。
- 主文件名保留原样。因 ossutil metadata 格式限制，拒绝文件名中的 `#`、控制字符、非 ASCII 及路径分隔符；父目录支持中文、空格。
- OSS 路径为 `oss://<bucket>/original_data/ego_human_data/head_cam/<YYMMDD>/<设备号>_device/<文件名>`。
- 空文件单独记 empty。未完成 MCAP 不上传、不修复，记录失败；已有对应完整文件时忽略 incomplete。
- C 解析器读取消息时间戳，支持无压缩、Zstd、LZ4，校验结构及已提供的 CRC；时长为最大与最小 log time 的差，保留三位小数。解析失败记失败。
- 上传前后计算 SHA256 并检查文件属性变化。请在采集结束后提交目录，上传期间不要修改文件。
- 远端明确返回 NoSuchKey 才上传；权限、网络错误不当作对象不存在。
- 已存在且 SHA256 相同：跳过重传，刷新 metadata；不同：报错不覆盖；历史对象无有效 SHA256：跳过、标记 unknown，不修改对象。比较依据远端 metadata，不下载远端文件重新计算。
- 同批多个本地文件映射到同一 OSS key 时均报错；单文件失败不阻止后续文件。
- 文件和 metadata 都处理成功才计成功；metadata 失败可重新提交补写。默认保留本地数据；传 delete_after_upload=true 时仅删除校验及 metadata 均成功的文件。
- 单任务最多 10,000 个主数据文件，递归深度最多 128。需解析的单条记录及 Chunk 解压结果最多 64 MiB，超限明确报错；大文件不整体载入内存。

## OSS metadata

以下字段写成 `x-oss-meta-<字段名>`，值规则参考 develop 的 head_cam；OSS header 名称不区分大小写。

| 字段 | 取值 |
| --- | --- |
| ossPath / rootPrefix | 完整 OSS URI / original_data |
| dataDomain / dataKind | ego_human_data / head_cam_v2 |
| captureDate | YYYY-MM-DD |
| deviceNo / deviceDir | 设备号 / `<设备号>_device` |
| sessionName / relativePath | YYMMDD / `<YYMMDD>/<设备号>_device/<文件名>` |
| objectRole / fileFormat | data / mcap |
| fileSize / duration | 字节数 / 秒 |
| etag / lastModified | OSS 返回的 ETag / 转换为 UTC ISO 格式的时间 |
| storageClass / ossMetadata | Standard / `{}` |
| sha256 | 额外校验字段，小写十六进制 |

不写附件关联、遥操 quality 或 MNS 字段。当前两种头环命名不提取 task。

## 任务管理

后台串行执行任务，HTTP 可并发。最多 100 个未完成任务；同目录且选择范围相同的重复提交返回原任务，同目录不同选择及父子目录重叠返回 409。终态任务重新提交创建新任务。

状态：`queued/scanning/uploading` 为处理中；`completed/partial_failed/failed/no_data/interrupted` 为终态。completed 可能包含空文件或内容未知的 skipped，应展示逐文件结果。

任务查询提供整批及 current_file 的 progress_percent、bytes_total、bytes_uploaded 等字节进度字段，以及解析、传输、校验和 metadata 阶段。App 使用百分比字段更新进度条，处理时最多 99.9%，completed 才显示 100%。processed/total 为文件计数。

`runtime/tasks/` 保存任务 JSON，`runtime/logs/` 保存日志。状态通过临时文件、fsync 和原子重命名落盘；保存失败后拒绝新任务。历史不自动清理。

同一个 runtime 仅允许一个服务。SIGINT/SIGTERM 停止接收请求并等待已接收任务结束；强制结束时正在运行的 ossutil 一并终止。重启后未完成任务标记 interrupted，由 App 重新提交，不自动续跑。

不要用不同 runtime 的多个服务或外部客户端同时写同一批 OSS key；远端查询和上传之间没有跨进程事务锁。ossutil 连接超时 10 秒、读取超时 30 秒、内部重试 2 次，单次进程最多 30 分钟。

## 打包与离线验证

在 Linux 或 WSL Ubuntu 22.04 准备构建环境并生成 ARM64 成品。

```sh
sudo apt-get update
sudo apt-get install -y cmake build-essential gcc-aarch64-linux-gnu \
  libssl-dev liblz4-dev libzstd-dev python3 python3-cryptography qemu-user
bash scripts/prepare_arm64.sh
CREDENTIALS_SOURCE=/mnt/d/real-device-upload-teleop/upload_core.py bash build_release.sh
```

已有 `.deps/arm64` 时可直接运行最后一条命令。也可用 `CREDENTIALS_JSON` 指向私有凭证配置。默认使用仓库内的 ARM64 ossutil v1.7.19，可用 `OSSUTIL_BIN` 和 `CA_BUNDLE` 指定替代输入。生成后可运行 `python3 tests/smoke_release.py dist/head-cam-service --qemu` 离线验证。Windows 可用 `./scripts/publish_oss_release.ps1` 预览，显式加 `-Upload` 才发布。

成品加密内嵌凭证，运行时在私有 `runtime/session-*` 释放 `0600` 凭证配置，正常退出清理，异常残留下次启动时清理。AK/SK 不传入进程参数、不输出到接口或日志。二进制含解密材料，设备被控制后仍可能提取密钥。

第三方依赖包括 cJSON v1.7.19、OpenSSL、LZ4、Zstandard、glibc、BusyBox 和 ossutil。仓库保留 cJSON 的 MIT 许可证，Ubuntu 依赖许可证随解包文件保存在 `.deps/arm64/usr/share/doc/`。 [cJSON 许可证](third_party/cjson/LICENSE) · [ossutil](https://github.com/aliyun/ossutil) · [MCAP 格式](https://mcap.dev/spec)


### 真实固件 OTA（第一版）

ARM64 成品内嵌供应商 `libegosdk.so` 与桥接程序，启动后释放到私有 `/tmp/head-cam-sdk-版本/`。主服务静态链接，SDK 桥接依赖设备的 glibc、libstdc++ 与供应商通信服务。设备上缺少 `/tmp/ego_upgrade.sock` 时，先由供应商启动对应服务；`EgoUpgradeInit()` 成功不代表设备查询已连通。

内置配置使用 `head_cam_v2` 作为 `hardware_model`，云端 `ota.start` 的 `data.hardwareModel` 须一致。录制扫描根目录为 `/mnt/sd`，存储容量仍读取 `/mnt/sd/sd0`。如果部署了外部 `monitor.json`，守护进程优先读取该文件，也支持 `HEAD_CAM_MONITOR_CONFIG` 显式指定。`device_id` 和 `device_sn` 在运行时由 SDK 覆盖，不需要手工写 SN。

1. `ota.start` 从 `orin/prod/device/{sn}/downlink/req` 下发。校验参数、型号、重复指令后查询整机固件版本。版本格式为 `1.105.11.12.T`，后缀相同时依次按数字比较首段、递增版本号、月份、日期。目标版本更高时可受理。操作人明确选择降级时，云端在 `data` 中加布尔字段 `"allowDowngrade": true`，设备才受理较低的目标版本，并走同样的下载、校验、安装和版本确认流程。未授权降级、目标版本相同、后缀不同或格式不符时返回 `4222 VERSION_CONFLICT`。SDK 查询失败返回 `5001 TEMPORARY_UNAVAILABLE`。
2. 回复 `accepted: true` 后检查工作状态和电量。录制、上传、非空闲状态结束本次任务并上报 `DEVICE_BUSY`。SDK 明确报告充电中（`charge == 1`）时跳过电量检查；其余情况下电量必须有效且至少为 20%，低于门槛上报 `LOW_BATTERY`。SDK 状态查询失败仍终止任务。安装前再次检查。
3. OSS 临时 HTTPS URL 下载到 `/tmp/head-cam-ota-随机目录/firmware.bin`。下载前检查空间和空闲内存，除包大小外预留 32 MiB。校验 TLS 证书、文件大小和 SHA-256；401/403 时用 `ota.getDownloadUrl` 为原 `firmwareId` 续签一次，等待上限 120 秒。当前不支持下载重定向和断点续传。
4. 再查状态和电量，把本地文件路径交给 `EgoExecuteUpgradeFile()`。整个下载、安装和完成核对期间，与本服务上传及 OSS 服务自更新互斥。SDK 回调驱动安装进度，变化时最多每 5 秒上报一次，阶段切换立即上报。下载映射到 0–50%，校验 50%，安装 55–95%，回调成功仍不报 100%。
5. SDK 成功后等待设备恢复，重新查询实际版本及空闲状态；目标版本一致才上报 `success / completed / 100`。180 秒未确认则失败。任务及待确认上报保存在 `runtime/monitor/mqtt-session.json`，服务重启后恢复核对；执行中断且没有 SDK 成功记录时报告 `UPGRADE_INTERRUPTED`。这一版不实现启动分区验收和自动回滚，也不主动调用 Linux reboot。
6. `ota.reportStatus` 从 `orin/prod/device/{sn}/uplink/req` 上报；云端在 `downlink/resp` 用相同 `requestId`、`method` 回复 `code: 0`。未确认的最新状态每 10 秒重发，终态确认后停止；`ota.getStatus` 可查询当前持久化任务。下载/校验期间支持取消，安装及核对阶段拒绝取消，暂停暂不支持。

电量和充电状态统一放在 `orin/prod/device/{sn}/recording/summary` 的 `battery` 内：`valid`、`soc`、`charging`。查询失败时值为 `null`，不伪造 0%；在线/离线仍由 MQTT 判断。

外接供电测试机 `8L6AR120005` 在 OTA 电量检查中按充电处理。匹配依据是 SDK 返回的完整设备编号，监控仍上报 SDK 的实际读数。

查看日志：`./head-cam-service tail -f /mnt/cdata/logs/monitor.log`。发布文件预算为 5.5 MB，升级包只放 `/tmp`，不写入空间紧张的安装分区。现有服务自更新仍要求安装分区能同时容纳新旧服务文件，空间不足会明确拒绝更新。

### 上传成功后删除本地文件

App 提供“上传成功后删除设备本地文件”开关，默认关闭。目录上传和所选文件上传均可携带可选布尔参数 `delete_after_upload`：

```json
{"path":"/mnt/sd/sd0/采集目录","delete_after_upload":true}
```

- 不传或传 `false` 保留本地文件；仅接受 JSON 布尔值，字符串、数字和 null 返回 400。
- `true` 仅删除 SHA256 校验一致且 metadata 写入成功的文件，包括远端已存在且内容一致的 skipped 文件。空文件、内容 unknown、校验不一致、上传或 metadata 失败的文件均保留。不删除目录。
- 提交响应与任务查询返回 `delete_after_upload`；逐文件结果返回 `local_deleted`。删除失败时该文件记 failed，返回 error，可重新提交补做；远端已上传的对象保留。历史任务可能缺少新字段，App 按 false 处理。
- 同目录、同文件范围且删除选项相同的活动任务才视为重复提交；删除选项不同返回 409，不修改进行中的任务。
- App 保存和重试任务时同时保存并传回该选项；根据 `local_deleted` 显示“已删除本地文件”并刷新文件列表。上传期间仍不得修改文件或目录。
