"""Durable per-object MNS notifications, delivered by a single background worker."""
import copy
import hashlib
import json
import os
from pathlib import Path
import threading
import time

ENDPOINT = 'https://1766661213327930.mns.cn-shanghai.aliyuncs.com'
QUEUE_NAME = 'data-platfrom-event'
EVENTS = ('mcap_extract_preview', 'mcap_qc')


def eligible(result):
    return (result.get('ok') is True
            and result.get('status') in ('uploaded', 'skipped')
            and result.get('checksum_status') == 'match'
            and bool(result.get('ossPath')) and bool(result.get('sha256'))
            and Path(result.get('file', '')).suffix.lower() in ('.mcap', '.hdf5', '.h5'))


class MNSSender:
    def __init__(self, core):
        self.core = core

    def __call__(self, body):
        from mns.account import Account
        from mns.queue import Message
        ak = self.core.decrypt_credential(self.core._T_AK_ENC)
        sk = self.core.decrypt_credential(self.core._T_SK_ENC)
        if not ak or not sk:
            raise RuntimeError('MNS 凭证初始化失败')
        # SDK objects are confined to the sender thread. Recreate after failures.
        account = Account(ENDPOINT, ak, sk)
        client = account.get_client()
        client.set_connection_timeout(15)
        # SDK 1.3.0's setter only rebuilds HTTP connections; HTTPS otherwise has
        # no socket timeout. Set it on the HTTPSConnection before first connect.
        client.http.conn.timeout = 15
        try:
            queue = account.get_queue(QUEUE_NAME)
            queue.set_encoding(False)  # Plain JSON, not a Base64 message body.
            response = queue.send_message(Message(json.dumps(body, ensure_ascii=False)))
            if not response.message_id:
                raise RuntimeError('MNS 未返回 MessageId')
            return response.message_id
        finally:
            client.close_connection()


class NotificationOutbox:
    def __init__(self, runtime, sender, log=print):
        self.store = Path(runtime) / 'mns'
        self.store.mkdir(parents=True, exist_ok=True)
        self.sender, self.log = sender, log
        self.lock = threading.RLock()
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.records = {}
        for file in self.store.glob('*.json'):
            record = json.loads(file.read_text(encoding='utf-8'))
            self.records[file.stem] = record
        self.worker = None

    @staticmethod
    def key(result):
        identity = [ENDPOINT, QUEUE_NAME, result['ossPath'], result['sha256']]
        return hashlib.sha256(json.dumps(identity).encode('utf-8')).hexdigest()

    def _save(self, key, record):
        dest = self.store / (key + '.json')
        temp = dest.with_suffix('.tmp')
        with temp.open('w', encoding='utf-8') as stream:
            json.dump(record, stream, ensure_ascii=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, dest)
        self.records[key] = record

    def enqueue(self, result):
        key = self.key(result)
        with self.lock:
            if key not in self.records:
                self._save(key, dict(ossPath=result['ossPath'], sha256=result['sha256'],
                    events={event: dict(status='pending', attempts=0, next_attempt=0,
                                        error='', message_id='') for event in EVENTS}))
        self.wake.set()
        return key

    def snapshot(self, key):
        with self.lock:
            record = self.records.get(key)
            if record is None:
                return {event: dict(status='pending', error='等待保存消息记录') for event in EVENTS}
            return copy.deepcopy(record['events'])

    def deliver_due(self):
        with self.lock:
            keys = list(self.records)
        for key in keys:
            for event in EVENTS:
                if self.stop.is_set():
                    return
                with self.lock:
                    record = copy.deepcopy(self.records[key])
                state = record['events'][event]
                if state['status'] == 'sent' or state['next_attempt'] > time.time():
                    continue
                body = dict(event=event, code=200, msg='', traceId='',
                            data={'ossPath': record['ossPath']})
                state['attempts'] += 1
                try:
                    state['message_id'] = self.sender(body)
                    state.update(status='sent', error='', next_attempt=0)
                except Exception as exc:
                    # Never persist SDK exception text, which may contain request headers.
                    error = str(getattr(exc, 'type', type(exc).__name__))[:120]
                    delay = min(300, 5 * 2 ** min(state['attempts'] - 1, 6))
                    state.update(status='pending', error=error, next_attempt=time.time() + delay)
                with self.lock:
                    self._save(key, record)
                self.log(f"MNS {event}: {state['status']} ({record['ossPath']})"
                         + (f"，{state['error']}，等待重试" if state['error'] else ''))

    def start(self):
        self.worker = threading.Thread(target=self._run, name='mns-worker', daemon=True)
        self.worker.start()

    def _run(self):
        while not self.stop.is_set():
            self.wake.clear()
            try:
                self.deliver_due()
            except Exception as exc:
                self.log(f'MNS 本地状态处理失败，将重试: {type(exc).__name__}')
            self.wake.wait(1)

    def close(self):
        self.stop.set()
        self.wake.set()
        if self.worker is not None:
            self.worker.join()
