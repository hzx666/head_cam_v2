"""在 Linux 上验证组件独立恢复和统一退出。"""
import pathlib
import subprocess
import tempfile
import time
import unittest


class SupervisorTest(unittest.TestCase):
    def test_independent_restart(self):
        source = pathlib.Path(__file__).resolve().parents[1] / 'src/service_supervisor.c'
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            binary = root / 'supervisor'
            subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            for name, behavior in [('upload', 'sleep 60'), ('monitor', 'exit 1')]:
                script = root / name
                script.write_text(f'#!/bin/sh\necho started >> "{root / (name + ".starts")}"\n{behavior}\n')
                script.chmod(0o700)
            process = subprocess.Popen([str(binary), str(root / 'lock'), str(root / 'upload'), str(root / 'monitor')])
            try:
                deadline = time.monotonic() + 18
                while time.monotonic() < deadline:
                    monitor = root / 'monitor.starts'
                    if monitor.exists() and len(monitor.read_text().splitlines()) >= 2:
                        break
                    time.sleep(0.2)
                self.assertEqual(len((root / 'upload.starts').read_text().splitlines()), 1)
                self.assertGreaterEqual(len(monitor.read_text().splitlines()), 2)
                self.assertIsNone(process.poll())
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            self.assertEqual(process.returncode, 0)


if __name__ == '__main__':
    unittest.main()
