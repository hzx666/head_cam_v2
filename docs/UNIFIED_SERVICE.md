# 头环统一服务说明

部署、启动和更新操作统一见 [BUSYBOX_DEPLOY.md](../BUSYBOX_DEPLOY.md)，构建与验证见 [BUILD.md](BUILD.md)。

上传、监控由同一二进制启动为独立进程，异常退出后分别重启。上传继续提供 HTTP 接口和任务持久化；监控独立扫描并上报 MQTT。

## 监控配置及待定项目

编辑程序旁的 `monitor.json`，补齐后设置 `enabled=true` 并重启服务。默认禁用，不会向参考设备的 Broker 发送消息。

已确定：Broker=mqtt.simplesilicon-data.com:1883，topic_prefix=orin/prod，无用户名密码、无 TLS，设备类型 head_cam_v2，presence/summary 字段沿用 X5。待定：device_id、battery_capacity_path；recording_root 默认 /mnt/sd/sd0，实际目录不同直接修改 JSON；storage_path 留空使用 recording_root；周期及阈值保留可配置。示例中的 60/120 秒和 5/2 GiB 只是可配置初值。

当前实现 MQTT 3.1.1、可选用户名密码，不支持 TLS。配置 qos 仍须为 0，presence/summary 使用 QoS 0；新增 RPC 和属性通道固定使用 QoS 1。presence 使用 retained online/offline 和 retained LWT；summary 不保留。连接检查 CONNACK，定期 PINGREQ/PINGRESP 检测断线，每次重连补报快照。

## MQTT RPC 与属性上报

新增主题前缀为 `<topic_prefix>/device/<编码后的device_id>/`，device_id 使用百分号编码。订阅 `downlink/req`、`downlink/resp`，Broker 必须授予 QoS 1；发布 `uplink/req`、`uplink/resp`、`uplink/property`，均为非 retained QoS 1。连接后及每 60 秒上报属性，包括硬件型号和当前服务版本。

RPC 使用 protocolVersion=1.0，ts/expiresAt 为 Unix 毫秒。支持 properties.get、ota.start、ota.getStatus、ota.cancel 和设备发起的 ota.reportStatus。请求检查设备身份、有效期并缓存最近 16 条请求用于幂等重放；任务和请求记录保存在监控运行目录的 mqtt-session.json。状态每 10 秒重报，收到匹配的成功业务响应后停止。QoS 1 消息约 5 秒未确认时带 DUP 重传一次，再次超时则重连。

ota.start 需要在 monitor.json 中配置真实的 hardware_model，并与请求 hardwareModel 一致；缺失或不匹配返回 4221。当前仅受理并持久化 pending/queued 任务，尚未连接下载、安装、重启和自检流程。ota.pause/ota.resume 尚无实际执行支持，ota.register 返回未授权。firmware.version 为服务编译版本，upgradeSource/upgradeId 当前为 null。

本地 status.json 额外提供 subscribed、subscriptions 和最近一次 rpc 结果。离线验证运行 `python3 tests/test_monitor.py`，使用本地模拟 Broker，不访问生产 MQTT。

主题为 `<topic_prefix>/device/<device_id>/presence` 和 `.../recording/summary`。presence 包括 device_id、device_type=head_cam_v2、event_type、status、ts、seq、service、version、ip。summary 沿用 X5，包含 schema_version、device_sn、event_type、ts、seq、recordings、storage、battery、alert；schema_version 使用发布版本的八位日期部分。时间输出 +08:00，service/version 使用新服务名称和版本。扫描诊断信息仅保存在本地状态文件。

监控仅递归扫描 JSON 的 recording_root，默认 /mnt/sd/sd0；不执行挂载点自动识别或多目录合并。storage_path 留空时统计同一目录所在文件系统容量。目录不可用时保留最近成功快照并标记错误，后续自动重试。递归最多 32 层、遍历最多 100000 项、每轮最多约 30 秒；跳过隐藏项、符号链接和 incomplete。只计普通 `.mcap`（大小写不敏感），空文件计入文件数，不做内容校验。小时统计从文件名 `YYYYMMDD_HHMMSS` 提取当天数据，以设备本地时区为准。非法/未知命名只计总量。监控口径不代表文件满足上传校验。

扫描失败保留最后成功统计和时间，附加 error；缺失挂载点标为 unknown，电池路径未设置则 valid=false、soc=null。电池路径应指向能直接读取 0–100 整数电量的文件。状态保存在 `runtime/monitor/status.json`。
