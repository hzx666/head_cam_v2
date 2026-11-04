#!/usr/bin/env bash
# Build one static ARM64 C executable with embedded ossutil, CA and credentials.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ $(uname -s) == Linux ]] || { echo 'Build on Linux (WSL is supported).' >&2; exit 1; }
ARCH=${TARGET_ARCH:-arm64}
[[ $ARCH == arm64 || $ARCH == x86_64 ]] || { echo 'Unsupported TARGET_ARCH' >&2; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
umask 077
if [[ -n ${CREDENTIALS_JSON:-} ]]; then
    CREDS=(--credentials-json "$CREDENTIALS_JSON")
elif [[ -n ${CREDENTIALS_SOURCE:-} ]]; then
    CREDS=(--credentials-source "$CREDENTIALS_SOURCE")
else
    echo 'Set CREDENTIALS_SOURCE to teleop/upload_core.py or CREDENTIALS_JSON to private build credentials.' >&2
    exit 1
fi
if [[ $ARCH == arm64 ]]; then
    OSSUTIL=${OSSUTIL_BIN:-"$ROOT/third_party/ossutil/ossutilarm64"}
    DEPS=${SERVICE_DEPS:-"$ROOT/.deps/arm64"}
    [[ -f "$DEPS/usr/lib/aarch64-linux-gnu/libcrypto.a" ]] || { echo 'First run: bash scripts/prepare_arm64.sh' >&2; exit 1; }
    CMAKE_TARGET=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm64.cmake" -DSERVICE_DEPS="$DEPS")
    STRIP=aarch64-linux-gnu-strip
else
    OSSUTIL=${OSSUTIL_BIN:?Set OSSUTIL_BIN to static Linux x86_64 ossutil}
    CMAKE_TARGET=()
    STRIP=strip
fi
python3 "$ROOT/scripts/embed_bundle.py" "${CREDS[@]}" --ossutil "$OSSUTIL" \
    --ca "${CA_BUNDLE:-/etc/ssl/certs/ca-certificates.crt}" --out "$WORK/bundle" --arch "$ARCH"
cmake -S "$ROOT" -B "$WORK/build" "${CMAKE_TARGET[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DBUSYBOX_BIN="${BUSYBOX_BIN:-$ROOT/.deps/arm64/bin/busybox}" -DSERVICE_STATIC=ON -DSERVICE_TESTING=OFF -DBUNDLE_DIR="$WORK/bundle"
cmake --build "$WORK/build" -j "${BUILD_JOBS:-4}"
"$STRIP" "$WORK/build/head-cam-service"
if readelf -l "$WORK/build/head-cam-service" | grep -q INTERP; then
    echo 'Refusing non-static release.' >&2; exit 1
fi
# Running a cross-built self-check requires ARM binfmt support for its child helper.
if [[ $(uname -m) == aarch64 && $ARCH == arm64 || $(uname -m) == x86_64 && $ARCH == x86_64 ]]; then
    "$WORK/build/head-cam-service" --self-check
elif [[ -n ${QEMU_SELF_CHECK:-} ]]; then
    qemu-aarch64 "$WORK/build/head-cam-service" --self-check
fi
mkdir -p "$ROOT/dist"
install -m 700 "$WORK/build/head-cam-service" "$ROOT/dist/head-cam-service"
# 清理旧交付组件，避免误用旧进程入口。
rm -f "$ROOT/dist/head-cam-upload" "$ROOT/dist/head-cam-monitor" "$ROOT/dist/busybox"
install -m 700 "$ROOT/scripts/boot_autostart.sh" "$ROOT/scripts/service_entry.sh" "$ROOT/scripts/run-upload" "$ROOT/scripts/run-monitor" "$ROOT/dist/"
install -m 700 "$ROOT/scripts/manual_dhcp.sh" "$ROOT/dist/manual_dhcp.sh"
install -m 600 "$ROOT/config/monitor.json" "$ROOT/dist/monitor.json"
install -m 600 "$ROOT/SERVICE_VERSION" "$ROOT/dist/SERVICE_VERSION"
(
    cd "$ROOT/dist"
    sha256sum head-cam-service service_entry.sh run-upload run-monitor boot_autostart.sh manual_dhcp.sh monitor.json SERVICE_VERSION > SHA256SUMS
    {
        echo "Target: Linux $ARCH; static C11 executable"
        echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo 'SERVICE_TESTING=OFF'
        echo "Embedded ossutil SHA256: $(sha256sum "$OSSUTIL" | cut -d ' ' -f 1)"
        echo "Embedded CA SHA256: $(sha256sum "${CA_BUNDLE:-/etc/ssl/certs/ca-certificates.crt}" | cut -d ' ' -f 1)"
        echo 'Source references: cJSON v1.7.19 (MIT); target OpenSSL/LZ4/Zstd from Ubuntu packages'
        if [[ $ARCH == arm64 ]]; then aarch64-linux-gnu-gcc --version | head -n 1; else gcc --version | head -n 1; fi
    } > build-info.txt
    if [[ $ARCH == arm64 && -f "$DEPS/package-sha256.txt" ]]; then
        cat "$DEPS/package-sha256.txt" >> build-info.txt
    fi
)
echo "Built unified service: $ROOT/dist (deploy the complete directory)."
