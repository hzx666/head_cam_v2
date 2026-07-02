#!/usr/bin/env python3
"""OSS mcap 元信息读取（共享模块）。

复用并合并 backup/ 里散落的实现：用 OSS HTTP Range 把对象包装成可 seek 的
file-like，喂给 mcap 官方库 get_summary()，一次拿到完整 parsed_metadata，
无需下载整文件（请求个位数、拉取 KB~MB 级）。

刻意不 import upload_mcap.py（它在 Python 3.9 解析失败），常量在此内联。
"""
import base64
import hashlib
from datetime import datetime
from pathlib import Path

import oss2
from mcap.reader import make_reader

# ── 与 client/upload_mcap.py 一致的常量（内联，避免 import）──
OSS_BUCKET = "ss-oss-data-platform"
OSS_ENDPOINT = "oss-cn-shanghai.aliyuncs.com"
_K = "ssim_record_upload_2026_v1"
_T_AK_ENC = "U2FsdGVkX18QkKLFQm+J8l2nQihkIM2hHrd9RJ5k8EGxhgrjXquNchWCaUgb6rD2"
_T_SK_ENC = "U2FsdGVkX19H6vKZBFlqCT7c6J7iqYtnKj2A+S1HxIWDpTpEiJ4WAcfdlCwIhK5d"


def decrypt_credential(enc_text: str) -> str:
    """AES-256-CBC + PBKDF2(SHA256,10000) 解密内嵌凭证（cryptography 库，无外部依赖）。"""
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
    return "" if pad < 1 or pad > 16 else pt[:-pad].decode("utf-8")


def make_bucket() -> oss2.Bucket:
    ak = decrypt_credential(_T_AK_ENC)
    sk = decrypt_credential(_T_SK_ENC)
    if not ak or not sk:
        raise RuntimeError("OSS 凭证解密失败")
    return oss2.Bucket(oss2.Auth(ak, sk), "https://" + OSS_ENDPOINT, OSS_BUCKET)


def oss_key_from_path(oss_path: str) -> str:
    """oss://bucket/key -> key；已是 key 则原样返回。"""
    pfx = f"oss://{OSS_BUCKET}/"
    return oss_path[len(pfx):] if oss_path.startswith(pfx) else oss_path


class OSSRangeBufferedFile:
    """把 OSS 对象包装成只读、可 seek 的 file-like，供 mcap SeekingReader 使用。

    固定 block（默认 1MiB）+ 按 block_idx 永久缓存，适合 summary 解析时在
    footer/summary/chunk 索引间的多次跳转；记录 n_req/n_bytes 用于观测。
    """

    def __init__(self, bucket: oss2.Bucket, key: str, block_size: int = 1 << 20):
        self.bucket = bucket
        self.key = key
        self.block_size = block_size
        self.size = int(bucket.head_object(key).content_length)
        self.pos = 0
        self.cache = {}
        self.n_req = 0
        self.n_bytes = 0

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset: int, whence: int = 0):
        if whence == 0:
            nxt = offset
        elif whence == 1:
            nxt = self.pos + offset
        elif whence == 2:
            nxt = self.size + offset
        else:
            raise ValueError(f"bad whence: {whence}")
        self.pos = max(0, min(nxt, self.size))
        return self.pos

    def _read_block(self, block_idx: int) -> bytes:
        block = self.cache.get(block_idx)
        if block is not None:
            return block
        start = block_idx * self.block_size
        end = min(self.size, start + self.block_size) - 1
        if end < start:
            data = b""
        else:
            data = self.bucket.get_object(self.key, byte_range=(start, end)).read()
            self.n_req += 1
            self.n_bytes += len(data)
        self.cache[block_idx] = data
        return data

    def read(self, n: int = -1) -> bytes:
        if self.pos >= self.size:
            return b""
        target_end = self.size if n is None or n < 0 else min(self.size, self.pos + n)
        if target_end <= self.pos:
            return b""
        out = bytearray()
        while self.pos < target_end:
            block_idx = self.pos // self.block_size
            block = self._read_block(block_idx)
            in_block = self.pos % self.block_size
            take = min(target_end - self.pos, len(block) - in_block)
            if take <= 0:
                break
            out.extend(block[in_block:in_block + take])
            self.pos += take
        return bytes(out)


def summarize_remote_mcap(bucket: oss2.Bucket, key: str):
    """Range 读 OSS mcap 的 summary，返回 (parsed_metadata, n_req, n_bytes)。

    parsed_metadata 形状与 client/mcap_parser.summarize() 一致：
    file/duration/duration_sec/message_count/channel_count/schema_count/
    chunk_count/size_mb/start/end/channels[{topic,type,encoding,msgs,hz}]。
    """
    reader = OSSRangeBufferedFile(bucket, key)
    try:
        sm = make_reader(reader).get_summary()
    finally:
        n_req, n_bytes, size = reader.n_req, reader.n_bytes, reader.size
    st = sm.statistics if sm else None

    # 时长：优先 summary statistics，缺失则回退逐条遍历
    if st and st.message_start_time is not None and st.message_end_time is not None:
        start_ns, end_ns = st.message_start_time, st.message_end_time
    else:
        # 极少情况：summary 缺时间戳，回退逐条遍历（用新 reader，观测值仍用上面的 n_req/n_bytes）
        start_ns = end_ns = None
        for _, _, message in make_reader(OSSRangeBufferedFile(bucket, key)).iter_messages():
            t = message.log_time
            start_ns = t if start_ns is None else min(start_ns, t)
            end_ns = t if end_ns is None else max(end_ns, t)
    if start_ns is None:
        raise RuntimeError(f"{key}: mcap 无消息时间戳（可能未写 summary）")

    dur = (end_ns - start_ns) / 1e9
    cmc = (st.channel_message_counts if st else None) or {}
    channels = []
    for cid, ch in sorted((sm.channels if sm else {}).items()):
        sch = sm.schemas.get(ch.schema_id) if sm else None
        cnt = cmc.get(cid, 0)
        channels.append({
            "topic": ch.topic,
            "type": sch.name if sch else None,
            "encoding": ch.message_encoding,
            "msgs": cnt,
            "hz": round(cnt / dur, 1) if dur else 0.0,
        })

    meta = {
        "file": Path(key).name,
        "duration": f"{dur:.3f}s",
        "duration_sec": round(dur, 3),
        "message_count": st.message_count if st else None,
        "channel_count": st.channel_count if st else None,
        "schema_count": st.schema_count if st else None,
        "chunk_count": st.chunk_count if st else None,
        "size_mb": round(size / 1e6, 1),
        "start": datetime.fromtimestamp(start_ns / 1e9).isoformat(),
        "end": datetime.fromtimestamp(end_ns / 1e9).isoformat(),
        "channels": channels,
    }
    return meta, n_req, n_bytes
