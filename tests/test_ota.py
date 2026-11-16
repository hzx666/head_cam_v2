"""Real OTA state machine with offline SDK and MQTT peers."""
import fcntl
import hashlib
import http.server
import json
import os
import pathlib
import socket
import ssl
import subprocess
import tempfile
import time
import threading
import unittest
from urllib.parse import quote
from test_monitor import ROOT, handshake, app_message, mqtt_publish


class OtaTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        (self.root / 'runtime').mkdir()
        self.broker = socket.socket()
        self.addCleanup(self.broker.close)
        self.broker.bind(('127.0.0.1', 0))
        self.broker.listen()
        self.broker.settimeout(10)
        self.device = dict(deviceValid=True, stateValid=True, deviceCode='EVT-1/0021',
                           version='1.105.11.12.T', hardware='TEST', state=0, battery=80, charge=0)
        self.state = self.root / 'sdk.json'
        self.save_device()
        helper = ROOT / 'tests/fake_ego_bridge.py'
        helper.chmod(0o755)
        self.env = dict(os.environ, HEAD_CAM_HOME=str(self.root), HEAD_CAM_EGO_HELPER=str(helper),
                        HEAD_CAM_EGO_LIBRARY='unused', TEST_EGO_STATE=str(self.state),
                        TEST_EGO_INSTALLED=str(self.root / 'installed.bin'))
        config = json.loads((ROOT / 'config/monitor.json').read_text())
        config.update(host='127.0.0.1', port=self.broker.getsockname()[1], device_id='test',
                      hardware_model='TEST', topic_prefix='lab', recording_root=str(self.root),
                      storage_path=str(self.root), scan_interval_sec=10, report_interval_sec=10)
        self.config = self.root / 'config.json'
        self.config.write_text(json.dumps(config))
        self.data = dict(taskId='2001', upgradeId='upgrade-1', commandSeq=1, hardwareModel='TEST',
                         fromVersion=None, toVersion='1.106.11.11.T', package=dict(firmwareId='1001',
                         fileName='firmware.bin', fileSize=128, sha256='ab'*32,
                         downloadUrl='https://localhost/firmware.bin', downloadExpiresAt='2099-01-01T00:00:00Z'))

    def save_device(self):
        self.state.write_text(json.dumps(self.device))

    def start(self):
        binary = os.environ.get('TEST_MONITOR', str(ROOT / 'build-ota-native/head-cam-monitor'))
        self.process = subprocess.Popen([binary, str(self.config), str(self.root / 'runtime/monitor')], env=self.env)
        self.addCleanup(self.stop)
        self.conn = self.broker.accept()[0]
        self.addCleanup(self.conn.close)
        serial = self.device['deviceCode']
        handshake(self, self.conn, 'head_cam_v2', quote(serial, safe=''), client_id=serial)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=35)

    def send(self, request='start-1', method='ota.start', data=None):
        now = int(time.time()*1000)
        serial = self.device['deviceCode']
        value = dict(protocolVersion='1.0', device_id=serial if method.startswith('ota.') else serial[-4:], device_type='head_cam_v2',
                     event_type='rpc_request', ts=now, expiresAt=now+120000,
                     requestId=request, method=method, data=self.data if data is None else data)
        topic = f"device/head_cam_v2/{quote(serial, safe='')}/downlink/req"
        self.conn.sendall(mqtt_publish(topic, json.dumps(value), qos=1))

    def wait(self, predicate):
        for _ in range(80):
            topic, value = app_message(self.conn)
            self.assertTrue(topic.startswith(f"device/head_cam_v2/{quote(self.device['deviceCode'], safe='')}/"), topic)
            if predicate(value):
                return value
        self.fail('expected message missing')

    def reply(self):
        return self.wait(lambda x: x.get('event_type') == 'rpc_response' and x.get('method') == 'ota.start')

    def test_recording_ends_task_and_ack_persists(self):
        self.device['state'] = 1
        self.save_device()
        self.start()
        self.send()
        self.assertTrue(self.reply()['data']['accepted'])
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus')
        self.assertEqual(report['device_id'], 'EVT-1/0021')
        self.assertEqual(report['data']['upgradeStatus'], 'failed')
        self.assertEqual(report['data']['error']['code'], 'DEVICE_BUSY')
        response = dict(report, event_type='rpc_response', code=0, data={})
        self.conn.sendall(mqtt_publish('device/head_cam_v2/EVT-1%2F0021/downlink/resp', json.dumps(response)))
        session = self.root / 'runtime/monitor/mqtt-session.json'
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            value = json.loads(session.read_text())
            if value.get('acknowledgedStatusVersion') == report['data']['statusVersion']:
                break
            time.sleep(.1)
        self.assertEqual(value.get('acknowledgedStatusVersion'), report['data']['statusVersion'])
        self.assertFalse((self.root/'installed.bin').exists())

    def test_not_charging_requires_twenty_percent(self):
        self.device.update(battery=19, charge=0)
        self.save_device()
        self.start()
        summary = self.wait(lambda x: x.get('event_type') == 'recording_summary')
        self.assertEqual(summary['device_sn'], 'EVT-1/0021')
        self.assertEqual(summary['battery']['soc'], 19)
        self.assertFalse(summary['battery']['charging'])
        self.send()
        self.assertEqual(self.reply()['code'], 0)
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus')
        self.assertEqual(report['data']['error']['code'], 'LOW_BATTERY')

    def test_upload_lock_rejects_install(self):
        with (self.root/'runtime/operation.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_SH)
            self.start()
            self.send()
            self.assertEqual(self.reply()['code'], 0)
            report = self.wait(lambda x: x.get('method') == 'ota.reportStatus')
            self.assertEqual(report['data']['error']['code'], 'DEVICE_BUSY')

    def test_equal_higher_invalid_versions_and_sdk_failure(self):
        self.start()
        for index, version in enumerate(['1.105.11.12.T', '1.104.12.31.T', '1.105.11.11.T',
                                         '1.106.11.12.R', '1.106.02.30.T', 'v2.4.0', '2.4.0']):
            self.data['toVersion'] = version
            self.send(str(index))
            self.assertEqual(self.reply()['code'], 4222)
        self.device['deviceValid'] = False
        self.save_device()
        self.data['toVersion'] = '1.105.11.13.T'
        self.send('retry')
        self.assertEqual(self.reply()['code'], 5001)
        self.device.update(deviceValid=True, state=1)
        self.save_device()
        self.send('retry')
        self.assertEqual(self.reply()['code'], 0)

    def https_server(self, corrupt=False, expired=False):
        content = b'firmware-content' * 1024
        self.data['package'].update(fileSize=len(content), sha256='00'*32 if corrupt else hashlib.sha256(content).hexdigest())
        cert, key = self.root/'cert.pem', self.root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-keyout', str(key), '-out', str(cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if expired and self.path == '/expired':
                    self.send_response(403)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                self.wfile.write(f'{len(content):x}\r\n'.encode()+content+b'\r\n0\r\n\r\n')
            def log_message(self, *args):
                pass
        server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.env['SSL_CERT_FILE'] = str(cert)
        self.data['package']['downloadUrl'] = f'https://localhost:{server.server_port}/' + ('expired' if expired else 'firmware')
        return content, f'https://localhost:{server.server_port}/firmware'

    def test_bad_checksum_never_invokes_sdk(self):
        self.https_server(corrupt=True)
        self.device.update(deviceCode='8L6AR120005', battery=0, charge=0)
        self.save_device()
        self.start()
        self.send()
        self.assertEqual(self.reply()['code'], 0)
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'completed')
        self.assertEqual(report['data']['error']['code'], 'CHECKSUM_MISMATCH')
        self.assertFalse((self.root/'installed.bin').exists())

    def test_restore_sdk_success_rechecks_device(self):
        self.device['version'] = self.data['toVersion']
        self.save_device()
        runtime = self.root/'runtime/monitor'
        runtime.mkdir()
        job = dict(self.data, statusVersion='7', upgradeStatus='upgrading', stage='rebooting',
                   progress=95, sdkSucceeded=True, installBootId='old-boot')
        (runtime/'mqtt-session.json').write_text(json.dumps(dict(task=job, propertyVersion='1')))
        self.start()
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'completed')
        self.assertEqual(report['data']['upgradeStatus'], 'success')
        self.assertFalse((self.root/'installed.bin').exists())

    def test_interrupted_install_is_not_reported_success(self):
        runtime = self.root/'runtime/monitor'
        runtime.mkdir()
        job = dict(self.data, statusVersion='6', upgradeStatus='upgrading', stage='installing', progress=70)
        (runtime/'mqtt-session.json').write_text(json.dumps(dict(task=job, propertyVersion='1')))
        self.start()
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus')
        self.assertEqual(report['data']['error']['code'], 'UPGRADE_INTERRUPTED')
        self.assertEqual(report['data']['upgradeStatus'], 'failed')

    def test_signed_url_renewal_install_and_version_confirmation(self):
        content, url = self.https_server(expired=True)
        self.start()
        self.send()
        self.assertEqual(self.reply()['code'], 0)
        renewal = self.wait(lambda x: x.get('method') == 'ota.getDownloadUrl')
        self.assertEqual(renewal['data']['firmwareId'], '1001')
        response = dict(renewal, event_type='rpc_response', code=0,
                        data=dict(firmwareId='1001', downloadUrl=url, downloadExpiresAt='2099-01-01T00:00:00Z'))
        self.conn.sendall(mqtt_publish('device/head_cam_v2/EVT-1%2F0021/downlink/resp', json.dumps(response)))
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'rebooting')
        self.assertLess(report['data']['progress'], 100)
        self.assertEqual((self.root/'installed.bin').read_bytes(), content)
        self.device['version'] = self.data['toVersion']
        self.save_device()
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'completed')
        self.assertEqual(report['data']['upgradeStatus'], 'success')
        self.assertEqual(report['data']['progress'], 100)
        self.assertEqual(report['data']['firmware']['version'], self.data['toVersion'])

    def test_explicit_downgrade_while_charging_at_zero_percent(self):
        content, _ = self.https_server()
        self.device.update(battery=0, charge=1)
        self.save_device()
        self.start()
        self.data['toVersion'] = '1.104.12.31.T'
        self.send('implicit-downgrade')
        self.assertEqual(self.reply()['code'], 4222)
        self.data['allowDowngrade'] = 'true'
        self.send('invalid-downgrade-flag')
        self.assertEqual(self.reply()['code'], 4001)
        self.data['allowDowngrade'] = True
        self.send('explicit-downgrade')
        self.assertTrue(self.reply()['data']['accepted'])
        self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'rebooting')
        self.assertEqual((self.root/'installed.bin').read_bytes(), content)
        self.device['version'] = self.data['toVersion']
        self.save_device()
        report = self.wait(lambda x: x.get('method') == 'ota.reportStatus' and x['data']['stage'] == 'completed')
        self.assertEqual(report['data']['upgradeStatus'], 'success')
        self.assertEqual(report['data']['firmware']['version'], self.data['toVersion'])


if __name__ == '__main__':
    unittest.main()
