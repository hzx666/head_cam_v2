# 缓存目录上传服务

客户端提交缓存目录的完整 `path`，服务递归扫描数据、按文件名解析身份、上传 OSS 并写入元数据头。独立于 `D:\develop\data-record-upload` 运行，不需要配置 JSON 中的 `source_dir`，也没有 GUI 或交互确认。

## Linux x86_64 单文件构建与部署

`build_release.sh` 使用 Nuitka 将上传服务编译成单文件程序，包含 Python 运行时、MCAP/HDF5/OSS SDK 等依赖和 ossutil。目标设备无需安装 Python、pip 或 ossutil。API、task 名称解析和 SHA256 校验沿用当前源码。

### 构建

构建环境需要 Linux x86_64、Python 3.10+、pip、GCC 和 Python 开发头文件，以及下载依赖的网络访问。Ubuntu 构建机可先安装：

```bash
sudo apt-get update
sudo apt-get install -y python3 python3-pip python3-dev build-essential
cd /path/to/new-upload
bash build_release.sh
```

Windows 电脑可在已安装的 WSL Ubuntu 中构建：

```powershell
wsl -d Ubuntu-22.04 -- bash /mnt/d/new-upload/build_release.sh
```

脚本使用独立的临时 Python 环境，固定 Nuitka 和辅助工具版本；应用依赖按 `requirements.txt` 安装。构建成功后记录实际依赖版本，退出时清理临时依赖、源码副本、编译缓存和测试数据。不会修改系统 Python 的已安装包。默认两个编译进程，可用 `BUILD_JOBS=4 bash build_release.sh` 调整。

ossutil 版本取自 `upload_core.py` 的 `OSSUTIL_VERSION`。默认从官方地址下载，也可指定已有的 Linux x86_64 可执行文件：

```bash
OSSUTIL_BIN=/path/to/ossutil64 bash build_release.sh
```

构建产物：

| 文件 | 用途 |
| --- | --- |
| dist/upload-service | 单文件服务，设备端实际需要的程序 |
| dist/SHA256SUMS | 成品文件完整性校验值，与数据上传 SHA256 无关 |
| dist/build-info.txt | 构建架构、时间、Python 与 glibc 版本 |
| dist/build-dependencies.txt | 构建时使用的依赖版本 |

脚本会执行离线自检及 HTTP 提交/查询验证，通过后才更新成品文件。验证不会访问真实 OSS。

### 设备端运行

将 `upload-service` 和 `SHA256SUMS` 放入设备同一个目录，例如 `/opt/upload-service/`：

```bash
cd /opt/upload-service
sha256sum -c SHA256SUMS
chmod +x upload-service
./upload-service --self-check

unset NEW_UPLOAD_TOKEN
./upload-service --host 0.0.0.0 --port 8000
```

设备启动时无需预设数据目录，由客户端在每次请求中通过 `path` 指定设备上的实际目录，服务进程需有读取权限。`--allowed-root` 仅用于可选的目录访问限制，默认不设置。默认运行记录保存在可执行文件旁边的 `runtime/`，不写入一次性解包目录，也不随当前工作目录变化。如果安装目录不可写，可指定运行目录：

```bash
./upload-service --host 0.0.0.0 --port 8000 \
  --runtime-dir /path/to/writable/upload-runtime
```

客户端仍调用 `POST http://设备IP:8000/upload` 并传入设备目录，随后调用 `GET /upload/{task_id}` 查询。未设置 `NEW_UPLOAD_TOKEN` 时不需要接口密码。

`--self-check` 检查 MCAP/HDF5 读写依赖、内嵌凭证能否解密及内嵌 ossutil 能否启动，不验证 OSS 权限或网络，不输出 AK/SK，不留下采样文件。源码模式下该选项也要求存在内嵌 ossutil；通常用于成品部署检查。

### 兼容范围

需要在 Linux x86_64 上构建，Windows 可通过 WSL 执行。Linux 系统基础库不等同于 Python 依赖：成品仍依赖目标系统的 glibc 等基础库。当前构建环境为 Ubuntu 22.04 / glibc 2.35，建议部署到同版本系统，或在目标设备支持的最旧发行版上重新构建。不保证运行于旧于该基线的系统或 Alpine/musl 系统，也不适用于 ARM64。

