#!/usr/bin/env python3
"""Independent upload core, derived from develop; invoked by upload_server.py.
No GUI/config-path entry point. Invalid MCAP files fail without modification.
"""
import argparse
import base64
import errno
import hashlib
import json
import os
import oss2
import platform
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from urllib import request as urlrequest
from urllib.parse import quote, unquote
# 同级 mcap_parser.py（仓库非 Python 包）
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcap_parser import extract_episode_quality, summarize  # noqa: E402
from hdf5_parser import summarize as summarize_hdf5  # noqa: E402

# ─── 配置 ─────────────────────────────────────────────────────
OSS_BUCKET = "ss-oss-data-platform"
OSSUTIL_VERSION = "1.7.19"
MAX_RETRIES = 3
WORKERS = 1                      # 串行上传，避免 OSS 并发导致超时或冲突
OSS_CONNECT_TIMEOUT = 30         # 单次建连最多等待 30 秒
OSS_READ_TIMEOUT = 120           # 连接无数据 2 分钟后交给重试流程
OSS_PROCESS_TIMEOUT = 3600       # 最终保护：单文件上传进程最多运行 1 小时
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
)
# OSS 凭证 (AES-256-CBC + PBKDF2 混淆, 与 teleop_upload 共用同一 bucket 账号)
_K = "ssim_record_upload_2026_v1"
_T_AK_ENC = "U2FsdGVkX1/VzDWejYu8i2uBYzcbFr7v6vGCSj/yI0AMVocfXuDkmBzH5TVN7oEA"
_T_SK_ENC = "U2FsdGVkX18qVUiJHdX5br00N0CDJg3QoAtPGr1ILoEeU/nrO8lRsG3CjRyWYnvM"
OSS_ENDPOINT = "oss-cn-shanghai.aliyuncs.com"

RUNTIME_DIR = Path(os.environ.get("NEW_UPLOAD_RUNTIME", str(Path(__file__).resolve().parent / "runtime")))
LOG_DIR = RUNTIME_DIR / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_FILE = LOG_DIR / f"upload_mcap_{time.strftime('%Y%m%d_%H%M%S')}_{os.getpid()}.log"

YMDHM_DIR = RUNTIME_DIR / "ymdhm"
YMDHM_DIR.mkdir(parents=True, exist_ok=True)

_ymdhm_path = None

_LOG_LOCK = threading.Lock()
_LOG_CALLBACKS = []


def register_log_callback(cb):
    """注册日志回调（GUI 等界面实时接收日志行）。返回取消函数。"""
    _LOG_CALLBACKS.append(cb)

    def unregister():
        try:
            _LOG_CALLBACKS.remove(cb)
        except ValueError:
            pass
    return unregister

# 文件名 → 设备号/日期
# 规则文件:
#   {brand}_{machine}_{dev}_{YYYYMMDD}_{task?}_{dedup_seq}.mcap/hdf5/parquet
#   {brand}_{machine}_{dev}_{task}_{YYYYMMDD}_{HHMMSS}_{short_uuid}_{slice_index}.mcap/hdf5/parquet
# head_cam: SSIEGO_Cam_{dev}_{YYYYMMDD}_*   teleop 老格式: m1_{dev}_camera_{YYYYMMDD}_*
_HEAD_CAM_RE = re.compile(r"^SSIEGO_Cam_(?P<dev>[A-Za-z0-9]+)_(?P<date>\d{8})_")
_TELEOP_RE = re.compile(r"^m1_(?P<dev>\w+?)_camera_(?P<date>\d{8})_")
_REGULAR_DATA_RE = re.compile(
    r"^(?P<brand>[^_]+)_(?P<machine>[^_]+)_(?P<dev>[^_]+)_(?P<date>\d{8})"
    r"(?:_(?P<task>.+?))?_(?P<seq>\d+)\.")
