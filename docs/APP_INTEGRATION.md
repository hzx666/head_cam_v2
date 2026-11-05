# 头环上传服务：App 接入技术文档

本文面向手机 App 开发人员，依据当前服务实现整理。完整 JSON 示例另见 [API.md](API.md)。

更新时间：2026-11-02。当前支持整目录、同目录单文件及多文件上传。

设备写入 OSS 的元数据头 `x-oss-meta-dataKind` 值为 `head_cam_v2`，由服务自动设置，App 无需传入。

## 接口总览（共 5 个）

所有路径均相对于 `http://设备IP:38127`；启用令牌后全部接口携带 `Authorization: Bearer <令牌>`。

| 方法 | 接口 | 请求参数 | 用途 |
| --- | --- | --- | --- |
| GET | `/health` | 无 | 检查服务运行 |
| GET | `/directories` | 可选 query：path、offset | 初始存储位置或子目录列表 |
| GET | `/files` | 必填 query：path；可选 offset | 当前目录完整 MCAP 文件列表 |
| POST | `/upload` | JSON：path；可选 files | 创建整目录、单文件或多文件任务 |
| GET | `/upload/{task_id}` | URL 中的任务 ID | 查询进度和逐文件结果 |

目录与文件列表分别分页，每页最多 200 项，根据 next_offset 加载下一页。path 查询参数使用标准 URL 编码。

### 三种上传请求速查

以下请求均发送至 `POST /upload`，使用 `Content-Type: application/json`。

整目录上传（递归包含子目录）：

```json
{"path":"/mnt/sd/sd0/recordings"}
```

单文件上传：

```json
{"path":"/mnt/sd/sd0/recordings","files":["SSIEGO_Cam_A001_20261102_001.mcap"]}
```

多文件上传：

```json
{"path":"/mnt/sd/sd0/recordings","files":["SSIEGO_Cam_A001_20261102_001.mcap","SSIEGO_Cam_A001_20261102_002.mcap"]}
```

files 使用 `/files` 返回的 name，只处理当前目录所选文件，不递归。每批 1–100 个不重复名称，整个请求最多 16 KiB。空数组/null 无效。成功提交后保存 task_id，每 1–2 秒查询 `/upload/{task_id}`，直到终态；提交成功不等于上传完成。

## 1. 接入约定

| 项目 | 约定 |
| --- | --- |
| Base URL | `http://<头环设备IP>:38127`，例如 `http://192.168.1.10:38127` |
| 协议 | HTTP，响应为 UTF-8 JSON |
| 目录路径 | 头环设备上的绝对路径，使用接口返回的 `path` |
| 上传方式 | App 提交目录或目录及所选文件名，设备读取本地 MCAP 并上传 OSS；App 不传文件内容 |
| 鉴权 | 设备配置 `HEAD_CAM_UPLOAD_TOKEN` 后，所有接口携带 `Authorization: Bearer <令牌>` |
| 云端凭证 | App 不需要配置 OSS AK/SK |

服务必须已经在设备运行；只复制 dist 不会自动启动。直接执行 `./head-cam-upload` 默认监听 `0.0.0.0:38127`，App 连接时使用设备实际 IP，不能填 `0.0.0.0`。开机自启动由设备侧配置。

手机与设备需网络可达，设备需能够访问 OSS。App 应按自身平台配置局域网访问和明文 HTTP 访问权限；当前服务不提供 HTTPS。

接口适用于原生 App HTTP 客户端。当前服务没有实现 CORS 响应或 OPTIONS 预检；WebView/H5 接入需使用原生网络桥接，或另行适配服务。

## 2. 推荐页面与调用顺序

