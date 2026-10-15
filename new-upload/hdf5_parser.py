#!/usr/bin/env python3
"""打印 hdf5 文件的 summary 元信息（JSON 全量）。

用法:
    python3 hdf5_parser.py foo.hdf5
    python3 hdf5_parser.py a.hdf5 b.hdf5      # 多文件输出 JSON 数组

Astribot 采集的 hdf5 内各流均带 timestamp_* / start_timestamp_* /
end_timestamp_* 数据集（float64 Unix 秒），duration = 全局最大时间戳
- 全局最小时间戳。仅读取时间戳数组（KB 级），不读取图像/关节数据。

输出形状与 mcap_parser.summarize() 对齐（duration 保持 "12.345s" 字符串格式）。
依赖: pip install h5py numpy
"""
import json
import sys
from datetime import datetime
from pathlib import Path

import h5py
import numpy as np


def summarize(path: str) -> dict:
    """读取单个 hdf5 的时间戳元信息，返回元信息字典。"""
    p = Path(path)
    size_mb = p.stat().st_size / 1e6
    start = None
    end = None
    streams = []
    total_msgs = 0
    with h5py.File(p, "r") as f:

        def walk(name, obj):
            nonlocal start, end, total_msgs
            if isinstance(obj, h5py.Group):
                if "count" in obj.attrs:
                    c = int(obj.attrs["count"])
                    streams.append({"name": name, "msgs": c})
                    total_msgs += c
            elif isinstance(obj, h5py.Dataset):
                base = obj.name.rsplit("/", 1)[-1].lower()
                if "timestamp" in base:
                    a = np.asarray(obj[()]).ravel()
                    a = a[np.isfinite(a)]
                    if a.size:
                        lo, hi = float(a.min()), float(a.max())
                        start = lo if start is None else min(start, lo)
                        end = hi if end is None else max(end, hi)

        f.visititems(walk)
    if start is None:
        raise ValueError(f"{path}: hdf5 中无 timestamp 数据集")
    dur = end - start
    channels = [{
        "name": s["name"],
        "msgs": s["msgs"],
        "hz": round(s["msgs"] / dur, 1) if dur else 0.0,
    } for s in streams]

    return {
        "file": p.name,
        "duration": f"{dur:.3f}s",
        "duration_sec": round(dur, 3),
        "message_count": total_msgs,
        "channel_count": len(streams),
        "size_mb": round(size_mb, 1),
        "start": datetime.fromtimestamp(start).isoformat(),
        "end": datetime.fromtimestamp(end).isoformat(),
        "channels": channels,
    }


def main():
    if len(sys.argv) < 2:
        print("用法: python3 hdf5_parser.py <file.hdf5> [...]", file=sys.stderr)
        sys.exit(1)
    results = [summarize(p) for p in sys.argv[1:]]
    print(json.dumps(results, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