# 新版切片文件：时间精确到秒；short_uuid 是 4 位十六进制哈希码，
# 用于避免系统时钟回拨后重用历史时间戳；slice_index 是从 0 开始的切片序号。
_CUT_DATA_RE = re.compile(
    r"^(?P<brand>[^_]+)_(?P<machine>[^_]+)_(?P<dev>[^_]+)_(?P<task>.+)_"
    r"(?P<date>\d{8})_(?P<time>\d{6})_"
    r"(?P<short_uuid>[0-9a-f]{4})_(?P<seq>\d+)\."
    r"(?:mcap|hdf5|h5|parquet)$", re.IGNORECASE)
# 文件名中的采集时间不能作为任务名上传：HHMMSS、YYYYMMDD、YYYYMMDDHHMMSS。
_TIMESTAMP_TASK_RE = re.compile(r"^(?:\d{6}|\d{8}|\d{14})$")


def find_config():
    exe = Path(sys.argv[0]).resolve()
    # Windows 构建使用 "程序名.json"，而不是容易误点的 "程序.exe.json"。
    # config.json 及旧命名仍保留兼容。
    for cfg_path in (
        exe.parent / "config.json",
        exe.parent / (exe.stem + ".json"),
        exe.parent / (exe.name + ".json"),
    ):
        if not cfg_path.is_file():
            continue
        try:
            # PowerShell 5.1 的 Out-File -Encoding utf8 会写入 BOM；
            # utf-8-sig 同时兼容带 BOM 与不带 BOM 的 UTF-8 JSON。
            return json.loads(cfg_path.read_text(encoding="utf-8-sig"))
        except Exception:
            continue
    return None


def log(msg: str):
    """每行输出前置绝对时间（本地 wall-clock）。线程安全。"""
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    with _LOG_LOCK:
        try:
            print(line, flush=True)
        except UnicodeEncodeError:
            encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
            print(line.encode(encoding, errors="replace").decode(encoding), flush=True)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        for cb in _LOG_CALLBACKS:
            try:
                cb(line)
            except Exception:
                pass


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
    base = RUNTIME_DIR / "bin"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _bundled_ossutil():
    if '__compiled__' in globals():
        return Path(__file__).resolve().parent / 'ossutil' / _ossutil_name()
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS) / "ossutil" / _ossutil_name()
    payload = Path(__file__).resolve().parent / ".build" / "ossutil" / _ossutil_name()
    return payload if payload.exists() else None


def _ossutil_download_url():
    """返回 ossutil 下载 URL（v1.7.19+ 统一 ZIP 格式）。"""
    system = platform.system().lower()
    machine = platform.machine().lower()
    v = OSSUTIL_VERSION
    if system == "windows":
        return f"https://gosspublic.alicdn.com/ossutil/{v}/ossutil-v{v}-windows-amd64.zip"
    if "arm" in machine or "aarch" in machine:
        return f"https://gosspublic.alicdn.com/ossutil/{v}/ossutil-v{v}-linux-arm64.zip"
    return f"https://gosspublic.alicdn.com/ossutil/{v}/ossutil-v{v}-linux-amd64.zip"


def get_ossutil_path():
    """获取 ossutil：优先内嵌包，否则从 CDN 下载 ZIP 并解压到缓存目录。"""
    import zipfile
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

    url = _ossutil_download_url()
    zip_name = url.rsplit("/", 1)[-1]
    zip_path = _runtime_bin_dir() / zip_name
    log(f"[信息] 下载 ossutil {zip_name} (~12MB)...")
    urlrequest.urlretrieve(url, zip_path)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(_runtime_bin_dir())
    zip_path.unlink()
    # ZIP 解压后二进制可能在子目录中，移动至预期位置
    if not ossutil_path.exists():
        for root, _dirs, files in os.walk(_runtime_bin_dir()):
            for f in files:
                if f == name:
                    src = Path(root) / f
                    if src != ossutil_path:
                        shutil.move(str(src), str(ossutil_path))
                    break
    if platform.system().lower() != "windows":
        os.chmod(ossutil_path, 0o755)
    return str(ossutil_path)