单文件启动时会将依赖解包到临时目录，因此设备需要可写、允许执行的临时目录。若系统 `/tmp` 禁止执行，可将 `TMPDIR` 指向满足条件且当前用户可写的目录后启动。

### 凭证处理边界

AK/SK 保留当前加密内嵌方式，没有替换凭证，也没有引入 STS 服务。Nuitka 编译业务代码以提高还原门槛；设备端不需要分发 `.py` 源码。服务生成的 ossutil 凭证配置在 Linux 上限制为 `0600`，正常结束任务后删除，不通过接口返回凭证。

这不是不可逆的密钥保护：能控制设备或进程的人仍可能提取凭证。设备暂存的配置在进程被强制终止时也可能来不及清理。二进制及相关运行目录应按含凭证的软件管理。

参考：[Nuitka 独立程序与单文件模式](https://nuitka.net/user-documentation/user-manual.html)、[Nuitka 内嵌可执行文件配置](https://nuitka.net/user-documentation/nuitka-package-config.html)。

## 源码运行（开发调试）

直接运行源码时需安装 `requirements.txt`，并使用同一个 Python 解释器启动服务。在 PowerShell 中运行：

```powershell
cd D:\new-upload
$pythonExe = (Get-Command python -ErrorAction Stop).Source
& $pythonExe -m pip install -r requirements.txt
.\start.ps1 -PythonExe $pythonExe
```

启动脚本依次选择本目录 `.venv`、本机 Codex 自带 Python、PATH 中的 Python。也可以指定：

```powershell
.\start.ps1 -PythonExe 'C:\Python312\python.exe' -BindHost 0.0.0.0 -Port 8000
```

默认只监听 `127.0.0.1:8000`。其他机器调用时使用 `-BindHost 0.0.0.0`，客户端地址填写上传服务器 IP。按实际部署放行端口。

在其他机器直接运行源码：使用 Python 3.10+（建议 3.12），安装 `requirements.txt`，执行 `python upload_server.py --host 0.0.0.0 --port 8000`。源码模式缺少内嵌 ossutil 时首次使用会下载对应平台版本；单文件成品已内嵌 Linux x86_64 ossutil。

可选限制访问范围和调用者：

```powershell
$env:NEW_UPLOAD_TOKEN = '自行设置的访问令牌'
.\start.ps1 -BindHost 0.0.0.0 -AllowedRoot 'D:\data-cache'
```

设置令牌后所有接口都需要 `Authorization: Bearer <令牌>`。未设置时不启用鉴权。OSS 凭证及 bucket/endpoint 沿用现有上传核心。

## 接口

### 提交上传

```http
POST /upload
Content-Type: application/json

{"path":"D:\\data-cache\\20260924"}
```

请求体必须且只能包含 `path`。该路径就是缓存目录本身，不额外拼接 `cache`。路径必须能被**运行服务的机器**访问；支持其本地绝对路径和有读取权限的共享目录路径。客户端应在数据写入完成后提交目录。

返回 HTTP 202：

```json
{
  "accepted": true,
  "duplicate": false,
  "task_id": "生成的任务ID",
  "status": "queued",
  "status_url": "/upload/生成的任务ID",
  "message": "已接收上传任务"
}
```

`accepted=true` 表示任务已接收，**不表示数据已上传成功**。状态可能已经进入 `scanning`。递归扫描和上传在后台执行，不阻塞本次请求。相同目录尚未结束时再次提交，返回 HTTP 200、`duplicate=true` 和原任务 ID；父子目录重叠返回 409。任务已结束后再次提交会创建新任务，可用于失败重试。

路径无效返回 400，不可读或超出允许根目录返回 403，令牌无效返回 401。错误响应包含 `accepted=false` 和 `error`。最多接受 100 个未完成任务，队列满返回 503。

### 查询结果

`GET /upload/{task_id}` 返回任务和逐文件结果：

```json
{
  "task_id": "任务ID",
  "path": "D:\\data-cache\\20260924",
  "status": "completed",
  "total": 3,
  "processed": 3,
  "uploaded": 1,
  "skipped": 1,
  "empty": 1,
  "failed": 0,
  "ignored": 2,
  "results": []
}
```

示例省略了 `results` 明细，实际每条包含 `path`、`file`、`status`、`ok`，成功时包含 `ossPath`、`duration`、`size`，失败时包含 `error`。`ignored` 是不作为主数据处理的文件数，可能包括已随主数据上传的 JSON/YAML 附件。

通过 `GET /upload/{task_id}` 的逐文件 `results` 读取 SHA256 校验结果；提交接口仍异步返回任务 ID。有效非空主数据文件完成上传或同名检查后，包含以下字段：

| 字段 | 含义 |
| --- | --- |
| sha256 | 本地文件的 SHA256，小写十六进制 |
| remote_sha256 | 远端保存的 SHA256；缺失时为 null |
| already_exists | 上传前目标 OSS 路径是否已有对象 |
| same_content | true 一致、false 不一致、null 无法确认 |
| checksum_status | match / mismatch / unknown |

新上传文件会将指纹保存到 `x-oss-meta-sha256`，原有 ETag 保留。上传前后计算本地 SHA256，检测上传期间文件变化。远端指纹来自上传源文件及保存的元数据，不是下载远端文件重新计算；等待任务完成前请勿修改本地文件。

同名判断以完整 OSS 路径为准：指纹一致时跳过主文件上传并更新元数据；不一致时返回 `status=failed`，不覆盖对象或更新附件。历史对象没有 SHA256 时返回 `status=skipped`、`checksum_status=unknown`，不修改对象。客户端不能仅凭 skipped 判定内容一致，应检查 `ok` 和 `same_content`。空文件及前置解析失败的结果不包含校验字段。

| 状态 | 含义 |
| --- | --- |
| queued / scanning / uploading | 排队 / 扫描 / 上传中 |
| completed | 全部处理结束，无失败；空文件单独计数，不上传 |
| partial_failed | 部分成功、部分失败 |
| failed | 任务异常或文件上传失败且没有成功项 |
| no_data | 未扫描到支持的主数据文件 |
| interrupted | 上次进程退出时任务未完成，重新提交 path 重试 |

`GET /health` 用于检查 HTTP 服务是否运行，不验证 OSS 连通性。

PowerShell 调用示例：

```powershell
$body = @{ path = 'D:\data-cache\20260924' } | ConvertTo-Json
$task = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8000/upload' -ContentType 'application/json' -Body $body
Invoke-RestMethod -Uri "http://127.0.0.1:8000/upload/$($task.task_id)"
```

## 数据规则

- 递归扫描 `.mcap`、`.mcap.incomplete`、`.hdf5`、`.h5`、`.parquet`，扩展名不区分大小写。跳过符号链接、目录联接以及服务 runtime 目录。
- 支持旧版 `m1_{设备}_camera_{YYYYMMDD}_...`、头戴相机 `SSIEGO_Cam_{设备}_{YYYYMMDD}_...`、通用 `{品牌}_{机器}_{设备}_{YYYYMMDD}_{任务}_{序号}` 和新版 `{品牌}_{机器}_{设备}_{任务}_{YYYYMMDD}_{HHMMSS}_{4位十六进制码}_{切片号}`。
- 无法解析的主数据文件逐条记失败，不静默丢弃。`episode_0.hdf5` 这类缺少身份信息的文件名不能通过仅传 path 的接口上传。
- 自动识别头戴相机和遥操作数据。`m1/M1` 强制走 Orin；其他品牌沿用文件名中的品牌拼写和大写机器名生成路径及设备标识，包含 Astribot。
- Astribot HDF5/H5 从时间戳读取时长；MCAP 读取消息时间及最终 `episode_quality`；Parquet 沿用原逻辑，时长为 0，不解析内容。
- 同级 YAML/YML/JSON 作为附件上传；MCAP 切片共享附件目录，附件关联写入 `x-oss-meta-relatedfiles`；保留原有 UUID、持久缓存及 OSS 关联恢复规则。
- 主文件、附件、元数据头都成功才计为成功。OSS 已存在且 SHA256 一致时跳过主文件重传，仍处理附件并刷新元数据；不一致时报错，缺少 SHA256 时返回 unknown 并跳过，不修改对象。OSS 状态查询失败不会当作对象不存在而覆盖上传。
- 默认上传文件名中的有效任务名。元数据头包括原有路径、日期、设备、格式、大小、时长等字段，以及 task/quality/relatedfiles。MCAP/HDF5 完成后通过 MNS 通知预览和质检；原有 OSS 元数据协议保持兼容。
- 所有上传任务串行执行。一次扫描中多个本地文件映射到同一 OSS key 时，这些文件均记失败，避免相互覆盖。
- 本地数据及附件均保留。不提供 MCAP 自动修复：损坏 MCAP 解析失败后记为失败；`.mcap.incomplete` 记为未完成，不上传、不修复。已有对应完整 MCAP 时只处理完整文件。单文件失败不影响后续文件处理。无需部署 `mcap_recover` 或 `mcap_repair.py`。

## 文件及运行状态

- `upload_server.py`：HTTP 接口、任务队列、递归扫描、状态存储。
- `upload_core.py`：从 develop 上传脚本独立提取的核心，保留上传和元数据协议，移除 GUI/交互入口。
- `mns_notifier.py`：MNS 发送、本地持久化、独立重试与去重。
- `mcap_parser.py`、`hdf5_parser.py`：数据解析模块。
- `runtime/tasks/`：任务 JSON，重启后可继续查询；未完成任务标记 interrupted，不自动继续。
- `runtime/logs/`：上传日志。HTTP 访问日志输出到终端。
- `runtime/attachments/`：附件关联缓存。
- `runtime/mns/`：按 OSS 地址和 SHA256 保存两种事件的发送记录，重启恢复待发送消息；已发送记录用于去重，不应随意删除。

`--runtime-dir` 可指定状态目录。相同 runtime 目录只允许一个服务进程；使用不同 runtime 目录的多个进程不要处理同一批数据。Ctrl+C 会停止接收新请求并等待已接收的任务结束；强制退出后请重新提交未完成目录。任务历史暂不自动清理。

## MNS 预览和质检通知

安装更新后的 `requirements.txt`（新增 `aliyun-mns-sdk==1.3.0`）；单文件部署需要重新运行 `build_release.sh`，已有二进制不会自动更新。构建脚本已包含通知模块，离线自检也会检查 MNS SDK。

固定使用公网 Endpoint `https://1766661213327930.mns.cn-shanghai.aliyuncs.com` 和队列 `data-platfrom-event`，复用现有 OSS AK/SK。账号需要该队列的 SendMessage 权限。服务不创建队列，也不消费消息。

`.mcap`、`.hdf5`、`.h5`（大小写不敏感）的主文件、附件及元数据全部成功后，每个文件分别发送以下两条消息，消息体为原始 JSON，不额外做 Base64 编码：

```json
{"event":"mcap_extract_preview","code":200,"msg":"","traceId":"","data":{"ossPath":"oss://ss-oss-data-platform/original_data/..."}}
{"event":"mcap_qc","code":200,"msg":"","traceId":"","data":{"ossPath":"oss://ss-oss-data-platform/original_data/..."}}
```

HDF5 沿用这两个事件名。Parquet、空文件、未完成文件、失败文件及历史对象 SHA256 未知的 skipped 结果不发送。已存在且 SHA256 一致、附件及元数据处理成功的文件会补建消息记录；相同 OSS 地址和 SHA256 已发送成功的事件不会因重新提交目录而再次发送。

消息在发送前保存到本地，后台逐条发送。失败后按 5、10、20 秒等退避重试，最大间隔 300 秒；一条失败不阻止另一条发送。MNS 失败不更改 OSS 上传结果、不重新上传文件。退出时保留待发送记录，下次启动继续；旧版本任务历史不会自动投递。若文件处理完成但尚未保存任务结果时进程被强制终止，需要按原流程重新提交 interrupted 目录。消息本地落盘失败会使任务报告异常，已保存的通知意图在重启时恢复。

`GET /upload/{task_id}` 新增任务级 `mns_status`：`not_required`、`pending`、`sent`。需要通知的逐文件结果包含 `mns_required=true`、`mns_status` 及 `mns_events`，分别展示两个事件的 `status`、`attempts`、`error`、`message_id`、`next_attempt`（Unix 秒）。查询时读取最新发送状态；`completed` 仍表示文件处理完成，确认通知全部发出还要检查 `mns_status=sent`。`sent` 表示 MNS 已接收，不代表下游预览或质检已完成。

发送成功但响应丢失或本地状态未保存时仍可能重复投递，下游应按 `event + ossPath` 幂等处理。离线测试不会向真实 OSS/MNS 发送数据：

```bash
python -m unittest -v test_mns_notifier
```

SDK 接入参考：[阿里云 Python SDK](https://www.alibabacloud.com/help/en/mns/developer-reference/python-foreword)。

