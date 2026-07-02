#!/usr/bin/env python3
"""upload_mcap — 通用 mcap 上传 + 元数据登记（teleop / head_cam 共用）。

纯交互式（与旧 teleop_upload.py / upload_head_cam.sh 交互一致），不接受任何参数：
自动检测存储设备 → 扫描 mcap → 检测 OSS 已存在 → 选策略 → 选是否清本地 →
确认 → 并发上传（每文件：解析 meta → OSS 上传 → 成功后 POST 写 raw_data_object）→ 汇总。

策略：
  - 跳过已存在：OSS 已有的文件跳过重传，但仍 POST 让后端 upsert 刷新 duration/parsedMetadata。
  - 全部覆盖：所有文件重新上传 OSS，OSS 成功后再写库。
OSS 上传失败则不写库（中断 = 零脏数据）。

单文件自包含：内嵌 ossutil + 加密 OSS 凭证（cryptography），运行/打包无需外部依赖。
PyInstaller 打包：--onefile --add-data ossutil --collect-all mcap --collect-submodules cryptography

运行：直接执行编译产物（kind 已固化）；源码运行需 UMCAP_DEFAULT_KIND=teleop|head_cam python3 upload_mcap.py
"""
import base64
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from urllib import request as urlrequest
from urllib.error import HTTPError, URLError

# 同级 mcap_parser.py（仓库非 Python 包）
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcap_parser import summarize  # noqa: E402
from mcap_repair import scan_and_repair, repair_mcap_file  # noqa: E402

# ─── 配置 ─────────────────────────────────────────────────────
API_BASE = os.getenv("DATA_API_BASE", "http://47.116.77.177:8080")
OBJECTS_URL = f"{API_BASE}/api/raw-data/objects"
OSS_BUCKET = "ss-oss-data-platform"
OSSUTIL_VERSION = "1.7.18"
MAX_RETRIES = 3
WORKERS = 1                      # 串行上传，避免 OSS/API 并发导致超时或冲突
EXCLUDE_KEYWORDS = ["WiFi Driver", "软件"]   # 非数据盘关键词

KIND_LABEL = {"teleop": "遥操作(teleop)", "head_cam": "头戴相机(head_cam)"}
RAW_DATA_OBJECT_FIELDS = (
    "ossPath",
    "rootPrefix",
    "dataDomain",
    "dataKind",
    "captureDate",
    "deviceNo",
    "deviceDir",
    "sessionName",
    "relativePath",
    "objectRole",
    "fileFormat",
    "fileSize",
    "duration",
    "etag",
    "storageClass",
    "lastModified",
    "ossMetadata",
    "parsedMetadata",
)
RAW_DATA_OBJECT_FIELD_SET = set(RAW_DATA_OBJECT_FIELDS)

# OSS 凭证 (AES-256-CBC + PBKDF2 混淆, 与 teleop_upload 共用同一 bucket 账号)
_K = "ssim_record_upload_2026_v1"
_T_AK_ENC = "U2FsdGVkX18QkKLFQm+J8l2nQihkIM2hHrd9RJ5k8EGxhgrjXquNchWCaUgb6rD2"
_T_SK_ENC = "U2FsdGVkX19H6vKZBFlqCT7c6J7iqYtnKj2A+S1HxIWDpTpEiJ4WAcfdlCwIhK5d"
OSS_ENDPOINT = "oss-cn-shanghai.aliyuncs.com"

LOG_DIR = Path.home() / ".cache" / "upload_mcap" / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_FILE = LOG_DIR / f"upload_mcap_{time.strftime('%Y%m%d_%H%M%S')}.log"

_LOG_LOCK = threading.Lock()

# 文件名 → 设备号/日期
# head_cam: SSIEGO_Cam_{dev}_{YYYYMMDD}_*   teleop: m1_{dev}_camera_{YYYYMMDD}_*
_HEAD_CAM_RE = re.compile(r"^SSIEGO_Cam_(?P<dev>[A-Za-z0-9]+)_(?P<date>\d{8})_")
_TELEOP_RE = re.compile(r"^m1_(?P<dev>\w+?)_camera_(?P<date>\d{8})_")


def log(msg: str):
    """每行输出前置绝对时间（本地 wall-clock）。线程安全。"""
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    with _LOG_LOCK:
        print(line, flush=True)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def now_ts() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


