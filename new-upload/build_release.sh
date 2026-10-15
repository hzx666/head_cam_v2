#!/usr/bin/env bash
# Linux x86_64 standalone service. Build tools and intermediate files are temporary.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
    echo 'Build requires Linux x86_64, including WSL Ubuntu.' >&2; exit 1;
}
PYTHON="${PYTHON:-python3}"
command -v gcc >/dev/null || { echo 'Install gcc/build-essential on the build machine.' >&2; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/new-upload-build.XXXXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
export PIP_CACHE_DIR="$WORK/pip-cache"
export NUITKA_CACHE_DIR="$WORK/nuitka-cache"
export PYTHONDONTWRITEBYTECODE=1
# virtualenv also works when distribution Python has no ensurepip/venv package.
"$PYTHON" -m pip install --disable-pip-version-check --target "$WORK/bootstrap" 'virtualenv==20.36.1'
PYTHONPATH="$WORK/bootstrap" "$PYTHON" -m virtualenv --no-periodic-update --app-data "$WORK/virtualenv-cache" "$WORK/venv"
PY="$WORK/venv/bin/python"
export PATH="$WORK/venv/bin:$PATH"
"$PY" -m pip install -r "$ROOT/requirements.txt" 'Nuitka==4.2.2' 'patchelf==0.19.1.0'
mkdir -p "$WORK/src/ossutil" "$WORK/out" "$ROOT/dist"
cp "$ROOT/"{upload_server,upload_core,mcap_parser,hdf5_parser,mns_notifier}.py "$WORK/src/"

"$PY" - "$WORK/src" <<'PY'
import ast
import os
from pathlib import Path
import shutil
import sys
import urllib.request
import zipfile
root = Path(sys.argv[1])
tree = ast.parse((root / 'upload_core.py').read_text(encoding='utf-8'))
version = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'OSSUTIL_VERSION' for t in n.targets))
dest = root / 'ossutil' / 'ossutil64'
if os.environ.get('OSSUTIL_BIN'):
    shutil.copyfile(os.environ['OSSUTIL_BIN'], dest)
else:
    archive = root / 'ossutil.zip'
    urllib.request.urlretrieve(
        f'https://gosspublic.alicdn.com/ossutil/{version}/ossutil-v{version}-linux-amd64.zip', archive)
    with zipfile.ZipFile(archive) as z:
        matches = [n for n in z.namelist() if Path(n).name == 'ossutil64']
        if len(matches) != 1:
            raise RuntimeError('Unexpected ossutil archive')
        with z.open(matches[0]) as src, dest.open('wb') as out:
            shutil.copyfileobj(src, out)
dest.chmod(0o755)
with dest.open('rb') as f:
    header = f.read(20)
if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
    raise RuntimeError('ossutil must be a Linux x86_64 ELF executable')
PY

cat > "$WORK/ossutil.yml" <<'YAML'
- module-name: 'upload_core'
  dlls:
    - from_filenames:
        relative_path: 'ossutil'
        prefixes: ['ossutil64']
        executable: 'yes'
      dest_path: 'ossutil'
      when: 'linux'
YAML

cd "$WORK/src"
"$PY" -m nuitka --mode=onefile --assume-yes-for-downloads \
    --output-dir="$WORK/out" --output-filename=upload-service \
    --jobs="${BUILD_JOBS:-2}" --lto=no \
    --user-package-configuration-file="$WORK/ossutil.yml" \
    --include-module=mcap.writer \
    --include-package-data=aliyunsdkcore --include-package-data=aliyunsdkkms \
    --noinclude-pytest-mode=nofollow --noinclude-setuptools-mode=nofollow \
    upload_server.py

# Run without Python/pip/ossutil in PATH, to exercise the actual deployment artifact.
env PATH=/nonexistent "$WORK/out/upload-service" --self-check
"$PY" - "$WORK/out/upload-service" <<'PY'
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
with tempfile.TemporaryDirectory(prefix='upload-smoke-') as d:
    root = Path(d)
    data = root / 'data'
    data.mkdir()
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    env = dict(os.environ)
    env.pop('NEW_UPLOAD_TOKEN', None)
    env.pop('PYTHONPATH', None)
    env['PATH'] = '/nonexistent'
    with (root / 'server.log').open('w+') as log:
        proc = subprocess.Popen([exe, '--port', str(port), '--allowed-root', str(data)],
                                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            base = f'http://127.0.0.1:{port}'
            client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            for _ in range(120):
                if proc.poll() is not None:
                    raise RuntimeError('Server exited early')
                try:
                    with client.open(base + '/health', timeout=1) as r:
                        assert json.load(r)['status'] == 'ok'
                    break
                except OSError:
                    time.sleep(0.25)
            else:
                raise RuntimeError('Server startup timed out')
            req = urllib.request.Request(base + '/upload', method='POST',
                data=json.dumps({'path': str(data)}).encode(),
                headers={'Content-Type': 'application/json'})
            with client.open(req, timeout=5) as r:
                assert r.status == 202
                task = json.load(r)
            for _ in range(40):
                with client.open(base + task['status_url'], timeout=5) as r:
                    result = json.load(r)
                if result['status'] == 'no_data':
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError('Task did not complete')
            record = Path(exe).parent / 'runtime' / 'tasks' / (task['task_id'] + '.json')
            assert json.loads(record.read_text())['status'] == 'no_data'
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
print('PASS: HTTP startup, submission, query, persistent runtime; no OSS upload.')
PY

install -m 755 "$WORK/out/upload-service" "$ROOT/dist/upload-service"
"$PY" -m pip freeze > "$ROOT/dist/build-dependencies.txt"
{
    echo 'Target: Linux x86_64; Nuitka onefile'
    echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    ldd --version | head -n 1
    "$PY" --version
} > "$ROOT/dist/build-info.txt"
cd "$ROOT/dist"
sha256sum upload-service > SHA256SUMS
echo "Built and verified: $ROOT/dist/upload-service"
