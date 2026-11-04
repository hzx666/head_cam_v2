"""假 DHCP 回调验证默认网关配置；不修改宿主网络。"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import signal
import shutil
import time

ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''#!/usr/bin/python3
import json, os, subprocess, sys
a=sys.argv[1:]
if a[:2]==['id','-u']:
    print('0');sys.exit(0)
if a[:3]==['ip','-4','addr']:
    print('    inet 192.168.20.114/24');sys.exit(0)
if a[:3]==['ip','route','replace']:
    with open(os.environ['TEST_LOG'],'a') as f:f.write(json.dumps(a)+'\n')
    sys.exit(0)
if a[:2]==['ip','route']:
    print('default via '+os.environ.get('TEST_ROUTER','')+' dev wlan0');sys.exit(0)
if a[0]=='udhcpc':
    if os.environ.get('TEST_FAIL'):sys.exit(1)
    env=dict(os.environ, interface=a[a.index('-i')+1], router=os.environ.get('TEST_ROUTER',''))
    sys.exit(subprocess.call([a[a.index('-s')+1],'bound'],env=env))
os.execv('/usr/bin/busybox',['busybox']+a)
'''


class DHCPTest(unittest.TestCase):
    def test_automatic_recovery_after_route_disappears(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'runtime').mkdir()
            shutil.copyfile(ROOT/'scripts/manual_dhcp.sh',root/'manual_dhcp.sh')
            tool=root/'tool'
            mock=MOCK.replace("if a[:2]==['ip','route']:", "if a[:3]==['ip','route','show']:\n    if os.path.exists(os.environ['TEST_LOG']): print('default via 192.168.20.1 dev wlan0')\n    sys.exit(0)\nif a==['sleep','15']:\n    import time;time.sleep(0.1);sys.exit(0)\nif a[:2]==['ip','route']:")
            tool.write_text(mock)
            tool.chmod(0o700)
            log=root/'routes'
            body=(ROOT/'scripts/boot_autostart.sh').read_text().split('watch_gateway() (',1)[1].split('\nrun_service()',1)[0]
            script=root/'watch.sh'
            script.write_text('log() { :; }\nwatch_gateway() ('+body+'\nwatch_gateway "$$"\n')
            env=dict(os.environ,BB=str(tool),HEAD_CAM_HOME=str(root),HEAD_CAM_NETWORK_IFACE='wlan0',TEST_ROUTER='192.168.20.1',TEST_LOG=str(log))
            process=subprocess.Popen(['/usr/bin/busybox','sh',str(script)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
            try:
                for attempt in range(2):
                    deadline=time.monotonic()+4
                    while not log.exists() and time.monotonic()<deadline:time.sleep(0.05)
                    self.assertTrue(log.exists(),'gateway was not recovered')
                    if attempt==0:log.unlink()
                # 默认路由存在时不重复运行 DHCP。
                before=log.read_text()
                time.sleep(0.3)
                self.assertEqual(log.read_text(),before)
            finally:
                os.killpg(process.pid,signal.SIGTERM)
                process.wait(timeout=5)

    def run_case(self, router='', fail=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tool = root/'tool'
            tool.write_text(MOCK)
            tool.chmod(0o700)
            log = root/'routes'
            env = dict(os.environ, HEAD_CAM_TOOL=str(tool), TEST_ROUTER=router, TEST_LOG=str(log))
            if fail:
                env['TEST_FAIL']='1'
            result = subprocess.run(['/usr/bin/busybox','sh',str(ROOT/'scripts/manual_dhcp.sh')],env=env,capture_output=True,text=True,timeout=5)
            routes = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return result, routes

    def test_router_from_dhcp(self):
        result, routes = self.run_case('192.168.20.1 192.168.20.2')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(routes,[['ip','route','replace','default','via','192.168.20.1','dev','wlan0']])

    def test_no_router(self):
        result, routes = self.run_case()
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(routes,[])

    def test_invalid_router(self):
        result, routes = self.run_case('not-an-ip')
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(routes,[])

    def test_dhcp_failure(self):
        result, routes = self.run_case(fail=True)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(routes,[])


if __name__=='__main__':
    unittest.main()
