import pyzed.sl as sl
import cv2
import threading
import os
import time
import numpy as np

# 配置参数
RESOLUTION = sl.RESOLUTION.HD720
FPS = 15
BASE_SAVE_PATH = "./data_capture"

class ZEDCollector(threading.Thread):
    def __init__(self, serial_number, session_path):
        super().__init__()
        self.serial_number = serial_number
        self.cam_root = os.path.join(session_path, f"cam_{serial_number}")
        self.rgb_path = os.path.join(self.cam_root, "rgb")
        self.depth_path = os.path.join(self.cam_root, "depth")
        
        os.makedirs(self.rgb_path, exist_ok=True)
        os.makedirs(self.depth_path, exist_ok=True)
        
        self.zed = sl.Camera()
        self.running = True
        
    def run(self):
        init_params = sl.InitParameters()
        init_params.camera_resolution = RESOLUTION
        init_params.camera_fps = FPS
        init_params.coordinate_units = sl.UNIT.MILLIMETER
        init_params.depth_mode = sl.DEPTH_MODE.PERFORMANCE
        init_params.set_from_serial_number(self.serial_number)

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
                
                self.zed.retrieve_image(image, sl.VIEW.LEFT)
                self.zed.retrieve_measure(depth, sl.MEASURE.DEPTH)

                img_data = image.get_data()
                cv2.imwrite(f"{self.rgb_path}/{timestamp}.jpg", img_data)

                np.save(f"{self.depth_path}/{timestamp}.npy", depth.get_data())

                frame_count += 1
                if frame_count % 100 == 0:
                    print(f"相机 {self.serial_number} 已采集 {frame_count} 帧")

        self.zed.close()

def main():
    # 核心修改：命名格式为 年月日_时分秒
    session_name = time.strftime("%Y%m%d_%H%M%S")
    session_path = os.path.join(BASE_SAVE_PATH, session_name)
    
    os.makedirs(session_path, exist_ok=True)
    
    devices = sl.Camera.get_device_list()
    if not devices:
        print("未检测到相机")
        return
    
    collectors = []
    for dev in devices:
        sn = dev.serial_number
        collector = ZEDCollector(sn, session_path)
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
        print(f"采集完成。数据保存在: {session_path}")

if __name__ == "__main__":
    main()