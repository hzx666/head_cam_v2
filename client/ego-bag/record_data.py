import os
import json
import csv
import time
import cv2
import numpy as np
import pyrealsense2 as rs
import pyzed.sl as sl
from multiprocessing import Process, Event, set_start_method
from datetime import datetime

# --- 加载配置 ---
with open(os.path.join(os.path.dirname(__file__), "config.json")) as f:
    _cfg = json.load(f)

# --- 硬件配置 ---
RS_SERIALS = ["260322271258", "260322278406"]
FPS = 30
WIDTH, HEIGHT = 640, 480
DEVICE_TYPE = _cfg["device_type"]
DEVICE_ID = _cfg["device_id"]
SAVE_ROOT = f"./data_saving/{DEVICE_TYPE}/{datetime.now().strftime('%y%m%d')}/{DEVICE_ID}_device"

def get_video_writer(path, width, height, fps):
    """Create video writer - each camera writes its own file"""
    # Try MJPG first (most compatible)
    fourcc = cv2.VideoWriter_fourcc(*'MJPG')
    writer = cv2.VideoWriter(path, fourcc, fps, (width, height), isColor=True)
    if writer.isOpened():
        print(f"[视频] 开始录制 (MJPG): {path}")
        return writer

    # Fallback to raw I420
    print(f"[警告] MJPG 失败，尝试 I420: {path}")
    fourcc = cv2.VideoWriter_fourcc(*'I420')
    writer = cv2.VideoWriter(path.replace('.avi', '_i420.avi'), fourcc, fps, (width, height), isColor=True)
    if writer.isOpened():
        print(f"[视频] 开始录制 (I420): {path}")
        return writer

    print(f"[错误] 无法创建视频写入器: {path}")
    return writer

def run_d405(serial, cam_id, stop_event):
    """D405 采集进程 - 直接写入视频文件"""
    cam_name = f"d405_{cam_id}"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    video_path = f"{SAVE_ROOT}/{cam_name}_{timestamp}.avi"
    os.makedirs(SAVE_ROOT, exist_ok=True)

    # 初始化视频写入器
    writer = get_video_writer(video_path, WIDTH, HEIGHT, FPS)
    if not writer.isOpened():
        return

    pipeline = rs.pipeline()
    config = rs.config()
    config.enable_device(serial)
    config.enable_stream(rs.stream.color, WIDTH, HEIGHT, rs.format.bgr8, FPS)

    try:
        pipeline.start(config)
    except Exception as e:
        print(f"[错误] D405-{cam_id} 无法启动: {e}")
        writer.release()
        return

    # --- 统计变量 ---
    frame_count = 0
    start_time = time.time()
    last_report_time = time.time()

    try:
        while not stop_event.is_set():
            frames = pipeline.wait_for_frames(timeout_ms=5000)
            color_img = np.asanyarray(frames.get_color_frame().get_data())

            writer.write(color_img)
            frame_count += 1

            # 每 5 秒输出一次统计信息
            current_time = time.time()
            if current_time - last_report_time >= 5.0:
                avg_fps = frame_count / (current_time - start_time)
                print(f"[统计] D405-{cam_id} ({serial}): 已采集 {frame_count} 帧, 平均 FPS: {avg_fps:.2f}")
                last_report_time = current_time

    except Exception as e:
        if not stop_event.is_set():
            print(f"[错误] D405-{cam_id}: {e}")
    finally:
        pipeline.stop()
        writer.release()
        end_ts = datetime.now().strftime("%H%M%S")
        final_path = f"{SAVE_ROOT}/{cam_name}_{timestamp}_{end_ts}.avi"
        os.rename(video_path, final_path)
        print(f"[完成] D405-{cam_id} 视频已保存: {final_path}")

