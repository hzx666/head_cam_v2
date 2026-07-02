# data_record_upload

统一 mcap 数据采集与上传工具。支持 **遥操作(teleop)** 和 **头戴相机(head_cam)** 两种场景，覆盖实时数据录制、断线重连后自动修复、OSS 上传、元数据登记全链路。

## 项目结构

```
client/
├── upload_mcap.py          # 统一上传脚本（teleop / head_cam 共用）
├── mcap_parser.py           # mcap 文件元信息解析
├── mcap_repair.py           # 损坏 mcap 自动修复（.mcap.incomplete → .mcap）
├── build_release.sh         # PyInstaller 构建两个产物
├── requirements.txt         # 运行依赖
└── ego-bag/                 # 实时数据录制采集
    ├── record_data.py       # 多相机同步采集（RealSense D405 + ZED Mini）
    ├── config.json          # 设备配置（device_type / device_id）
    ├── start_record.sh      # 采集启动脚本
    └── upload_data.sh       # 采集后自动上传

tools/
├── upload_status.py         # 上传进度查询与历史汇总
├── backfill_duration.py     # 按日期回填 mcap duration
├── oss_meta.py              # OSS 远程 mcap 元信息读取
└── db.env.example           # DB 配置示例
```

## 实时数据录制

`ego-bag/record_data.py` 支持多相机同步采集：

- **RealSense D405**：动态检测已连接的相机，每台 D405 独立进程写入 `.avi` 视频
- **ZED Mini**：RGB 视频 + 深度视频（伪彩色） + IMU 数据（CSV）
- 每 5 秒输出采集帧率和统计
- 按 `config.json` 中的 `device_type` / `device_id` 自动组织存储路径
- 停止录制后自动重命名文件（追加结束时间戳）

## 上传流程

```
检测存储设备 → 扫描 .mcap → 自动修复不完整文件 → 选策略 → 确认 → 并发上传 → 汇总
```

每文件处理流水线：

```
解析 mcap meta → 自动修复（损坏时） → 确认 OSS 已存在或上传成功 → POST 写 raw_data_object
```

### 自动修复（mcap_repair）

上传时自动检测并修复不完整的 mcap 文件：

- 扫描所有 `.mcap.incomplete` 文件，使用 `mcap recover` 命令恢复有效数据
- 修复后的文件以 `_fixed.mcap` 后缀保存
- 上传日志中会记录修复结果（成功/失败/耗时）
- 构建产物内嵌 `mcap CLI`，首次运行自动下载到本地缓存

### 上传策略

- **跳过已存在**：OSS 已有文件不重传，但仍 POST 写库刷新 meta
- **全部覆盖**：重新上传 OSS，成功后再写库
- 0 字节 mcap 记为"空文件"，不上传、不写库
- OSS 上传失败不写库，零脏数据

### 盘类型筛选

- `teleop`：固定硬盘（RM=0），上传路径 `Orin/{YYMMDD}/{dev}_device/{filename}`
- `head_cam`：SD 卡（RM=1），上传路径 `{YYMMDD}/{dev}_device/{filename}`

## 构建

```bash
bash client/build_release.sh
```

产物：

- `dist/teleop/teleop_upload_release_<ts>`（默认 kind=teleop）
- `dist/head_cam/head_cam_upload_<ts>`（默认 kind=head_cam）

单文件可执行，内嵌 ossutil + mcap CLI，无需外部依赖。

## 运行

```bash
# 直接运行产物
dist/teleop/teleop_upload_release_<ts>
dist/head_cam/head_cam_upload_<ts>

# 源码运行需指定 kind
UMCAP_DEFAULT_KIND=teleop python3 client/upload_mcap.py
UMCAP_DEFAULT_KIND=head_cam python3 client/upload_mcap.py
```

## 辅助工具

| 工具 | 用途 |
|------|------|
| `tools/upload_status.py` | 查询上传进度与历史汇总，支持 `--watch` 持续监控 |
| `tools/backfill_duration.py` | 三阶段回填 mcap duration：query → compute → apply |
| `tools/oss_meta.py` | OSS 远程 mcap 元信息读取（Range 请求，无需下载整文件） |

## 日志

终端输出带本地时间戳，同时写入：

```
~/.cache/upload_mcap/logs/upload_mcap_<日期>.log
```
