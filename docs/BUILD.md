# 构建与验证

当前构建同时生成上传、监控、守护程序和启动更新脚本，部署整个 `dist/`。统一服务安装及更新说明见 [UNIFIED_SERVICE.md](UNIFIED_SERVICE.md)。下方原有单文件说明仅针对上传组件。

设备端全部业务使用 C11；Python 仅用于构建生成内嵌资源和离线测试，不部署到头环。

## ARM64 单文件发布

Linux / WSL Ubuntu 22.04 构建机准备工具：

```sh
sudo apt-get update
sudo apt-get install -y cmake build-essential gcc-aarch64-linux-gnu \
  libssl-dev liblz4-dev libzstd-dev python3 python3-cryptography qemu-user
```

准备静态库，脚本只下载并解包目标开发包到 `.deps/arm64`，不安装 ARM64 包到宿主系统：

```sh
bash scripts/prepare_arm64.sh
```

构建成品：

```sh
CREDENTIALS_SOURCE=/mnt/d/real-device-upload-teleop/upload_core.py \
  bash build_release.sh
```

构建从 teleop 源码 AST 读取已有凭证配置，不导入、不执行源码，不打印 AK/SK。也支持 `CREDENTIALS_JSON=/私有路径/credentials.json`，内容包含 endpoint、bucket、access_key_id、access_key_secret、可选 security_token。不要将私有 JSON 提交 Git。

默认使用 `third_party/ossutil/ossutilarm64`；可设置 OSSUTIL_BIN 指定同架构静态 ossutil、CA_BUNDLE 指定 CA 文件。所有构建中间材料放在临时目录，退出时清理。

输出：

- `dist/head-cam-upload`：唯一需要部署的运行文件。
- `dist/SHA256SUMS`：成品完整性校验。
- `dist/build-info.txt`：架构、编译器、依赖包与内嵌资源校验值。

正式构建 SERVICE_TESTING=OFF，不接受测试凭证/ossutil 参数。AK/SK 再加密内嵌；最终文件仍含解密材料，应按含凭证软件管理。

如已启用 qemu-aarch64 binfmt，设置 `QEMU_SELF_CHECK=1` 会在交叉构建后运行离线自检；同架构构建自动运行自检。构建脚本检查 ELF 不含动态解释器。

静态 OpenSSL 可能产生 glibc 的 dlopen/getaddrinfo 链接警告。服务只使用内建 AES/SHA/RNG，禁用外部 OpenSSL 配置加载；网络由内嵌静态 ossutil 处理。实际设备还需验证 Linux ABI、可执行文件系统和网络环境。

## 离线测试

```sh
cmake -S . -B build-service-native -DSERVICE_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-service-native -j4
python3 tests/test_service.py build-service-native/head-cam-upload
python3 tests/smoke_release.py dist/head-cam-upload --qemu
```

集成测试使用模拟 ossutil，覆盖目录浏览、身份解析、Zstd/LZ4、CRC、metadata、SHA256、冲突、失败、鉴权、持久化和重启，不访问真实 OSS。成品 smoke test 只提交空目录。

内存检查：CMake 额外传入 `-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`，再运行同一组测试。

## 源码模块

| 文件 | 职责 |
| --- | --- |
| src/service_main.c | 启动参数、runtime 锁与生命周期 |
| src/service_http.c | HTTP、目录浏览与令牌 |
| src/service_tasks.c | 队列、扫描、持久化与恢复 |
| src/head_mcap.c | 文件名、MCAP 解析、解压和时长 |
| src/service_upload.c | OSS 查询、校验、上传和 metadata |
| src/service_bundle.c | 内嵌依赖与凭证、受控子进程 |
| src/service_util.c | JSON、文件持久化、SHA256 |
| src/tf_scan.c | TF 卡挂载点识别 |
