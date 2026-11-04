"""离线 MQTT 协议和头环统计验证，无真实 Broker 依赖。"""
import json
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]



def packet(sock):
    def read(n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise EOFError
            data += chunk
        return data
    kind = read(1)[0]
    size = 0
    multiplier = 1
    while True:
        digit = read(1)[0]
        size += (digit & 127) * multiplier
        if digit < 128:
            break
        multiplier *= 128
    return kind, read(size)


class MonitorTest(unittest.TestCase):
    def test_summary_and_reconnect(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as broker:
            root = pathlib.Path(tmp)
            data = root / 'recordings'
            data.mkdir()
            today = time.strftime('%Y%m%d')
            (data / f'SSIEGO_Cam_001_{today}_120000_130000.mcap').write_bytes(b'abc')
            (data / 'unfinished.mcap.incomplete').write_bytes(b'ignored')
            (data / 'link.mcap').symlink_to(data / f'SSIEGO_Cam_001_{today}_120000_130000.mcap')
            broker.bind(('127.0.0.1', 0))
            broker.listen()
            broker.settimeout(35)
            config = json.loads((ROOT / 'config/monitor.json').read_text())
            config.update(enabled=True, host='127.0.0.1', port=broker.getsockname()[1], device_id='test', topic_prefix='test', recording_root=str(data), storage_path=str(root), scan_interval_sec=10, report_interval_sec=10)
            path = root / 'config.json'
            path.write_text(json.dumps(config))
            process = subprocess.Popen([str(ROOT / 'build-service-native/head-cam-monitor'), str(path), str(root / 'runtime')])
            try:
                with broker.accept()[0] as conn:
                    conn.settimeout(15)
                    kind, body = packet(conn)
                    self.assertEqual(kind, 0x10)
                    self.assertIn(b'offline', body)
                    conn.sendall(b'\x20\x02\x00\x00')
                    deadline = time.monotonic() + 15
                    summary = None
                    while time.monotonic() < deadline:
                        kind, body = packet(conn)
                        if kind == 0xc0:
                            conn.sendall(b'\xd0\x00')
                            continue
                        n = int.from_bytes(body[:2], 'big')
                        message = json.loads(body[2+n:])
                        if message['event_type'] == 'online':
                            self.assertEqual(message['device_type'], 'head_cam_v2')
                            self.assertEqual(message['ip'], '127.0.0.1')
                        if message['event_type'] == 'recording_summary' and 'recordings' in message:
                            summary = message
                            break
                    self.assertIsNotNone(summary)
                    self.assertEqual(summary['recordings']['file_count'], 1)
                    self.assertEqual(set(summary), {'schema_version', 'device_sn', 'event_type', 'ts', 'seq', 'recordings', 'storage', 'battery', 'alert'})
                    self.assertEqual(summary['device_sn'], 'test')
                    self.assertNotIn('total_size_bytes', summary['recordings'])
                    self.assertEqual(summary['recordings']['hourly']['rows'], [[today+'12', 1, 3]])
                    self.assertFalse(summary['battery']['valid'])
                # 网络中断不能终止监控进程，本地扫描持续运行。
                time.sleep(2)
                self.assertIsNone(process.poll())
                state = json.loads((root / 'runtime/status.json').read_text())
                self.assertEqual(state['snapshot']['recordings']['file_count'], 1)
                with broker.accept()[0] as conn:
                    conn.settimeout(15)
                    self.assertEqual(packet(conn)[0], 0x10)
                    conn.sendall(b'\x20\x02\x00\x00')
                    messages = []
                    for _ in range(2):
                        kind, body = packet(conn)
                        n = int.from_bytes(body[:2], 'big')
                        messages.append(json.loads(body[2+n:])['event_type'])
                    self.assertEqual(messages, ['online', 'recording_summary'])
            finally:
                process.terminate()
                process.wait(timeout=15)

    def test_unconfigured_stays_alive(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = pathlib.Path(tmp)/'disabled.json'
            config_path.write_text('{"enabled":false}')
            process = subprocess.Popen([str(ROOT / 'build-service-native/head-cam-monitor'), str(config_path), tmp])
            try:
                time.sleep(0.5)
                self.assertIsNone(process.poll())
                self.assertEqual(json.loads((pathlib.Path(tmp)/'status.json').read_text())['state'], 'unconfigured')
            finally:
                process.terminate()
                process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main()
