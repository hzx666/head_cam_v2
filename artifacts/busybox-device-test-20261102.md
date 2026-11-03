# BusyBox 版实机部署测试

- 设备：192.168.20.114；SN：8L6AR120012
- 来源：D:/新建文件夹/head-cam-service-busybox/dist
- 版本：20261102.001
- 安装目录：/mnt/config/oss_upload
- 正式版本：/mnt/config/oss_upload/releases/20261102.001
- 服务地址：http://192.168.20.114:38127

## 已通过

- 本地清单校验、传输文件校验与三个静态程序离线自检。
- BusyBox + /bin/sh 安装，不依赖 Bash/systemd。
- 统一守护程序成功运行上传和监控两个独立子进程。
- GET /health 返回 200，status=ok。
- GET /directories 返回 200；当前未检测到 TF 卡。
- 分别向上传和监控子进程发 SIGTERM 后，守护进程约 10 秒自动重启两个子进程，健康接口恢复 200。

## 实机限制与未测试项

- / 为只读 squashfs，/etc/init.d 不可写。安装时 HEAD_CAM_INIT_DIR 指向 /mnt/config/oss_upload/init.d，生成的钩子没有接入厂商启动流程。未宣称开机自启成功，未重启整机。
- 本次启动测试使用 BOOT_DELAY_SEC=0、OSS_UPDATE_ENABLED=false；未测试 OSS 自动更新，未修改发布目录。
- config/monitor.json 使用默认禁用配置，监控状态为 unconfigured。未连接 MQTT、未发送监控消息。
- 当前无 TF 卡，未测试录制目录实际扫描和 MCAP 上传。
- 原独立上传 runtime 与历史记录保留；统一服务上传 runtime 在 runtime/upload。
- 旧根目录程序备份在设备 /tmp/headcam-before-b7ad5948fa4f（临时内存目录，重启会丢失）。
