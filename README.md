# data_record_upload

统一 mcap 上传工具。当前只保留 `client` 顶层上传链路，`teleop` 和 `head_cam` 共用同一套代码。

## 当前入口

- `client/upload_mcap.py`：统一上传脚本
- `client/mcap_parser.py`：mcap 元信息解析
- `client/build_release.sh`：一次构建 `teleop` / `head_cam` 两个产物
- `client/requirements.txt`：运行依赖

历史 `client/teleop/`、`client/head_cam/`、`tracker/`、`deploy/` 已移除，不再作为上传入口或进度服务。

## 上传规则

每个文件的处理顺序：

```text
解析 mcap meta -> 确认 OSS 已存在或上传成功 -> POST /api/raw-data/objects 写 raw_data_object
```

- OSS 对象既不存在、上传也失败时，不写数据库。
- 选择“跳过已存在”时：OSS 已有该文件则不重传，但仍 POST 写库刷新 meta；写库前会 sleep `0.2s`，避免瞬时大量请求。
- 选择“全部覆盖”时：重新上传 OSS，上传成功后再写库。
- 上传线程数固定为 `1`，真实上传时显示 `ossutil cp` 原生输出，脚本同时输出 `[进度]` 汇总。
- 0 字节 mcap 记为 `空文件`，不上传、不写库，也不计入 `失败`。

## OSS 路径

`teleop`：

```text
oss://ss-oss-data-platform/original_data/teleoperation/Orin/{YYMMDD}/{dev}_device/{filename}
```

`head_cam`：

```text
oss://ss-oss-data-platform/original_data/ego_human_data/head_cam/{YYMMDD}/{dev}_device/{filename}
```

## 数据库字段

客户端 POST 只允许提交接口示例中的字段：

```text
ossPath, rootPrefix, dataDomain, dataKind, captureDate, deviceNo, deviceDir,
sessionName, relativePath, objectRole, fileFormat, fileSize, duration, etag,
storageClass, lastModified, ossMetadata, parsedMetadata
```

如果 payload 多字段或少字段，脚本会直接失败，不会提交。

`teleop` 写库时提交 `dataDomain=teleoperation`、`dataKind=orin`，服务端分区应落到 `teleoperation_orin_{YYYYMM}`，不要使用 `teleoperation_teleop_{YYYYMM}`。

客户端只提交 `captureDate`。生产表里的 `capture_month` 是月份分桶列，值为采集日期所在月份第一天，例如 `capture_date=2026-06-16` 时 `capture_month=2026-06-01`。

## 构建

```bash
bash client/build_release.sh
```

构建产物：

- `dist/teleop/teleop_upload_release_<ts>`
- `dist/head_cam/head_cam_upload_<ts>`

## 运行

```bash
dist/teleop/teleop_upload_release_<ts>
dist/head_cam/head_cam_upload_<ts>
```

运行后会自动检测外部存储、按盘类型筛选、扫描 `.mcap` 文件、选择上传策略和本地清理策略，然后开始上传。

盘类型筛选：

- `teleop`：固定硬盘，`RM=0`
- `head_cam`：SD 卡，`RM=1`

## 日志

终端每行带本地时间戳，同时写入：

```text
~/.cache/upload_mcap/logs/
```

运行时缓存：

```text
~/.cache/upload_mcap/.bin/
```

## 源码运行

源码运行需要指定 kind：

```bash
UMCAP_DEFAULT_KIND=teleop python3 client/upload_mcap.py
UMCAP_DEFAULT_KIND=head_cam python3 client/upload_mcap.py
```
