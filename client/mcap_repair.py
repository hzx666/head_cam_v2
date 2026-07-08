#!/usr/bin/env python3
"""修复不完整的 MCAP 文件 (.mcap.incomplete -> .mcap)。

使用 mcap CLI 工具的 recover 命令从损坏的 MCAP 文件中恢复有效数据。
"""
import os
import platform
import subprocess
import time
from pathlib import Path
from urllib import request as urlrequest

MCAP_CLI_VERSION = "0.0.48"


def _cli_cache_dir() -> Path:
    base = Path.home() / ".cache" / "upload_mcap" / ".bin"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _mcap_cli_name() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "windows":
        return "mcap.exe"
    if system == "darwin":
        if "arm" in machine or "aarch" in machine:
            return "mcap-macos-arm64"
        return "mcap-macos-amd64"
    if "arm" in machine or "aarch" in machine:
        return "mcap-linux-arm64"
    return "mcap-linux-amd64"


def get_mcap_cli(log_cb=None) -> str:
    """获取 mcap CLI 工具，如不存在则从 GitHub 下载。"""
    log = log_cb or (lambda m: None)
    cache = _cli_cache_dir()
    dest = cache / "mcap"
    if dest.exists():
        return str(dest)

    name = _mcap_cli_name()
    url = (f"https://github.com/foxglove/mcap/releases/download/"
           f"releases%2Fmcap-cli%2Fv{MCAP_CLI_VERSION}/{name}")
    log(f"[信息] 下载 mcap CLI ({name})...")
    urlrequest.urlretrieve(url, dest)
    os.chmod(dest, 0o755)
    return str(dest)


def _run_mcap(args: list, timeout: int = 600):
    mcap = get_mcap_cli()
    cmd = [mcap] + args
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    except Exception:
        return None


def repair_mcap_file(source: str, dest: str, log_cb=None) -> bool:
    """使用 mcap recover 从损坏文件中恢复有效数据。"""
    log = log_cb or (lambda m: None)
    dest_path = Path(dest)
    if dest_path.exists():
        return False

    r = _run_mcap(["recover", source, "-o", dest])
    if r is None:
        log("    mcap recover 超时")
        return False
    if r.returncode == 0 and dest_path.exists() and dest_path.stat().st_size > 0:
        return True
    err = (r.stderr or "").strip()
    if err:
        log(f"    mcap recover: {err[:200]}")
    else:
        log(f"    mcap recover 退出码={r.returncode}")
    try:
        if dest_path.exists() and dest_path.stat().st_size == 0:
            dest_path.unlink()
    except OSError:
        pass
    return False


def scan_and_repair(mount: str, delete_original: bool = False, progress_cb=None, log_cb=None) -> list:
    """扫描挂载点下所有 .mcap.incomplete 文件并尝试修复。

    返回修复成功的 [(原路径, 新路径, 0), ...] 列表。
    """
    log = log_cb or (lambda m: None)
    root = Path(mount)
    incomplete_files = sorted(root.rglob("*.mcap.incomplete"))

    if not incomplete_files:
        return []

    log(f"[修复] 发现 {len(incomplete_files)} 个不完整的 mcap 文件")
    repaired = []

    for src in incomplete_files:
        try:
            base = src.name[:-len(".mcap.incomplete")]
            dest = src.parent / (base + "_fixed.mcap")
            if dest.exists():
                log(f"  - 跳过 {src.name}: {dest.name} 已存在")
                continue

            src_size = src.stat().st_size
            log(f"  > 修复 {src.name} ({src_size / 1e6:.1f} MB) ...")

            t0 = time.time()
            if repair_mcap_file(str(src), str(dest), log_cb=log):
                elapsed = time.time() - t0
                dest_size = Path(dest).stat().st_size
                log(f"    ✓ 完成: {dest.name} ({dest_size / 1e6:.1f} MB), 耗时 {elapsed:.1f}s")
                repaired.append((str(src), str(dest), 0))
                if delete_original:
                    try:
                        src.unlink()
                    except OSError:
                        pass
            else:
                log(f"    ✗ 修复失败: {src.name}")

        except Exception as e:
            log(f"  ✗ 修复异常 {src.name}: {e}")

    return repaired