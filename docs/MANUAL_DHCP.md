# 临时手动获取网关

从 20261103.002 开始，一键启动流程默认自动调用此脚本：WiFi 已有 IP、默认路由缺失时获取网关；运行中每15秒检查，路由丢失后自动重试。因此正常使用开机自启不再需要手动执行。脚本不会主动连接 WiFi，也不替换已有默认路由。

正式固件接管 DHCP 后，在设备安装目录创建 network.env 并写入 `HEAD_CAM_AUTO_DHCP=false`，然后重启服务。可用 `HEAD_CAM_NETWORK_IFACE=wlan0` 指定网卡。不要同时启用两套互相竞争的网络配置流程。

将 dist/manual_dhcp.sh 与 head-cam-service 放在设备同一目录。WiFi 已连接、网卡已有 IPv4 地址后执行：

```sh
sh manual_dhcp.sh
```

手动调用默认 wlan0，其他网卡用 `sh manual_dhcp.sh eth0`。使用内嵌 BusyBox 的 udhcpc，最多25秒；从 DHCP router 选项取首个网关并替换该接口的默认路由。保留现有IP和DNS，不清空网卡、不重启WiFi、不杀已有客户端。无网关或请求失败时返回非零，不写死兜底地址。

这是已有IP、仅缺网关时的临时辅助，不是完整的持久 DHCP 网络管理方案。正式固件接管 DHCP 后停止使用。成功显示 Default gateway 后监控自行重连，无需重启服务。

脚本通过本地假客户端测试正常返回、多网关、无网关、非法地址及客户端失败；没有在开发电脑实际发送 DHCP 或修改网络。真实 DHCP 获取仍需设备上运行验证。