def setup_ossutil_config(ak, sk):
    cfg = tempfile.NamedTemporaryFile(mode="w", suffix=".ossutil", delete=False,
                                      prefix=".ossutil_")
    cfg.write(f"[Credentials]\nlanguage=CH\nendpoint={OSS_ENDPOINT}\n"
              f"accessKeyID={ak}\naccessKeySecret={sk}\n")
    cfg.close()
    if platform.system().lower() != "windows":
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
    """从文件名解析元数据，解析不了返回 None。

    返回: brand?, dev(设备号), machine(机器名), captureDate, yymmdd, task?, seq?
    - 规则格式: {brand}_{machine}_{dev}_{date}_{task?}_{dedup_seq}
    - 切片格式: {brand}_{machine}_{dev}_{task}_{YYYYMMDD}_{HHMMSS}_{short_uuid}_{slice_index}
    - teleop 老格式 m1_xxx_camera_* → machine 恒为 "m1"
    """
    if kind == "head_cam":
        m = _HEAD_CAM_RE.match(filename)
    else:
        m = (_TELEOP_RE.match(filename) or _CUT_DATA_RE.match(filename)
             or _REGULAR_DATA_RE.match(filename))
    if not m:
        return None
    is_regular = m.re in (_CUT_DATA_RE, _REGULAR_DATA_RE)
    d = m.group("date")
    result = {
        "dev": m.group("dev"),
        "captureDate": f"{d[:4]}-{d[4:6]}-{d[6:8]}",
        "yymmdd": d[2:8],
    }
    if kind != "head_cam":
        result["machine"] = m.group("machine") if is_regular else "m1"
    if is_regular:
        result["brand"] = m.group("brand")
        # 保留完整任务名（包括下划线和数字后缀）；文件序号由正则单独解析。
        task = m.group("task") or ""
        if task and not _TIMESTAMP_TASK_RE.fullmatch(task):
            result["task"] = task
        result["seq"] = m.group("seq")
    return result


def _uses_plain_device_identity(kind: str, brand: str, config_mode: bool) -> bool:
    """Whether OSS paths/metadata should use only the raw device number."""
    return (kind == "head_cam" or not config_mode or
            (kind == "teleop" and (brand or "Orin").casefold() == "orin"))


def object_layout_for(kind: str, brand: str, yymmdd: str, dev: str,
                      filename: str, config_mode: bool, machine: str = "") -> dict:
    """生成 OSS 对象布局。

    - 配置模式(config_mode=True, 产物2): teleoperation/{品牌}/{yymmdd}/
      Orin 使用 {dev}_device/{file}；其他品牌使用
      {MACHINE}_{dev}_device/{file}；MACHINE 使用大写机器名，与 deviceDir 一致
      dataKind 使用品牌名
    - 设备检测模式(config_mode=False, 产物1): teleoperation/Orin/{yymmdd}/{dev}_device/{file}
    - head_cam: 路径与品牌无关, 两种模式一致
    """
    plain_device = _uses_plain_device_identity(kind, brand, config_mode)
    device_dir = (f"{dev}_device" if plain_device
                  else f"{machine.upper()}_{dev}_device")
    if kind == "head_cam":
        relative_path = f"{yymmdd}/{device_dir}/{filename}"
        return {
            "ossPath": f"oss://{OSS_BUCKET}/original_data/ego_human_data/head_cam/{relative_path}",
            "rootPrefix": "original_data",
            "dataDomain": "ego_human_data",
            "dataKind": "head_cam",
            "relativePath": relative_path,
        }
    if config_mode:
        b = brand or "Orin"
        relative_path = f"{b}/{yymmdd}/{device_dir}/{filename}"
    else:
        relative_path = f"Orin/{yymmdd}/{device_dir}/{filename}"
    return {
        "ossPath": f"oss://{OSS_BUCKET}/original_data/teleoperation/{relative_path}",
        "rootPrefix": "original_data",
        "dataDomain": "teleoperation",
        "dataKind": b if config_mode else "Orin",
        "relativePath": relative_path,
    }