# ─── 凭证解密 ─────────────────────────────────────────────────
def decrypt_credential(enc_text):
    try:
        from cryptography.hazmat.backends import default_backend
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        data = base64.b64decode(enc_text)
        if data[:8] != b"Salted__":
            return ""
        salt, ct = data[8:16], data[16:]
        key_iv = hashlib.pbkdf2_hmac("sha256", _K.encode(), salt, 10000, dklen=48)
        dec = Cipher(algorithms.AES(key_iv[:32]), modes.CBC(key_iv[32:48]),
                     backend=default_backend()).decryptor()
        pt = dec.update(ct) + dec.finalize()
        pad = pt[-1]
        if pad < 1 or pad > 16:
            return ""
        return pt[:-pad].decode("utf-8")
    except Exception:
        return ""


# ─── ossutil 管理（内嵌 + CDN 兜底）──────────────────────────
def _ossutil_name():
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "windows":
        return "ossutil64.exe"
    if "arm" in machine or "aarch" in machine:
        return "ossutilarm64"
    return "ossutil64"


def _runtime_bin_dir():
    base = Path.home() / ".cache" / "upload_mcap" / ".bin"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _bundled_ossutil():
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS) / "ossutil" / _ossutil_name()
    payload = Path(__file__).resolve().parent / ".build" / "ossutil" / _ossutil_name()
    return payload if payload.exists() else None


def get_ossutil_path():
    """获取 ossutil：优先内嵌包，否则从 CDN 下载到缓存目录。"""
    name = _ossutil_name()
    ossutil_path = _runtime_bin_dir() / name
    if ossutil_path.exists():
        return str(ossutil_path)
    bundled = _bundled_ossutil()
    if bundled and bundled.exists():
        shutil.copy2(bundled, ossutil_path)
        if platform.system().lower() != "windows":
            os.chmod(ossutil_path, 0o755)
        log(f"[信息] 从内嵌包解出 ossutil: {name}")
        return str(ossutil_path)
    url = f"https://gosspublic.alicdn.com/ossutil/{OSSUTIL_VERSION}/{name}"
    log(f"[信息] 下载 ossutil {name} (~12MB)...")
    urlrequest.urlretrieve(url, ossutil_path)
    if platform.system().lower() != "windows":
        os.chmod(ossutil_path, 0o755)
    return str(ossutil_path)


def setup_ossutil_config(ak, sk):
    cfg = tempfile.NamedTemporaryFile(mode="w", suffix=".ossutil", delete=False,
                                      prefix=".ossutil_")
    cfg.write(f"[Credentials]\nlanguage=CH\nendpoint={OSS_ENDPOINT}\n"
              f"accessKeyID={ak}\naccessKeySecret={sk}\n")
    cfg.close()
    os.chmod(cfg.name, 0o600)
    return cfg.name


