#!/bin/sh
# BusyBox 设备入口；随包携带工具，不依赖设备 Bash 或 systemd。
set -eu
umask 077
case "$0" in */*) BASE=$(cd "${0%/*}" && pwd);; *) BASE=$(pwd);; esac
HEAD_CAM_HOME=${HEAD_CAM_HOME:-$BASE}
export HEAD_CAM_HOME
BB=${HEAD_CAM_BUSYBOX:-$BASE/head-cam-service}
if [ -z "${HEAD_CAM_BUSYBOX:-}" ] && [ -x "$BASE/current/head-cam-service" ];then BB=$BASE/current/head-cam-service;fi
if [ ! -x "$BB" ]; then echo 'Missing head-cam-service; copy complete dist' >&2;exit 1;fi
# 先用显式路径运行随包工具，系统无需 dirname/env 等命令。
"$BB" mkdir -p "$HEAD_CAM_HOME/runtime/tools"
for app in cat chmod cp date find grep head id ln mkdir mktemp mv readlink rm sed sha256sum sleep sort sync tail timeout wc; do
    link=$HEAD_CAM_HOME/runtime/tools/$app
    if [ "$("$BB" readlink "$link" 2>/dev/null || true)" != "$BB" ];then
        "$BB" ln -sf "$BB" "$link" 2>/dev/null ||
            [ "$("$BB" readlink "$link" 2>/dev/null || true)" = "$BB" ]
    fi
done
PATH=$HEAD_CAM_HOME/runtime/tools:/bin:/sbin:/usr/bin:/usr/sbin
export PATH
LOCK_HELPER=${HEAD_CAM_LOCK_HELPER:-$BASE/head-cam-service}
if [ -z "${HEAD_CAM_LOCK_HELPER:-}" ] && [ -x "$BASE/current/head-cam-service" ];then LOCK_HELPER=$BASE/current/head-cam-service;fi
flock() { "$LOCK_HELPER" --flock "$2"; }
BOOT_DELAY_SEC=${BOOT_DELAY_SEC:-60}
BACKUP_KEEP=${BACKUP_KEEP:-2}
OSS_UPDATE_ENABLED=${OSS_UPDATE_ENABLED:-false}
# 正式固件接管 DHCP 后，可在 network.env 中设置 HEAD_CAM_AUTO_DHCP=false。
[ ! -f "$HEAD_CAM_HOME/network.env" ] || . "$HEAD_CAM_HOME/network.env"
HEAD_CAM_AUTO_DHCP=${HEAD_CAM_AUTO_DHCP:-true}
HEAD_CAM_NETWORK_IFACE=${HEAD_CAM_NETWORK_IFACE:-wlan0}
PREFIX=oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/
FILES='head-cam-service service_entry.sh run-upload run-monitor boot_autostart.sh manual_dhcp.sh monitor.json SERVICE_VERSION'
DIY_PATH=${HEAD_CAM_DIY_PATH:-/config/diy.sh}
log() { printf '[head-cam] %s %s\n' "$(date -u +%FT%TZ)" "$*"; }
valid_version() { printf '%s\n' "$1" | grep -Eq '^[0-9]{8}\.[0-9]{3}$'; }
prepare() {
    case "$HEAD_CAM_HOME" in /*) ;; *) return 1;; esac
    [ "$HEAD_CAM_HOME" != / ] || return 1
    case "$HEAD_CAM_HOME" in *[!a-zA-Z0-9_./-]*) echo 'Install path must use ASCII without spaces' >&2;return 1;; esac
    case "$BOOT_DELAY_SEC:$BACKUP_KEEP" in *[!0-9:]*|:*|*:) return 1;; esac
    mkdir -p "$HEAD_CAM_HOME/config" "$HEAD_CAM_HOME/runtime/upload" "$HEAD_CAM_HOME/runtime/monitor" "$HEAD_CAM_HOME/logs" "$HEAD_CAM_HOME/releases" "$HEAD_CAM_HOME/.update-cache"
}
active() { if [ -x "$HEAD_CAM_HOME/current/head-cam-service" ]; then readlink -f "$HEAD_CAM_HOME/current"; else printf '%s\n' "$HEAD_CAM_HOME"; fi; }
verify() (
    dir=$1; version=$2
    [ "$(cat "$dir/SERVICE_VERSION")" = "$version" ] || exit 1
    [ "$(wc -l < "$dir/SHA256SUMS")" -eq 8 ] || exit 1
    for item in $FILES; do
        [ -f "$dir/$item" ] && [ ! -L "$dir/$item" ] || exit 1
        grep -F "  $item" "$dir/SHA256SUMS" | grep -Eq '^[0-9a-f]{64}  [a-zA-Z0-9_.-]+$' || exit 1
    done
    # 白名单逐项验证，避免读取清单中的任意路径。
    for item in $FILES; do
        [ "$item" != monitor.json ] || continue
        expected=$(grep "  $item\$" "$dir/SHA256SUMS")
        actual=$(cd "$dir" && sha256sum "$item")
        [ "$expected" = "$actual" ] || exit 1
    done
    chmod 700 "$dir"/head-cam-* "$dir"/*.sh "$dir"/run-*
    for item in boot_autostart.sh manual_dhcp.sh service_entry.sh run-upload run-monitor; do /bin/sh -n "$dir/$item" || exit 1;done
    timeout 30 "$dir/head-cam-service" --self-check >/dev/null || exit 1
)
switch_to() {
    rm -f "$HEAD_CAM_HOME/current.new"
    ln -s "releases/$1" "$HEAD_CAM_HOME/current.new" || return 1
    "$LOCK_HELPER" --rename "$HEAD_CAM_HOME/current.new" "$HEAD_CAM_HOME/current"
}
update_release() (
    [ "$OSS_UPDATE_ENABLED" = true ] || exit 0
    exec 8>"$HEAD_CAM_HOME/.update-cache/update.lock"
    flock -n 8 || exit 0
    old=$(active); current=$(cat "$old/SERVICE_VERSION")
    listing=$(timeout 130 "$old/head-cam-service" --update-list "$PREFIX") || exit 1
    remote=$(printf '%s\n' "$listing" | sed -n "s#^${PREFIX}\([0-9]\{8\}\.[0-9]\{3\}\)/SHA256SUMS\$#\1#p" | sort | tail -n 1)
    [ -n "$remote" ] || exit 0
    valid_version "$remote" || exit 1
    [ "$remote" != "$current" ] || exit 0
    [ "$(printf '%s\n%s\n' "$current" "$remote" | sort | tail -n 1)" = "$remote" ] || exit 0
    stage=$(mktemp -d "$HEAD_CAM_HOME/.update-cache/download.XXXXXX")
    trap 'rm -rf "$stage"' 0
    for item in $FILES SHA256SUMS; do
        timeout 130 "$old/head-cam-service" --update-get "$PREFIX$remote/$item" --update-output "$stage/$item" || exit 1
    done
    verify "$stage" "$remote" || exit 1
    if [ -e "$HEAD_CAM_HOME/releases/$remote" ]; then
        verify "$HEAD_CAM_HOME/releases/$remote" "$remote" || exit 1
    else
        sync; mv "$stage" "$HEAD_CAM_HOME/releases/$remote" || exit 1
    fi
    ln -sfn "releases/$current" "$HEAD_CAM_HOME/previous" || exit 1
    switch_to "$remote" || exit 1
    sync
    count=0
    for version in $(find "$HEAD_CAM_HOME/releases" -mindepth 1 -maxdepth 1 -type d | sed 's#.*/##' | sort -r); do
        valid_version "$version" || continue
        [ "$version" != "$remote" ] || continue
        count=$((count+1))
        if [ "$count" -gt "$BACKUP_KEEP" ]; then rm -rf "$HEAD_CAM_HOME/releases/$version";fi
    done
    log "Updated to $remote"
)
watch_gateway() (
    owner=$1
    # 网络辅助进程不持有主服务锁，避免停止后阻塞重启。
    exec 9>&-
    exec 7>&-
    while kill -0 "$owner" 2>/dev/null && [ ! -f "$HEAD_CAM_HOME/runtime/stop.request" ]; do
        if ! "$BB" ip route show default | "$BB" grep -q '^default '; then
            if "$BB" ip -4 addr show dev "$HEAD_CAM_NETWORK_IFACE" 2>/dev/null | "$BB" grep -q 'inet '; then
                log 'Default gateway missing; requesting DHCP router'
                HEAD_CAM_TOOL=$BB
                export HEAD_CAM_TOOL
                /bin/sh "$HEAD_CAM_HOME/manual_dhcp.sh" "$HEAD_CAM_NETWORK_IFACE" || log 'DHCP not ready; retry later'
            fi
        fi
        "$BB" sleep 15
    done
)
run_service() {
    prepare
    exec 9>"$HEAD_CAM_HOME/runtime/service.lock"
    flock -n 9 || { log 'Already running';exit 1; }
    printf '%s\n' "$$" > "$HEAD_CAM_HOME/runtime/service.pid"
    if [ "$HEAD_CAM_AUTO_DHCP" = true ]; then
        if [ -f "$HEAD_CAM_HOME/manual_dhcp.sh" ]; then watch_gateway "$$" &
        else log 'Missing manual_dhcp.sh; automatic gateway recovery unavailable'; fi
    fi
    # 等待可被 stop 中断，且 sleep 不持有服务或启动器的文件锁。
    (exec 9>&-; exec 7>&-; exec sleep "$BOOT_DELAY_SEC") &
    delay_pid=$!
    trap 'kill "$delay_pid" 2>/dev/null || true; exit 0' 2 15
    wait "$delay_pid" || true
    trap - 2 15
    timeout 300 /bin/sh "$HEAD_CAM_HOME/boot_autostart.sh" update-internal || log 'Update failed; using local release'
    release=$(active)
    if ! timeout 10 "$release/head-cam-service" --self-check >/dev/null 2>&1; then
        previous=$(readlink -f "$HEAD_CAM_HOME/previous")
        [ -x "$previous/head-cam-service" ] || return 1
        switch_to "${previous##*/}";release=$previous
    fi
    printf '%s\n' "$$" > "$HEAD_CAM_HOME/runtime/service.pid"
    HEAD_CAM_MONITOR_CONFIG=$HEAD_CAM_HOME/monitor.json
    [ -f "$HEAD_CAM_MONITOR_CONFIG" ] || HEAD_CAM_MONITOR_CONFIG=$HEAD_CAM_HOME/config/monitor.json
    export HEAD_CAM_MONITOR_CONFIG
    exec "$release/head-cam-service" supervise "$HEAD_CAM_HOME/runtime/supervisor.lock" "$release/head-cam-service"
}
start_service() {
    prepare
    # 持锁启动避免重复后台入口。
    ( exec 7>"$HEAD_CAM_HOME/runtime/start.lock";flock -n 7 || { log 'Background launcher already active';exit 0; }
      if [ -f "$HEAD_CAM_HOME/runtime/service.pid" ];then
        pid=$(cat "$HEAD_CAM_HOME/runtime/service.pid")
        case "$pid" in ''|*[!0-9]*) ;; *) if kill -0 "$pid" 2>/dev/null;then exit 0;fi;; esac
      fi
      rm -f "$HEAD_CAM_HOME/runtime/stop.request"
      log "Starting service in background (delay ${BOOT_DELAY_SEC}s)"
      trap '' HUP
      ( while :; do
          [ ! -f "$HEAD_CAM_HOME/runtime/stop.request" ] || break
          /bin/sh "$HEAD_CAM_HOME/boot_autostart.sh" run
          [ ! -f "$HEAD_CAM_HOME/runtime/stop.request" ] || break
          sleep 10
        done ) </dev/null >>"$HEAD_CAM_HOME/logs/boot.log" 2>&1 &
    )
}
wait_launcher_exit() (
    exec 7>"$HEAD_CAM_HOME/runtime/start.lock"
    attempts=0
    while ! flock -n 7; do
        [ "$attempts" -lt 20 ] || { log 'Background launcher is still stopping';return 1; }
        sleep 1;attempts=$((attempts+1))
    done
)
stop_service() {
    : > "$HEAD_CAM_HOME/runtime/stop.request"
    [ -f "$HEAD_CAM_HOME/runtime/service.pid" ] || { wait_launcher_exit;return; }
    pid=$(cat "$HEAD_CAM_HOME/runtime/service.pid")
    case "$pid" in ''|*[!0-9]*) return 1;; esac
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$HEAD_CAM_HOME/runtime/service.pid"
        wait_launcher_exit;return
    fi
    # 验证 PID 对应服务二进制，避免 PID 复用误杀。
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
    case "$exe" in "$HEAD_CAM_HOME"/head-cam-service|"$HEAD_CAM_HOME"/releases/*/head-cam-service) ;; *) grep -F "$HEAD_CAM_HOME/boot_autostart.sh" "/proc/$pid/cmdline" >/dev/null 2>&1 || { log 'No matching service';return 1; };; esac
    kill -TERM "$pid" 2>/dev/null || true
    n=0;while kill -0 "$pid" 2>/dev/null; do
        [ "$n" -lt 20 ] || { log 'Still stopping; wait for accepted uploads';return 1; }
        sleep 1;n=$((n+1))
    done
    rm -f "$HEAD_CAM_HOME/runtime/service.pid"
    # restart 必须等旧后台启动器释放锁，再拉起新启动器。
    wait_launcher_exit
}
install_service() {
    prepare
    [ "$(id -u)" = 0 ] || { log 'Install requires root';return 1; }
    [ "$BASE" = "$HEAD_CAM_HOME" ] || { log 'Run install from the final device directory';return 1; }
    version=$(cat "$BASE/SERVICE_VERSION");valid_version "$version"
    verify "$BASE" "$version"
    [ -f "$HEAD_CAM_HOME/monitor.json" ] || { log 'Missing monitor.json';return 1; }
    # 仅注册启动入口，不再复制整份程序，避免小分区重复占用。
    exec 6>"$HEAD_CAM_HOME/runtime/install.lock"
    flock -n 6 || { log 'Another installer is running';return 1; }
    if [ -f "$DIY_PATH" ] && grep -F '# BEGIN HEAD_CAM_SERVICE' "$DIY_PATH" >/dev/null; then
        grep -F "HEAD_CAM_HOME='$HEAD_CAM_HOME'" "$DIY_PATH" >/dev/null || { log 'Hook belongs to another directory';return 1; }
        log 'Autostart already enabled'
    else
        [ -d "${DIY_PATH%/*}" ] || { log 'Device DIY directory unavailable';return 1; }
        temporary=$(mktemp "${DIY_PATH}.head-cam.XXXXXX")
        if [ -f "$DIY_PATH" ]; then
            cp "$DIY_PATH" "$temporary"
            [ -f "${DIY_PATH}.head-cam.bak" ] || cp "$DIY_PATH" "${DIY_PATH}.head-cam.bak"
        else
            printf '#!/bin/sh\n' > "$temporary"
        fi
        cat >> "$temporary" <<EOF

# BEGIN HEAD_CAM_SERVICE
(
    HEAD_CAM_HOME='$HEAD_CAM_HOME'
    export HEAD_CAM_HOME
    if [ -f "\$HEAD_CAM_HOME/boot_autostart.sh" ]; then
        /bin/sh "\$HEAD_CAM_HOME/boot_autostart.sh" start
    fi
)
# END HEAD_CAM_SERVICE
EOF
        chmod 755 "$temporary"
        /bin/sh -n "$temporary" || { rm -f "$temporary";return 1; }
        "$LOCK_HELPER" --rename "$temporary" "$DIY_PATH" || { rm -f "$temporary";return 1; }
        sync
        log 'Enabled boot via device diy.sh; no program copy made'
    fi
    exec 6>&-
    start_service
}
case "${1:-run}" in
 install) install_service;; run) run_service;; start) start_service;; stop) stop_service;;
 restart) stop_service && start_service;;
 update-internal) prepare;update_release;;
 update) prepare;exec 9>"$HEAD_CAM_HOME/runtime/service.lock";flock -n 9 || { log 'Running; update deferred until restart';exit 0; };update_release;;
 status) if [ -f "$HEAD_CAM_HOME/runtime/service.pid" ];then cat "$HEAD_CAM_HOME/runtime/service.pid";else log 'Not started';fi;;
 *) echo 'Usage: boot_autostart.sh install|start|stop|restart|status|run|update' >&2;exit 64;;
esac
