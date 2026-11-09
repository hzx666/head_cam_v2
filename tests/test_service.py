#!/usr/bin/env python3
"""Black-box HTTP, persistence and upload tests, always against a fake OSS CLI."""
import ctypes
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import urlencode
import zlib

EXE = str(Path(sys.argv[1]).resolve())
sys.argv[1:] = []
FAKE = Path(__file__).with_name('fake_ossutil.py').resolve()
FAKE.chmod(0o755)
MAGIC = b'\x89MCAP0\r\n'


def rec(op, body):
    return bytes([op]) + struct.pack('<Q', len(body)) + body


def string(value):
    return struct.pack('<I', len(value)) + value


def mcap(compression='', delta=2_500_000_000, crc=True):
    header = rec(1, string(b'') + string(b'test'))
    channel = rec(4, struct.pack('<HH', 1, 0) + string(b'camera') + string(b'raw') + string(b''))
    messages = b''.join(rec(5, struct.pack('<HIQQ', 1, i, 1_000_000_000 + i * delta, 0) + b'data') for i in range(2))
    content = channel + messages
    if compression:
        if compression == 'zstd':
            lib = ctypes.CDLL('libzstd.so.1')
            lib.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
            lib.ZSTD_compressBound.restype = ctypes.c_size_t
            lib.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            lib.ZSTD_compress.restype = ctypes.c_size_t
            dest = ctypes.create_string_buffer(lib.ZSTD_compressBound(len(content)))
            size = lib.ZSTD_compress(dest, len(dest), content, len(content), 1)
        else:
            lib = ctypes.CDLL('liblz4.so.1')
            lib.LZ4F_compressFrameBound.argtypes = [ctypes.c_size_t, ctypes.c_void_p]
            lib.LZ4F_compressFrameBound.restype = ctypes.c_size_t
            lib.LZ4F_compressFrame.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
            lib.LZ4F_compressFrame.restype = ctypes.c_size_t
            dest = ctypes.create_string_buffer(lib.LZ4F_compressFrameBound(len(content), None))
            size = lib.LZ4F_compressFrame(dest, len(dest), content, len(content), None)
        packed = dest.raw[:size]
        content = rec(6, struct.pack('<QQQI', 1_000_000_000, 1_000_000_000+delta, len(content), zlib.crc32(content) if crc else 0) + string(compression.encode()) + struct.pack('<Q', len(packed)) + packed)
    before_end = MAGIC + header + content
    return before_end + rec(15, struct.pack('<I', zlib.crc32(before_end) if crc else 0)) + rec(2, struct.pack('<QQI', 0, 0, 0)) + MAGIC


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='head-cam-test-')
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.runtime = self.root / 'runtime'
        self.oss = self.root / 'oss'
        self.oss.mkdir()
        self.config = self.root / 'config.json'
        self.config.write_text(json.dumps(dict(endpoint='oss.invalid', bucket='test-bucket', access_key_id='test-id', access_key_secret='test-secret')))
        self.config.chmod(0o600)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.proc = None
        self.log = (self.root / 'service.log').open('w+')
        self.start()

    def start(self, token=None):
        env = dict(os.environ, FAKE_OSS_ROOT=str(self.oss))
        env.pop('HEAD_CAM_UPLOAD_TOKEN', None)
        if token:
            env['HEAD_CAM_UPLOAD_TOKEN'] = token
        self.token = token
        self.command = [EXE, '--host', '127.0.0.1', '--port', str(self.port), '--runtime-dir', str(self.runtime),
                        '--allowed-root', str(self.data), '--test-config', str(self.config), '--test-ossutil', str(FAKE), '--test-timeout', '2']
        self.proc = subprocess.Popen(self.command, env=env, stdout=self.log, stderr=self.log)
        for _ in range(100):
            if self.proc.poll() is not None:
                self.log.seek(0)
                self.fail('Service exited: ' + self.log.read())
            try:
                if self.request('GET', '/health')[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(.03)
        self.fail('Startup timed out')

    def stop(self, kill=False):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
            self.proc.wait(timeout=15)

    def tearDown(self):
        self.stop()
        self.log.flush()
        self.log.seek(0)
        logged = self.log.read()
        self.log.close()
        self.temp.cleanup()
        self.assertNotIn('ERROR: AddressSanitizer', logged)
        self.assertNotIn('ERROR: LeakSanitizer', logged)
        self.assertNotIn('runtime error:', logged)

    def request(self, method, path, body=None, headers=None):
        hdr = {'Content-Type': 'application/json'}
        if self.token:
            hdr['Authorization'] = 'Bearer ' + self.token
        if headers:
            hdr.update(headers)
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=8)
        connection.request(method, path, json.dumps(body) if body is not None else None, hdr)
        response = connection.getresponse()
        status, data = response.status, json.loads(response.read())
        connection.close()
        return status, data

    def submit(self, directory=None):
        code, task = self.request('POST', '/upload', {'path': str(directory or self.data)})
        self.assertIn(code, (200, 202), task)
        return task

    def wait(self, task):
        for _ in range(250):
            code, result = self.request('GET', task['status_url'])
            self.assertEqual(code, 200)
            if result['status'] not in ('queued', 'scanning', 'uploading'):
                return result
            time.sleep(.03)
        self.fail('Task timed out')

    def file(self, name='SSIEGO_Cam_ABC123_20261027_001.mcap', content=None, folder=None):
        path = (folder or self.data) / name
        path.write_bytes(mcap() if content is None else content)
        return path

    def records(self):
        return [json.loads(p.read_text()) for p in self.oss.glob('*.json') if p.name != 'control.json']

    def rules(self, **values):
        (self.oss / 'control.json').write_text(json.dumps(values))

    def test_file_list_upload_history(self):
        file = self.file('SSI_IRIS2_EGO_All_0003_20261105_104254_906_01.mcap')

        def listing(state):
            code, data = self.request('GET', '/files?' + urlencode(dict(path=str(self.data), upload_state=state)))
            self.assertEqual(code, 200)
            return data

        before = listing('NOT_UPLOADED')
        self.assertEqual(before['total_bytes'], file.stat().st_size)
        original = before['files'][0]
        self.assertEqual(original['recordedAt'], 1793846574906)
        self.assertEqual(original['uploadState'], 'NOT_UPLOADED')
        self.assertEqual(listing('UPLOADED')['total'], 0)
        result = self.wait(self.submit())
        self.assertEqual(result['uploaded'], 1, result)
        first = listing('UPLOADED')['files'][0]
        self.assertEqual(first['fileId'], original['fileId'])
        self.assertEqual(first['progressPercent'], 100)
        self.assertEqual(first['sha256'], first['remoteSha256'])
        self.assertTrue(first['canUpload'])
        self.assertEqual(self.wait(self.submit())['skipped'], 1)
        self.stop()
        # A remount can change inode/device identity while preserving file content.
        replacement = file.with_suffix('.replacement')
        replacement.write_bytes(file.read_bytes())
        replacement.replace(file)
        self.start()
        self.assertEqual(listing('UPLOADED')['files'][0]['uploadedAt'], first['uploadedAt'])
        recovered = listing('UPLOADED')['files'][0]
        self.assertNotEqual(recovered['fileId'], original['fileId'])
        self.stop()
        self.start()
        self.assertEqual(listing('UPLOADED')['files'][0]['fileId'], recovered['fileId'])
        file.write_bytes(mcap(delta=3_000_000_000))
        self.assertEqual(listing('UPLOADED')['total'], 0)
        self.assertNotEqual(listing('NOT_UPLOADED')['files'][0]['fileId'], original['fileId'])

    def test_deleted_upload_history_48_hours(self):
        def listing(state='UPLOADED', offset=0):
            code, data = self.request('GET', '/files?' + urlencode(dict(path=str(self.data), upload_state=state, offset=offset)))
            self.assertEqual(code, 200, data)
            return data

        file = self.file()
        size = file.stat().st_size
        _, task = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        self.assertEqual(self.wait(task)['status'], 'completed')
        self.assertFalse(file.exists())
        data = listing()
        self.assertEqual(data['total'], 1)
        self.assertEqual(data['total_bytes'], size)
        self.assertEqual(data['local_bytes'], 0)
        item = data['files'][0]
        self.assertEqual(item['localState'], 'DELETED')
        self.assertGreater(item['deletedAt'], 0)
        self.assertEqual(item['durationMs'], 2500)
        self.assertFalse(item['canUpload'])
        self.assertFalse(item['canDelete'])
        self.assertEqual(item['blockedReason'], 'LOCAL_FILE_DELETED')
        self.assertEqual(listing('NOT_UPLOADED')['total'], 0)
        self.assertEqual(listing(offset=200)['files'], [])
        self.stop()
        self.start()
        self.assertEqual(listing()['files'][0]['deletedAt'], item['deletedAt'])
        # Recover an interruption after unlink but before the final task save.
        self.stop()
        task_path = self.runtime / 'tasks' / (task['task_id'] + '.json')
        saved = json.loads(task_path.read_text())
        saved['results'][0]['deletionPendingAt'] = saved['results'][0].pop('deletedAt')
        task_path.write_text(json.dumps(saved))
        self.start()
        self.assertEqual(listing()['files'][0]['localState'], 'DELETED')

        # A same-name replacement appears once and cannot inherit deletion state.
        self.file(content=mcap(delta=3_000_000_000))
        self.assertEqual(listing()['total'], 0)
        self.assertEqual(listing('NOT_UPLOADED')['files'][0]['localState'], 'PRESENT')
        file.unlink()
        self.stop()
        task_path = self.runtime / 'tasks' / (task['task_id'] + '.json')
        saved = json.loads(task_path.read_text())
        saved['results'][0]['deletedAt'] = time.time() * 1000 - 48 * 3600 * 1000 - 1
        saved['results'][0].pop('deletionPendingAt', None)
        task_path.write_text(json.dumps(saved))
        self.start()
        self.assertEqual(listing()['total'], 0)

        # Manual deletion has the same retained history as automatic deletion.
        file = self.file('SSIEGO_Cam_OTHER_20261105_001.mcap')
        self.wait(self.submit())
        present = listing()['files'][0]
        body = dict(path=str(self.data), name=file.name, fileId=present['fileId'])
        self.assertEqual(self.request('POST', '/files/delete', body)[0], 200)
        self.assertEqual(listing()['files'][0]['localState'], 'DELETED')
        self.stop()
        self.start()
        self.assertEqual(listing()['files'][0]['localState'], 'DELETED')

    def test_deleted_history_global_limit(self):
        file = self.file()
        _, task = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        self.assertEqual(self.wait(task)['status'], 'completed')
        self.stop()
        task_path = self.runtime / 'tasks' / (task['task_id'] + '.json')
        saved = json.loads(task_path.read_text())
        template = saved['results'][0]
        records = []
        now = time.time() * 1000
        for i in range(1501):
            record = dict(template, file=f'capture_{i:04d}.mcap', path=str(self.data / f'capture_{i:04d}.mcap'),
                          deletedAt=now - 2000 + i, fileId='f_' + f'{i:064x}')
            records.append(record)
        saved['results'] = records
        task_path.write_text(json.dumps(saved))
        self.start()
        _, data = self.request('GET', '/files?' + urlencode(dict(path=str(self.data), upload_state='UPLOADED')))
        self.assertEqual(data['total'], 1500)
        self.assertEqual(data['files'][0]['name'], 'capture_0001.mcap')
        persisted = json.loads(task_path.read_text())
        self.assertEqual(len(persisted['results']), 1500)
        self.stop()
        self.start()
        self.assertEqual(len(self.request('GET', task['status_url'])[1]['results']), 1500)

    def test_manual_file_deletion(self):
        file = self.file('not_a_valid_capture.mcap')

        def listing():
            code, data = self.request('GET', '/files?' + urlencode(dict(path=str(self.data))))
            self.assertEqual(code, 200)
            return data['files'][0]

        item = listing()
        self.assertTrue(item['canDelete'])
        body = dict(path=str(self.data), name=file.name, fileId=item['fileId'])
        self.assertEqual(self.request('POST', '/files/delete', dict(body, name='../escape.mcap'))[0], 400)
        file.write_bytes(b'replaced')
        self.assertEqual(self.request('POST', '/files/delete', body)[0], 409)
        self.assertTrue(file.exists())
        body['fileId'] = listing()['fileId']
        self.assertEqual(self.request('POST', '/files/delete', body)[0], 200)
        self.assertFalse(file.exists())
        self.assertEqual(self.request('POST', '/files/delete', body)[0], 404)

        file = self.file()
        self.rules(delay=.2)
        task = self.submit()
        item = listing()
        body = dict(path=str(self.data), name=file.name, fileId=item['fileId'])
        self.assertFalse(item['canDelete'])
        code, response = self.request('POST', '/files/delete', body)
        self.assertEqual(code, 409)
        self.assertEqual(response['errorCode'], 'ACTIVE_UPLOAD_TASK')
        self.assertTrue(file.exists())
        self.wait(task)
        self.assertTrue(listing()['canDelete'])
        self.assertEqual(self.request('POST', '/files/delete', body)[0], 200)
        self.assertFalse(file.exists())
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(self.request('GET', task['status_url'])[1]['uploaded'], 1)

    def test_delete_after_upload(self):
        file = self.file()
        for value in ('true', 1, None, [], {}):
            code, _ = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=value))
            self.assertEqual(code, 400)
        retained = self.wait(self.submit())
        self.assertFalse(retained['delete_after_upload'])
        self.assertFalse(retained['results'][0]['local_deleted'])
        self.assertTrue(file.exists())
        for existing in (True, False):
            if not existing:
                file = self.file('SSIEGO_Cam_NEW_20261027_001.mcap')
            code, task = self.request('POST', '/upload', dict(path=str(self.data),
                files=[file.name], delete_after_upload=True))
            self.assertEqual(code, 202)
            self.assertTrue(task['delete_after_upload'])
            result = self.wait(task)
            self.assertEqual(result['status'], 'completed', result)
            self.assertEqual(result['results'][0]['status'], 'skipped' if existing else 'uploaded')
            self.assertTrue(result['results'][0]['local_deleted'])
            self.assertFalse(file.exists())
        self.stop()
        self.start()
        self.assertTrue(self.request('GET', task['status_url'])[1]['results'][0]['local_deleted'])

    def test_delete_retains_unverified_files(self):
        file = self.file()
        self.wait(self.submit())
        record_path = next(self.oss.glob('*.json'))
        record = json.loads(record_path.read_text())
        record['metadata'].pop('x-oss-meta-sha256')
        record_path.write_text(json.dumps(record))
        empty = self.file('SSIEGO_Cam_EMPTY_20261027_001.mcap', b'')
        bad = self.file('SSIEGO_Cam_BAD_20261027_001.mcap', b'invalid')
        _, task = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        result = self.wait(task)
        self.assertEqual(result['status'], 'partial_failed')
        self.assertTrue(all(p.exists() for p in (file, empty, bad)))
        self.assertTrue(all(not r['local_deleted'] for r in result['results']))

    def test_delete_metadata_failure_and_retry(self):
        file = self.file()
        self.rules(fail=['set-meta'])
        _, task = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        result = self.wait(task)
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(file.exists())
        self.assertFalse(result['results'][0]['local_deleted'])
        self.rules()
        _, task = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        self.assertTrue(self.wait(task)['results'][0]['local_deleted'])
        self.assertFalse(file.exists())

    def test_delete_option_conflicts_with_active_task(self):
        self.file()
        self.rules(delay=.2)
        task = self.submit()
        code, _ = self.request('POST', '/upload', dict(path=str(self.data), delete_after_upload=True))
        self.assertEqual(code, 409)
        self.assertEqual(self.submit()['task_id'], task['task_id'])
        self.wait(task)

    def test_browse_select_upload_metadata_and_skip(self):
        child = self.data / '采集 one'
        child.mkdir()
        file = self.file(folder=child)
        (self.data / 'link').symlink_to(child, target_is_directory=True)
        code, roots = self.request('GET', '/directories')
        self.assertEqual(code, 200)
        self.assertEqual(roots['directories'][0]['path'], str(self.data))
        code, listing = self.request('GET', '/directories?' + urlencode({'path': str(self.data)}))
        self.assertEqual([x['name'] for x in listing['directories']], ['采集 one'])
        self.assertIsNone(listing['parent'])
        task = self.submit(child)
        result = self.wait(task)
        self.assertEqual(result['status'], 'completed', result)
        self.assertEqual(result['uploaded'], 1)
        item = result['results'][0]
        self.assertEqual(item['checksum_status'], 'match')
        self.assertTrue(file.exists())
        record = self.records()[0]
        meta = record['metadata']
        self.assertEqual(meta['x-oss-meta-dataDomain'], 'ego_human_data')
        self.assertEqual(meta['x-oss-meta-dataKind'], 'head_cam_v2')
        self.assertEqual(meta['x-oss-meta-deviceNo'], 'ABC123')
        self.assertEqual(meta['x-oss-meta-sessionName'], '261027')
        self.assertEqual(meta['x-oss-meta-duration'], '2.500')
        self.assertEqual(meta['x-oss-meta-lastModified'], '2026-10-27T00:00:00.000Z')
        self.assertEqual(meta['x-oss-meta-sha256'], record['actual_sha256'])
        self.assertEqual(len(meta), 18)
        self.assertFalse(any('relatedfiles' in key or 'quality' in key for key in meta))
        repeated = self.wait(self.submit(child))
        self.assertEqual(repeated['skipped'], 1)
        self.assertTrue(repeated['results'][0]['same_content'])
        self.stop()
        self.assertFalse(list(self.runtime.glob('session-*')))
        self.start()
        self.assertEqual(self.request('GET', task['status_url'])[1]['uploaded'], 1)

    def test_iris_compressed_and_partial_errors(self):
        self.file('SSI_IRIS2_EGO_A_DEV1_20261027_001.mcap', mcap('zstd'))
        self.file('SSIEGO_Cam_DEV2_20261027_001.MCAP', mcap('lz4'))
        self.file('SSIEGO_Cam_DEV3_20261027_001.mcap', b'')
        self.file('SSIEGO_Cam_DEV4_20261027_001.mcap.incomplete', b'incomplete')
        self.file('SSIEGO_Cam_DEV5_20260230_001.mcap')
        self.file('SSIEGO_Cam_DEV6_20261027_001.mcap', mcap()[:-10])
        self.file('SSIEGO_Cam_DEV2_20261027_001.MCAP.incomplete', b'incomplete')
        result = self.wait(self.submit())
        self.assertEqual(result['status'], 'partial_failed', result)
        self.assertEqual((result['uploaded'], result['empty'], result['failed'], result['ignored']), (2, 1, 3, 1))

    def test_conflict_and_unknown_do_not_overwrite(self):
        file = self.file()
        self.wait(self.submit())
        file.write_bytes(mcap(delta=3_000_000_000))
        result = self.wait(self.submit())
        self.assertEqual(result['results'][0]['checksum_status'], 'mismatch')
        record = next(p for p in self.oss.glob('*.json'))
        data = json.loads(record.read_text())
        data['metadata'].pop('x-oss-meta-sha256')
        record.write_text(json.dumps(data))
        before = record.read_bytes()
        result = self.wait(self.submit())
        self.assertEqual(result['skipped'], 1)
        self.assertIsNone(result['results'][0]['same_content'])
        self.assertEqual(record.read_bytes(), before)

    def test_duplicate_key_detected_before_upload(self):
        a, b = self.data / 'a', self.data / 'b'
        a.mkdir(); b.mkdir()
        self.file(folder=a); self.file(folder=b)
        result = self.wait(self.submit())
        self.assertEqual(result['failed'], 2)
        self.assertEqual(self.records(), [])

    def test_duplicate_overlap_and_restart_interrupted(self):
        child = self.data / 'child'; child.mkdir(); self.file(folder=child)
        self.rules(delay=.3)
        task = self.submit()
        repeated = self.submit()
        self.assertTrue(repeated['duplicate'])
        self.assertEqual(repeated['task_id'], task['task_id'])
        self.assertEqual(self.request('POST', '/upload', {'path': str(child)})[0], 409)
        self.stop(kill=True)
        time.sleep(.5)
        self.rules()
        self.start()
        self.assertEqual(self.request('GET', task['status_url'])[1]['status'], 'interrupted')
        self.assertEqual(self.wait(self.submit())['status'], 'completed')

    def test_failed_stat_and_metadata(self):
        self.file()
        self.rules(fail=['stat'])
        self.assertEqual(self.wait(self.submit())['failed'], 1)
        self.assertEqual(self.records(), [])
        self.rules(fail=['set-meta'])
        result = self.wait(self.submit())
        self.assertEqual(result['failed'], 1)
        self.assertIn('metadata', result['results'][0]['error'])
        self.rules()
        result = self.wait(self.submit())
        self.assertEqual(result['skipped'], 1)

    def test_mutation_timeout_and_auth(self):
        self.file()
        self.rules(mutate_source=True)
        result = self.wait(self.submit())
        self.assertEqual(result['failed'], 1)
        self.assertEqual(result['results'][0]['checksum_status'], 'unknown')
        self.stop(); self.start(token='test-token')
        self.assertEqual(self.request('GET', '/health', headers={'Authorization': 'Bearer wrong'})[0], 401)
        self.assertEqual(self.request('POST', '/upload', {'path': '/tmp'})[0], 403)
        self.assertEqual(self.request('GET', '/directories?' + urlencode({'path': '/tmp'}))[0], 403)
        self.assertEqual(self.request('POST', '/upload', {'path': str(self.data), 'other': 1})[0], 400)
        self.assertEqual(self.request('POST', '/upload', {'path': str(self.data) + '\x00junk'})[0], 400)
        self.file(content=mcap())
        self.rules(delay=3)
        self.assertEqual(self.wait(self.submit())['failed'], 1)

    def test_runtime_lock_empty_directory_and_pagination(self):
        other = subprocess.run(self.command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertNotEqual(other.returncode, 0)
        self.assertIn(b'locked', other.stderr)
        self.assertEqual(self.wait(self.submit())['status'], 'no_data')
        for i in range(205):
            (self.data / f'{i:03}').mkdir()
        _, page = self.request('GET', '/directories?' + urlencode({'path': str(self.data)}))
        self.assertEqual(len(page['directories']), 200)
        self.assertEqual(page['next_offset'], 200)
        _, page = self.request('GET', '/directories?' + urlencode({'path': str(self.data), 'offset': 200}))
        self.assertEqual(len(page['directories']), 5)
        self.assertIsNone(page['next_offset'])

    def test_selected_files_listing_and_upload(self):
        a = self.file()
        b = self.file('SSIEGO_Cam_ABC123_20261027_002.mcap')
        self.file('SSIEGO_Cam_ABC123_20261027_003.mcap')
        (self.data / 'link.mcap').symlink_to(a)
        self.file('unfinished.mcap.incomplete', b'incomplete')
        code, listing = self.request('GET', '/files?' + urlencode({'path': str(self.data)}))
        self.assertEqual(code, 200)
        self.assertEqual(len(listing['files']), 3)
        self.assertEqual(listing['files'][0]['size'], a.stat().st_size)
        code, task = self.request('POST', '/upload', {'path': str(self.data), 'files': [b.name, a.name]})
        self.assertEqual(code, 202)
        result = self.wait(task)
        self.assertEqual((result['uploaded'], result['total'], result['mode']), (2, 2, 'files'))
        self.assertEqual({x['file'] for x in result['results']}, {a.name, b.name})
        code, task = self.request('POST', '/upload', {'path': str(self.data), 'files': [a.name]})
        self.assertEqual(code, 202)
        self.assertEqual(self.wait(task)['skipped'], 1)
        for files in [[], [a.name, a.name], ['../' + a.name], ['link.mcap'], ['missing.mcap'], ['unfinished.mcap.incomplete'], [1], None]:
            self.assertEqual(self.request('POST', '/upload', {'path': str(self.data), 'files': files})[0], 400, files)

    def test_selection_retry_and_directory_overlap(self):
        a = self.file()
        b = self.file('SSIEGO_Cam_ABC123_20261027_002.mcap')
        self.rules(delay=.3)
        body = {'path': str(self.data), 'files': [a.name, b.name]}
        _, task = self.request('POST', '/upload', body)
        _, duplicate = self.request('POST', '/upload', dict(body, files=[b.name, a.name]))
        self.assertEqual(task['task_id'], duplicate['task_id'])
        self.assertTrue(duplicate['duplicate'])
        self.assertEqual(self.request('POST', '/upload', {'path': str(self.data)})[0], 409)
        self.assertEqual(self.request('POST', '/upload', dict(body, files=[a.name]))[0], 409)
        self.assertEqual(self.wait(task)['uploaded'], 2)
        self.stop(); self.start()
        self.assertEqual(self.request('GET', task['status_url'])[1]['files'], sorted([a.name, b.name]))

    def test_live_byte_progress_and_finalization(self):
        a = self.file()
        b = self.file('SSIEGO_Cam_ABC123_20261027_002.mcap')
        self.rules(progress=True, progress_noise=True)
        task = self.submit()
        seen = []
        for _ in range(200):
            _, result = self.request('GET', task['status_url'])
            seen.append(result)
            if result['status'] not in ('queued', 'scanning', 'uploading'):
                break
            time.sleep(.03)
        self.assertEqual(result['status'], 'completed', result)
        self.assertTrue(any(0 < r.get('current_file', {}).get('bytes_uploaded', 0) < a.stat().st_size for r in seen))
        self.assertTrue(any(0 < r.get('progress_percent', 0) < 99 for r in seen))
        self.assertTrue(all(r.get('progress_percent', 0) < 100 for r in seen if r['status'] in ('queued', 'scanning', 'uploading')))
        self.assertTrue(any(r.get('speed_bytes_per_second') == 10240 for r in seen))
        self.assertTrue(any(r.get('current_file', {}).get('speed_bytes_per_second') == 10240 for r in seen))
        self.assertEqual(result['speed_bytes_per_second'], 0)
        self.assertEqual(result['current_file']['speed_bytes_per_second'], 0)
        log = (self.runtime / 'logs' / 'service.log').read_text()
        self.assertIn('speed=10.00 KiB/s speed_bytes_per_second=10240.00', log)
        self.log.flush()
        self.assertIn('speed=10.00 KiB/s', (self.root / 'service.log').read_text())
        self.assertEqual(result['bytes_total'], a.stat().st_size + b.stat().st_size)
        self.assertEqual(result['bytes_uploaded'], result['bytes_total'])
        self.assertEqual(result['bytes_done'], result['bytes_total'])
        self.assertEqual(result['progress_percent'], 100)
        skipped = self.wait(self.submit())
        self.assertEqual(skipped['speed_bytes_per_second'], 0)
        self.assertEqual(skipped['bytes_uploaded'], 0)
        self.assertEqual(skipped['bytes_done'], skipped['bytes_total'])
        self.assertEqual(skipped['progress_percent'], 100)

    def test_failed_byte_transfer_does_not_report_complete(self):
        self.file()
        self.rules(progress=True, fail_after_progress=True)
        result = self.wait(self.submit())
        self.assertEqual(result['status'], 'failed')
        self.assertLess(result['progress_percent'], 100)
        self.assertGreater(result['bytes_uploaded'], 0)
        self.assertEqual(result['phase'], 'failed')
        self.assertEqual(result['speed_bytes_per_second'], 0)
        self.assertEqual(result['current_file']['speed_bytes_per_second'], 0)

    def test_crc_errors_and_malformed_http(self):
        content = bytearray(mcap('zstd'))
        content[-41] ^= 1  # data CRC, leaving record framing intact
        self.file(content=content)
        result = self.wait(self.submit())
        self.assertEqual(result['failed'], 1)
        self.assertEqual(self.records(), [])
        for request in [
            b'POST /upload HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}',
            b'POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n',
            b'POST /upload HTTP/1.1\r\nContent-Length: -1\r\n\r\n',
        ]:
            with socket.create_connection(('127.0.0.1', self.port)) as sock:
                sock.sendall(request)
                self.assertTrue(sock.recv(1024).startswith(b'HTTP/1.1 400'))
        self.assertEqual(self.request('GET', '/health')[0], 200)


if __name__ == '__main__':
    unittest.main(verbosity=2)