def oss_path_for(kind: str, brand: str, yymmdd: str, dev: str,
                 filename: str, config_mode: bool, machine: str = "") -> str:
    return object_layout_for(kind, brand, yymmdd, dev, filename, config_mode,
                             machine)["ossPath"]


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
    if r is None:
        raise RuntimeError("OSS 对象状态查询超时")
    if r.returncode != 0:
        detail = (r.stderr or "") + (r.stdout or "")
        if "NoSuchKey" in detail:
            return None
        raise RuntimeError("OSS 对象状态查询失败，未执行覆盖上传")
    info = {}
    for line in (r.stdout or "").splitlines():
        if " :" in line:
            k, _, v = line.partition(" :")
            info[k.strip()] = v.strip()
    etag = info.get("Etag", "").strip('"')
    if not etag:
        raise RuntimeError("OSS 对象状态缺少 Etag，未执行覆盖上传")
    headers = {k.lower(): v for k, v in info.items()}
    digest = headers.get("x-oss-meta-sha256", "").lower()
    return {"etag": etag, "lastModified": _parse_oss_time(info.get("Last-Modified", "")),
            "sha256": digest if re.fullmatch(r"[0-9a-f]{64}", digest) else None}


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()



def oss_upload(local: str, oss_dest: str, config: str):
    """上传单文件到 OSS，成功返回 {etag,lastModified}，失败 None。"""
    for attempt in range(1, MAX_RETRIES + 1):
        size = human_size(os.path.getsize(local)) if os.path.isfile(local) else "未知大小"
        log(f"  ↑ 开始上传 {os.path.basename(local)} ({size})，第 {attempt}/{MAX_RETRIES} 次")
        # ossutil 默认读超时 20 分钟、内部重试 10 次，断网时看起来会永久卡住。
        # 缩短连接/无数据超时和内部重试；外层重试会继续复用 checkpoint 断点。
        r = run_ossutil([
            "cp", "-f", local, oss_dest,
            f"--connect-timeout={OSS_CONNECT_TIMEOUT}",
            f"--read-timeout={OSS_READ_TIMEOUT}",
            "--retry-times=3",
        ], config, capture=False, timeout=OSS_PROCESS_TIMEOUT)
        if r and r.returncode == 0:
            return oss_stat(oss_dest, config)
        if r is None:
            err = f"ossutil执行超过 {OSS_PROCESS_TIMEOUT // 60} 分钟，已终止本次并准备断点重试"
        else:
            detail = (r.stderr or r.stdout or "").strip()
            err = (detail[:120] if detail else
                   f"ossutil退出码={r.returncode}（详见上方ossutil输出）")
        log(f"    OSS 上传失败(第{attempt}次): {err}")
        if attempt < MAX_RETRIES:
            time.sleep(2 * attempt)
    return None


# ─── 同级附件上传与目录关联 ──────────────────────────────────
RELATED_FILES_HEADER = "x-oss-meta-relatedfiles"
# Legacy fields are read only to recover existing attachment directories.
ATTACHMENT_HEADERS = (RELATED_FILES_HEADER, "x-oss-meta-yamlpaths", "x-oss-meta-jsonpaths")


def data_group(filename):
    """Only MCAP has slice suffixes; HDF5/H5 keep their complete stem."""
    path = Path(filename)
    stem = path.stem
    sliced = path.suffix.lower() == ".mcap" and re.search(r"_\d+$", stem)
    return re.sub(r"_\d+$", "", stem) if sliced else stem, bool(sliced)


def attachment_directories(headers, parent):
    """Decode each URI after splitting, so encoded commas remain unambiguous."""
    directories = set()
    normalized = {k.lower(): v for k, v in headers.items()}
    for field in ATTACHMENT_HEADERS:
        for value in (normalized.get(field) or "").split(","):
            if not value:
                continue
            uri = unquote(value)
            directory, _, filename = uri.rpartition("/")
            if (not filename or not directory.startswith(parent)
                    or not directory[len(parent):]
                    or "/" in directory[len(parent):]):
                raise ValueError("已有附件路径不在数据同级附件目录中")
            directories.add(directory)
    return directories


