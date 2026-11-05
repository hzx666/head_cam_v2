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


def remaining(n):
    out = bytearray()
    while True:
        digit = n % 128
        n //= 128
        out.append(digit | (128 if n else 0))
        if not n:
            return bytes(out)


def mqtt_publish(topic, payload, qos=0, packet_id=1):
    topic_bytes = topic.encode()
    body = len(topic_bytes).to_bytes(2, 'big') + topic_bytes
    if qos:
        body += packet_id.to_bytes(2, 'big')
    body += payload.encode()
    return bytes([0x30 | (qos << 1)]) + remaining(len(body)) + body


def handshake(test, conn, prefix, device):
    conn.settimeout(15)
    kind, body = packet(conn)
    test.assertEqual(kind, 0x10)
    test.assertIn(b'offline', body)
    conn.sendall(b'\x20\x02\x00\x00')
    kind, body = packet(conn)
    test.assertEqual(kind, 0x82)
    test.assertEqual(body[:2], b'\x00\x01')
    topics = []
    index = 2
    while index < len(body):
        length = int.from_bytes(body[index:index + 2], 'big')
        index += 2
        topics.append(body[index:index + length].decode())
        index += length
        test.assertEqual(body[index], 1)
        index += 1
    test.assertEqual(topics, [
        f'{prefix}/device/{device}/downlink/req',
        f'{prefix}/device/{device}/downlink/resp',
    ])
    conn.sendall(bytes([0x90, 2 + len(topics), 0x00, 0x01, 0x01, 0x01]))


