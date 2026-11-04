# 20261103.002 自动恢复默认网关

复制完整最新版 dist（包含 manual_dhcp.sh），配置好 monitor.json。首次执行 chmod 700 head-cam-service，然后执行 sh boot_autostart.sh install。已安装设备应先正常停止旧服务，替换文件并保留现场 JSON，再执行 sh boot_autostart.sh start；重复 install 也不会重复注册自启。

当前已自动补齐联网前置条件：开机及运行中发现缺少默认路由且 wlan0 已有 IP，会通过 DHCP 获取网关；每15秒检查，已有默认路由时不重复请求。无需每次登录运行 manual_dhcp.sh。实际 WiFi 连接、IP 分配仍由固件负责；无线断开或 DHCP 服务器不可用时继续重试，不阻塞上传本地接口。

正式固件自动 DHCP 后，在安装目录 network.env 写入 HEAD_CAM_AUTO_DHCP=false，然后重启服务即可关闭临时补偿。

自更新仍因存储方案未完成而默认关闭。当前启动和恢复流程已做离线测试，仍需实际设备断网恢复和重启验收。

# 一键开机自启（最新用法）

完整复制当前 dist 到设备固定目录，配置旁边 monitor.json，确保旧服务已停止。执行 chmod 700 head-cam-service，然后执行 sh boot_autostart.sh install。

此命令注册 /config/diy.sh 的带标记启动段，备份并保留原内容，重复执行不会重复注册；直接在当前目录启动，不再复制一份到 releases，不改设备固件启动脚本。以后开机自动拉起上传和监控。只修改 JSON 后执行 sh boot_autostart.sh restart。

自更新由于设备空间不足，默认关闭；后续有合适存储方案再显式启用 OSS_UPDATE_ENABLED=true。默认路由仍需设备网络配置持久化。安装工具不会自行重配 WiFi 或添加网关。实际断电重启需要设备确认 /config/diy.sh 被执行并持久保留。

# 单二进制头环服务 20261103.001

当前只有一个业务二进制 head-cam-service，内含上传、监控、守护及静态 BusyBox；上传和监控仍通过 exec 启动为独立进程。原项目未修改。

## 不安装，前台测试

完整复制本分支 dist 到设备的独立可写目录，保留现场 JSON，避免覆盖正在运行的程序。修改 monitor.json：enabled=true、device_id=8L6AR120012；默认数据目录 /mnt/sd/sd0，设备类型 head_cam_v2，MQTT 地址和主题沿用 X5。

```sh
chmod 700 head-cam-service
./head-cam-service
```

默认读取可执行文件旁的 monitor.json，在旁边 runtime、logs 下保存状态并独立运行上传和监控。已有服务应先正常停止，避免端口和实例锁冲突。Ctrl+C 优雅停止两个业务。

只验证监控：

```sh
mkdir -p runtime/monitor
./head-cam-service monitor ./monitor.json ./runtime/monitor
```

只启动上传：

```sh
./head-cam-service upload --runtime-dir ./runtime/upload --host 0.0.0.0 --port 38127
```

先确保 runtime 父目录存在。单独上传不启动监控。

状态和日志：runtime/monitor/status.json、logs/monitor.log、logs/upload.log。EMQX Client ID 为 head-cam-8L6AR120012。旧版 head-cam-upload、head-cam-monitor、busybox 文件不再需要。

## 自启和更新的实际状态

boot_autostart.sh 提供 install、start、stop、restart、status 和 update 命令。安装直接使用当前目录及旁边的 monitor.json，不复制程序；更新功能仍保留版本目录机制，默认关闭。

设备启动时通过 /config/diy.sh 拉起服务；实际掉电持久化仍需实机验收。

/mnt/config 约 24 MB，不能保证同时容纳当前版、新版和备份。启用更新前需确认空间；设备网络和默认路由单独配置。

不要混合发布其他分支文件。未来发布使用新的 SERVICE_VERSION，不覆盖旧版。设备 JSON 保留独立维护。

## 构建和测试

构建机执行 scripts/prepare_arm64.sh 获取静态 ARM64 BusyBox 及依赖，再按原 build_release.sh 的凭证参数构建。Bash/Python 仅构建机使用。设备只需 /bin/sh、Linux ARM64、/proc、可写且可执行的 /tmp 及数据目录。内嵌 BusyBox 按内容摘要缓存在 /tmp/head-cam-tools-*，不占持久分区副本。

已验证：原上传13项、监控2项、更新安装7项、独立守护1项、实际多模式业务隔离1项；ARM64 上传 smoke test、默认单文件同时启动两项业务、内嵌工具和 SHA256 清单通过。测试不访问真实 OSS/MQTT。真实设备仍需再次验证 DNS、网络及内核兼容。