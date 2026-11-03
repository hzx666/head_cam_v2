"""用本地假 OSS 验证更新、校验失败和运行中互斥，不访问网络。"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import fcntl

ROOT = Path(__file__).resolve().parents[1]
FILES = ['head-cam-upload', 'head-cam-monitor', 'head-cam-service', 'service_entry.sh', 'run-upload', 'run-monitor', 'boot_autostart.sh', 'monitor.example.json', 'SERVICE_VERSION']
FAKE = '''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args=sys.argv[1:]
if args==['--self-check']: sys.exit(0)
remote=pathlib.Path(os.environ['FAKE_RELEASE'])
if args[0]=='--update-list':
    print(args[1]+remote.name+'/')
elif args[0]=='--update-get':
    shutil.copyfile(remote/args[1].rsplit('/',1)[-1],args[3])
else: sys.exit(1)
'''


class UpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.home = self.root / 'home'
        self.old = self.home / 'releases/20261102.001'
        self.remote = self.root / '20261102.002'
        for release in (self.old, self.remote):
            release.mkdir(parents=True)
            for name in FILES:
                path = release / name
                if name == 'head-cam-upload':
                    path.write_text(FAKE)
                elif name.startswith('head-cam-'):
                    path.write_text('#!/bin/sh\nexit 0\n')
                elif name == 'SERVICE_VERSION':
                    path.write_text(release.name+'\n')
                elif name == 'monitor.example.json':
                    path.write_text('{}')
                else:
                    shutil.copyfile(ROOT / 'scripts' / name, path)
                path.chmod(0o700)
            self.manifest(release)
        (self.home/'current').symlink_to('releases/'+self.old.name)
        self.env = dict(os.environ, HEAD_CAM_HOME=str(self.home), FAKE_RELEASE=str(self.remote))

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, release):
        (release/'SHA256SUMS').write_text(''.join(hashlib.sha256((release/name).read_bytes()).hexdigest()+'  '+name+'\n' for name in FILES))

    def update(self):
        return subprocess.run(['bash', str(ROOT/'scripts/boot_autostart.sh'), 'update'], env=self.env, capture_output=True, text=True)

    def test_success_preserves_configuration(self):
        (self.home/'config').mkdir()
        (self.home/'config/monitor.json').write_text('{"device_id":"keep"}')
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve().name, self.remote.name)
        self.assertEqual((self.home/'previous').resolve(), self.old)
        self.assertEqual(json.loads((self.home/'config/monitor.json').read_text())['device_id'], 'keep')

    def test_corrupt_release_keeps_old(self):
        (self.remote/'head-cam-monitor').write_text('corrupt')
        self.assertNotEqual(self.update().returncode, 0)
        self.assertEqual((self.home/'current').resolve(), self.old)

    def test_failed_selfcheck_keeps_old(self):
        (self.remote/'head-cam-monitor').write_text('#!/bin/sh\nexit 1\n')
        self.manifest(self.remote)
        self.assertNotEqual(self.update().returncode, 0)
        self.assertEqual((self.home/'current').resolve(), self.old)

    def test_running_service_defers_update(self):
        (self.home/'runtime').mkdir()
        with (self.home/'runtime/service.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertEqual(self.update().returncode, 0)
            self.assertEqual((self.home/'current').resolve(), self.old)

    def test_resumes_after_release_move_before_switch(self):
        shutil.copytree(self.remote, self.home/'releases'/self.remote.name)
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve().name, self.remote.name)

    def test_keeps_two_backups(self):
        for version in ('20261030.001', '20261031.001'):
            (self.home/'releases'/version).mkdir()
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(sorted(p.name for p in (self.home/'releases').iterdir()), ['20261031.001', '20261102.001', '20261102.002'])


if __name__ == '__main__':
    unittest.main()
