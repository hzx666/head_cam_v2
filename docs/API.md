# 手机 App 对接：头环上传服务

服务地址示例：`http://192.168.1.10:8000`。所有响应为 JSON。目录来自头环设备，App 应调用目录接口展示选择页面，不使用手机本地文件选择器。

建议流程：检查服务 → 展示存储位置 → 点击进入目录 → “上传当前目录” → 查询任务进度。

## 检查服务

`GET /health` 返回 `{"status":"ok","version":"1.0.0"}`。只检查服务运行，不检查 OSS 网络或权限。

## 初始目录列表

`GET /directories`：

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

默认列出识别的 TF 卡；配置 `--allowed-root` 后列出该根目录。没有 TF 卡且未配置根目录时列表为空，并返回 message。虚拟根目录不可直接上传。

## 点击目录

`GET /directories?path=%2Fmnt%2Fsd%2Fsd0`：

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

- 点击子目录，用其 path 再请求本接口。
- 返回上级，用 parent；为 null 时禁用。配置允许根目录时 parent 不会越界。
- “上传当前目录”按钮直接使用当前 path，无需用户手输或自行拼接。
- 使用标准 URL 编码，尤其是中文、空格、`&`、`+`、`#`。
- 每页最多 200 个子目录。next_offset 非 null 时请求同一 path 和对应 offset，追加列表。
- mcap_files 只统计当前层的 `.mcap` 文件，不递归，不是任务总文件数。
- 不显示符号链接和服务 runtime；扫描上传时还会跳过系统索引目录。

## 提交上传

```http
POST /upload
Content-Type: application/json

{"path":"/mnt/sd/sd0/recordings"}
```

HTTP 202：

```json
{
  "accepted":true,
  "duplicate":false,
  "task_id":"83b31f8409844ff1a22a7ca470e6aa12",
  "status":"queued",
  "status_url":"/upload/83b31f8409844ff1a22a7ca470e6aa12"
}
```

请求必须且只能包含字符串 path，指向头环已有、可读的绝对目录。请求体最多 16 KiB。后台执行，无 YES 或其他终端交互。accepted 只表示接收成功。

同目录未完成时重复提交返回 HTTP 200、duplicate=true 和原 task_id。父子目录重叠返回 409。任务结束后再次提交创建新任务，用于重试或追加数据。

## 查询任务

`GET /upload/{task_id}`：

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

建议每 1–2 秒查询一次；保存 task_id 可恢复查询。queued/scanning/uploading 时继续查询；completed/partial_failed/failed/no_data/interrupted 是终态。

扫描结束后 total 才确定。processed/total 是文件处理进度，不是上传字节百分比。单文件状态为 uploaded/skipped/empty/failed；失败时读取 error。空文件及前置失败可能不包含校验字段。

历史对象没有 SHA256 时，结果为 skipped、checksum_status=unknown、same_content=null，不能显示为“校验一致”。completed 也可能包含这些跳过项或空文件，App 应展示明细。

没有附件、MNS、预览或质检事件状态。

## 错误与鉴权

错误示例：`{"accepted":false,"error":"Directory overlaps an active upload task"}`。

| HTTP | 含义 |
| --- | --- |
| 400 | 请求格式或路径无效 |
| 401 | 令牌无效 |
| 403 | 目录不可读或越界 |
| 404 | 接口或任务不存在 |
| 408 | 读取请求超时 |
| 409 | 与未完成目录重叠 |
| 413 / 431 | 请求体 / 请求头过大 |
| 415 | 未使用 application/json |
| 503 | 队列/连接已满、服务停止中或持久化异常 |

设备设置 HEAD_CAM_UPLOAD_TOKEN 时，每次请求添加 `Authorization: Bearer <令牌>`。AK/SK 不出现在请求或响应里。
