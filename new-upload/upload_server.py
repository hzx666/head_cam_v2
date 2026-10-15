#!/usr/bin/env python3
"""Path-based HTTP upload service. Only the upload worker touches OSS."""
import argparse
import copy
import hmac
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
from mns_notifier import MNSSender, NotificationOutbox, eligible

if '__compiled__' in globals():
    HERE = Path(__compiled__.containing_dir).resolve()
elif getattr(sys, 'frozen', False):
    HERE = Path(sys.executable).resolve().parent
else:
    HERE = Path(__file__).resolve().parent
if (HERE / 'vendor').is_dir() and os.name == 'nt' and sys.version_info[:2] == (3, 12):
    sys.path.insert(0, str(HERE / 'vendor'))

ACTIVE = {'queued', 'scanning', 'uploading'}
SUFFIXES = {'.mcap', '.hdf5', '.h5', '.parquet'}


class RequestError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def timestamp():
    return time.strftime('%Y-%m-%dT%H:%M:%S%z')


def classify(core, path):
    name = path.name
    if name.lower().endswith('.mcap.incomplete'):
        raise ValueError('MCAP 尚未完成，不支持自动修复，请提供完整 .mcap 文件')
    kind = 'head_cam' if core.parse_name(name, 'head_cam') else 'teleop'
    meta = core.parse_name(name, kind)
    if not meta:
        raise ValueError('文件名无法解析品牌、设备和日期')
    # Validate actual calendar dates, not just an eight-digit string.
    time.strptime(meta['captureDate'], '%Y-%m-%d')
    brand = meta.get('brand') or 'Orin'
    if kind == 'teleop' and meta.get('machine', '').casefold() == 'm1':
        brand = 'Orin'
    destination = core.object_layout_for(kind, brand, meta['yymmdd'], meta['dev'],
                                         name, True, meta.get('machine', ''))['ossPath']
    return kind, destination


