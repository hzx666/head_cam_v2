#!/bin/sh
set -eu
RELEASE_DIR=$(cd "${0%/*}" && pwd)
: "${HEAD_CAM_HOME:?HEAD_CAM_HOME required}"
mkdir -p "$HEAD_CAM_HOME/runtime/upload" "$HEAD_CAM_HOME/runtime/monitor"
case "${1:-}" in
 upload)
 set -- --runtime-dir "$HEAD_CAM_HOME/runtime/upload" --host "${UPLOAD_HOST:-0.0.0.0}" --port "${UPLOAD_PORT:-38127}"
 [ -z "${UPLOAD_ALLOWED_ROOT:-}" ] || set -- "$@" --allowed-root "$UPLOAD_ALLOWED_ROOT"
 exec "$RELEASE_DIR/head-cam-service" upload "$@";;
 monitor) exec "$RELEASE_DIR/head-cam-service" monitor "${HEAD_CAM_MONITOR_CONFIG:-$HEAD_CAM_HOME/monitor.json}" "$HEAD_CAM_HOME/runtime/monitor";;
 *) exit 64;;
esac
