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
FILES = ['head-cam-service', 'service_entry.sh', 'run-upload', 'run-monitor', 'boot_autostart.sh', 'manual_dhcp.sh', 'monitor.json', 'SERVICE_VERSION']
FAKE = '''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args=sys.argv[1:]
if args==['--self-check']: sys.exit(0)
remote=pathlib.Path(os.environ['FAKE_RELEASE'])
if args[0]=='--update-list':
    print(args[1]+remote.name+'/SHA256SUMS')
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
                if name == 'busybox':
                    shutil.copyfile('/usr/bin/busybox', path)
                elif name == 'head-cam-service':
                    path.write_text(FAKE)
                elif name.startswith('head-cam-'):
                    path.write_text('#!/bin/sh\nexit 0\n')
                elif name == 'SERVICE_VERSION':
                    path.write_text(release.name+'\n')
                elif name == 'monitor.json':
                    path.write_text('{}')
                else:
                    shutil.copyfile(ROOT / 'scripts' / name, path)
                path.chmod(0o700)
            self.manifest(release)
        (self.home/'current').symlink_to('releases/'+self.old.name)
        self.env = dict(os.environ, HEAD_CAM_HOME=str(self.home), FAKE_RELEASE=str(self.remote), OSS_UPDATE_ENABLED='true', HEAD_CAM_AUTO_DHCP='false', HEAD_CAM_BUSYBOX='/usr/bin/busybox', HEAD_CAM_LOCK_HELPER=str(ROOT/'build-service-native/head-cam-service'))

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, release):
        (release/'SHA256SUMS').write_text(''.join(hashlib.sha256((release/name).read_bytes()).hexdigest()+'  '+name+'\n' for name in FILES))

    def update(self):
        return subprocess.run(['/bin/sh', str(ROOT/'scripts/boot_autostart.sh'), 'update'], env=self.env, capture_output=True, text=True)

    def test_success_preserves_configuration(self):
        (self.home/'config').mkdir()
        (self.home/'config/monitor.json').write_text('{"device_id":"keep"}')
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve().name, self.remote.name)
        self.assertEqual((self.home/'previous').resolve(), self.old)
        self.assertEqual(json.loads((self.home/'config/monitor.json').read_text())['device_id'], 'keep')

    def test_corrupt_release_keeps_old(self):
        (self.remote/'head-cam-service').write_text('corrupt')
        self.assertNotEqual(self.update().returncode, 0)
        self.assertEqual((self.home/'current').resolve(), self.old)

    def test_failed_selfcheck_keeps_old(self):
        (self.remote/'head-cam-service').write_text('#!/bin/sh\nexit 1\n')
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

    def test_keeps_one_backup(self):
        for version in ('20261030.001', '20261031.001'):
            (self.home/'releases'/version).mkdir()
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(sorted(p.name for p in (self.home/'releases').iterdir()), ['20261102.001', '20261102.002'])

    def test_two_updates_from_flat_install(self):
        (self.home/'current').unlink()
        for name in FILES + ['SHA256SUMS']:
            shutil.copy2(self.old/name, self.home/name)
        original_config = (self.home/'monitor.json').read_bytes()
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual((self.home/'previous').resolve(), self.home)
        self.assertTrue((self.home/'head-cam-service').exists())
        next_release = self.root/'20261102.003'
        shutil.copytree(self.remote, next_release)
        (next_release/'SERVICE_VERSION').write_text(next_release.name+'\n')
        self.manifest(next_release)
        self.env['FAKE_RELEASE'] = str(next_release)
        result = self.update()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve().name, next_release.name)
        self.assertEqual((self.home/'previous').resolve().name, self.remote.name)
        self.assertFalse((self.home/'head-cam-service').exists())
        self.assertEqual((self.home/'monitor.json').read_bytes(), original_config)

    @unittest.skipUnless(os.geteuid() == 0, 'installer needs root')
    def test_init_install_start_stop(self):
        import time
        shutil.copyfile(ROOT/'build-service-native/head-cam-service', self.old/'head-cam-service')
        (self.old/'head-cam-service').chmod(0o700)
        self.manifest(self.old)
        install_home = self.root/'installed'
        init_dir = self.root/'init.d'
        env = dict(self.env, HEAD_CAM_HOME=str(install_home), HEAD_CAM_DIY_PATH=str(self.root/'diy.sh'), BOOT_DELAY_SEC='0', OSS_UPDATE_ENABLED='false')
        shutil.copytree(self.old, install_home)
        (self.root/'diy.sh').write_text('#!/bin/sh\necho existing-device-setup\n')
        script = install_home/'boot_autostart.sh'
        result = subprocess.run(['/bin/sh', str(script), 'install'], env=env, capture_output=True, text=True, timeout=15)
        try:
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('# BEGIN HEAD_CAM_SERVICE', (self.root/'diy.sh').read_text())
            self.assertIn('echo existing-device-setup', (self.root/'diy.sh').read_text())
            self.assertFalse((install_home/'releases/20261102.001').exists())
            repeated = subprocess.run(['/bin/sh', str(script), 'install'], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(repeated.returncode, 0, repeated.stdout+repeated.stderr)
            self.assertEqual((self.root/'diy.sh').read_text().count('# BEGIN HEAD_CAM_SERVICE'), 1)
            self.assertTrue((install_home/'monitor.json').exists())
            deadline = time.monotonic()+5
            pidfile = install_home/'runtime/service.pid'
            while not pidfile.exists() and time.monotonic()<deadline:
                time.sleep(0.1)
            self.assertTrue(pidfile.exists())
            time.sleep(0.5)
            pid = int(pidfile.read_text())
            self.assertTrue(Path(f'/proc/{pid}/exe').exists())
            self.assertNotIn('not found', (install_home/'logs/boot.log').read_text())
            for _ in range(3):
                previous = int(pidfile.read_text())
                restarted = subprocess.run(['/bin/sh', str(script), 'restart'], env=env, capture_output=True, text=True, timeout=25)
                self.assertEqual(restarted.returncode, 0, restarted.stdout+restarted.stderr)
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    if pidfile.exists() and int(pidfile.read_text())!=previous:
                        break
                    time.sleep(0.1)
                self.assertTrue(pidfile.exists())
                self.assertNotEqual(int(pidfile.read_text()), previous)
                time.sleep(0.2)
        finally:
            subprocess.run(['/bin/sh', str(install_home/'boot_autostart.sh'), 'stop'], env=env, capture_output=True, timeout=25)

if __name__ == '__main__':
    unittest.main()