class AttachmentUploader:
    """One instance per serial upload batch; successful sidecars are reused."""

    def __init__(self, cache_dir, recover_directories, upload, log):
        self.cache_dir = Path(cache_dir)
        self.recover_directories = recover_directories
        self.upload = upload
        self.log = log
        self.directories = {}
        self.completed = {}

    def prepare(self, path, oss_dest):
        """Resolve before overwriting the data object and its old metadata."""
        if path.suffix.lower() not in (".mcap", ".hdf5", ".h5"):
            return []
        files = sorted((p for p in path.parent.iterdir()
                        if p.is_file() and p.suffix.lower() in (".yaml", ".yml", ".json")),
                       key=lambda p: p.name)
        if not files:
            return []
        parent, _, filename = oss_dest.rpartition("/")
        parent += "/"
        stem, sliced = data_group(filename)
        identity = parent + stem + ("|mcap" if sliced else "|" + Path(filename).suffix.lower())
        directory = self.directories.get(identity)
        if directory is None:
            cache_file = self.cache_dir / (hashlib.sha256(identity.encode()).hexdigest() + ".cache")
            cached = None
            try:
                cached = cache_file.read_text(encoding="utf-8").strip()
                if (not cached.startswith(parent) or not cached[len(parent):]
                        or "/" in cached[len(parent):]):
                    cached = None
            except FileNotFoundError:
                pass
            # OSS is authoritative, including recovery on a different computer.
            remote = self.recover_directories(oss_dest, stem, sliced)
            if len(remote) > 1:
                raise ValueError("同组数据已有多个附件目录，请先统一关联后重试")
            directory = next(iter(remote), None) or cached
            if directory is None:
                siblings = {p.name
                            for p in path.parent.iterdir()
                            if p.is_file() and p.suffix.lower() == ".mcap"
                            and data_group(p.name) == (stem, True)} if sliced else set()
                directory = parent + (str(uuid.uuid4()) if len(siblings) > 1 else stem)
            # Persist before the first write to OSS, so failures retain the UUID.
            self.cache_dir.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(dir=self.cache_dir, suffix=".tmp")
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as stream:
                    stream.write(directory)
                os.replace(temporary, cache_file)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
            self.directories[identity] = directory
        return [(p, directory + "/" + p.name) for p in files]

    def upload_headers(self, plan):
        paths = []
        for path, destination in plan:
            stat = path.stat()
            fingerprint = (str(path.resolve()), stat.st_size, stat.st_mtime_ns)
            if self.completed.get(destination) != fingerprint:
                self.log(f"  [附件] {path.name} → {destination}")
                if not self.upload(str(path), destination):
                    raise RuntimeError(f"附件上传失败: {path.name}")
                self.completed[destination] = fingerprint
            # Encode %, commas, Unicode and other unsafe header characters.
            paths.append(quote(destination, safe=":/"))
        return {RELATED_FILES_HEADER: ",".join(paths)} if paths else {}


def recover_attachment_directories(oss_dest: str, stem: str, sliced: bool) -> set:
    """Read existing sidecar associations before cp can replace object metadata."""
    ak, sk = decrypt_credential(_T_AK_ENC), decrypt_credential(_T_SK_ENC)
    bucket = oss2.Bucket(oss2.Auth(ak, sk), f"https://{OSS_ENDPOINT}", OSS_BUCKET)
    root = f"oss://{OSS_BUCKET}/"
    key = oss_dest[len(root):]
    parent_key = key.rpartition("/")[0] + "/"
    keys = {key}
    if sliced:
        # Include slices no longer present locally (auto-clean or another machine).
        prefix = parent_key + stem + "_"
        pattern = re.compile(re.escape(prefix) + r"\d+\.mcap$", re.IGNORECASE)
        keys.update(obj.key for obj in oss2.ObjectIterator(bucket, prefix=prefix)
                    if pattern.fullmatch(obj.key))
    directories = set()
    for candidate in sorted(keys):
        try:
            headers = bucket.head_object(candidate).headers
        except oss2.exceptions.NoSuchKey:
            continue
        # Permission/network failures must not silently produce a new UUID.
        directories.update(attachment_directories(headers, root + parent_key))
    return directories