1. 进入上传页面，调用 `GET /health`。失败则展示“设备未连接或上传服务未启动”，提供重试。
2. 调用 `GET /directories`，展示设备存储位置。
3. 点击存储位置或子目录，使用其 `path` 调用 `/directories` 展示子目录，并调用 `/files` 展示当前层文件。
4. 文件旁提供复选框，支持单选、多选和取消选择；“上传所选文件（N）”在 N=0 时禁用。切换目录清空选择。保留独立的“上传整个目录”按钮，仅在 `selectable=true` 时允许提交。
5. 点击上传后确认范围，调用 `POST /upload`：整目录仅传 path，所选文件传 path 和 files。请求期间禁用按钮，避免连续点击。
6. 收到 HTTP 200 或 202 且 `accepted=true` 后保存任务信息，进入任务页。
7. 每 1–2 秒调用 `GET /upload/{task_id}`，直到终态。
8. 展示汇总与逐文件结果；失败或中断时允许用户重新提交原目录及原文件选择。

整目录上传会递归处理子目录；所选文件上传只处理明确勾选的文件。提交前提示用户确认采集已经结束，上传期间不要修改文件。

## 3. 服务检查

```http
GET /health HTTP/1.1
Host: 192.168.1.10:38127
```

HTTP 200：

```json
{"status":"ok","version":"1.0.0"}
```

此接口只表示服务可响应，不表示 OSS 网络、权限或真实上传已验证。

## 4. 浏览设备目录

### 4.1 获取初始存储位置

```http
GET /directories
```

```json
{
  "path":"",
  "parent":null,
  "selectable":false,
  "directories":[{"name":"/mnt/sd/sd0","path":"/mnt/sd/sd0"}],
  "total":1,
  "next_offset":null
}
```

这是虚拟首页，不能上传。默认列出识别到的 TF 卡；设备设置 `--allowed-root` 时列出该允许根目录。无存储位置时列表为空，可能附带 `message`；页面显示空态并提供刷新。

### 4.2 进入目录和分页

```http
GET /directories?path=%2Fmnt%2Fsd%2Fsd0&offset=0
```

```json
{
  "path":"/mnt/sd/sd0",
  "parent":"/mnt/sd",
  "selectable":true,
  "mcap_files":2,
  "directories":[{"name":"recordings","path":"/mnt/sd/sd0/recordings"}],
  "total":1,
  "next_offset":null
}
```

| 字段 | App 使用方式 |
| --- | --- |
| `path` | 当前目录的规范路径，用于提交上传及继续分页 |
| `parent` | 服务返回的上级目录路径；null 时禁用服务目录的“上一级”操作 |
| `selectable` | 是否允许上传当前目录 |
| `directories` | 子目录列表；展示 name，点击时使用 path |
| `total` | 当前目录下子目录总数，不是上传文件数 |
| `next_offset` | 非 null 时，使用同一 path 和该 offset 请求下一页并追加列表 |
| `mcap_files` | 当前层 `.mcap` 文件数量，不含子目录，不代表递归任务总数；虚拟首页无此字段 |

每页最多 200 个子目录。切换目录后清空旧分页结果，防止迟到响应覆盖当前页面。使用 HTTP 库的 query 参数编码，中文、空格、`&`、`+`、`#` 等字符不要手工拼接或重复编码。

上传按钮使用当前响应的 `path`，不要根据显示名称自行拼路径。`mcap_files=0` 时仍可上传，因为子目录可能有文件。App 可单独提供“返回存储位置”调用不带 path 的目录接口；默认配置下 TF 卡目录返回的 parent 可能位于 TF 卡之外，服务仅在配置 allowed-root 后限制浏览边界。

## 5. 提交上传任务

```http
POST /upload
Content-Type: application/json

{"path":"/mnt/sd/sd0/recordings"}
```

请求体包含字符串字段 `path`，可选 `files` 文件名数组，最多 16 KiB。路径必须是设备上存在且可读的绝对目录。除可选 files 外，不要增加设备号、OSS 配置等字段，不要使用 multipart 文件上传。

新任务返回 HTTP 202：

