"""同一二进制的上传、监控进程独立恢复验证。"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UnifiedTest(unittest.TestCase):
    def test_monitor_crash_keeps_upload_process(self):
        binary = ROOT/'build-service-native/head-cam-service'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            credentials = root/'credentials.json'
            credentials.write_text(json.dumps(dict(endpoint='oss.invalid', bucket='test-bucket', access_key_id='test', access_key_secret='test')))
            config = root/'monitor.json'
            config.write_text('{"enabled":false}')
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            fake = ROOT/'tests/fake_ossutil.py'
            fake.chmod(0o700)
            entries = []
            arguments = [f'upload --runtime-dir {root}/upload --host 127.0.0.1 --port {port} --test-config {credentials} --test-ossutil {fake}', f'monitor {config} {root}/monitor']
            for index, args in enumerate(arguments):
                entry = root/f'entry{index}'
                entry.write_text(f'#!/bin/sh\nexec "{binary}" {args}\n')
                entry.chmod(0o700)
                entries.append(str(entry))
            env = dict(os.environ, FAKE_OSS_ROOT=str(root/'oss'))
            env.pop('HEAD_CAM_HOME', None)
            process = subprocess.Popen([str(binary), 'supervise', str(root/'lock'), *entries], env=env)
            def children():
                pids = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
                result = {}
                for pid in pids:
                    try:
                        words = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                    except FileNotFoundError:
                        continue
                    if len(words)>1 and words[1] in (b'upload', b'monitor'):
                        result[words[1].decode()] = int(pid)
                return result
            try:
                deadline = time.monotonic()+5
                while time.monotonic()<deadline:
                    initial = children()
                    if 'monitor' in initial and 'upload' in initial and (root/'monitor/status.json').exists():
                        break
                    time.sleep(0.1)
                self.assertIn('monitor', initial)
                self.assertIn('upload', initial)
                os.kill(initial['monitor'], signal.SIGKILL)
                deadline = time.monotonic()+15
                current = {}
                while time.monotonic()<deadline:
                    current = children()
                    if current.get('monitor') and current['monitor']!=initial['monitor']:
                        break
                    time.sleep(0.2)
                self.assertEqual(current['upload'], initial['upload'])
                self.assertNotEqual(current['monitor'], initial['monitor'])
                with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
                    sock.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                    self.assertIn(b'200', sock.recv(1024))
            finally:
                process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
