# 头环统一服务部署

版本：`20261102.001`。完整交付目录为 `dist/`。设备业务使用三个静态 ARM64 C 程序，不需要 Python；启动和更新需要 Bash、systemd、GNU coreutils（含 timeout、sha256sum）、util-linux flock、find、sed、grep。

## 结构与隔离

systemd → 启动/更新入口 → `head-cam-service` 守护进程 → 上传、监控两个独立进程。

上传继续提供原有 HTTP 接口和任务持久化。监控独立扫描和 MQTT 上报，不使用上传任务、业务锁或凭证。各组件退出后等待 10 秒单独重启。监控未配置时保持运行并写入 `unconfigured` 状态，上传仍启动。

## 首次安装

将完整 dist 内容复制到设备一个专用目录（例如 `/opt/head-cam-service`；最终设备路径待定），目录路径不要包含空白字符：

```sh
cd /opt/head-cam-service
chmod 700 boot_autostart.sh
sudo bash ./boot_autostart.sh install
```

默认安装到脚本所在目录；可用 `HEAD_CAM_HOME` 指定专用安装目录，用 `RUN_USER` 指定服务用户。必须给该用户提供录制目录读取权限及运行目录写权限。安装只需一次；不自动修改设备 WiFi。

安装生成 `head-cam-service.service`，开机启动，先延迟 60 秒再检查更新。整体异常退出后 systemd 等待 10 秒重启；子进程异常由守护进程分别处理。

```sh
sudo systemctl status head-cam-service
journalctl -u head-cam-service -n 100
sudo systemctl restart head-cam-service
```

`/etc/default/head-cam-service` 可设置 `BOOT_DELAY_SEC=60`、`BACKUP_KEEP=2`、`OSS_UPDATE_ENABLED=true`、`UPLOAD_HOST=0.0.0.0`、`UPLOAD_PORT=38127`、`UPLOAD_ALLOWED_ROOT`、`HEAD_CAM_UPLOAD_TOKEN`。它是 systemd EnvironmentFile，不使用 `export` 前缀。令牌文件应限制读取权限。

## 固定数据与版本目录

```text
安装目录/
  boot_autostart.sh -> current/boot_autostart.sh
  current -> releases/20261102.001
  previous -> releases/上一版本
  releases/版本号/       发布程序、脚本、版本、校验清单
  config/monitor.json   设备监控配置，更新不覆盖
  runtime/upload/       原有上传任务、日志及私有凭证会话
  runtime/monitor/      status.json、监控实例锁
  logs/run-upload.log   上传进程输出
  logs/run-monitor.log  监控进程输出
  .update-cache/        更新下载及互斥锁
```

配置、runtime、日志和录制数据独立于版本目录。日志目前不自动轮转，需由设备运维设置 logrotate；上传任务历史保留原有策略。

## 监控配置及待定项目

编辑 `config/monitor.json`，补齐后设置 `enabled=true` 并重启服务。默认禁用，不会向参考设备的 Broker 发送消息。

已确定：Broker=mqtt.simplesilicon-data.com:1883，topic_prefix=orin/prod，无用户名密码、无 TLS，设备类型 head_cam_v2，presence/summary 字段沿用 X5。待定：device_id、battery_capacity_path；recording_root 默认 /mnt/sd/sd0，实际目录不同直接修改 JSON；storage_path 留空使用 recording_root；周期及阈值保留可配置。示例中的 60/120 秒和 5/2 GiB 只是可配置初值。

当前实现 MQTT 3.1.1、QoS0、可选用户名密码；TLS 或其他 QoS 配置会使监控进入 unconfigured，不能直接用于要求 TLS/QoS1 的 Broker。presence 使用 retained online/offline 和 retained LWT；summary 不保留。连接检查 CONNACK，定期 PINGREQ/PINGRESP 检测断线，每次重连补报快照。

主题为 `<topic_prefix>/device/<device_id>/presence` 和 `.../recording/summary`。presence 包括 device_id、device_type=head_cam_v2、event_type、status、ts、seq、service、version、ip。summary 沿用 X5，包含 schema_version、device_sn、event_type、ts、seq、recordings、storage、battery、alert；schema_version 使用发布版本的八位日期部分。时间输出 +08:00，service/version 使用新服务名称和版本。扫描诊断信息仅保存在本地状态文件。

监控仅递归扫描 JSON 的 recording_root，默认 /mnt/sd/sd0；不执行挂载点自动识别或多目录合并。storage_path 留空时统计同一目录所在文件系统容量。目录不可用时保留最近成功快照并标记错误，后续自动重试。递归最多 32 层、遍历最多 100000 项、每轮最多约 30 秒；跳过隐藏项、符号链接和 incomplete。只计普通 `.mcap`（大小写不敏感），空文件计入文件数，不做内容校验。小时统计从文件名 `YYYYMMDD_HHMMSS` 提取当天数据，以设备本地时区为准。非法/未知命名只计总量。监控口径不代表文件满足上传校验。

扫描失败保留最后成功统计和时间，附加 error；缺失挂载点标为 unknown，电池路径未设置则 valid=false、soc=null。电池路径应指向能直接读取 0–100 整数电量的文件。状态保存在 `runtime/monitor/status.json`。

## 更新与发布

发布根路径：`oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/`。

目录格式为 `YYYYMMDD.NNN/`，其中包含 dist 的九个清单文件及 `SHA256SUMS`。`build-info.txt` 为附加构建说明，可一并发布。先上传所有数据，最后上传 SHA256SUMS；不覆盖已发布版本。

每次整体服务启动检查更高版本；不定时检查，也不自动降级。使用程序内嵌加密凭证，运行时释放私有认证文件并显式传给 ossutil，无需 `.ossutilconfig`。当前复用上传构建凭证，需具备发布根路径的列举和读取权限。

下载在临时目录执行，更新总预算 300 秒；版本、白名单校验清单、shell 语法及三个程序离线自检全部通过才切换 current。下载或校验失败继续使用旧版本，默认保留两个旧版本。切换采用同文件系统符号链接原子替换，previous 指向上一版；启动器检测守护程序不可执行时回退 previous。网络服务不可达不视为版本故障。

运行中执行 `bash boot_autostart.sh update` 会延后到下次启动，避免打断上传。需要立即更新时，应先在 App 确认上传完成再重启。systemd 停止期限沿用 Orin 的 20 秒，超过后强制终止；被打断的上传下次启动标记 interrupted，需重新提交，不自动续传任务。

## 验证

```sh
cmake -S . -B build-service-native -DSERVICE_TESTING=ON
cmake --build build-service-native -j4
python3 tests/test_service.py build-service-native/head-cam-upload
python3 tests/test_monitor.py
python3 tests/test_supervisor.py
python3 tests/test_update.py
python3 tests/smoke_release.py dist/head-cam-upload --qemu
```

离线验证不连接真实 OSS/MQTT，不安装宿主 systemd 服务。真实头环仍需验证 systemd/工具可用性、静态 glibc DNS 兼容性、录制/电池路径、MQTT 平台协议和发布目录权限。



