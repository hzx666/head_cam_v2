#!/bin/sh
# 临时手动获取 DHCP 网关；不改现有 IP/DNS，不注册自启。
set -eu
set -f
case "$0" in */*) SCRIPT_DIR=$(cd "${0%/*}" && pwd);; *) SCRIPT_DIR=$(pwd);; esac
TOOL=${HEAD_CAM_TOOL:-$SCRIPT_DIR/head-cam-service}
if [ ! -x "$TOOL" ] && [ -x "$SCRIPT_DIR/../dist/head-cam-service" ];then TOOL=$SCRIPT_DIR/../dist/head-cam-service;fi
[ -x "$TOOL" ] || { echo 'Missing executable head-cam-service' >&2;exit 1; }

if [ "${1:-}" = --lease ]; then
    case "${2:-}" in
        deconfig|nak|leasefail) exit 0;;
        bound|renew)
            [ "${interface:-}" = "${HEAD_CAM_DHCP_IFACE:-}" ] || exit 1
            # DHCP router 可能包含多个地址，采用首个，不写死网关。
            set -- ${router:-}
            [ "$#" -gt 0 ] || { echo 'DHCP response has no router option' >&2;exit 1; }
            gateway=$1
            case "$gateway" in ''|*[!0-9.]*) exit 1;; esac
            "$TOOL" ipcalc -m "$gateway" >/dev/null || exit 1
            "$TOOL" ip route replace default via "$gateway" dev "$interface" || exit 1
            printf '%s\n' "$gateway" > "$HEAD_CAM_DHCP_STATE/gateway"
            ;;
    esac
    exit 0
fi

iface=${1:-wlan0}
case "$iface" in ''|*[!a-zA-Z0-9_.:-]*) echo 'Invalid interface' >&2;exit 64;; esac
[ "$("$TOOL" id -u)" = 0 ] || { echo 'Run as root' >&2;exit 1; }
if ! "$TOOL" ip -4 addr show dev "$iface" | "$TOOL" grep -q 'inet ';then
    echo 'Interface has no IPv4 address; connect WiFi first' >&2;exit 1
fi
HEAD_CAM_DHCP_STATE=$("$TOOL" mktemp -d /tmp/head-cam-dhcp.XXXXXX)
HEAD_CAM_DHCP_IFACE=$iface
HEAD_CAM_DHCP_SCRIPT=$SCRIPT_DIR/${0##*/}
HEAD_CAM_TOOL=$TOOL
export HEAD_CAM_DHCP_STATE HEAD_CAM_DHCP_IFACE HEAD_CAM_DHCP_SCRIPT HEAD_CAM_TOOL
trap '"$TOOL" rm -rf "$HEAD_CAM_DHCP_STATE"' 0
trap 'exit 1' 1 2 15
cat > "$HEAD_CAM_DHCP_STATE/hook.sh" <<'EOF'
#!/bin/sh
exec /bin/sh "$HEAD_CAM_DHCP_SCRIPT" --lease "$@"
EOF
"$TOOL" chmod 700 "$HEAD_CAM_DHCP_STATE/hook.sh"
echo "Requesting DHCP router on $iface (keeps existing IP and DNS)..."
if ! "$TOOL" timeout 25 "$TOOL" udhcpc -f -n -q -t 3 -T 3 -i "$iface" -s "$HEAD_CAM_DHCP_STATE/hook.sh"; then
    echo 'DHCP request failed; no gateway confirmed' >&2;exit 1
fi
[ -s "$HEAD_CAM_DHCP_STATE/gateway" ] || { echo 'DHCP supplied no usable gateway' >&2;exit 1; }
printf 'Default gateway: '
"$TOOL" cat "$HEAD_CAM_DHCP_STATE/gateway"
"$TOOL" ip route
