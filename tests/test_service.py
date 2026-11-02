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
        self.assertEqual(meta['x-oss-meta-dataKind'], 'head_cam')
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