```json
{
  "accepted":true,
  "duplicate":false,
  "task_id":"83b31f8409844ff1a22a7ca470e6aa12",
  "status":"queued",
  "status_url":"/upload/83b31f8409844ff1a22a7ca470e6aa12"
}
```

- `accepted=true`：仅表示任务已接收，页面显示“已提交”或“排队中”。
- `status_url`：相对路径，拼接当前设备 Base URL 查询。
- 同一目录且所选范围相同的未完成任务：HTTP 200、`duplicate=true`，返回原 task_id，继续查询该任务。
- 与未完成任务存在父子目录重叠：HTTP 409，提示等待已有任务完成。
- 已终结任务再次提交同一目录：创建新任务。已存在的云端文件由设备判断是否跳过或报错。

任务由设备串行处理，最多 100 个未完成任务；App 同时创建多个任务不代表并行上传。

## 6. 查询进度与结果

```http
GET /upload/83b31f8409844ff1a22a7ca470e6aa12
```

HTTP 200 示例：

```json
{
  "task_id":"83b31f8409844ff1a22a7ca470e6aa12",
  "path":"/mnt/sd/sd0/recordings",
  "status":"completed",
  "created_at":"2026-10-27T03:00:00Z",
  "updated_at":"2026-10-27T03:00:10Z",
  "started_at":"2026-10-27T03:00:00Z",
  "finished_at":"2026-10-27T03:00:10Z",
  "total":1,
  "processed":1,
  "uploaded":1,
  "skipped":0,
  "empty":0,
  "failed":0,
  "ignored":0,
  "results":[{
    "path":"/mnt/sd/sd0/recordings/SSIEGO_Cam_A001_20261027_001.mcap",
    "file":"SSIEGO_Cam_A001_20261027_001.mcap",
    "status":"uploaded",
    "ok":true,
    "size":1048576,
    "duration":12.345,
    "ossPath":"oss://<bucket>/original_data/ego_human_data/head_cam/261027/A001_device/SSIEGO_Cam_A001_20261027_001.mcap",
    "sha256":"<64位小写十六进制>",
    "remote_sha256":"<64位小写十六进制>",
    "already_exists":false,
    "same_content":true,
    "checksum_status":"match"
  }]
}
```

### 6.1 任务状态与页面文案

| status | 建议展示 | 是否停止轮询 |
| --- | --- | --- |
| `queued` | 排队中 | 否 |
| `scanning` | 正在扫描文件 | 否 |
| `uploading` | 正在处理文件 | 否 |
| `completed` | 处理完成，并展示汇总和明细 | 是 |
| `partial_failed` | 部分失败，可查看失败文件 | 是 |
| `failed` | 任务失败，展示任务 error 和文件错误 | 是 |
| `no_data` | 未发现可处理的 MCAP 文件 | 是 |
| `interrupted` | 任务已中断，可重新提交 | 是 |

不要只凭 `processed==total` 判断结束；以 status 是否为终态为准。扫描时 total 尚未确定，显示不定进度；处理时 total 大于 0 才显示 `processed / total`。processed/total 仅为文件数量；新版字节进度使用 progress_percent 和 current_file.progress_percent，详见第 12 节。尚无实时速度或剩余时间字段。

| 统计字段 | 含义 |
| --- | --- |
| `total` | 本任务扫描得到的主数据文件数，扫描完成后确定 |
| `processed` | 已完成处理的文件数 |
| `uploaded` | 文件上传及 metadata 处理成功的数量 |
| `skipped` | 已存在而跳过重传的数量，需结合校验字段展示 |
| `empty` | 空文件数量，不上传 |
| `failed` | 文件处理失败数量 |
| `ignored` | 扫描时忽略的条目数，可能包含目录，不纳入处理进度 |
| `results` | 已产生的文件结果；每次查询返回当前完整结果，App 应替换或按 path 更新，不能整批重复追加 |

