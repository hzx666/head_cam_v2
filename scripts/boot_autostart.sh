#!/usr/bin/env bash
# 固定启动器负责下载预构建版本；配置、任务和录制数据不进入版本替换范围。
set -euo pipefail
umask 077
BASE=$(cd -- "$(dirname -- "$0")" && pwd)
export HEAD_CAM_HOME=${HEAD_CAM_HOME:-$BASE}
UNIT_NAME=head-cam-service
PREFIX=oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/
BOOT_DELAY_SEC=${BOOT_DELAY_SEC:-60}
BACKUP_KEEP=${BACKUP_KEEP:-2}
OSS_UPDATE_ENABLED=${OSS_UPDATE_ENABLED:-true}
log() { printf '[head-cam] %s %s\n' "$(date -u +%FT%TZ)" "$*"; }
valid_version() { [[ $1 =~ ^[0-9]{8}\.[0-9]{3}$ ]]; }
validate_settings() {
  [[ $BOOT_DELAY_SEC =~ ^[0-9]+$ && $BACKUP_KEEP =~ ^[0-9]+$ ]] || return 1
  [[ $HEAD_CAM_HOME == /* && $HEAD_CAM_HOME != / && $HEAD_CAM_HOME != *[$'\n\r\t ']* ]] || return 1
}
prepare() {
  validate_settings
  mkdir -p "$HEAD_CAM_HOME"/{config,runtime/upload,runtime/monitor,logs,releases,.update-cache}
}
active() { readlink -f -- "$HEAD_CAM_HOME/current"; }
files=(head-cam-upload head-cam-monitor head-cam-service service_entry.sh run-upload run-monitor boot_autostart.sh monitor.example.json SERVICE_VERSION)
verify() {
  local dir=$1 version=$2 item
  [[ $(cat "$dir/SERVICE_VERSION") == "$version" ]] || return 1
  # 清单仅允许约定文件，禁止路径逃逸及额外文件。
  [[ $(wc -l < "$dir/SHA256SUMS") -eq ${#files[@]} ]] || return 1
  for item in "${files[@]}"; do
    [[ -f "$dir/$item" && ! -L "$dir/$item" ]] || return 1
    grep -Eq "^[0-9a-f]{64}  ${item//./\\.}$" "$dir/SHA256SUMS" || return 1
  done
  (cd "$dir" && sha256sum -c --status SHA256SUMS) || return 1
  chmod 700 "$dir"/head-cam-* "$dir"/*.sh "$dir"/run-*
  bash -n "$dir/boot_autostart.sh" "$dir/service_entry.sh" "$dir/run-upload" "$dir/run-monitor" || return 1
  timeout 30 "$dir/head-cam-upload" --self-check >/dev/null || return 1
  timeout 10 "$dir/head-cam-monitor" --self-check >/dev/null || return 1
  timeout 10 "$dir/head-cam-service" --self-check >/dev/null || return 1
}
switch_to() {
  rm -f -- "$HEAD_CAM_HOME/current.new"
  ln -s "releases/$1" "$HEAD_CAM_HOME/current.new" || return 1
  mv -Tf "$HEAD_CAM_HOME/current.new" "$HEAD_CAM_HOME/current" || return 1
}
update_release() (
  [[ $OSS_UPDATE_ENABLED == true ]] || return 0
  exec 8>"$HEAD_CAM_HOME/.update-cache/update.lock"
  flock -n 8 || return 0
  local old current remote listing stage item
  old=$(active) || return 1
  current=$(cat "$old/SERVICE_VERSION")
  listing=$(timeout 130 "$old/head-cam-upload" --update-list "$PREFIX") || { log '版本查询失败，使用本地版本'; return 1; }
  remote=$(printf '%s\n' "$listing" | sed -n "s#^${PREFIX}\([0-9]\{8\}\.[0-9]\{3\}\)/\$#\1#p" | LC_ALL=C sort | tail -1)
  [[ -n $remote ]] || return 0
  valid_version "$remote" || return 1
  [[ $remote > $current ]] || return 0
  stage=$(mktemp -d "$HEAD_CAM_HOME/.update-cache/download.XXXXXX")
  trap 'rm -rf -- "$stage"' EXIT
  for item in "${files[@]}" SHA256SUMS; do
    timeout 130 "$old/head-cam-upload" --update-get "$PREFIX$remote/$item" --update-output "$stage/$item" || { log '下载失败，使用本地版本'; return 1; }
  done
  verify "$stage" "$remote" || { log '新版本校验失败，使用本地版本'; return 1; }
  if [[ -e "$HEAD_CAM_HOME/releases/$remote" ]]; then
    # 断电可能发生在落盘完成、current 切换之前；验证后继续切换，不覆盖。
    verify "$HEAD_CAM_HOME/releases/$remote" "$remote" || return 1
  else
    sync -f "$stage" || return 1
    mv "$stage" "$HEAD_CAM_HOME/releases/$remote" || return 1
  fi
  ln -sfn "releases/$current" "$HEAD_CAM_HOME/previous" || return 1
  switch_to "$remote" || return 1
  sync -f "$HEAD_CAM_HOME" || return 1
  log "已切换版本 $remote"
  # 只清理经过格式验证的历史版本，保留当前版本及指定份数的备份。
  local count=0 version
  while IFS= read -r version; do
    valid_version "$version" || continue
    [[ $version == "$remote" ]] && continue
    count=$((count+1))
    if ((count>BACKUP_KEEP)); then rm -rf -- "$HEAD_CAM_HOME/releases/$version"; fi
  done < <(find "$HEAD_CAM_HOME/releases" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | LC_ALL=C sort -r)
)
run_service() {
  prepare
  exec 9>"$HEAD_CAM_HOME/runtime/service.lock"
  flock -n 9 || { log '服务已运行'; exit 1; }
  sleep "$BOOT_DELAY_SEC"
  # 更新总预算包含所有下载，超过预算后继续使用本地版本。
  timeout 300 "$BASE/boot_autostart.sh" update-internal || log '更新未完成，继续本地版本'
  local release previous
  release=$(active)
  if ! timeout 10 "$release/head-cam-service" --self-check >/dev/null 2>&1; then
    previous=$(readlink -f "$HEAD_CAM_HOME/previous")
    [[ -n $previous && -x $previous/head-cam-service ]] || return 1
    switch_to "$(basename "$previous")";release=$previous
    log '守护程序自检失败，已回滚'
  fi
  export HEAD_CAM_HOME
  exec "$release/head-cam-service" "$HEAD_CAM_HOME/runtime/supervisor.lock" "$release/run-upload" "$release/run-monitor"
}
install_service() {
  prepare
  [[ $(id -u) == 0 ]] || { log '安装 systemd 服务需要 root'; return 1; }
  command -v systemctl >/dev/null
  local version run_user=${RUN_USER:-${SUDO_USER:-root}} run_group
  run_group=$(id -gn "$run_user")
  version=$(cat "$BASE/SERVICE_VERSION");valid_version "$version"
  verify "$BASE" "$version"
  [[ ! -e "$HEAD_CAM_HOME/current" ]] || { log '已安装；请使用 update 或 restart'; return 1; }
  mkdir -p "$HEAD_CAM_HOME/releases/$version"
  for item in "${files[@]}" SHA256SUMS; do cp "$BASE/$item" "$HEAD_CAM_HOME/releases/$version/$item"; done
  [[ -f "$HEAD_CAM_HOME/config/monitor.json" ]] || cp "$BASE/monitor.example.json" "$HEAD_CAM_HOME/config/monitor.json"
  switch_to "$version"
  ln -s "current/boot_autostart.sh" "$HEAD_CAM_HOME/boot_autostart.sh.new"
  mv -Tf "$HEAD_CAM_HOME/boot_autostart.sh.new" "$HEAD_CAM_HOME/boot_autostart.sh"
  chown -R "$run_user:$run_group" "$HEAD_CAM_HOME"
  cat > "/etc/systemd/system/$UNIT_NAME.service" <<EOF
[Unit]
Description=Head camera upload and monitoring service
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=$run_user
Group=$run_group
Environment=HEAD_CAM_HOME=$HEAD_CAM_HOME
EnvironmentFile=-/etc/default/head-cam-service
ExecStart=$HEAD_CAM_HOME/boot_autostart.sh run
Restart=on-failure
RestartSec=10
TimeoutStartSec=infinity
TimeoutStopSec=20
KillMode=mixed
StandardOutput=journal
StandardError=journal
[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable --now "$UNIT_NAME.service"
}
case "${1:-run}" in
  install) install_service ;;
  run) run_service ;;
  update-internal) prepare; update_release ;;
  update)
    prepare;exec 9>"$HEAD_CAM_HOME/runtime/service.lock"
    flock -n 9 || { log '服务运行中；下次启动时自动更新，当前任务不受影响'; exit 0; }
    update_release ;;
  restart) systemctl restart "$UNIT_NAME.service" ;;
  status) systemctl status "$UNIT_NAME.service" --no-pager ;;
  remove) systemctl disable --now "$UNIT_NAME.service";rm -f "/etc/systemd/system/$UNIT_NAME.service";systemctl daemon-reload ;;
  *) exit 64 ;;
esac
