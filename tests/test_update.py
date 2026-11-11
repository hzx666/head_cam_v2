"""Single-object updates against a local mock; no OSS traffic."""
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE = '''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args=sys.argv[1:]
remote=pathlib.Path(os.environ['FAKE_RELEASE'])
if args==['--self-check']: sys.exit(0)
if args==['--version']: print('VERSION')
elif args[0]=='--update-list': print(args[1]+remote.name+'/head-cam-service')
elif args[0]=='--update-sha256': print(os.environ['FAKE_SHA'])
elif args[0]=='--update-get': shutil.copyfile(remote/'head-cam-service',args[3])
else: sys.exit(1)
'''


class UpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.home = self.root/'home'
        self.old = self.home/'releases/20261102.001'
        self.remote = self.root/'20261102.002'
        for release in (self.old, self.remote):
            release.mkdir(parents=True)
            self.write_binary(release)
        (self.home/'current').symlink_to('releases/'+self.old.name)
        self.env = dict(os.environ, HEAD_CAM_HOME=str(self.home), FAKE_RELEASE=str(self.remote),
                        OSS_UPDATE_ENABLED='true', HEAD_CAM_BUSYBOX='/usr/bin/busybox',
                        HEAD_CAM_LOCK_HELPER=str(ROOT/'build-service-native/head-cam-service'))
        self.checksum()

    def tearDown(self):
        self.tmp.cleanup()

    def write_binary(self, release):
        path = release/'head-cam-service'
        path.write_text(FAKE.replace('VERSION', release.name))
        path.chmod(0o700)

    def checksum(self):
        self.env['FAKE_SHA'] = hashlib.sha256((self.remote/'head-cam-service').read_bytes()).hexdigest()

    def update(self):
        return subprocess.run(['/bin/sh',str(ROOT/'scripts/boot_autostart.sh'),'update'],
                              env=self.env,capture_output=True,text=True,timeout=20)

    def assert_updated(self):
        result = self.update()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve().name,self.remote.name)
        self.assertEqual(list(p.name for p in (self.home/'current').iterdir()),['head-cam-service'])

    def test_single_file_update(self):
        self.assert_updated()
        self.assertEqual((self.home/'previous').resolve(),self.old)

    def test_automatic_update_defaults_off(self):
        env=dict(self.env)
        env.pop('OSS_UPDATE_ENABLED',None)
        result=subprocess.run(['/bin/sh',str(ROOT/'scripts/boot_autostart.sh'),'update-internal'],
                              env=env,capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_manual_update_when_automatic_disabled(self):
        self.env['OSS_UPDATE_ENABLED']='false'
        self.assert_updated()

    def test_corruption_keeps_old(self):
        (self.remote/'head-cam-service').write_text('corrupt')
        self.assertNotEqual(self.update().returncode,0)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_missing_checksum_keeps_old(self):
        self.env['FAKE_SHA']=''
        self.assertNotEqual(self.update().returncode,0)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_failed_selfcheck_keeps_old(self):
        path=self.remote/'head-cam-service'
        path.write_text(path.read_text().replace("if args==['--self-check']: sys.exit(0)","if args==['--self-check']: sys.exit(1)"))
        self.checksum()
        self.assertNotEqual(self.update().returncode,0)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_wrong_version_keeps_old(self):
        path=self.remote/'head-cam-service'
        path.write_text(path.read_text().replace(self.remote.name,'20200101.001'))
        self.checksum()
        self.assertNotEqual(self.update().returncode,0)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_running_service_defers_update(self):
        (self.home/'runtime').mkdir()
        with (self.home/'runtime/service.lock').open('w') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            self.assertNotEqual(self.update().returncode,0)
            self.assertEqual((self.home/'current').resolve(),self.old)

    def test_resume_before_switch(self):
        shutil.copytree(self.remote,self.home/'releases'/self.remote.name)
        self.assert_updated()

    def test_corrupt_cached_release_keeps_old(self):
        cached=self.home/'releases'/self.remote.name
        shutil.copytree(self.remote,cached)
        (cached/'head-cam-service').write_text('corrupt')
        self.assertNotEqual(self.update().returncode,0)
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_keeps_one_backup(self):
        for version in ('20261030.001','20261031.001'):
            (self.home/'releases'/version).mkdir()
        self.assert_updated()
        self.assertEqual(sorted(p.name for p in (self.home/'releases').iterdir()),['20261102.001','20261102.002'])

    def test_failed_switch_keeps_backup(self):
        backup=self.home/'releases/20261031.001'
        backup.mkdir()
        (self.home/'current.new').mkdir()
        self.assertNotEqual(self.update().returncode,0)
        self.assertTrue(backup.is_dir())
        self.assertEqual((self.home/'current').resolve(),self.old)

    def test_two_updates_keep_stable_entry(self):
        (self.home/'current').unlink()
        shutil.copy2(self.old/'head-cam-service',self.home/'head-cam-service')
        self.assert_updated()
        self.assertEqual((self.home/'previous').resolve(),self.home)
        self.remote=self.root/'20261102.003'
        self.remote.mkdir()
        self.write_binary(self.remote)
        self.env['FAKE_RELEASE']=str(self.remote)
        self.checksum()
        self.assert_updated()
        self.assertEqual((self.home/'previous').resolve().name,'20261102.002')
        self.assertTrue((self.home/'head-cam-service').is_file())


class ReleaseMetadataTest(unittest.TestCase):
    def test_sha256_stat_parser(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            config=root/'credentials.json'
            config.write_text(json.dumps(dict(endpoint='oss.invalid',bucket='test-bucket',
                                              access_key_id='test',access_key_secret='test')))
            oss=root/'oss'
            oss.mkdir()
            uri='oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/20261110.001/head-cam-service'
            record=oss/(hashlib.sha256(uri.encode()).hexdigest()+'.json')
            fake=ROOT/'tests/fake_ossutil.py'
            fake.chmod(0o700)
            command=[str(ROOT/'build-service-native/head-cam-service'),'--update-sha256',uri,
                     '--test-config',str(config),'--test-ossutil',str(fake)]
            env=dict(os.environ,FAKE_OSS_ROOT=str(oss))
            for value,ok in [('AB'*32,True),('invalid',False),('',False)]:
                record.write_text(json.dumps(dict(etag='test',metadata={'X-Oss-Meta-Sha256':value})))
                result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode==0,ok,result.stderr)
                if ok:self.assertEqual(result.stdout.strip(),'ab'*32)
                else:self.assertEqual(result.stdout,'')


if __name__=='__main__':
    unittest.main()
