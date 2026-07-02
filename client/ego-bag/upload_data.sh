#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

DATA_DIR="data_saving"
LOG_DIR="log"
OSS_BASE="oss://ss-oss-data-platform/original_data/ego_human_data"

# 从配置文件读取设备类型
CONFIG_FILE="config.json"
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[错误] 配置文件不存在: $CONFIG_FILE"
    exit 1
fi
DEVICE_TYPE=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE'))['device_type'])")

OSS_BUCKET="$OSS_BASE/$DEVICE_TYPE"

echo "========================================="
echo "  数据上传脚本 (ossutil)"
echo "========================================="

if [ ! -d "$DATA_DIR" ]; then
    echo "[错误] 数据目录不存在: $DATA_DIR"
    exit 1
fi

# 统计待上传文件
FILE_COUNT=$(find "$DATA_DIR/$DEVICE_TYPE" -type f 2>/dev/null | wc -l)
TOTAL_SIZE=$(du -sh "$DATA_DIR/$DEVICE_TYPE" 2>/dev/null | cut -f1)
LOG_COUNT=$(find "$LOG_DIR" -type f 2>/dev/null | wc -l)
LOG_SIZE=$(du -sh "$LOG_DIR" 2>/dev/null | cut -f1)

echo "[信息] 设备类型 : $DEVICE_TYPE"
echo "[信息] 数据文件 : $FILE_COUNT 个, $TOTAL_SIZE"
echo "[信息] 日志文件 : $LOG_COUNT 个, $LOG_SIZE"
echo "[信息] 目标路径 : $OSS_BUCKET/"
echo ""
read -p "确认上传? (y/N): " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "[取消] 已取消上传"
    exit 0
fi

echo ""
echo "[上传] 上传录制数据..."
# data_saving/Orin/260511/020_device/ -> oss://.../ego_human_data/Orin/260511/020_device/
ossutil cp -r "$DATA_DIR/$DEVICE_TYPE/" "$OSS_BUCKET/" -f

if [ -d "$LOG_DIR" ]; then
    echo "[上传] 上传日志文件..."
    # 获取最新的日期目录（YYMMDD）
    LATEST_DATE=$(ls "$DATA_DIR/$DEVICE_TYPE/" 2>/dev/null | sort | tail -1)
    if [ -n "$LATEST_DATE" ]; then
        DEVICE_ID=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE'))['device_id'])")
        LOG_OSS_PATH="$OSS_BUCKET/$LATEST_DATE/${DEVICE_ID}_device/log"
        ossutil cp -r "$LOG_DIR/" "$LOG_OSS_PATH/" -f
    else
        echo "[警告] 未找到数据日期目录，跳过日志上传"
    fi
fi

echo "[完成] 上传结束"

# 上传后自动清理
AUTO_CLEAN=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE')).get('auto_clean_after_upload', False))")
if [ "$AUTO_CLEAN" = "True" ]; then
    echo ""
    echo "[清理] auto_clean_after_upload = true，删除本地数据和日志..."
    rm -rf "$DATA_DIR/$DEVICE_TYPE"
    rm -rf "$LOG_DIR"/*
    echo "[清理] 完成"
fi
