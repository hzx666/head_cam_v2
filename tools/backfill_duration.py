#!/usr/bin/env python3
"""按日期回填 raw_data_object 中 mcap 的 duration。

三阶段（CSV 交接，可中途检查 / 断点续跑）:
  query  : PostgreSQL 查某日 duration 为空的 mcap -> null_duration_<date>.csv (默认 dry-run, --confirm 真跑)
  compute: OSS Range 读 duration + parsed_metadata   -> enriched_<date>.csv    (只读, 无需确认)
  apply  : PUT /api/raw-data/objects/duration/refresh -> apply_<date>.csv     (默认 dry-run, --commit 真调)

用法:
  python3 tools/backfill_duration.py --stage query   --date 2026-06-19            # dry-run: 打印 SQL + COUNT
  python3 tools/backfill_duration.py --stage query   --date 2026-06-19 --confirm  # 真导出清单
  python3 tools/backfill_duration.py --stage compute --date 2026-06-19 [--limit 3]
  python3 tools/backfill_duration.py --stage apply   --date 2026-06-19            # dry-run: 打印 URL
  python3 tools/backfill_duration.py --stage apply   --date 2026-06-19 --commit   # 真回写
  python3 tools/backfill_duration.py --stage all     --date 2026-06-19 --confirm --commit

DB 连接从环境变量或 tools/db.env 读取（见 tools/db.env.example）。
"""
import argparse
import csv
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path
from urllib import request as urlrequest
from urllib.parse import urlencode

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))  # tools/ —— oss_meta.py 同目录
from oss_meta import make_bucket, summarize_remote_mcap, oss_key_from_path  # noqa: E402

API_BASE = os.getenv("DATA_API_BASE", "http://47.116.77.177:8080")
REFRESH_URL = API_BASE + "/api/raw-data/objects/duration/refresh"
OUT_DIR = REPO  # 输出 CSV 放仓库根（*.csv 已 gitignore）

DB_KEYS = ("DB_HOST", "DB_PORT", "DB_NAME", "DB_USER", "DB_PASSWORD", "DB_SSLMODE")


# ── DB 配置：环境变量 > tools/db.env ──────────────────────────
def load_db_config():
    cfg = {}
    env_file = Path(__file__).resolve().parent / "db.env"
    if env_file.exists():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip()
    for k in DB_KEYS:                       # 环境变量优先
        if os.getenv(k):
            cfg[k] = os.getenv(k)
    return cfg


def require_db(cfg):
    missing = [k for k in ("DB_HOST", "DB_NAME", "DB_USER", "DB_PASSWORD") if not cfg.get(k)]
    if missing:
        sys.exit(f"[错误] DB 配置缺 {missing}；请 cp tools/db.env.example tools/db.env 并填写，或设环境变量。")


def pg_connect(cfg):
    import psycopg
    return psycopg.connect(
        host=cfg.get("DB_HOST"), port=int(cfg.get("DB_PORT", 5432)),
        dbname=cfg.get("DB_NAME"), user=cfg.get("DB_USER"),
        password=cfg.get("DB_PASSWORD"), sslmode=cfg.get("DB_SSLMODE", "prefer"),
    )


def build_sql(date, select_clause, include_zero):
    cond = "duration IS NULL" + (" OR duration = 0" if include_zero else "")
    return (
        f"SELECT {select_clause} FROM public.raw_data_object "
        f"WHERE capture_date = %(date)s "
        f"AND (file_format = 'mcap' OR file_name LIKE '%%.mcap') "
        f"AND ({cond})"
    ), {"date": date}


