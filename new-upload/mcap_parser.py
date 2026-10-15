#!/usr/bin/env python3
"""打印 mcap 文件的 summary 元信息（JSON 全量）。

用法:
    python3 mcap_parser.py foo.mcap
    python3 mcap_parser.py a.mcap b.mcap      # 多文件输出 JSON 数组

输出为 JSON 数组，每个文件一个对象；duration 字段保持 "12.345s" 字符串格式。
依赖: pip install mcap
"""
import json
import re
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


# 遥操打标 topic：std_msgs/msg/String，payload 支持纯字符串或 JSON
# 例: PASS 或 {"schema_version":"episode-quality.v1","quality":"PASS"}
EPISODE_QUALITY_TOPIC = "episode_quality"
_QUALITY_ENUM = ("PASS", "FAIL", "UNSET")


def _parse_episode_quality(data) -> str:
    """从 std_msgs/String 原始字节解析 quality。

    payload 前可能带 CDR 封装头；兼容纯字符串 PASS/FAIL/UNSET，
    以及包含 quality 字段的 JSON。
    """
    if not data:
        return "UNSET"
    if isinstance(data, (bytes, bytearray)):
        text = bytes(data).decode("utf-8", errors="ignore")
    else:
        text = str(data)
    start, end = text.find("{"), text.rfind("}")
    if start >= 0 and end > start:
        try:
            obj = json.loads(text[start:end + 1])
        except ValueError:
            obj = None
        quality = obj.get("quality") if isinstance(obj, dict) else None
        if quality in _QUALITY_ENUM:
            return quality

    # CDR 编码的 std_msgs/String 在正文前后带有二进制头和 NUL，
    # 按独立单词查找可兼容该编码及直接传入的普通字符串。
    match = re.search(r"\b(PASS|FAIL|UNSET)\b", text)
    return match.group(1) if match else "UNSET"


def extract_episode_quality(path: str) -> str:
    """匹配任意命名空间下的 episode_quality，取 log_time 最晚的打标。"""
    data = None
    try:
        with Path(path).open("rb") as f:
            reader = make_reader(f)
            summary = reader.get_summary()
            if summary is not None and summary.chunk_indexes:
                topics = sorted({
                    channel.topic for channel in summary.channels.values()
                    if channel.topic.rsplit("/", 1)[-1] == EPISODE_QUALITY_TOPIC
                })
                if not topics:
                    return "UNSET"
                # 有 chunk 索引时倒序读取，第一条即最终打标。
                for _, _, message in reader.iter_messages(
                        topics=topics, reverse=True):
                    data = message.data
                    break
            else:
                # 无索引时扫描所有 channel，不依赖可能缺失的 summary。
                latest_time = None
                for _, channel, message in reader.iter_messages(log_time_order=False):
                    if channel.topic.rsplit("/", 1)[-1] != EPISODE_QUALITY_TOPIC:
                        continue
                    if latest_time is None or message.log_time >= latest_time:
                        latest_time = message.log_time
                        data = message.data
    except Exception:
        return "UNSET"
    return _parse_episode_quality(data)


def main():
    if len(sys.argv) < 2:
        print("用法: python3 mcap_parser.py <file.mcap> [...]", file=sys.stderr)
        sys.exit(1)
    results = [summarize(p) for p in sys.argv[1:]]
    print(json.dumps(results, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
