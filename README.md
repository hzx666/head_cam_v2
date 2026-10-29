# 头环 MCAP 上传服务（C / ARM64 Linux）

手机 App 浏览头环目录、选择目录后提交任务。C 服务递归扫描 MCAP，解析头环文件名，校验 SHA256，上传 OSS 并写入 metadata。接口和任务管理参考 teleop，头环命名及 metadata 参考 develop 的 head_cam。

**没有附件处理，没有 MNS 通知，不自动修复或删除本地 MCAP。**

## 单文件部署

运行产物是 `dist/head-cam-upload`，一个 ARM64 Linux 静态可执行文件，内嵌 ossutil、CA 证书与加密凭证。设备无需 Python、额外 ossutil 或明文 AK/SK 配置。`SHA256SUMS`、`build-info.txt` 是校验和构建说明，不是运行依赖。

```sh
cd /mnt/config/oss_upload
chmod 700 head-cam-upload
./head-cam-upload --self-check
./head-cam-upload --host 0.0.0.0 --port 8000
```

执行二进制就启动 HTTP 服务，App 连接 `http://头环IP:8000`。开机自启动由设备环境按需配置，当前代码不修改设备启动项。`--self-check` 检查内嵌凭证初始化和 ossutil 启动，不访问 OSS，也不打印凭证。

默认运行目录为可执行文件旁的 `runtime/`，可通过 `--runtime-dir /可写路径/runtime` 指定（父目录必须存在）。运行目录需要可写且允许执行，用于释放内嵌依赖。设备还需可用的 `/proc` 和可写 `/tmp`。静态 ELF 的 Linux ABI 基线为 3.7，仍需在实际头环系统验证兼容性。

可选 `--allowed-root /mnt/sd/sd0` 限制浏览与上传范围。默认不限制目录根路径，初始浏览页列出自动识别的 TF 卡；配置后初始页列出该允许根目录。

设置环境变量 `HEAD_CAM_UPLOAD_TOKEN` 后，所有接口需 `Authorization: Bearer <令牌>`；未设置则不启用鉴权。

## App 接口

详细请求、响应及目录选择流程见 [docs/API.md](docs/API.md)。

| 接口 | 用途 |
| --- | --- |
| `GET /health` | 服务就绪检查，不验证 OSS 连通性 |
| `GET /directories` | TF 卡或允许根目录列表 |
| `GET /directories?path=...&offset=0` | 浏览子目录，每页最多 200 项 |
| `POST /upload` | 提交 `{"path":"/mnt/sd/sd0/采集目录"}` |
| `GET /upload/{task_id}` | 查询进度和逐文件结果 |

App 点击目录进入下一层，点击“上传当前目录”时提交返回的 path，无需用户输入路径。路径属于头环，不是手机。仓库提供设备接口，App 页面需在 App 工程中接入。

POST 立即返回任务 ID，后台逐个上传；`accepted=true` 不表示上传完成。提交 TF 卡根目录递归处理卡内数据，提交子目录只处理该目录。无终端输入或 YES 确认。

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
- 文件和 metadata 都处理成功才计成功；metadata 失败可重新提交补写。本地数据始终保留。
- 单任务最多 10,000 个主数据文件，递归深度最多 128。需解析的单条记录及 Chunk 解压结果最多 64 MiB，超限明确报错；大文件不整体载入内存。

## OSS metadata

以下字段写成 `x-oss-meta-<字段名>`，值规则参考 develop 的 head_cam；OSS header 名称不区分大小写。

| 字段 | 取值 |
| --- | --- |
| ossPath / rootPrefix | 完整 OSS URI / original_data |
| dataDomain / dataKind | ego_human_data / head_cam |
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

后台串行执行任务，HTTP 可并发。最多 100 个未完成任务；同目录重复提交返回原任务，父子目录重叠返回 409。终态任务重新提交创建新任务。

状态：`queued/scanning/uploading` 为处理中；`completed/partial_failed/failed/no_data/interrupted` 为终态。completed 可能包含空文件或内容未知的 skipped，应展示逐文件结果。

`runtime/tasks/` 保存任务 JSON，`runtime/logs/` 保存日志。状态通过临时文件、fsync 和原子重命名落盘；保存失败后拒绝新任务。历史不自动清理。

同一个 runtime 仅允许一个服务。SIGINT/SIGTERM 停止接收请求并等待已接收任务结束；强制结束时正在运行的 ossutil 一并终止。重启后未完成任务标记 interrupted，由 App 重新提交，不自动续跑。

不要用不同 runtime 的多个服务或外部客户端同时写同一批 OSS key；远端查询和上传之间没有跨进程事务锁。ossutil 连接超时 10 秒、读取超时 30 秒、内部重试 2 次，单次进程最多 30 分钟。

## 打包与离线验证

构建说明见 [docs/BUILD.md](docs/BUILD.md)。成品加密内嵌凭证，运行时在私有 `runtime/session-*` 释放 `0600` 凭证配置，正常退出清理，异常残留下次启动时清理。AK/SK 不传入进程参数、不输出到接口或日志。二进制含解密材料，设备被控制后仍可能提取密钥。

MCAP 格式依据 [MCAP Specification](https://mcap.dev/spec)，metadata CLI 依据 [ossutil set-meta](https://www.alibabacloud.com/help/en/oss/developer-reference/set-meta)。第三方来源见 [third_party/README.md](third_party/README.md)。