class TaskManager:
    def __init__(self, core, runtime, allowed_root=None, notification_sender=None):
        self.core = core
        self.runtime = Path(runtime).resolve()
        self.store = self.runtime / 'tasks'
        self.store.mkdir(parents=True, exist_ok=True)
        self.allowed_root = Path(allowed_root).resolve(strict=True) if allowed_root else None
        self.lock = threading.RLock()
        self.tasks = {}
        self.pending = queue.Queue()
        self.closing = False
        self.notifications = NotificationOutbox(self.runtime,
            notification_sender if notification_sender is not None else MNSSender(core))
        for file in self.store.glob('*.json'):
            task = json.loads(file.read_text(encoding='utf-8'))
            self.tasks[task['task_id']] = task
            if task['status'] in ACTIVE:
                task.update(status='interrupted', error='服务退出，任务未完成；请重新提交 path',
                            finished_at=timestamp())
                self._save(task)
        # Only records created by the notification-aware service are recovered.
        # Old task history must not unexpectedly trigger downstream processing.
        for task in self.tasks.values():
            for result in task['results']:
                if result.get('mns_required') and eligible(result):
                    self.notifications.enqueue(result)
        self.notifications.start()
        self.worker = threading.Thread(target=self._work, name='upload-worker', daemon=True)
        self.worker.start()

    def _save(self, task):
        dest = self.store / (task['task_id'] + '.json')
        temp = dest.with_suffix('.tmp')
        temp.write_text(json.dumps(task, ensure_ascii=False, indent=2), encoding='utf-8')
        os.replace(temp, dest)

    def update(self, task_id, **fields):
        with self.lock:
            self.tasks[task_id].update(fields, updated_at=timestamp())
            self._save(self.tasks[task_id])

    def get(self, task_id):
        with self.lock:
            if task_id not in self.tasks:
                raise RequestError(404, '任务不存在')
            task = copy.deepcopy(self.tasks[task_id])
        statuses = []
        for result in task['results']:
            if result.get('mns_required'):
                result['mns_events'] = self.notifications.snapshot(self.notifications.key(result))
                states = [entry['status'] for entry in result['mns_events'].values()]
                result['mns_status'] = 'sent' if all(s == 'sent' for s in states) else 'pending'
                statuses.append(result['mns_status'])
        task['mns_status'] = ('pending' if 'pending' in statuses else 'sent') if statuses else 'not_required'
        return task

    def submit(self, raw_path):
        if not isinstance(raw_path, str) or not raw_path.strip() or '\x00' in raw_path:
            raise RequestError(400, 'path 必须是非空的完整目录路径')
        path = Path(raw_path)
        if not path.is_absolute():
            raise RequestError(400, 'path 必须是绝对路径')
        try:
            path = path.resolve(strict=True)
            if not path.is_dir():
                raise RequestError(400, 'path 必须指向缓存目录')
            if self.allowed_root and not path.is_relative_to(self.allowed_root):
                raise RequestError(403, 'path 不在允许的缓存根目录内')
            if path.is_relative_to(self.runtime):
                raise RequestError(400, '不能上传服务运行目录')
            with os.scandir(path) as entries:
                next(entries, None)
        except FileNotFoundError:
            raise RequestError(400, '缓存目录不存在')
        except PermissionError:
            raise RequestError(403, '缓存目录不可读')
        except (OSError, ValueError) as exc:
            raise RequestError(400, f'无效路径: {exc}')
        with self.lock:
            if self.closing:
                raise RequestError(503, '服务正在关闭')
            active = [t for t in self.tasks.values() if t['status'] in ACTIVE]
            for task in active:
                previous = Path(task['path'])
                if previous == path:
                    return self.get(task['task_id']), True
                if path.is_relative_to(previous) or previous.is_relative_to(path):
                    raise RequestError(409, '该目录与正在处理的任务目录重叠')
            if len(active) >= 100:
                raise RequestError(503, '上传队列已满')
            task = dict(task_id=uuid.uuid4().hex, path=str(path), status='queued',
                        created_at=timestamp(), updated_at=timestamp(), total=0, processed=0,
                        uploaded=0, skipped=0, empty=0, failed=0, ignored=0, results=[])
            self._save(task)
            self.tasks[task['task_id']] = task
            self.pending.put(task['task_id'])
            return self.get(task['task_id']), False

    def _scan(self, root):
        files, ignored = [], 0
        def fail(error):
            raise error
        for current, dirs, names in os.walk(root, onerror=fail, followlinks=False):
            dirs[:] = sorted(d for d in dirs if not (Path(current) / d).is_symlink()
                              and not (Path(current) / d).resolve().is_relative_to(self.runtime)
                              and not (hasattr(Path(current) / d, 'is_junction')
                                       and (Path(current) / d).is_junction()))
            for name in sorted(names):
                path = Path(current) / name
                if path.is_symlink():
                    ignored += 1
                    continue
                incomplete = name.lower().endswith('.mcap.incomplete')
                if path.suffix.lower() not in SUFFIXES and not incomplete:
                    ignored += 1
                    continue
                if incomplete and path.with_name(name[:-11]).exists():
                    ignored += 1
                    continue
                files.append(path)
        return files, ignored

    def _record(self, task_id, result):
        result['mns_required'] = eligible(result)
        with self.lock:
            task = self.tasks[task_id]
            task['results'].append(result)
            task['processed'] += 1
            task[result['status']] += 1
            task['updated_at'] = timestamp()
            self._save(task)
        if result['mns_required']:
            self.notifications.enqueue(result)

    def _execute(self, task_id):
        self.update(task_id, status='scanning', started_at=timestamp())
        files, ignored = self._scan(self.get(task_id)['path'])
        self.update(task_id, total=len(files), ignored=ignored)
        if not files:
            self.update(task_id, status='no_data', error='缓存目录内没有支持的数据文件',
                        finished_at=timestamp())
            return
        plans, destinations = [], {}
        for path in files:
            try:
                kind, destination = classify(self.core, path)
                destinations.setdefault(destination, []).append(path)
                plans.append((path, kind, destination, None))
            except Exception as exc:
                plans.append((path, None, None, str(exc)))
        self.update(task_id, status='uploading')
        config = None
        contexts = {}
        try:
            for path, kind, destination, error in plans:
                if destination and len(destinations[destination]) > 1:
                    error = '多个本地文件映射到同一个 OSS 对象，拒绝上传以避免覆盖'
                try:
                    if error:
                        raise ValueError(error)
                    if config is None:
                        ak = self.core.decrypt_credential(self.core._T_AK_ENC)
                        sk = self.core.decrypt_credential(self.core._T_SK_ENC)
                        if not ak or not sk:
                            raise RuntimeError('OSS 凭证初始化失败')
                        config = self.core.setup_ossutil_config(ak, sk)
                    ctx = contexts.setdefault(kind, dict(mode='config', meta_mode='filename',
                                                          brand='', cfg={}, upload_task=True))
                    result = self.core.process_file(path, kind, True, config, ctx)
                    result['path'] = str(path)
                except Exception as exc:
                    result = dict(file=path.name, path=str(path), ok=False, status='failed', error=str(exc))
                self._record(task_id, result)
        finally:
            if config:
                Path(config).unlink(missing_ok=True)
        task = self.get(task_id)
        status = ('partial_failed' if task['uploaded'] + task['skipped'] > 0 else 'failed') if task['failed'] else 'completed'
        self.update(task_id, status=status, finished_at=timestamp())

    def _work(self):
        while True:
            task_id = self.pending.get()
            try:
                if task_id is None:
                    return
                try:
                    self._execute(task_id)
                except Exception as exc:
                    self.update(task_id, status='failed', error=str(exc), finished_at=timestamp())
            finally:
                self.pending.task_done()

    def close(self):
        with self.lock:
            self.closing = True
        self.pending.put(None)
        self.worker.join()
        self.notifications.close()


