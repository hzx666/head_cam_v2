#!/usr/bin/env python3
"""Exercise actual packaged binary offline: only an empty directory is submitted."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

exe = str(Path(sys.argv[1]).resolve())
runner = ['qemu-aarch64'] if '--qemu' in sys.argv else []
with tempfile.TemporaryDirectory(prefix='head-cam-release-') as temp:
    root = Path(temp)
    data = root / 'data'
    data.mkdir()
    runtime = root / 'runtime'
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    env = dict(os.environ)
    env.pop('HEAD_CAM_UPLOAD_TOKEN', None)
    # Avoid relying on any external CLI from PATH. Embedded ossutil uses an absolute path.
    env['PATH'] = '/nonexistent'
    if runner:
        runner = ['/usr/bin/qemu-aarch64']
    with (root / 'log').open('w+') as log:
        proc = subprocess.Popen(runner + [exe, '--host', '127.0.0.1', '--port', str(port), '--runtime-dir', str(runtime), '--allowed-root', str(data)], env=env, stdout=log, stderr=log)
        client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        base = f'http://127.0.0.1:{port}'
        def get(path):
            with client.open(base + path, timeout=5) as response:
                return json.load(response)
        try:
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError('Packaged service exited early')
                try:
                    assert get('/health')['status'] == 'ok'
                    break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError('Packaged service startup timeout')
            assert get('/directories')['directories'][0]['path'] == str(data)
            request = urllib.request.Request(base + '/upload', method='POST', data=json.dumps({'path': str(data)}).encode(), headers={'Content-Type': 'application/json'})
            with client.open(request, timeout=5) as response:
                assert response.status == 202
                task = json.load(response)
            for _ in range(100):
                result = get(task['status_url'])
                if result['status'] == 'no_data':
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('Empty directory task did not finish')
            assert json.loads((runtime / 'tasks' / (task['task_id'] + '.json')).read_text())['status'] == 'no_data'
        except BaseException:
            log.flush(); log.seek(0); print(log.read())
            raise
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=15)
        assert not list(runtime.glob('session-*'))
print('PASS: packaged binary, embedded helper, HTTP browsing/submission/query/persistence; no OSS requests.')