时间字段为 UTC ISO 时间，App 按本地时区展示。started_at、finished_at、error 等字段依阶段存在，解析时允许缺省。

### 6.2 文件结果展示

| 文件 status / 条件 | 建议展示 |
| --- | --- |
| `uploaded` | 上传成功 |
| `skipped` 且 `checksum_status=match` | 云端已存在，校验一致，已跳过重传 |
| `skipped` 且 `checksum_status=unknown` | 云端已存在，缺少校验信息，已跳过 |
| `empty` | 空文件，未上传 |
| `failed` | 处理失败，展示 error |

`same_content` 可为 true、false 或 null。unknown/null 不能展示成“校验一致”。空文件、文件名前置校验失败等结果可能不包含 size、duration、ossPath 或校验字段。字段缺省时隐藏对应信息。

completed 不代表所有文件都新上传成功，也可能包含空文件和缺少校验信息的跳过项。建议结果页统一展示“上传 X、跳过 Y、空文件 Z、失败 N”。

## 7. 轮询、断线恢复和重试

- 每个任务使用单一轮询循环，上一次请求结束后再等待 1–2 秒发起下一次，避免并发堆积。
- 短暂断网或查询超时，保留 task_id，显示“连接中断，正在重连”；不能直接把设备任务判为失败。
- 查询失败可逐步延长重试间隔，例如 2、4、8 秒，最高 15 秒；这些是 App 建议策略，并非服务协议要求。
- 本地保存设备标识、Base URL、task_id、path。返回页面或 App 重启后，用对应设备的 task_id 恢复查询。
- POST 超时不代表任务未创建。可重新提交同一 path；若原任务仍在进行，服务返回原任务。若原任务已经结束，则会创建新任务，因此不要将此当作永久幂等机制。
- 服务重启后，未结束任务标记为 interrupted，不会自动续跑。由用户重新提交原目录及原文件选择。
- 已知任务查询返回 404 时提示“任务不存在，请确认连接的是原设备及服务数据是否保留”，不要无条件自动重传。
- 退出页面仅停止 App 轮询，设备任务继续；当前没有取消、暂停或删除任务接口。
- 当前没有任务列表接口，App 需自行保留已创建任务的 ID。

## 8. 错误处理

错误体示例：

```json
{"accepted":false,"error":"Directory overlaps an active upload task"}
```

| HTTP 状态码 | 含义与 App 处理 |
| --- | --- |
| 400 | 参数、JSON 或路径无效；检查请求，刷新目录，不盲目重试 |
| 401 | 令牌缺失或错误；检查 Bearer 配置，包含 health 请求 |
| 403 | 目录不可读或超出允许范围；提示更换目录或联系设备侧 |
| 404 | 接口或任务不存在；核对地址、路径、设备身份 |
| 408 | 服务读取请求超时；检查网络，按请求类型处理重试 |
| 409 | 目录与活动任务重叠；等待已有任务结束 |
| 413 | 请求体超过 16 KiB；检查请求构造 |
| 415 | Content-Type 非 application/json；修正请求头 |
| 417 | 请求携带不支持的 Expect；不要发送该头 |
| 431 | 请求头过大；精简请求头 |
| 500 | 服务内部异常；记录 error，提示稍后重试或排查设备 |
| 503 | 队列/连接已满、停止中、内存不足或任务持久化异常；展示错误并适当退避，持续失败联系设备侧 |

HTTP 200 的任务查询也可能返回 `status=failed`，HTTP 成功和业务成功要分别判断。error 是英文诊断信息，App 可展示中文概述并保留原文供排查，不建议按错误字符串进行业务分支。

服务不支持 Transfer-Encoding 分块请求。POST 使用可确定长度的 JSON 请求体，由 HTTP 客户端生成 Content-Length。每次响应后服务关闭连接，客户端应允许自动重建连接。

## 9. 联调命令