def handler_for(manager, token=''):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(15)

        def respond(self, status, value):
            body = json.dumps(value, ensure_ascii=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def authorize(self):
            if token and not hmac.compare_digest(self.headers.get('Authorization', '').encode(),
                                                ('Bearer ' + token).encode()):
                raise RequestError(401, '需要有效的 Bearer token')

        def do_GET(self):
            try:
                self.authorize()
                route = urlsplit(self.path).path
                if route == '/health':
                    self.respond(200, {'status': 'ok'})
                elif route.startswith('/upload/'):
                    self.respond(200, manager.get(route[len('/upload/'):]))
                else:
                    raise RequestError(404, '接口不存在')
            except RequestError as exc:
                self.respond(exc.status, {'error': exc.message})

        def do_POST(self):
            try:
                self.authorize()
                if urlsplit(self.path).path != '/upload':
                    raise RequestError(404, '接口不存在')
                if self.headers.get('Transfer-Encoding'):
                    raise RequestError(400, '不支持 Transfer-Encoding')
                if self.headers.get_content_type() != 'application/json':
                    raise RequestError(415, 'Content-Type 必须为 application/json')
                try:
                    size = int(self.headers.get('Content-Length', '0'))
                except ValueError:
                    raise RequestError(400, '无效 Content-Length')
                if not 0 < size <= 16384:
                    raise RequestError(413, '请求体须为 1 至 16384 字节')
                try:
                    payload = json.loads(self.rfile.read(size))
                except (ValueError, UnicodeError):
                    raise RequestError(400, '请求体不是合法 JSON')
                if not isinstance(payload, dict) or set(payload) != {'path'}:
                    raise RequestError(400, '请求体必须且只能包含 path 字段')
                task, duplicate = manager.submit(payload['path'])
                self.respond(200 if duplicate else 202, dict(accepted=True, duplicate=duplicate,
                    task_id=task['task_id'], status=task['status'],
                    status_url='/upload/' + task['task_id'],
                    message='任务已存在' if duplicate else '已接收上传任务'))
            except RequestError as exc:
                self.respond(exc.status, dict(accepted=False, error=exc.message))
            except TimeoutError:
                self.respond(408, dict(accepted=False, error='读取请求超时'))
            except Exception:
                self.respond(500, dict(accepted=False, error='服务内部错误，请检查服务日志'))
                import traceback
                traceback.print_exc()
    return Handler


def check_runtime():
    """Offline deployment check; no credentials are printed and no OSS requests made."""
    import tempfile
    import subprocess
    with tempfile.TemporaryDirectory(prefix='upload-check-') as directory:
        os.environ['NEW_UPLOAD_RUNTIME'] = directory
        import upload_core as core
        from mns.account import Account
        from mns.queue import Message
        from mns_notifier import ENDPOINT, QUEUE_NAME
        # Construct SDK objects offline so release checks catch missing dependencies.
        mns_account = Account(ENDPOINT, 'self-check', 'self-check')
        mns_account.get_queue(QUEUE_NAME).set_encoding(False)
        Message('{}')
        mns_account.get_client().close_connection()
        import h5py
        from mcap.writer import Writer
        root = Path(directory)
        for credential in (core._T_AK_ENC, core._T_SK_ENC):
            if not core.decrypt_credential(credential):
                raise RuntimeError('凭证解密自检失败')
        with h5py.File(root / 'sample.h5', 'w') as f:
            f.create_dataset('camera/timestamp_rgb', data=[100.0, 104.0])
        assert core.summarize_hdf5(str(root / 'sample.h5'))['duration_sec'] == 4.0
        with (root / 'sample.mcap').open('wb') as f:
            writer = Writer(f)
            writer.start()
            channel = writer.register_channel('/check', 'json', 0)
            writer.add_message(channel, 1_000_000_000, b'{}', 1_000_000_000)
            writer.add_message(channel, 3_000_000_000, b'{}', 3_000_000_000)
            writer.finish()
        assert core.summarize(str(root / 'sample.mcap'))['duration_sec'] == 2.0
        bundled = core._bundled_ossutil()
        if not bundled or not bundled.is_file():
            raise RuntimeError('内嵌 ossutil 缺失')
        subprocess.run([core.get_ossutil_path(), '--version'], check=True, timeout=15)
    print('自检通过：MCAP、HDF5、MNS SDK、凭证解密、内嵌 ossutil；未访问 OSS/MNS。')


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--runtime-dir', type=Path, default=HERE / 'runtime')
    parser.add_argument('--allowed-root', type=Path, help='Optional permitted cache root')
    parser.add_argument('--self-check', action='store_true', help='Offline dependency and bundled ossutil check')
    args = parser.parse_args()
    if args.self_check:
        check_runtime()
        return
    runtime = args.runtime_dir.resolve()
    os.environ['NEW_UPLOAD_RUNTIME'] = str(runtime)
    runtime.mkdir(parents=True, exist_ok=True)
    # Prevent separate service processes from uploading the same groups concurrently.
    with (runtime / 'service.lock').open('a+b') as lock_file:
        lock_file.seek(0)
        lock_file.write(b'0')
        lock_file.flush()
        lock_file.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            parser.error('此 runtime-dir 已有服务运行')
        import upload_core
        manager = TaskManager(upload_core, runtime, args.allowed_root)
        try:
            with ThreadingHTTPServer((args.host, args.port), handler_for(manager, os.environ.get('NEW_UPLOAD_TOKEN', ''))) as server:
                print(f'Upload API: http://{args.host}:{args.port}', flush=True)
                try:
                    server.serve_forever()
                except KeyboardInterrupt:
                    print('等待已接受的任务完成后退出……', flush=True)
        finally:
            manager.close()


if __name__ == '__main__':
    main()
