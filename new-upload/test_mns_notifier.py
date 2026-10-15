import copy
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from mns_notifier import EVENTS, MNSSender, NotificationOutbox, eligible
from upload_server import TaskManager


def result(file='sample.mcap', **overrides):
    value = dict(file=file, ok=True, status='uploaded', checksum_status='match',
                 ossPath='oss://bucket/' + file, sha256='a' * 64)
    value.update(overrides)
    return value


class NotificationTests(unittest.TestCase):
    def test_file_and_success_gates(self):
        for suffix in ('.mcap', '.hdf5', '.h5', '.HDF5', '.MCAP'):
            self.assertTrue(eligible(result('sample' + suffix)))
        for value in (result('sample.parquet'), result('sample.mcap.incomplete'),
                      result(status='empty'), result(ok=False),
                      result(status='skipped', checksum_status='unknown'),
                      result(checksum_status='mismatch')):
            self.assertFalse(eligible(value))
        self.assertTrue(eligible(result(status='skipped')))

    def test_independent_retry_restart_and_dedup(self):
        sent = []
        def sender(body):
            sent.append(copy.deepcopy(body))
            if body['event'] == 'mcap_qc':
                raise TimeoutError('must not persist secret request headers')
            return 'preview-id'
        with tempfile.TemporaryDirectory() as directory:
            box = NotificationOutbox(directory, sender, lambda _: None)
            key = box.enqueue(result())
            box.deliver_due()
            self.assertEqual(box.snapshot(key)['mcap_extract_preview']['status'], 'sent')
            self.assertEqual(box.snapshot(key)['mcap_qc']['status'], 'pending')
            self.assertEqual(box.snapshot(key)['mcap_qc']['error'], 'TimeoutError')
            box.deliver_due()
            self.assertEqual(len(sent), 2)  # Backoff is respected.
            self.assertEqual(sent[0], dict(event=EVENTS[0], code=200, msg='', traceId='',
                                          data={'ossPath': 'oss://bucket/sample.mcap'}))
            recovered = []
            box = NotificationOutbox(directory, lambda body: recovered.append(body) or 'qc-id', lambda _: None)
            self.assertEqual(box.enqueue(result(status='skipped')), key)
            with patch('mns_notifier.time.time', return_value=time.time() + 600):
                box.deliver_due()
            self.assertEqual([b['event'] for b in recovered], ['mcap_qc'])
            box.enqueue(result())
            box.deliver_due()
            self.assertEqual(len(recovered), 1)
            self.assertTrue(all(e['status'] == 'sent' for e in box.snapshot(key).values()))

    def test_enqueue_failure_never_sends_unpersisted_messages(self):
        with tempfile.TemporaryDirectory() as directory:
            calls = []
            box = NotificationOutbox(directory, calls.append, lambda _: None)
            with patch('mns_notifier.os.replace', side_effect=OSError('disk full')):
                with self.assertRaises(OSError):
                    box.enqueue(result())
            box.deliver_due()
            self.assertEqual(calls, [])

    def test_sdk_uses_plain_json_and_public_queue(self):
        from mns.mns_http import ResponseInternal
        from xml.etree import ElementTree as ET
        captured = []
        def request(http, req):
            self.assertTrue(http.is_https)
            self.assertEqual(http.host, '1766661213327930.mns.cn-shanghai.aliyuncs.com')
            self.assertEqual(http.conn.timeout, 15)
            captured.append(req)
            return ResponseInternal(201, {'x-mns-request-id': 'test'},
                b'<Message xmlns="http://mns.aliyuncs.com/doc/v1/">'
                b'<MessageId>test-id</MessageId><MessageBodyMD5>abc</MessageBodyMD5></Message>')
        core = SimpleNamespace(_T_AK_ENC='fake-ak', _T_SK_ENC='fake-sk',
                               decrypt_credential=lambda value: value)
        body = dict(event='mcap_qc', code=200, msg='', traceId='',
                    data={'ossPath': 'oss://bucket/中文.h5'})
        with patch('mns.mns_http.MNSHttp.send_request', request):
            self.assertEqual(MNSSender(core)(body), 'test-id')
        req = captured[0]
        self.assertEqual(req.uri, '/queues/data-platfrom-event/messages')
        node = ET.fromstring(req.data).find('{http://mns.aliyuncs.com/doc/v1/}MessageBody')
        self.assertEqual(json.loads(node.text), body)

    def test_task_integration_and_dynamic_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / 'data'
            data.mkdir()
            for name in ('one.mcap', 'two.hdf5', 'three.h5', 'four.parquet', 'bad.mcap'):
                (data / name).write_bytes(b'test')
            credentials = root / 'fake-config'
            credentials.touch()
            core = SimpleNamespace(_T_AK_ENC='', _T_SK_ENC='', decrypt_credential=lambda _: 'fake',
                setup_ossutil_config=lambda *args: str(credentials),
                process_file=lambda path, *args: result(path.name, ok=path.name != 'bad.mcap',
                    status='failed' if path.name == 'bad.mcap' else 'uploaded'))
            messages = []
            with patch('upload_server.classify', side_effect=lambda core, path: ('teleop', path.name)):
                manager = TaskManager(core, root / 'runtime', notification_sender=lambda b: messages.append(b) or 'id')
                try:
                    task, _ = manager.submit(str(data))
                    manager.pending.join()
                    deadline = time.monotonic() + 5
                    while manager.get(task['task_id'])['mns_status'] != 'sent' and time.monotonic() < deadline:
                        time.sleep(0.01)
                    task = manager.get(task['task_id'])
                    self.assertEqual(task['status'], 'partial_failed')
                    self.assertEqual(task['mns_status'], 'sent')
                    self.assertEqual(len(messages), 6)
                    self.assertEqual({b['data']['ossPath'] for b in messages},
                        {'oss://bucket/one.mcap', 'oss://bucket/two.hdf5', 'oss://bucket/three.h5'})
                finally:
                    manager.close()

    def test_recovers_gap_between_task_save_and_outbox_save(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Path(directory) / 'tasks'
            store.mkdir()
            task = dict(task_id='test', status='completed', results=[result(mns_required=True)])
            (store / 'test.json').write_text(json.dumps(task), encoding='utf-8')
            messages = []
            manager = TaskManager(None, directory, notification_sender=lambda b: messages.append(b) or 'id')
            try:
                deadline = time.monotonic() + 5
                while manager.get('test')['mns_status'] != 'sent' and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(manager.get('test')['mns_status'], 'sent')
                self.assertEqual(len(messages), 2)
            finally:
                manager.close()


if __name__ == '__main__':
    unittest.main()
