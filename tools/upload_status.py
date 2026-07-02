#!/usr/bin/env python3
"""查询 upload_mcap 上传进度与历史汇总(表格输出)。

用法:
    python3 tools/upload_status.py
    python3 tools/upload_status.py --history 10   # 显示最近 10 条已完成任务
    python3 tools/upload_status.py --watch 30     # 每 30 秒刷新
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
import unicodedata
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path

LOG_DIR = Path.home() / ".cache" / "upload_mcap" / "logs"
LEGACY_TELEOP_LOG_DIR = Path("/tmp/teleop_upload_logs")

KIND_RE = re.compile(r"(遥操作\(teleop\)|头戴相机\(head_cam\))")
TOTAL_FILES_RE = re.compile(r"共\s+(\d+)\s+个\s+mcap\s+文件|文件数\s+:\s+(\d+)|开始上传\s+\(共\s+(\d+)\s+个文件\)")
TOTAL_SIZE_RE = re.compile(r"共\s+\d+\s+个\s+mcap\s+文件,\s+([^\n]+)")
PROGRESS_RE = re.compile(
    r"\[进度\]\s+(\d+)/(\d+)\s+\(([^/]+)/([^,]+),\s*(\d+)%\)\s*-\s*"
    r"新传(\d+)\s+跳过(\d+)\s+空文件(\d+)\s+失败(\d+)"
)
SUMMARY_COUNTS_RE = re.compile(
    r"\[汇总\]\s+新传\s+(\d+)\s+/\s+跳过\(OSS已存在\)\s+(\d+)\s+/\s*"
    r"空文件\s+(\d+)\s+/\s+失败\s+(\d+)\s+/\s+共\s+(\d+)"
)
SUMMARY_STATS_RE = re.compile(
    r"\[汇总\]\s+数据量:\s+(\S+)\s+耗时:\s+(\S+)\s+平均速率:\s+(\S+)"
)
TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
CURRENT_FILE_RE = re.compile(r"^\[[^\]]+\]\s+✓\s+(\S+):\s+")


@dataclass
class UploadTask:
    log_path: Path
    kind: str = "unknown"
    started_at: datetime | None = None
    upload_started_at: datetime | None = None
    total_files: int = 0
    total_size: str = ""
    done: int = 0
    uploaded_size: str = ""
    pct: int = 0
    new_upload: int = 0
    skipped: int = 0
    empty: int = 0
    failed: int = 0
    finished: bool = False
    finished_at: datetime | None = None
    summary_size: str = ""
    summary_duration: str = ""
    summary_rate: str = ""
    current_file: str = ""

    @property
    def label(self) -> str:
        return {"teleop": "teleop", "head_cam": "head_cam"}.get(self.kind, self.kind)


def parse_ts(line: str) -> datetime | None:
    m = TS_RE.match(line)
    if not m:
        return None
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")


def parse_log(path: Path) -> UploadTask:
    task = UploadTask(log_path=path)
    if not path.is_file():
        return task

    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    for line in lines[:30]:
        ts = parse_ts(line)
        if ts and task.started_at is None:
            task.started_at = ts
        km = KIND_RE.search(line)
        if km:
            task.kind = "teleop" if "teleop" in km.group(1) else "head_cam"
        tm = TOTAL_FILES_RE.search(line)
        if tm:
            task.total_files = int(next(g for g in tm.groups() if g))
        sm = TOTAL_SIZE_RE.search(line)
        if sm:
            task.total_size = sm.group(1).strip()

    for line in lines:
        ts = parse_ts(line)
        if "[上传] 开始上传" in line and ts:
            task.upload_started_at = ts

        pm = PROGRESS_RE.search(line)
        if pm:
            task.done = int(pm.group(1))
            task.total_files = int(pm.group(2))
            task.uploaded_size = pm.group(3).strip()
            task.total_size = pm.group(4).strip()
            task.pct = int(pm.group(5))
            task.new_upload = int(pm.group(6))
            task.skipped = int(pm.group(7))
            task.empty = int(pm.group(8))
            task.failed = int(pm.group(9))
            if ts:
                task.started_at = task.started_at or ts

        cm = CURRENT_FILE_RE.match(line)
        if cm:
            task.current_file = cm.group(1)

        m = SUMMARY_COUNTS_RE.search(line)
        if m:
            task.new_upload = int(m.group(1))
            task.skipped = int(m.group(2))
            task.empty = int(m.group(3))
            task.failed = int(m.group(4))
            task.total_files = int(m.group(5))
            task.done = task.total_files
            task.pct = 100
            task.finished = True
            if ts:
                task.finished_at = ts

        sm = SUMMARY_STATS_RE.search(line)
        if sm:
            task.summary_size = sm.group(1)
            task.summary_duration = sm.group(2)
            task.summary_rate = sm.group(3)

    return task


def running_kinds() -> set[str]:
    kinds: set[str] = set()
    try:
        out = subprocess.check_output(["pgrep", "-af", "upload"], text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return kinds
    for line in out.splitlines():
        if "upload_status.py" in line:
            continue
        if "head_cam_upload" in line or "head_cam" in line.lower():
            kinds.add("head_cam")
        if "teleop_upload" in line or "teleop" in line.lower():
            kinds.add("teleop")
    return kinds


def collect_tasks() -> list[UploadTask]:
    logs: list[Path] = []
    if LOG_DIR.is_dir():
        logs.extend(sorted(LOG_DIR.glob("upload_mcap_*.log"), key=lambda p: p.stat().st_mtime, reverse=True))
    tasks = [parse_log(p) for p in logs]
    return [t for t in tasks if t.kind != "unknown" or t.total_files or t.done]


def pick_active_and_history(tasks: list[UploadTask], history_n: int) -> tuple[list[UploadTask], list[UploadTask]]:
    running = running_kinds()
    active: list[UploadTask] = []
    seen_kind: set[str] = set()

    for task in tasks:
        if task.kind in seen_kind:
            continue
        if not task.finished and task.kind in running:
            active.append(task)
            seen_kind.add(task.kind)
        elif task.finished:
            seen_kind.add(task.kind)

    # 若进程在跑但最新日志已有汇总，仍取该 kind 最新未完成任务
    for kind in running:
        if kind in {t.kind for t in active}:
            continue
        for task in tasks:
            if task.kind == kind and not task.finished and task.done > 0:
                active.append(task)
                break

    completed = [t for t in tasks if t.finished]
    completed.sort(key=lambda t: t.log_path.stat().st_mtime, reverse=True)
    return active, completed[:history_n]


def _eta(task: UploadTask, now: datetime) -> tuple[float | None, datetime | None]:
    """返回 (剩余秒数, 预计完成时间); 无法估算返回 (None, None)。"""
    start = task.upload_started_at or task.started_at
    if not start or task.done <= 0 or task.finished:
        return None, None
    elapsed = (now - start).total_seconds()
    if elapsed <= 0:
        return None, None
    rate = task.done / elapsed
    if rate <= 0:
        return None, None
    eta_sec = (task.total_files - task.done) / rate
    return eta_sec, now + timedelta(seconds=eta_sec)


def fmt_eta(task: UploadTask, now: datetime) -> str:
    eta_sec, _ = _eta(task, now)
    if eta_sec is None:
        return "—"
    hours = eta_sec / 3600
    return f"~{hours:.1f}h" if hours >= 1 else f"~{eta_sec / 60:.0f}m"


def fmt_eta_finish(task: UploadTask, now: datetime) -> str:
    _, finish = _eta(task, now)
    return finish.strftime("%m-%d %H:%M") if finish else "—"


def fmt_elapsed(task: UploadTask, now: datetime) -> str:
    start = task.upload_started_at or task.started_at
    if not start:
        return "—"
    secs = int((now - start).total_seconds())
    if secs < 0:
        return "—"
    hours, rem = divmod(secs, 3600)
    mins = rem // 60
    if hours:
        return f"{hours}h{mins}m"
    return f"{mins}m"


def _fmt_dur(raw: str) -> str:
    """日志里的 'Xm Ys' 耗时:>=60min 换成 'Hh Mm',否则保持原样。"""
    if not raw:
        return ""
    m = re.match(r"\s*(\d+)m(\d+)s\s*$", raw)
    if not m:
        return raw
    mins, secs = int(m.group(1)), int(m.group(2))
    if mins >= 60:
        h, mm = divmod(mins, 60)
        return f"{h}h{mm}m"
    return f"{mins}m{secs}s"


# ─── 表格渲染(中日韩字符按 2 宽对齐)─────────────────────────────
def _ew(s) -> int:
    """字符串显示宽度:East Asian Wide/Fullwidth 字符算 2。"""
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in str(s))


def _pad(s, width: int, align: str = "left") -> str:
    s = str(s)
    gap = width - _ew(s)
    if gap <= 0:
        return s
    return (" " * gap + s) if align == "right" else (s + " " * gap)


def _trunc(s, width: int) -> str:
    s = str(s)
    if _ew(s) <= width:
        return s
    out, cur = "", 0
    for c in s:
        cw = 2 if unicodedata.east_asian_width(c) in "WF" else 1
        if cur + cw > width - 1:
            break
        out += c
        cur += cw
    return out + "…"


def _table(title: str, headers: list[str], rows: list[list[str]],
           aligns: list[str], trunc_cols: dict | None = None) -> None:
    trunc_cols = trunc_cols or {}
    print(f"================ {title} ================")
    if not rows:
        print("  (无)\n")
        return
    proc = [[_trunc(c, trunc_cols[i]) if i in trunc_cols else str(c) for i, c in enumerate(row)] for row in rows]
    widths = [_ew(h) for h in headers]
    for row in proc:
        for i, cell in enumerate(row):
            if _ew(cell) > widths[i]:
                widths[i] = _ew(cell)

    def _line(cells):
        return "  ".join(_pad(cells[i], widths[i], aligns[i]) for i in range(len(headers)))

    print("  " + _line(headers))
    for row in proc:
        print("  " + _line(row))
    print()


def render(history_n: int) -> None:
    now = datetime.now()
    tasks = collect_tasks()
    active, completed = pick_active_and_history(tasks, history_n)

    print("=" * 60)
    print(f"  upload_mcap 上传进度  |  {now.strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 60)
    print()

    running = running_kinds()
    print(f"运行中进程: {', '.join(sorted(running)) if running else '无'}\n")

    # 已完成(上)
    done_headers = ["类型", "进度", "新传", "跳过", "空", "失败", "开始", "结束", "耗时", "数据量", "速率"]
    done_aligns = ["left", "right", "right", "right", "right", "right", "right", "right", "right", "right", "right"]
    done_rows = [[
        t.label,
        f"{t.done}/{t.total_files} ({t.pct}%)",
        str(t.new_upload), str(t.skipped), str(t.empty), str(t.failed),
        t.started_at.strftime("%m-%d %H:%M") if t.started_at else "—",
        t.finished_at.strftime("%m-%d %H:%M") if t.finished_at else "—",
        _fmt_dur(t.summary_duration) or "—",
        t.summary_size or "—",
        t.summary_rate or "—",
    ] for t in completed]
    _table("已完成", done_headers, done_rows, done_aligns)

    # 进行中(下)
    act_headers = ["类型", "进度", "已传/总量", "新传", "跳过", "空", "失败", "开始", "已运行", "预计剩余", "预计结束", "当前文件"]
    act_aligns = ["left", "right", "right", "right", "right", "right", "right", "right", "right", "right", "right", "left"]
    act_rows = [[
        t.label,
        f"{t.done}/{t.total_files} ({t.pct}%)",
        f"{t.uploaded_size}/{t.total_size}" if (t.uploaded_size or t.total_size) else "—",
        str(t.new_upload), str(t.skipped), str(t.empty), str(t.failed),
        t.started_at.strftime("%m-%d %H:%M") if t.started_at else "—",
        fmt_elapsed(t, now),
        fmt_eta(t, now),
        fmt_eta_finish(t, now),
        t.current_file or "—",
    ] for t in active]
    _table("进行中", act_headers, act_rows, act_aligns, trunc_cols={11: 28})

    # 失败提示
    tips = [t for t in completed if t.failed]
    if tips:
        for t in tips:
            print(f"  ℹ {t.label}: {t.failed} 条失败,详见 {t.log_path.name}「失败列表」")
        print()


def main() -> int:
    parser = argparse.ArgumentParser(description="查询 upload_mcap 上传进度")
    parser.add_argument("--history", type=int, default=5, help="显示最近 N 条已完成任务")
    parser.add_argument("--watch", type=int, metavar="SEC", help="每 N 秒自动刷新")
    args = parser.parse_args()

    if args.watch:
        try:
            while True:
                if sys.stdout.isatty():
                    print("\033[2J\033[H", end="")
                render(args.history)
                time.sleep(max(args.watch, 5))
        except KeyboardInterrupt:
            print("\n已停止监控")
            return 0

    render(args.history)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
