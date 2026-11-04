"""验证 ARM64 单文件默认启动，无 run-* 脚本依赖，不访问 OSS/MQTT。"""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    home = Path(tmp)
    (home/'monitor.json').write_text('{"enabled":false}')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, HEAD_CAM_HOME=tmp, UPLOAD_HOST='127.0.0.1', UPLOAD_PORT=str(port))
    env.pop('HEAD_CAM_MONITOR_CONFIG', None)
    process = subprocess.Popen(['qemu-aarch64', str(ROOT/'dist/head-cam-service')], env=env)
    try:
        deadline = time.monotonic()+20
        ready = False
        while time.monotonic()<deadline:
            if process.poll() is not None:
                raise RuntimeError('Unified process exited')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=1) as sock:
                    sock.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                    ready = b'200' in sock.recv(1024)
            except OSError:
                pass
            status = home/'runtime/monitor/status.json'
            if ready and status.exists():
                assert json.loads(status.read_text())['state'] == 'unconfigured'
                break
            time.sleep(0.2)
        else:
            raise RuntimeError('Components did not become ready')
        print('PASS: ARM64 single binary starts isolated upload and monitor without external component scripts')
    finally:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
