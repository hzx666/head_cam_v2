#!/bin/bash
# build_release.sh — 构建 upload_mcap 两个产物（teleop / head_cam）
# 每个产物内嵌 ossutil + mcap + cryptography，并固化默认 --kind，运行无需任何外部依赖。
# 名称参考旧脚本: teleop_upload_release_<ts> / head_cam_upload_<ts>

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
BUILD_DIR="$SCRIPT_DIR/.build"
OSSUTIL_VERSION="1.7.18"
KEEP_RELEASES=5
OSSUTIL_PAYLOAD_DIR="$BUILD_DIR/ossutil"
mkdir -p "$BUILD_DIR" "$OSSUTIL_PAYLOAD_DIR"

SYSTEM=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64) OSSUTIL_NAME="ossutil64" ;;
    aarch64|arm64) OSSUTIL_NAME="ossutilarm64" ;;
    *) OSSUTIL_NAME="ossutil64" ;;
esac
if [ "$SYSTEM" = "windows" ]; then EXT=".exe"; OSSUTIL_NAME="ossutil64.exe"; \
else EXT=""; fi
SEP=":"; [ "$SYSTEM" = "windows" ] && SEP=";"

echo "========================================="
echo "  构建 upload_mcap (两产物: teleop / head_cam)"
echo "========================================="

# 构建依赖
for dep in PyInstaller mcap cryptography redis; do
    if ! python3 -c "import $dep" 2>/dev/null; then
        echo "[安装] $dep ..."
        python3 -m pip install "$dep" --user --break-system-packages 2>&1 | tail -2
    fi
done

# 内嵌 ossutil
OSSUTIL_BIN="$OSSUTIL_PAYLOAD_DIR/$OSSUTIL_NAME"
if [ ! -x "$OSSUTIL_BIN" ]; then
    echo "[下载] ossutil $OSSUTIL_NAME (~12MB)..."
    URL="https://gosspublic.alicdn.com/ossutil/${OSSUTIL_VERSION}/${OSSUTIL_NAME}"
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$URL" -o "$OSSUTIL_BIN"; \
    else wget -q "$URL" -O "$OSSUTIL_BIN"; fi
    chmod +x "$OSSUTIL_BIN"
fi
OSSUTIL_BIN_ABS="$(cd "$(dirname "$OSSUTIL_BIN")" && pwd)/$(basename "$OSSUTIL_BIN")"

# ─── 编译单个产物 ──────────────────────────────
# $1=kind $2=product目录名 $3=产物名前缀
build_product() {
    local kind="$1" product="$2" prefix="$3"
    local DIST_DIR="$REPO_ROOT/dist/$product"
    local OUTPUT_NAME="${prefix}${TIMESTAMP}${EXT}"
    local ENTRY="$SCRIPT_DIR/_entry_${product}.py"
    mkdir -p "$DIST_DIR"

    # 生成 wrapper 入口：固化默认 kind，复用 upload_mcap 全部逻辑
    cat > "$ENTRY" <<PYEOF
import os
os.environ["UMCAP_DEFAULT_KIND"] = "$kind"
from upload_mcap import main
main()
PYEOF

    echo ""
    echo "[编译] $product → $OUTPUT_NAME (默认 kind=$kind)"
    python3 -m PyInstaller \
        --onefile \
        --name "$OUTPUT_NAME" \
        --distpath "$DIST_DIR" \
        --workpath "$BUILD_DIR/work_$product" \
        --specpath "$BUILD_DIR" \
        --add-data "${OSSUTIL_BIN_ABS}${SEP}ossutil" \
        --collect-all mcap \
        --hidden-import=cryptography \
        --hidden-import=redis \
        --collect-submodules=cryptography \
        --noupx \
        --clean \
        "$ENTRY" 2>&1 | grep -E "(INFO: Building EXE from|ERROR)" || true

    rm -f "$ENTRY"
    if [ ! -f "$DIST_DIR/$OUTPUT_NAME" ]; then
        echo "[错误] $product 编译失败"
        exit 1
    fi
    chmod +x "$DIST_DIR/$OUTPUT_NAME"
    rm -rf "$BUILD_DIR/work_$product" "$BUILD_DIR/$OUTPUT_NAME.spec"

    # 保留最近 N 个产物
    local OLD_RELEASES
    OLD_RELEASES=$(ls -1t "$DIST_DIR" 2>/dev/null | tail -n +$((KEEP_RELEASES + 1)) || true)
    if [ -n "$OLD_RELEASES" ]; then
        while IFS= read -r old; do [ -n "$old" ] && rm -f "$DIST_DIR/$old"; done <<< "$OLD_RELEASES"
    fi

    echo "  ✓ $DIST_DIR/$OUTPUT_NAME ($(du -h "$DIST_DIR/$OUTPUT_NAME" | cut -f1))"
}

build_product teleop   teleop   "teleop_upload_release_"
build_product head_cam head_cam "head_cam_upload_"

echo ""
echo "========================================="
echo "  构建完成"
echo "========================================="
echo "  dist/teleop/teleop_upload_release_${TIMESTAMP}     (纯交互, 默认 kind=teleop)"
echo "  dist/head_cam/head_cam_upload_${TIMESTAMP}         (纯交互, 默认 kind=head_cam)"
echo "  用法: 直接运行 <产物>  # 不接受参数, 自动检测磁盘进入交互"
echo "  说明: 单文件可执行，内嵌 ossutil + mcap，无需任何外部依赖"