# ── Stage 1: query ────────────────────────────────────────────
def stage_query(date, confirm, include_zero):
    select_clause = "oss_path, file_name, capture_date, data_domain, data_kind"
    sql, params = build_sql(date, select_clause, include_zero)
    cnt_sql, _ = build_sql(date, "count(*)", include_zero)

    out_csv = OUT_DIR / f"null_duration_{date}.csv"
    print("[query] 将执行的 SQL：")
    print("  " + sql.replace("%%", "%"))
    if not confirm:
        print(f"[query] dry-run：未连库、未导出。加 --confirm 才连库（先 COUNT 再 SELECT 导出 {out_csv.name}）")
        return

    cfg = load_db_config()
    require_db(cfg)
    written = 0
    with pg_connect(cfg) as conn, conn.cursor() as cur:
        cur.execute(cnt_sql, params)
        n = cur.fetchone()[0]
        print(f"[query] COUNT = {n} 行命中")
        if n == 0:
            print("[query] 0 行，跳过导出。")
            return
        cur.execute(sql, params)
        cols = [d[0] for d in cur.description]
        with open(out_csv, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            for row in cur.fetchall():
                w.writerow(dict(zip(cols, row)))
                written += 1
    print(f"[query] 已导出 {written} 行 -> {out_csv}")


# ── Stage 2: compute（OSS 只读）───────────────────────────────
def stage_compute(date, limit):
    in_csv = OUT_DIR / f"null_duration_{date}.csv"
    out_csv = OUT_DIR / f"enriched_{date}.csv"
    if not in_csv.exists():
        sys.exit(f"[错误] 找不到 {in_csv}；请先跑 --stage query --confirm。")

    rows = list(csv.DictReader(open(in_csv, encoding="utf-8-sig")))
    if limit:
        rows = rows[:limit]
    bucket = make_bucket()
    fields = ["file_name", "oss_path", "duration_sec", "parsed_metadata",
              "status", "error", "range_requests", "range_bytes"]
    ok = err = 0
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for i, r in enumerate(rows, 1):
            key = oss_key_from_path(r["oss_path"])
            row = {"file_name": r.get("file_name"), "oss_path": r["oss_path"],
                   "duration_sec": "", "parsed_metadata": "", "status": "ok",
                   "error": "", "range_requests": "", "range_bytes": ""}
            try:
                meta, n_req, n_bytes = summarize_remote_mcap(bucket, key)
                row["duration_sec"] = meta["duration_sec"]
                row["parsed_metadata"] = json.dumps(meta, ensure_ascii=False)
                row["range_requests"] = n_req
                row["range_bytes"] = n_bytes
                ok += 1
            except Exception as e:
                row["status"] = "error"
                row["error"] = f"{type(e).__name__}: {e}"
                err += 1
            w.writerow(row)
            if i % 20 == 0 or i == len(rows):
                print(f"  [compute] {i}/{len(rows)} ok={ok} err={err}", flush=True)
    print(f"[compute] done ok={ok} err={err} -> {out_csv}")


# ── Stage 3: apply（PUT refresh）──────────────────────────────
def stage_apply(date, commit, limit, sleep_s):
    in_csv = OUT_DIR / f"enriched_{date}.csv"
    out_csv = OUT_DIR / f"apply_{date}.csv"
    if not in_csv.exists():
        sys.exit(f"[错误] 找不到 {in_csv}；请先跑 --stage compute。")

    rows = [r for r in csv.DictReader(open(in_csv, encoding="utf-8-sig"))
            if r.get("status") == "ok" and r.get("duration_sec")]
    if limit:
        rows = rows[:limit]
    fields = ["file_name", "oss_path", "duration", "http_code", "updated", "error"]
    sent = 0
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for i, r in enumerate(rows, 1):
            params = {"filename": r["file_name"], "duration": r["duration_sec"],
                      "osspath": r["oss_path"]}
            url = REFRESH_URL + "?" + urlencode(params)
            row = {"file_name": r["file_name"], "oss_path": r["oss_path"],
                   "duration": r["duration_sec"], "http_code": "", "updated": "", "error": ""}
            if not commit:
                row["http_code"] = "dry-run"
                print(f"  [apply/dry] {i}/{len(rows)} {url}")
            else:
                try:
                    req = urlrequest.Request(url, method="PUT")
                    with urlrequest.urlopen(req, timeout=30) as resp:
                        body = json.loads(resp.read().decode("utf-8"))
                    row["http_code"] = resp.status
                    data = body.get("data") or {}
                    row["updated"] = data.get("updated", "")
                    if body.get("code") != 200:
                        row["error"] = f"code={body.get('code')} msg={body.get('msg')}"
                    sent += 1
                except Exception as e:
                    row["error"] = f"{type(e).__name__}: {e}"
                if sleep_s:
                    time.sleep(sleep_s)
            w.writerow(row)
    tag = "committed" if commit else "dry-run"
    print(f"[apply] {tag} 处理 {len(rows)} 行，实际发送 {sent} -> {out_csv}")


def parse_args():
    p = argparse.ArgumentParser(description="按日期回填 raw_data_object 中 mcap 的 duration")
    p.add_argument("--stage", required=True, choices=["query", "compute", "apply", "all"])
    p.add_argument("--date", required=True, help="采集日期 YYYY-MM-DD")
    p.add_argument("--confirm", action="store_true", help="query 阶段真正执行 SELECT（默认 dry-run）")
    p.add_argument("--commit", action="store_true", help="apply 阶段真正调用 PUT（默认 dry-run）")
    p.add_argument("--limit", type=int, help="compute/apply 只处理前 N 行")
    p.add_argument("--sleep", type=float, default=0.0, help="apply 每次调用间隔秒数")
    p.add_argument("--include-zero-duration", action="store_true",
                   help="query 同时命中 duration=0（默认只 IS NULL）")
    return p.parse_args()


def main():
    a = parse_args()
    try:
        date_obj = datetime.strptime(a.date, "%Y-%m-%d").date()
    except ValueError:
        sys.exit(f"[错误] --date 格式应为 YYYY-MM-DD，收到 {a.date!r}")

    if a.stage in ("query", "all"):
        stage_query(date_obj, a.confirm, a.include_zero_duration)
    if a.stage in ("compute", "all"):
        stage_compute(a.date, a.limit)
    if a.stage in ("apply", "all"):
        stage_apply(a.date, a.commit, a.limit, a.sleep)


if __name__ == "__main__":
    main()
