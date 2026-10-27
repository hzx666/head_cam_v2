# data_upload_scan

第一阶段C语言程序：在目标嵌入式Linux设备上识别TF卡挂载点，并只读递归扫描
`.mcap`和`.mcap.incomplete`文件。

## 安全边界

- 不上传文件。
- 不修改或修复MCAP。
- 不删除任何文件。
- 不跟随符号链接。
- 默认忽略常见系统索引目录。

## 目标设备规则

目标设备将TF卡挂载为：

```text
/dev/mmcblk0p1 -> /mnt/sd/sd0
```

该设备的`/sys/class/block/mmcblk0/removable`为`0`，所以程序使用
`/dev/mmcblk*p*`和`/mnt/sd/*`的联合规则识别TF卡，而不依赖removable标志。

## 构建

```bash
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=/home/ssi/aarch64-ca73-linux-gnueabihf-10.4.0/share/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j
```

## 使用

自动识别TF卡并列出文件：

```bash
./data_upload_scan
```

仅统计，不逐个打印：

```bash
./data_upload_scan --count-only
```

用普通目录测试：

```bash
./data_upload_scan --root /tmp/test-mcap
```

默认最多打印200条路径，但始终统计全部文件；`--limit 0`表示打印全部。

## 单文件上传Demo

`data_upload_demo`先扫描TF卡，然后读取同目录的`oss_upload.conf`。用户分别
输入MCAP目录和文件名，程序显示完整上传计划，只有输入大写`YES`后才调用
`ossutilarm64`上传这一个文件。源文件始终保留。

初始化配置：

```sh
./data_upload_demo --init-config
```

配置包含Endpoint、Bucket、RAM AccessKey ID、RAM AccessKey Secret、可选STS
Token、对象前缀和ossutil路径。配置包含明文密钥，程序创建时会设置为`0600`；
不要填写阿里云网页登录密码。

目标设备的CA证书位于`/usr/bin/ssl/cacert.crt`，配置中的`ca_cert_path`
用于让静态Go版ossutil完成HTTPS证书校验。

目标机实际部署目录是`/mnt/config/oss_upload`；原来的
`/mnt/cdata/oss_upload/data_upload_demo`是兼容启动软链接。
