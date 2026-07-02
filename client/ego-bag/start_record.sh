#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================="
echo "  多相机采集系统 启动脚本"
echo "========================================="

# --- 读取配置 ---
CONFIG_FILE="config.json"
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[错误] 配置文件不存在: $CONFIG_FILE"
    exit 1
fi

DEVICE_TYPE=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE'))['device_type'])")
DEVICE_ID=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE'))['device_id'])")

echo ""
echo "[配置] device_type : $DEVICE_TYPE"
echo "[配置] device_id   : $DEVICE_ID"

echo ""
echo "========================================="
echo ""

# --- 准备日志文件 ---
LOG_DIR="$SCRIPT_DIR/log"
mkdir -p "$LOG_DIR"
START_TS=$(date +"%Y%m%d_%H%M%S")
LOG_FILE="$LOG_DIR/${START_TS}.log"
echo "[日志] 日志文件: $LOG_FILE"
echo ""

# --- 激活虚拟环境并运行，同时记录日志 ---
source ./angenv/bin/activate

finish_log() {
    if [ -f "$LOG_FILE" ]; then
        END_TS=$(date +"%Y%m%d_%H%M%S")
        FINAL_LOG="$LOG_DIR/${START_TS}~${END_TS}.log"
        mv "$LOG_FILE" "$FINAL_LOG"
        echo "[日志] 日志已保存: $FINAL_LOG"
    fi
}
trap finish_log EXIT

python record_data.py 2>&1 | tee "$LOG_FILE"
