import pyzed.sl as sl
import cv2
import threading
import os
import time

# 配置参数
RESOLUTION = sl.RESOLUTION.HD720
FPS = 30
SAVE_PATH = "./data_capture"

class ZEDCollector(threading.Thread):
    def __init__(self, serial_number, output_dir):
        super().__init__()
        self.serial_number = serial_number
        self.output_dir = output_dir
        self.zed = sl.Camera()
        self.running = True
        
        # 创建相机专属目录
        self.rgb_path = os.path.join(output_dir, f"cam_{serial_number}/rgb")
        self.depth_path = os.path.join(output_dir, f"cam_{serial_number}/depth")
        os.makedirs(self.rgb_path, exist_ok=True)
        os.makedirs(self.depth_path, exist_ok=True)

    def run(self):
        # 初始化参数
        init_params = sl.InitParameters()
        init_params.camera_resolution = RESOLUTION
        init_params.camera_fps = FPS
        init_params.coordinate_units = sl.UNIT.MILLIMETER
        init_params.depth_mode = sl.DEPTH_MODE.ULTRA # 或 PERFORMANCE 以降低计算负载
        init_params.set_from_serial_number(self.serial_number)

        # 打开相机
        if self.zed.open(init_params) != sl.ERROR_CODE.SUCCESS:
            print(f"无法打开相机: {self.serial_number}")
            return

        runtime_params = sl.RuntimeParameters()
        
        image = sl.Mat()
        depth = sl.Mat()

        print(f"开始采集相机: {self.serial_number}")
        
        frame_count = 0
        while self.running:
            if self.zed.grab(runtime_params) == sl.ERROR_CODE.SUCCESS:
                timestamp = self.zed.get_timestamp(sl.TIME_REFERENCE.IMAGE).get_milliseconds()
                
                # 获取左目 RGB 图像
                self.zed.retrieve_image(image, sl.VIEW.LEFT)
                # 获取深度数据 (32-bit float)
                self.zed.retrieve_measure(depth, sl.MEASURE.DEPTH)

                # 保存图像 (OpenCV 格式转换)
                img_data = image.get_data()
                cv2.imwrite(f"{self.rgb_path}/{timestamp}.jpg", img_data)

                # 保存深度图 (为了速度，建议保存为二进制 raw 或 numpy 压缩格式)
                # 注意：直接保存 numpy 可能会影响 30fps 的写入，建议视 IO 情况优化
                import numpy as np
                np.save(f"{self.depth_path}/{timestamp}.npy", depth.get_data())

                frame_count += 1
                if frame_count % 100 == 0:
                    print(f"相机 {self.serial_number} 已采集 {frame_count} 帧")

        self.zed.close()

def main():
    # 1. 获取所有连接的相机序列号
    devices = sl.Camera.get_device_list()
    if len(devices) < 3:
        print(f"警告：检测到 {len(devices)} 个相机，预期 3 个。")
    
    collectors = []
    for dev in devices:
        sn = dev.serial_number
        collector = ZEDCollector(sn, SAVE_PATH)
        collectors.append(collector)
        collector.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n正在停止采集...")
        for c in collectors:
            c.running = False
        for c in collectors:
            c.join()
        print("采集完成。")

if __name__ == "__main__":
    main()