def run_zed_mini(stop_event):
    """ZED Mini 采集进程 - 直接写入视频文件（含深度视频 + IMU 数据）"""
    cam_name = "zed_mini"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    video_path = f"{SAVE_ROOT}/{cam_name}_{timestamp}.avi"
    depth_video_path = f"{SAVE_ROOT}/{cam_name}_depth_{timestamp}.avi"
    imu_path = f"{SAVE_ROOT}/{cam_name}_imu_{timestamp}.csv"
    os.makedirs(SAVE_ROOT, exist_ok=True)

    zed = sl.Camera()
    init_params = sl.InitParameters()
    init_params.camera_resolution = sl.RESOLUTION.VGA
    init_params.camera_fps = FPS
    init_params.depth_mode = sl.DEPTH_MODE.PERFORMANCE
    init_params.camera_image_flip = sl.FLIP_MODE.OFF
    init_params.sensors_required = True

    if zed.open(init_params) != sl.ERROR_CODE.SUCCESS:
        print("[错误] 无法打开 ZED Mini 相机")
        return

    image = sl.Mat()
    depth_map = sl.Mat()
    sensors_data = sl.SensorsData()
    runtime = sl.RuntimeParameters()
    writer = None
    depth_writer = None
    imu_file = open(imu_path, 'w', newline='')
    imu_writer = csv.writer(imu_file)
    imu_writer.writerow(['timestamp_ns', 'accel_x', 'accel_y', 'accel_z',
                         'gyro_x', 'gyro_y', 'gyro_z'])

    # --- 统计变量 ---
    frame_count = 0
    imu_count = 0
    start_time = time.time()
    last_report_time = time.time()

    try:
        while not stop_event.is_set():
            if zed.grab(runtime) == sl.ERROR_CODE.SUCCESS:
                # --- 采集 IMU 数据 ---
                zed.get_sensors_data(sensors_data, sl.TIME_REFERENCE.IMAGE)
                imu_data = sensors_data.get_imu_data()
                ts_ns = imu_data.timestamp.get_nanoseconds()
                accel = imu_data.get_linear_acceleration()
                gyro = imu_data.get_angular_velocity()
                imu_writer.writerow([ts_ns,
                                     accel[0], accel[1], accel[2],
                                     gyro[0], gyro[1], gyro[2]])
                imu_count += 1

                # --- 采集图像 ---
                zed.retrieve_image(image, sl.VIEW.LEFT)
                color_img = image.get_data()
                # Convert RGBA to BGR for VideoWriter
                if color_img.shape[2] == 4:
                    color_img = cv2.cvtColor(color_img, cv2.COLOR_RGBA2BGR)
                else:
                    color_img = cv2.cvtColor(color_img, cv2.COLOR_RGB2BGR)

                # Retrieve depth as a 32-bit float (meters)
                zed.retrieve_measure(depth_map, sl.MEASURE.DEPTH)
                depth_data = depth_map.get_data()  # float32, NaN for invalid

                # Normalize depth to 0-255 for visualization (ignore NaN/inf)
                valid_mask = np.isfinite(depth_data)
                depth_vis = np.zeros_like(depth_data, dtype=np.uint8)
                if valid_mask.any():
                    d_min = depth_data[valid_mask].min()
                    d_max = depth_data[valid_mask].max()
                    if d_max > d_min:
                        depth_norm = np.where(valid_mask,
                                              (depth_data - d_min) / (d_max - d_min),
                                              0.0)
                        depth_vis = (depth_norm * 255).astype(np.uint8)
                depth_color = cv2.applyColorMap(depth_vis, cv2.COLORMAP_JET)

                # Create writers on first frame using actual dimensions
                if writer is None:
                    h, w = color_img.shape[:2]
                    print(f"[ZED Mini] 实际分辨率: {w}x{h}")
                    writer = get_video_writer(video_path, w, h, FPS)
                    if not writer.isOpened():
                        return
                    depth_writer = get_video_writer(depth_video_path, w, h, FPS)
                    if not depth_writer.isOpened():
                        print("[警告] 深度视频写入器创建失败，将跳过深度保存")
                        depth_writer = None

                writer.write(color_img)
                if depth_writer is not None:
                    depth_writer.write(depth_color)
                frame_count += 1

                current_time = time.time()
                if current_time - last_report_time >= 5.0:
                    avg_fps = frame_count / (current_time - start_time)
                    print(f"[统计] ZED Mini: 视频 {frame_count} 帧, IMU {imu_count} 条, 平均 FPS: {avg_fps:.2f}")
                    last_report_time = current_time
    except Exception as e:
        if not stop_event.is_set():
            print(f"[错误] ZED Mini: {e}")
    finally:
        zed.close()
        imu_file.close()
        end_ts = datetime.now().strftime("%H%M%S")
        final_imu_path = f"{SAVE_ROOT}/{cam_name}_imu_{timestamp}_{end_ts}.csv"
        if os.path.exists(imu_path):
            os.rename(imu_path, final_imu_path)
        print(f"[完成] ZED Mini IMU 数据已保存: {final_imu_path} ({imu_count} 条)")
        if writer is not None:
            writer.release()
            final_path = f"{SAVE_ROOT}/{cam_name}_{timestamp}_{end_ts}.avi"
            if os.path.exists(video_path):
                os.rename(video_path, final_path)
            print(f"[完成] ZED Mini 视频已保存: {final_path}")
        if depth_writer is not None:
            depth_writer.release()
            final_depth_path = f"{SAVE_ROOT}/{cam_name}_depth_{timestamp}_{end_ts}.avi"
            if os.path.exists(depth_video_path):
                os.rename(depth_video_path, final_depth_path)
            print(f"[完成] ZED Mini 深度视频已保存: {final_depth_path}")

if __name__ == "__main__":
    set_start_method('spawn', force=True)
    os.makedirs(SAVE_ROOT, exist_ok=True)

    processes = []
    stop_event = Event()

    # 1. 动态检测并启动 RealSense 相机
    # 遍历配置的序列号，只启动实际连接的相机
    ctx = rs.context()
    connected_serials = [dev.get_info(rs.camera_info.serial_number) for dev in ctx.devices if dev.get_info(rs.camera_info.name).find("D405") != -1]

    print(f"[检测] 发现 D405 相机序列号: {connected_serials}")

    for i, serial in enumerate(RS_SERIALS):
        if serial in connected_serials:
            # 只有当相机物理连接时才创建进程
            p = Process(target=run_d405, args=(serial, i+1, stop_event))
            processes.append(p)
            print(f"[启动] 准备启动 D405-{i+1} ({serial})")
        else:
            print(f"[跳过] 未找到 D405 相机 ({serial})，跳过启动。")

    # 2. 启动 ZED Mini (假设 ZED 总是连接，也可以加类似检测)
    # 简单起见，这里直接启动，如果没连会在 run_zed_mini 内部报错退出
    processes.append(Process(target=run_zed_mini, args=(stop_event,)))
    print(f"[启动] 准备启动 ZED Mini")

    print(f"\n开始同步采集任务... (共启动 {len(processes)} 个进程)")
    print("各相机将每 5 秒汇报一次进度。按 Ctrl+C 停止。\n")

    for p in processes:
        p.start()
        # 同时启动所有相机进程

    try:
        for p in processes:
            p.join()
    except KeyboardInterrupt:
        print("\n[正在关闭] 通知各采集进程停止...")
        stop_event.set()
        for p in processes:
            p.join()
        print("[完成] 所有视频已保存。")
