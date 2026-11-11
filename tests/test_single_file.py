"""Exercise actual ARM64 single-file deployment using an isolated boot hook."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.geteuid()==0,'install requires root')
class SingleFileTest(unittest.TestCase):
    def test_install_restart_boot_and_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            home=Path(tmp)/'device'
            home.mkdir()
            binary=home/'head-cam-service'
            shutil.copy2(ROOT/'dist/head-cam-service',binary)
            binary.chmod(0o700)
            hook=Path(tmp)/'diy.sh'
            hook.write_text('#!/bin/sh\necho existing-device-setup\n')
            # Avoid contacting production MQTT; this override is for offline QA.
            config=Path(tmp)/'disabled.json'
            config.write_text('{"enabled":false}')
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0))
                port=sock.getsockname()[1]
            env=dict(os.environ,HEAD_CAM_DIY_PATH=str(hook),BOOT_DELAY_SEC='0',
                     HEAD_CAM_AUTO_DHCP='false',
                     HEAD_CAM_MONITOR_CONFIG=str(config),UPLOAD_HOST='127.0.0.1',UPLOAD_PORT=str(port))
            env.pop('HEAD_CAM_HOME',None)
            env.pop('OSS_UPDATE_ENABLED',None)

            def command(mode):
                return subprocess.run([str(binary),mode],env=env,capture_output=True,text=True,timeout=40)

            def ready():
                deadline=time.monotonic()+25
                while time.monotonic()<deadline:
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{port}/health',timeout=1) as response:
                            if response.status==200:return
                    except OSError:
                        pass
                    time.sleep(.2)
                self.fail((home/'logs/boot.log').read_text())

            try:
                version=command('--version')
                self.assertEqual(version.returncode,0,version.stderr)
                self.assertEqual(version.stdout.strip(),(ROOT/'SERVICE_VERSION').read_text().strip())
                self.assertIn('Compiler:',command('--build-info').stdout)
                installed=command('install')
                self.assertEqual(installed.returncode,0,installed.stdout+installed.stderr)
                ready()
                for name in ('monitor.json','SERVICE_VERSION','SHA256SUMS','boot_autostart.sh','manual_dhcp.sh'):
                    self.assertFalse((home/name).exists(),name)
                self.assertIn('echo existing-device-setup',hook.read_text())
                self.assertIn('"$HEAD_CAM_HOME/head-cam-service" start',hook.read_text())
                again=command('install')
                self.assertEqual(again.returncode,0,again.stdout+again.stderr)
                self.assertEqual(hook.read_text().count('# BEGIN HEAD_CAM_SERVICE'),1)
                status=command('status')
                self.assertIn('Running version: '+version.stdout.strip(),status.stdout)
                pid=(home/'runtime/service.pid').read_text()
                restarted=command('restart')
                self.assertEqual(restarted.returncode,0,restarted.stdout+restarted.stderr)
                ready()
                self.assertNotEqual((home/'runtime/service.pid').read_text(),pid)
                stopped=command('stop')
                self.assertEqual(stopped.returncode,0,stopped.stdout+stopped.stderr)
                boot=subprocess.run(['/bin/sh',str(hook)],env=env,capture_output=True,text=True,timeout=15)
                self.assertEqual(boot.returncode,0,boot.stdout+boot.stderr)
                ready()
            finally:
                command('stop')


if __name__=='__main__':
    unittest.main()
