#!/usr/bin/env bash
# 版本内入口。由启动器设置固定的数据目录，exec 保留业务进程身份。
set -euo pipefail
RELEASE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
: "${HEAD_CAM_HOME:?HEAD_CAM_HOME required}"
case "${1:-}" in
  upload)
    args=(--runtime-dir "$HEAD_CAM_HOME/runtime/upload" --host "${UPLOAD_HOST:-0.0.0.0}" --port "${UPLOAD_PORT:-38127}")
    [[ -z ${UPLOAD_ALLOWED_ROOT:-} ]] || args+=(--allowed-root "$UPLOAD_ALLOWED_ROOT")
    exec "$RELEASE_DIR/head-cam-upload" "${args[@]}" ;;
  monitor) exec "$RELEASE_DIR/head-cam-monitor" "$HEAD_CAM_HOME/config/monitor.json" "$HEAD_CAM_HOME/runtime/monitor" ;;
  *) exit 64 ;;
esac