def attachment_uploader(ctx: dict, config: str) -> AttachmentUploader:
    if "attachment_uploader" not in ctx:
        ctx["attachment_uploader"] = AttachmentUploader(
            RUNTIME_DIR / "attachments",
            recover_attachment_directories,
            lambda local, destination: oss_upload(local, destination, config), log)
    return ctx["attachment_uploader"]


# ─── OSS 对象元数据写入 ───────────────────────────────────────
def set_oss_metadata(oss_key: str, payload: dict, extra_headers: dict = None) -> bool:
    """将 DB 入库全量参数写入 OSS 对象的自定义元数据（x-oss-meta-*）。

    通过 oss2 copy_object 同 key 拷贝实现元数据写入，无数据搬运开销。
    服务端扫描 OSS 对象元数据完成 DB 入库，客户端不再直连 API。
    """
    try:
        ak = decrypt_credential(_T_AK_ENC)
        sk = decrypt_credential(_T_SK_ENC)
        auth = oss2.Auth(ak, sk)
        bucket = oss2.Bucket(auth, f"https://{OSS_ENDPOINT}", OSS_BUCKET)

        headers = {"x-oss-metadata-directive": "REPLACE"}
        for field in RAW_DATA_OBJECT_FIELDS:
            value = payload.get(field)
            if isinstance(value, (dict, list)):
                value = json.dumps(value, ensure_ascii=False)
            elif value is None:
                value = ""
            else:
                value = str(value)
            headers[f"x-oss-meta-{field}"] = value

        if extra_headers:
            headers.update(extra_headers)

        bucket.copy_object(OSS_BUCKET, oss_key, oss_key, headers=headers)
        return True
    except Exception as e:
        log(f"    OSS 元数据设置失败: {e}")
        return False