以下命令在能访问设备的开发机执行，把 IP、目录和任务 ID 替换为实际值。启用鉴权时，每条命令均添加 `-H 'Authorization: Bearer <令牌>'`。

```sh
curl 'http://192.168.1.10:38127/health'

curl 'http://192.168.1.10:38127/directories'

curl --get 'http://192.168.1.10:38127/directories' \
  --data-urlencode 'path=/mnt/sd/sd0' --data-urlencode 'offset=0'

curl 'http://192.168.1.10:38127/upload' \
  -H 'Content-Type: application/json' \
  --data '{"path":"/mnt/sd/sd0/recordings"}'

curl 'http://192.168.1.10:38127/upload/<task_id>'
```

## 10. App 联调验收清单

- 正常连接、设备离线、服务未启动时页面反馈正确。
- 开启令牌后，所有接口均携带正确鉴权头。
- 能进入目录、返回、分页，中文和特殊字符路径可正常请求。
- 虚拟首页不能上传；当前层无 MCAP 时仍允许提交有子目录的目录。
- 新任务 202 和重复任务 200 均能进入同一套查询流程；409 有明确提示。
- queued、scanning、uploading 及所有终态均能正确展示并停止/继续轮询。
- 能展示逐文件失败、空文件、已存在且校验一致、已存在但校验未知等结果。
- App 退出重进可恢复查询；断网重连不会直接判任务失败；服务重启后显示 interrupted。
- 至少完成一次真实 MCAP 上传，核对 OSS 文件与 metadata；仅 health 成功不算上传链路验收完成。

当前无附件、MNS 通知、预览、质检事件、WebSocket、取消/暂停或任务列表接口。上传后默认保留设备本地数据，可通过 delete_after_upload 选择成功后删除；未完成的 `.mcap.incomplete` 文件不自动修复或上传。文件命名与业务规则见 [README.md](../README.md)。

## 11. 新增：目录内单文件和多文件上传

进入目录后，调用 `GET /files?path=<URL编码的目录>&offset=0` 获取当前层文件，目录导航仍调用 `/directories`。返回的 files 数组包含 name、path、size；total 为文件总数，next_offset 非 null 时继续加载，每页最多 200 项。仅列出普通完整 `.mcap`，不含符号链接和 `.mcap.incomplete`。

文件旁放复选框，允许单选、多选与取消选择，底部展示“已选 N 个”和“上传所选文件（N）”；未勾选时禁用。切换目录清空勾选，分页追加保留选择；“全选”注明仅全选已加载文件。保留单独的“上传整个目录”操作，确认页面明确范围。

```json
{"path":"/mnt/sd/sd0/recordings","files":["SSIEGO_Cam_A001_20261102_001.mcap","SSIEGO_Cam_A001_20261102_002.mcap"]}
```

上例提交至 `POST /upload`，仅上传这两个文件，不递归。一个文件时数组只放一个 name。files 限 1–100 个不重复文件名且 JSON 总体最多 16 KiB；不可传完整文件路径或子目录路径。空数组、null、缺失文件或符号链接返回 400，整个请求不创建任务。列表文件名仍可能不符合业务规则，后台会返回逐文件错误。

不传 files 仍为整目录递归上传。HTTP 200/202、轮询和逐文件结果沿用原流程。任务查询新增 mode（files/directory），文件模式另有 files 数组；历史任务可能没有 mode。

同目录同文件集合（顺序不限）进行中重复提交返回原任务；同目录不同集合或整目录与文件任务重叠返回 409。当前不支持同目录不同选择并行提交，也不支持跨目录混选。

本地保存设备、task_id、path、files、mode。重试文件任务必须带原 files，遗漏会变成整目录上传。新增验收：单选、多选、未选、分页保留选择、切目录清空、空/null/重复/越界文件拒绝、仅处理所选文件、与整目录任务冲突、重启后恢复结果。