def run_ossutil(args, config, capture=True, timeout=600):
    ossutil = get_ossutil_path()
    cmd = [ossutil, "-c", config] + args
    try:
        if capture:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=timeout, stdin=subprocess.DEVNULL)
        # Let ossutil cp write directly to the terminal so native byte progress is visible.
        return subprocess.run(cmd, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None


# ─── 文件名解析 / OSS 路径 ────────────────────────────────────
def parse_name(filename: str, kind: str):
    m = _HEAD_CAM_RE.match(filename) if kind == "head_cam" else _TELEOP_RE.match(filename)
    if not m:
        return None
    d = m.group("date")
    return {
        "deviceNo": m.group("dev"),
        "captureDate": f"{d[:4]}-{d[4:6]}-{d[6:8]}",
        "yymmdd": d[2:8],
    }


def object_layout_for(kind: str, dev: str, yymmdd: str, filename: str) -> dict:
    if kind == "teleop":
        relative_path = f"Orin/{yymmdd}/{dev}_device/{filename}"
        return {
            "ossPath": f"oss://{OSS_BUCKET}/original_data/teleoperation/{relative_path}",
            "rootPrefix": "original_data",
            "dataDomain": "teleoperation",
            "dataKind": "orin",
            "relativePath": relative_path,
        }
    if kind == "head_cam":
        relative_path = f"{yymmdd}/{dev}_device/{filename}"
        return {
            "ossPath": f"oss://{OSS_BUCKET}/original_data/ego_human_data/head_cam/{relative_path}",
            "rootPrefix": "original_data",
            "dataDomain": "ego_human_data",
            "dataKind": "head_cam",
            "relativePath": relative_path,
        }
    raise ValueError(f"未知 kind: {kind}")


def oss_path_for(kind: str, dev: str, yymmdd: str, filename: str) -> str:
    return object_layout_for(kind, dev, yymmdd, filename)["ossPath"]


def _parse_oss_time(s: str) -> str:
    if not s:
        return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    try:
        dt = datetime.strptime(s.strip(), "%Y-%m-%d %H:%M:%S %z")
        return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    except ValueError:
        return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


# ─── OSS 上传 / 查询 ─────────────────────────────────────────
def oss_stat(oss_dest: str, config: str):
    """查 OSS 对象是否存在；存在返回 {etag,lastModified}，否则 None。"""
    r = run_ossutil(["stat", oss_dest], config)
    if not r or r.returncode != 0:
        return None
    info = {}
    for line in (r.stdout or "").splitlines():
        if " :" in line:
            k, _, v = line.partition(" :")
            info[k.strip()] = v.strip()
    etag = info.get("Etag", "").strip('"')
    if not etag:
        return None
    return {"etag": etag, "lastModified": _parse_oss_time(info.get("Last-Modified", ""))}


def oss_upload(local: str, oss_dest: str, config: str):
    """上传单文件到 OSS，成功返回 {etag,lastModified}，失败 None。"""
    for attempt in range(1, MAX_RETRIES + 1):
        r = run_ossutil(["cp", "-f", local, oss_dest], config,
                        capture=False, timeout=None)
        if r and r.returncode == 0:
            return oss_stat(oss_dest, config)
        if r is None:
            err = "ossutil执行超时"
        else:
            detail = (r.stderr or r.stdout or "").strip()
            err = (detail[:120] if detail else
                   f"ossutil退出码={r.returncode}（详见上方ossutil输出）")
        log(f"    OSS 上传失败(第{attempt}次): {err}")
        if attempt < MAX_RETRIES:
            time.sleep(2 * attempt)
    return None


# ─── 数据平台 API ─────────────────────────────────────────────
def validate_object_payload(payload: dict):
    """只允许提交接口示例中列出的 raw_data_object 字段。"""
    keys = set(payload)
    extra = sorted(keys - RAW_DATA_OBJECT_FIELD_SET)
    missing = [k for k in RAW_DATA_OBJECT_FIELDS if k not in payload]
    if extra or missing:
        parts = []
        if extra:
            parts.append(f"多余字段: {', '.join(extra)}")
        if missing:
            parts.append(f"缺少字段: {', '.join(missing)}")
        raise ValueError("payload字段不符合接口示例 (" + "; ".join(parts) + ")")


def post_object(payload: dict):
    validate_object_payload(payload)
    body = json.dumps(payload).encode("utf-8")
    req = urlrequest.Request(OBJECTS_URL, data=body, method="POST",
                             headers={"Content-Type": "application/json"})
    try:
        with urlrequest.urlopen(req, timeout=30) as resp:
            d = json.loads(resp.read().decode("utf-8"))
            if d.get("code") == 200:
                return d.get("data")
            log(f"    API 非200: {d}")
    except (HTTPError, URLError, json.JSONDecodeError, TimeoutError, OSError) as e:
        log(f"    API 请求异常: {e}")
    return None


# ─── 单文件处理 ───────────────────────────────────────────────
def process_file(path: Path, kind: str, skip_existing: bool, config: str) -> dict:
    """单文件：解析 meta → 确认 OSS 已存在或上传成功 → POST 写库。

    返回 {file, ok, status(uploaded|skipped|empty|failed), error, ossPath, duration, size}
    """
    name = path.name
    meta = parse_name(name, kind)
    if not meta:
        return {"file": name, "ok": False, "status": "failed",
                "error": "文件名无法解析设备/日期", "size": 0}

    try:
        size = int(path.stat().st_size)
    except OSError:
        size = 0
    else:
        if size < 2048:
            return {"file": name, "ok": True, "status": "empty",
                    "error": f"mcap解析失败: 文件过小({size}B)", "size": size}

    try:
        info = summarize(str(path))
    except Exception:
        # 自动修复损坏的 mcap 文件
        tmp = str(path) + ".recovered"
        try:
            if repair_mcap_file(str(path), tmp, log_cb=log):
                os.replace(tmp, str(path))
                size = int(path.stat().st_size)
                info = summarize(str(path))
            else:
                raise RuntimeError("自动修复失败")
        except Exception as e2:
            detail = str(e2).strip() or type(e2).__name__
            try:
                Path(tmp).unlink()
            except Exception:
                pass
            return {"file": name, "ok": False, "status": "failed",
                    "error": f"mcap解析失败: {detail}", "size": size}

    layout = object_layout_for(kind, meta["deviceNo"], meta["yymmdd"], name)
    oss_dest = layout["ossPath"]
    skipped_oss = False

    if skip_existing:
        up = oss_stat(oss_dest, config)
        if up:
            skipped_oss = True
        else:
            up = None
    else:
        up = None

    if not skipped_oss:
        up = oss_upload(str(path), oss_dest, config)
    if not up:
        return {"file": name, "ok": False, "status": "failed",
                "error": "OSS上传失败", "size": size}
    if skipped_oss:
        time.sleep(0.2)

    payload = {
        "ossPath": oss_dest,
        "rootPrefix": layout["rootPrefix"],
        "dataDomain": layout["dataDomain"],
        "dataKind": layout["dataKind"],
        "captureDate": meta["captureDate"],
        "deviceNo": meta["deviceNo"],
        "deviceDir": f"{meta['deviceNo']}_device",
        "sessionName": meta["yymmdd"],
        "relativePath": layout["relativePath"],
        "objectRole": "data",
        "fileFormat": "mcap",
        "fileSize": size,
        "duration": info["duration_sec"],          # float, 毫秒精度
        "etag": up["etag"],
        "storageClass": "Standard",
        "lastModified": up["lastModified"],
        "ossMetadata": {},
        "parsedMetadata": info,                     # 整份 mcap meta
    }
    try:
        data = post_object(payload)
    except ValueError as e:
        return {"file": name, "ok": False, "status": "failed",
                "error": str(e), "ossPath": oss_dest, "size": size}
    if data is None:
        return {"file": name, "ok": False, "status": "failed",
                "error": "写库失败(已上传OSS)", "ossPath": oss_dest, "size": size}

    status = "skipped" if skipped_oss else "uploaded"
    tag = "OSS已存在/DB完成" if skipped_oss else "OSS+DB完成"
    log(f"  ✓ {name}: {tag} inserted={data.get('inserted')} updated={data.get('updated')}")
    return {"file": name, "ok": True, "status": status, "ossPath": oss_dest,
            "duration": info["duration_sec"], "size": size}


# ─── 交互辅助 ─────────────────────────────────────────────────
def input_choice(prompt: str, options: list) -> int:
    print(f"\n{prompt}")
    for i, opt in enumerate(options):
        print(f"  [{i}] {opt}")
    while True:
        try:
            idx = int(input("请选择: ").strip())
            if 0 <= idx < len(options):
                return idx
        except EOFError:
            print("\n[取消] 输入结束, 已取消")
            sys.exit(0)
        except ValueError:
            pass
        print("无效选择，请重新输入")


def confirm(prompt: str) -> bool:
    try:
        return input(f"{prompt} (y/N): ").strip().lower() == "y"
    except EOFError:
        return False


def human_size(size_bytes: float) -> str:
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if abs(size_bytes) < 1024:
            return f"{size_bytes:.1f}{unit}"
        size_bytes /= 1024
    return f"{size_bytes:.1f}PB"


# ─── 存储设备检测 / 扫描 ──────────────────────────────────────
def detect_storage_devices() -> list:
    system = platform.system().lower()
    mounts = []
    if system == "windows":
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            drive = f"{letter}:\\"
            if os.path.isdir(drive):
                mounts.append(drive)
    else:
        try:
            r = subprocess.run(
                ["lsblk", "-o", "MOUNTPOINT,RM,TRAN", "-n", "-l"],
                capture_output=True, text=True, timeout=10)
            for line in r.stdout.splitlines():
                line = line.strip()
                if not line:
                    continue
                parts = line.split()
                mountpoint = parts[0] if len(parts) > 0 else ""
                rm = parts[1] if len(parts) > 1 else ""
                tran = parts[2] if len(parts) > 2 else ""
                if mountpoint and mountpoint != "/" and (rm == "1" or tran == "usb"):
                    mounts.append(mountpoint)
        except Exception:
            pass
    return [m for m in mounts if not any(kw in m for kw in EXCLUDE_KEYWORDS)]


def device_rm(mount: str):
    """返回挂载点源设备的 removable 标志：0=固定硬盘(如 Elements), 1=SD卡/可移除；无法判定返回 None。"""
    try:
        r = subprocess.run(["df", "--output=source", mount],
                           capture_output=True, text=True, timeout=10)
        lines = [l for l in r.stdout.splitlines()[1:] if l.strip()]
        if not lines:
            return None
        src = lines[0].strip()
        r2 = subprocess.run(["lsblk", "-no", "RM", src],
                            capture_output=True, text=True, timeout=10)
        for line in r2.stdout.splitlines():
            line = line.strip()
            if line in ("0", "1"):
                return int(line)
    except Exception:
        pass
    return None


def scan_mcap_files(mount: str, kind: str) -> list:
    """递归扫描挂载点下能被 parse_name 解析的 .mcap 文件。"""
    files = []
    try:
        for p in sorted(Path(mount).rglob("*.mcap")):
            if p.is_file() and parse_name(p.name, kind):
                files.append(p)
    except (PermissionError, OSError):
        pass
    return files


# ─── main ─────────────────────────────────────────────────────
def main():
    kind = os.getenv("UMCAP_DEFAULT_KIND")
    if kind not in ("teleop", "head_cam"):
        print("错误: 未固化 kind。请运行编译产物, 或: UMCAP_DEFAULT_KIND=teleop|head_cam python3 upload_mcap.py")
        sys.exit(1)

    log("=" * 52)
    log(f"  {KIND_LABEL[kind]} 数据上传  (写 raw_data_object)")
    log("=" * 52)
    log(f"日志文件: {LOG_FILE}")
    log("")

    log("[信息] 正在准备凭证...")
    ak = decrypt_credential(_T_AK_ENC)
    sk = decrypt_credential(_T_SK_ENC)
    if not ak or not sk:
        log("[错误] OSS 凭证解密失败")
        sys.exit(1)
    config = setup_ossutil_config(ak, sk)

    try:
        _run(kind, config)
    finally:
        try:
            os.unlink(config)
        except OSError:
            pass


def _run(kind: str, config: str):
    log("[检测] 正在扫描存储设备...")
    devices = detect_storage_devices()
    if not devices:
        log("[错误] 未检测到外部存储设备")
        sys.exit(1)

    log(f"[信息] 识别到 {len(devices)} 张存储设备：")
    for d in devices:
        device_name = os.path.basename(d.rstrip("/\\"))
        log(f"  - {device_name} ({d})")

    # 按盘类型分流: teleop→固定硬盘(RM=0, 如 Elements), head_cam→SD卡(RM=1)
    want_rm = 0 if kind == "teleop" else 1
    want_label = "固定硬盘" if kind == "teleop" else "SD卡"
    eligible, excluded = [], []
    for d in devices:
        rm = device_rm(d)
        if rm is None or rm == want_rm:
            eligible.append(d)
        else:
            excluded.append((d, rm))
    if excluded:
        log(f"[信息] 按盘类型筛选 ({kind} 仅 {want_label}):")
        for d, rm in excluded:
            other = "SD卡" if rm == 1 else "固定硬盘"
            device_name = os.path.basename(d.rstrip("/\\"))
            log(f"  - 跳过 {device_name} ({other})")
    devices = eligible
    if not devices:
        log(f"[错误] 未检测到适合 {kind} 的存储设备 ({want_label})")
        sys.exit(1)

    if len(devices) > 1:
        idx = input_choice("请选择要上传的设备:",
                           ["全部"] + [os.path.basename(d.rstrip("/\\")) for d in devices])
        selected = devices if idx == 0 else [devices[idx - 1]]
    else:
        selected = devices

    log("")
    delete_incomplete = input_choice("修复 .mcap.incomplete 后是否删除原始文件?",
                                     ["删除原始文件", "保留原始文件"]) == 0

    log("")
    log("[修复] 正在检查不完整的 mcap 文件 (.mcap.incomplete)...")
    total_repaired = 0
    for mp in selected:
        device_name = os.path.basename(mp.rstrip("/\\"))
        repaired = scan_and_repair(mp, delete_original=delete_incomplete, log_cb=log)
        total_repaired += len(repaired)
    if total_repaired:
        log(f"[修复] 共修复 {total_repaired} 个文件")
    else:
        log("[修复] 未发现不完整的 mcap 文件")

    log("")
    for mp in selected:
        device_name = os.path.basename(mp.rstrip("/\\"))
        log(f"[扫描] {device_name} ...")
    files = []
    for mp in selected:
        files.extend(scan_mcap_files(mp, kind))

    if not files:
        log(f"[错误] 存储设备上未找到 {kind} 的 mcap 文件")
        sys.exit(1)

    # 去重（同文件可能被多设备路径命中）
    seen, uniq = set(), []
    for f in files:
        key = str(f.resolve())
        if key not in seen:
            seen.add(key)
            uniq.append(f)
    files = uniq

    total_bytes = sum(f.stat().st_size for f in files)
    total_human = human_size(total_bytes)
    dev_counts = {}
    for f in files:
        dn = parse_name(f.name, kind)["deviceNo"]
        dev_counts[dn] = dev_counts.get(dn, 0) + 1

    log(f"\n[信息] 共 {len(files)} 个 mcap 文件, {total_human}")
    log("[信息] 设备分布：")
    for dn in sorted(dev_counts):
        log(f"  设备 {dn}: {dev_counts[dn]} 个文件")

    action = input_choice("OSS 上已存在的文件如何处理?",
                          ["跳过已存在（不重传, 仍写库meta）", "全部覆盖（重新上传+成功后写库）"])
    skip_existing = action == 0

    clean = input_choice("上传成功后是否删除本地文件?", ["保留本地文件", "删除本地文件"])
    auto_clean = clean == 1

    log(f"\n{'=' * 52}")
    log("  上传计划")
    log(f"{'=' * 52}")
    log(f"  文件数   : {len(files)}")
    log(f"  总大小   : {total_human}")
    log(f"  已存在策略: {'跳过重传(仍写库meta)' if skip_existing else '覆盖(上传后写库)'}")
    log(f"  本地清理 : {'删除' if auto_clean else '保留'}")
    log(f"  并发数   : {WORKERS}")
    log(f"{'=' * 52}")

    if not confirm("\n确认开始?"):
        log("[取消] 已取消")
        sys.exit(0)

    log(f"\n[{now_ts()}] [上传] 开始上传 (共 {len(files)} 个文件)\n")

    uploaded = skipped = empty = failed = 0
    done_bytes = 0
    fail_list = []
    empty_list = []
    start = time.time()

    cnt_lock = threading.Lock()

    def on_done(r):
        nonlocal uploaded, skipped, empty, failed, done_bytes
        with cnt_lock:
            if r["status"] == "empty":
                empty += 1
                empty_list.append((r["file"], r.get("error", "")))
            elif r["ok"]:
                if r["status"] == "skipped":
                    skipped += 1
                else:
                    uploaded += 1
            else:
                failed += 1
                fail_list.append((r["file"], r.get("error", "")))
            done_bytes += r.get("size", 0)
            n = uploaded + skipped + empty + failed
            pct = done_bytes * 100 // total_bytes if total_bytes else 0
            log(f"[进度] {n}/{len(files)} ({human_size(done_bytes)}/{total_human}, {pct}%) "
                f"- 新传{uploaded} 跳过{skipped} 空文件{empty} 失败{failed}")

    with ThreadPoolExecutor(max_workers=WORKERS) as ex:
        futs = {ex.submit(process_file, f, kind, skip_existing, config): f for f in files}
        for fut in as_completed(futs):
            try:
                r = fut.result()
            except Exception as e:
                r = {"file": "?", "ok": False, "status": "failed", "error": str(e), "size": 0}
            if r["status"] == "empty":
                log(f"  ! {r['file']}: {r.get('error')}")
            elif not r["ok"]:
                log(f"  ✗ {r['file']}: {r.get('error')}")
            on_done(r)

    elapsed = int(time.time() - start)
    speed = human_size(total_bytes // elapsed) + "/s" if elapsed > 0 and total_bytes else "N/A"

    log(f"\n{'=' * 52}")
    log(f"[汇总] 新传 {uploaded}  /  跳过(OSS已存在) {skipped}  /  空文件 {empty}  /  失败 {failed}  /  共 {len(files)}")
    log(f"[汇总] 数据量: {total_human}  耗时: {elapsed // 60}m{elapsed % 60}s  平均速率: {speed}")

    if auto_clean and (uploaded + skipped) > 0:
        log("")
        for f in files:
            if any(fn == f.name for fn, _ in fail_list) or any(fn == f.name for fn, _ in empty_list):
                continue
            try:
                f.unlink()
                log(f"[清理] 已删除 {f.name}")
                parent = f.parent
                for _ in range(3):
                    try:
                        parent.rmdir()
                    except OSError:
                        break
                    parent = parent.parent
            except OSError:
                pass

    if fail_list:
        log("失败列表:")
        for fn, err in fail_list:
            log(f"  - {fn}: {err}")
        sys.exit(1)

    if empty_list:
        log("空文件列表:")
        for fn, err in empty_list:
            log(f"  - {fn}: {err}")


if __name__ == "__main__":
    main()