# ─── 单文件处理 ───────────────────────────────────────────────
def process_file(path: Path, kind: str, skip_existing: bool, config: str, ctx: dict) -> dict:
    """单文件：解析 meta → 确认 OSS 已存在或上传成功 → OSS 元数据写入。

    ctx: {"mode": "config"|"device", "brand": str, "meta_mode": "filename"|"json", "cfg": dict}
      - mode=config:  配置文件路径上传 → 品牌文件夹 + {machine}_{dev}_device, dataKind 使用品牌名
      - mode=device:  设备检测上传 → Orin/{yymmdd}/{dev}_device, 无品牌/机器名前缀
      - meta_mode=filename: 机器名/设备号/日期从文件名解析（老格式文件）
      - meta_mode=json:     机器名/设备号/日期/任务名从配置 JSON 读取（新格式 episode_N 文件）

    返回 {file, ok, status(uploaded|skipped|empty|failed), error, ossPath, duration, size}
    """
    source_path = path
    name = path.name
    if name.lower().endswith(".mcap.incomplete"):
        return {"file": name, "ok": False, "status": "failed",
                "error": "MCAP 尚未完成，不支持自动修复，请提供完整 .mcap 文件", "size": 0}
    config_mode = ctx["mode"] == "config"
    meta_mode = ctx["meta_mode"]

    if meta_mode == "json":
        # 新格式文件名不带参数 → 元数据全部从配置 JSON 读
        cfg = ctx["cfg"] or {}
        machine = (cfg.get("machine_name") or "").strip()
        dev = (cfg.get("device_no") or "").strip()
        capture_date = (cfg.get("capture_date") or "").strip()
        task = (cfg.get("task") or "").strip()
        digits = re.sub(r"\D", "", capture_date)
        if not machine or not dev:
            return {"file": name, "ok": False, "status": "failed",
                    "error": "JSON 缺少 machine_name/device_no", "size": 0}
        if len(digits) != 8:
            return {"file": name, "ok": False, "status": "failed",
                    "error": f"JSON capture_date 无效: {capture_date} (需 YYYY-MM-DD)", "size": 0}
        meta = {
            "dev": dev,
            "machine": machine,
            "captureDate": f"{digits[:4]}-{digits[4:6]}-{digits[6:8]}",
            "yymmdd": digits[2:8],
            "task": task,
        }
    else:
        meta = parse_name(name, kind)
        if not meta:
            return {"file": name, "ok": False, "status": "failed",
                    "error": "文件名无法解析设备/日期", "size": 0}

    if config_mode and kind == "teleop" and not meta.get("machine"):
        return {"file": name, "ok": False, "status": "failed",
                "error": "配置上传缺少机器名，无法生成 deviceDir", "size": 0}

    try:
        size = int(path.stat().st_size)
    except OSError:
        size = 0
    else:
        if size == 0:
            return {"file": name, "ok": True, "status": "empty",
                    "error": "文件为空(0B)", "size": size}

    suffix = Path(name).suffix.lower().lstrip(".")
    if suffix == "mcap":
        try:
            info = summarize(str(path))
        except Exception as e:
            detail = str(e).strip() or type(e).__name__
            return {"file": name, "ok": False, "status": "failed",
                    "error": f"mcap解析失败（不自动修复）: {detail}", "size": size}
    elif suffix in ("hdf5", "h5"):
        try:
            info = summarize_hdf5(str(path))
        except Exception as e:
            detail = str(e).strip() or type(e).__name__
            return {"file": name, "ok": False, "status": "failed",
                    "error": f"hdf5解析失败: {detail}", "size": size}
    else:
        info = {"duration_sec": 0}

    # 规则命名从文件名提取品牌；旧命名或 JSON 元数据模式继续使用配置中的品牌。
    brand = ((meta.get("brand") or ctx.get("brand") or "").strip() or "Orin"
             if config_mode else None)
    # m1/M1 属于历史 Orin 数据。即使新版文件名或 JSON 携带了其他品牌，
    # 也统一走现有 Orin 路径和设备身份规则。
    if kind == "teleop" and meta.get("machine", "").casefold() == "m1":
        brand = "Orin"
    layout = object_layout_for(kind, brand, meta["yymmdd"], meta["dev"], name,
                               config_mode, meta.get("machine", ""))
    oss_dest = layout["ossPath"]
    skipped_oss = False
    local_sha256 = file_sha256(path)
    verification = {"sha256": local_sha256, "remote_sha256": None,
                    "same_content": None, "checksum_status": "unknown",
                    "already_exists": False}

    if skip_existing:
        up = oss_stat(oss_dest, config)
        if up:
            skipped_oss = True
            remote_sha256 = up.get("sha256")
            verification.update(already_exists=True, remote_sha256=remote_sha256)
            if remote_sha256:
                same = local_sha256 == remote_sha256
                verification.update(same_content=same,
                                    checksum_status="match" if same else "mismatch")
                if not same:
                    return {"file": name, "ok": False, "status": "failed",
                            "error": "同名 OSS 对象与本地文件 SHA256 不一致，未覆盖",
                            "ossPath": oss_dest, "size": size, **verification}
            else:
                return {"file": name, "ok": True, "status": "skipped",
                        "ossPath": oss_dest, "size": size, **verification,
                        "checksum_message": "已有对象未保存 SHA256，无法确认内容一致，未修改对象"}
        else:
            up = None
    else:
        up = None

    try:
        attachments = attachment_uploader(ctx, config)
        attachment_plan = attachments.prepare(source_path, oss_dest)
    except Exception as e:
        return {"file": name, "ok": False, "status": "failed",
                "error": f"附件目录准备失败: {e}", "ossPath": oss_dest, "size": size, **verification}

    if not skipped_oss:
        up = oss_upload(str(path), oss_dest, config)
    if not up:
        return {"file": name, "ok": False, "status": "failed",
                "error": "OSS上传失败", "ossPath": oss_dest, "size": size, **verification}
    if skipped_oss:
        time.sleep(0.2)
    elif file_sha256(path) != local_sha256:
        return {"file": name, "ok": False, "status": "failed",
                "error": "上传期间本地文件发生变化，无法确认远端 SHA256",
                "ossPath": oss_dest, "size": size, **verification}
    else:
        verification.update(remote_sha256=local_sha256, same_content=True,
                            checksum_status="match")

    try:
        attachment_headers = attachments.upload_headers(attachment_plan)
    except Exception as e:
        return {"file": name, "ok": False, "status": "failed",
                "error": str(e), "ossPath": oss_dest, "size": size, **verification}

    # 元数据头 deviceNo/deviceDir 规则:
    #   配置模式 Orin: deviceNo = 设备号,           deviceDir = {设备号}_device
    #   配置模式其他品牌:
    #                   deviceNo = {品牌名}_{大写机器名}_{设备号},
    #                   deviceDir = {大写机器名}_{设备号}_device
    #   设备检测模式:  deviceNo = 设备号,           deviceDir = {设备号}_device
    #   head_cam:      deviceNo = 设备号,           deviceDir = {设备号}_device
    # OSS 路径文件夹同样按 deviceDir 生成（见 object_layout_for）。
    if _uses_plain_device_identity(kind, brand, config_mode):
        device_no = meta["dev"]
        device_dir = f"{meta['dev']}_device"
    else:
        machine = meta["machine"].upper()
        device_no = f"{brand}_{machine}_{meta['dev']}"
        device_dir = f"{machine}_{meta['dev']}_device"

    payload = {
        "ossPath": oss_dest,
        "rootPrefix": layout["rootPrefix"],
        "dataDomain": layout["dataDomain"],
        "dataKind": layout["dataKind"],
        "captureDate": meta["captureDate"],
        "deviceNo": device_no,
        "deviceDir": device_dir,
        "sessionName": meta["yymmdd"],
        "relativePath": layout["relativePath"],
        "objectRole": "data",
        "fileFormat": suffix,
        "fileSize": size,
        "duration": info["duration_sec"],
        "etag": up["etag"],
        "storageClass": "Standard",
        "lastModified": up["lastModified"],
        "ossMetadata": {},
    }
    # task 默认上传；GUI 显式取消时不写入。无 GUI 的调用也采用默认上传行为。
    extra_headers = dict(attachment_headers)
    extra_headers["x-oss-meta-sha256"] = local_sha256
    if ctx.get("upload_task", True) and meta.get("task"):
        extra_headers["x-oss-meta-task"] = str(meta["task"])
    if layout["dataDomain"] == "teleoperation":
        # 遥操数据：取任意命名空间下 episode_quality 的最终打标。
        quality = extract_episode_quality(str(path)) if suffix == "mcap" else "UNSET"
        extra_headers = {**(extra_headers or {}), "x-oss-meta-quality": quality}
    oss_key = oss_dest[len(f"oss://{OSS_BUCKET}/"):]
    if not set_oss_metadata(oss_key, payload, extra_headers):
        ctx = "OSS已存在" if skipped_oss else "已上传"
        return {"file": name, "ok": False, "status": "failed",
                "error": f"OSS元数据写入失败({ctx})", "ossPath": oss_dest, "size": size, **verification}

    status = "skipped" if skipped_oss else "uploaded"
    tag = "OSS已存在/元数据已写入" if skipped_oss else "OSS上传+元数据写入完成"
    log(f"  ✓ {name}: {tag}")

    return {"file": name, "ok": True, "status": status, "ossPath": oss_dest,
            "duration": info["duration_sec"], "size": size, **verification}


# ─── 交互辅助 ─────────────────────────────────────────────────


def human_size(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}"
        n /= 1024
