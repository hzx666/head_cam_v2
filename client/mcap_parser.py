#!/usr/bin/env python3
"""打印 mcap 文件的 summary 元信息（JSON 全量）。

用法:
    python3 mcap_parser.py foo.mcap
    python3 mcap_parser.py a.mcap b.mcap      # 多文件输出 JSON 数组

输出为 JSON 数组，每个文件一个对象；duration 字段保持 "12.345s" 字符串格式。
依赖: pip install mcap
"""
import json
import sys
from datetime import datetime
from pathlib import Path

from mcap.reader import make_reader


def summarize(path: str) -> dict:
    """读取单个 mcap 的 summary，返回元信息字典。"""
    p = Path(path)
    size_mb = p.stat().st_size / 1e6
    with p.open("rb") as f:
        reader = make_reader(f)
        sm = reader.get_summary()
        st = sm.statistics if sm else None

        # 时长：优先用 summary 的 statistics，缺失则回退逐条遍历
        if st and st.message_start_time is not None and st.message_end_time is not None:
            start_ns, end_ns = st.message_start_time, st.message_end_time
        else:
            start_ns = end_ns = None
            for _, _, message in reader.iter_messages():
                t = message.log_time
                start_ns = t if start_ns is None else min(start_ns, t)
                end_ns = t if end_ns is None else max(end_ns, t)
        if start_ns is None:
            raise ValueError(f"{path}: mcap 中无消息时间戳")
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

    return {
        "file": p.name,
        "duration": f"{dur:.3f}s",
        "duration_sec": round(dur, 3),
        "message_count": st.message_count if st else None,
        "channel_count": st.channel_count if st else None,
        "schema_count": st.schema_count if st else None,
        "chunk_count": st.chunk_count if st else None,
        "size_mb": round(size_mb, 1),
        "start": datetime.fromtimestamp(start_ns / 1e9).isoformat(),
        "end": datetime.fromtimestamp(end_ns / 1e9).isoformat(),
        "channels": channels,
    }


def main():
    if len(sys.argv) < 2:
        print("用法: python3 mcap_parser.py <file.mcap> [...]", file=sys.stderr)
        sys.exit(1)
    results = [summarize(p) for p in sys.argv[1:]]
    print(json.dumps(results, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