def app_message(conn):
    while True:
        kind, body = packet(conn)
        if kind == 0xc0:
            conn.sendall(b'\xd0\x00')
            continue
        if kind == 0x40:
            continue
        if (kind & 0xf0) != 0x30:
            raise AssertionError(hex(kind))
        qos = (kind >> 1) & 0x03
        length = int.from_bytes(body[:2], 'big')
        topic = body[2:2 + length].decode()
        payload = body[2 + length:]
        if qos:
            conn.sendall(b'\x40\x02' + payload[:2])
            payload = payload[2:]
        return topic, json.loads(payload)


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
                    handshake(self, conn, 'test', 'test')
                    deadline = time.monotonic() + 15
                    summary = None
                    while time.monotonic() < deadline:
                        topic, message = app_message(conn)
                        self.assertTrue(topic.startswith('test/device/test/'))
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
                    handshake(self, conn, 'test', 'test')
                    messages = [app_message(conn)[1]['event_type'] for _ in range(2)]
                    self.assertEqual(messages, ['online', 'recording_summary'])
            finally:
                process.terminate()
                process.wait(timeout=15)

    def test_rpc_and_properties(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as broker:
            root = pathlib.Path(tmp)
            data = root / 'recordings'
            data.mkdir()
            broker.bind(('127.0.0.1', 0))
            broker.listen()
            broker.settimeout(35)
            config = json.loads((ROOT / 'config/monitor.json').read_text())
            config.update(enabled=True, host='127.0.0.1', port=broker.getsockname()[1], device_id='test', topic_prefix='lab', hardware_model='TEST_MODEL', recording_root=str(data), storage_path=str(root), scan_interval_sec=10, report_interval_sec=10)
            path = root / 'config.json'
            path.write_text(json.dumps(config))
            version = (ROOT / 'SERVICE_VERSION').read_text().strip()
            process = subprocess.Popen([str(ROOT / 'build-service-native/head-cam-monitor'), str(path), str(root / 'runtime')])
            downlink = 'lab/device/test/downlink/req'

            def rpc(request_id, method, body, now=None):
                now = int(time.time() * 1000) if now is None else now
                conn.sendall(mqtt_publish(downlink, json.dumps({
                    'protocolVersion': '1.0', 'device_id': 'test', 'device_type': 'head_cam_v2',
                    'event_type': 'rpc_request', 'ts': now, 'requestId': request_id, 'method': method,
                    'expiresAt': now + 120000, 'data': body,
                }), qos=1))

            def wait_for(predicate, seconds=10):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    topic, message = app_message(conn)
                    if predicate(topic, message):
                        return topic, message
                self.fail('expected MQTT message did not arrive')

            try:
                with broker.accept()[0] as conn:
                    handshake(self, conn, 'lab', 'test')
                    topic, report = wait_for(lambda topic, message: message.get('event_type') == 'properties_report')
                    self.assertEqual(topic, 'lab/device/test/uplink/property')
                    self.assertEqual(report['protocolVersion'], '1.0')
                    self.assertIsInstance(report['ts'], int)
                    self.assertEqual(report['data']['propertyVersion'], '1')
                    self.assertEqual(report['data']['properties']['firmware']['version'], version)
                    self.assertEqual(report['data']['properties']['hardwareModel'], 'TEST_MODEL')
                    self.assertNotIn('requestId', report)
                    rpc('00000000-0000-4000-8000-000000000001', 'properties.get', {'keys': ['firmware']})
                    topic, reply = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000001')
                    self.assertEqual(topic, 'lab/device/test/uplink/resp')
                    self.assertEqual(reply['code'], 0)
                    self.assertEqual(reply['data']['properties']['firmware']['version'], version)
                    rpc('00000000-0000-4000-8000-000000000002', 'ota.install', {})
                    _, unknown = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000002')
                    self.assertEqual(unknown['code'], 4002)
                    self.assertIsNone(unknown['data'])
                    package = {'fileName': 'v1.bin', 'fileSize': 128, 'sha256': 'ab' * 32, 'downloadUrl': 'https://downloads.example.com/v1.bin', 'downloadExpiresAt': '2026-11-02T19:00:00+08:00'}
                    rpc('00000000-0000-4000-8000-000000000003', 'ota.start', {
                        'taskId': '2001', 'upgradeId': '00000000-0000-4000-8000-000000000010', 'commandSeq': 1,
                        'hardwareModel': 'OTHER', 'fromVersion': None, 'toVersion': 'v1', 'package': package,
                    })
                    _, mismatch = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000003')
                    self.assertEqual(mismatch['code'], 4221)
                    rpc('00000000-0000-4000-8000-000000000004', 'ota.start', {
                        'taskId': '2001', 'upgradeId': '00000000-0000-4000-8000-000000000010', 'commandSeq': 1,
                        'hardwareModel': 'TEST_MODEL', 'fromVersion': None, 'toVersion': 'v1', 'package': package,
                    })
                    _, started = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000004')
                    self.assertEqual(started['code'], 0)
                    self.assertTrue(started['data']['accepted'])
                    status_topic, status = wait_for(lambda topic, message: message.get('method') == 'ota.reportStatus')
                    self.assertEqual(status_topic, 'lab/device/test/uplink/req')
                    self.assertEqual(status['event_type'], 'rpc_request')
                    self.assertGreater(status['expiresAt'], status['ts'])
                    self.assertEqual(status['data']['upgradeStatus'], 'pending')
                    self.assertEqual(status['data']['stage'], 'queued')
                    self.assertEqual(status['data']['statusVersion'], '1')
                    conn.sendall(mqtt_publish('lab/device/test/downlink/resp', json.dumps({
                        'protocolVersion': '1.0', 'device_id': 'test', 'device_type': 'head_cam_v2',
                        'event_type': 'rpc_response', 'ts': int(time.time() * 1000), 'requestId': status['requestId'],
                        'method': 'ota.reportStatus', 'code': 0, 'message': '操作成功', 'data': {'accepted': True},
                    }), qos=1))
                    rpc('00000000-0000-4000-8000-000000000004', 'ota.start', {
                        'taskId': '2001', 'upgradeId': '00000000-0000-4000-8000-000000000010', 'commandSeq': 1,
                        'hardwareModel': 'TEST_MODEL', 'fromVersion': None, 'toVersion': 'v1', 'package': package,
                    })
                    _, replay = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000004' and message.get('event_type') == 'rpc_response')
                    self.assertEqual(replay['code'], 0)
                    rpc('00000000-0000-4000-8000-000000000005', 'ota.cancel', {
                        'taskId': '2001', 'upgradeId': '00000000-0000-4000-8000-000000000010', 'commandSeq': 2,
                    })
                    _, cancelled = wait_for(lambda topic, message: message.get('requestId') == '00000000-0000-4000-8000-000000000005')
                    self.assertEqual(cancelled['code'], 0)
                    _, final = wait_for(lambda topic, message: message.get('method') == 'ota.reportStatus' and message['data'].get('upgradeStatus') == 'cancelled')
                    self.assertEqual(final['data']['stage'], 'completed')
                    self.assertEqual(final['data']['statusVersion'], '2')
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