具体接口样例见 [API.md 的单文件与多文件选择](API.md#单文件与多文件选择)。

## 12. 单文件与整批字节进度

新版仍使用 `GET /upload/{task_id}` 轮询，无新增接口。App 必须将进度条的数据源从 processed/total 改为返回的 progress_percent；文件计数可作为辅助文字。

```json
{
  "status":"uploading",
  "phase":"transferring",
  "bytes_total_known":true,
  "bytes_total":200000000,
  "bytes_done":125000000,
  "bytes_uploaded":125000000,
  "progress_percent":62.5,
  "current_file":{
    "path":"/mnt/sd/sd0/recordings/SSIEGO_Cam_A001_20261102_002.mcap",
    "file":"SSIEGO_Cam_A001_20261102_002.mcap",
    "phase":"transferring",
    "bytes_total":100000000,
    "bytes_uploaded":25000000,
    "progress_percent":25
  }
}
```

以上为示意片段，实际还包含 task_id、文件计数和 results 等原字段。

- 整批进度条：顶层 progress_percent；当前文件进度条：current_file.progress_percent。整数文字向下取整，不把 99.9 四舍五入成 100。
- bytes_total_known=false 时尚在排队/扫描，显示不定进度；为 true 后 bytes_total 是扫描得到的文件总大小。
- bytes_uploaded 是工具报告的有效上传/续传字节，bytes_done 还包含跳过文件对应的已处理工作量；不要把 bytes_done 标注为“成功上传字节”。这些值不代表重试消耗的网络流量。
- phase=preparing：正在解析文件并计算校验；checking_remote：检查云端；transferring：正在传输；verifying：正在校验；metadata：正在写入元信息。
- 处理中 progress_percent 最高 99.9；单文件传输结束后仍需校验和 metadata，任务 completed 才展示整批 100% 和最终结果。失败/中断即使已有传输字节，也以 status 显示失败/中断。
- 小文件可能在一次轮询间隔内完成，进度可跳变；重试或断点恢复可能使字节回退或跳跃，不要在客户端伪造平滑递增的进度。校验阶段字节不增长时展示阶段文字。
- 工具通常约每 5 秒报告一次字节进度，App 仍可每 1–2 秒轮询；两次查询值相同不代表任务卡住。
- 服务重启后以 interrupted 为准，实时进度仅在文件处理边界持久化。旧任务没有这些字段时，回退到计数和不定进度。

完整字段语义见 [API.md](API.md#实时字节进度新版)。App 验收需覆盖单个大文件的中间百分比、多文件整批进度、跳过、失败、校验阶段，以及 completed 之前不显示 100%。


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


### 上传速率

任务查询顶层和 `current_file` 均提供 `speed_bytes_per_second`（数字，字节/秒），取自 ossutil 最近一次 Speed 输出，KB/MB/GB 按 1024 换算。App 可除以 1024 显示 KiB/s，除以 1048576 显示 MiB/s。等待、扫描、解析、校验、metadata 和结束阶段为 0；尚无有效速率输出时也为 0，历史任务可能缺少字段。该值是工具最近一次报告的速率，不是每次轮询重新计算的瞬时网卡速率。

传输时向 `runtime/logs/service.log` 和服务标准错误输出速率日志（守护进程启动时标准错误进入 `logs/upload.log`），包含 task、已传字节/文件总字节、`speed=... KiB/s` 及 `speed_bytes_per_second`，最多每秒一条。实际更新频率取决于 ossutil；很小的文件可能结束前没有速率报告。

App 目录页可通过 `GET /files` 的 `upload_state=UPLOADED` 和 `NOT_UPLOADED` 分别加载两个页签。每行优先显示文件名、大小、录制时间和上传状态，文件详情可直接使用返回的 `files` 元素。已上传的保留文件允许再次提交，校验一致后显示“已上传，本次跳过”。任务完成后刷新两个列表。字段和分页规则见 [文件列表与上传状态](API.md#app-文件列表与上传状态)。
