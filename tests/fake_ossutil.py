#!/usr/bin/python3
"""Offline CLI double. No network access; enabled only in SERVICE_TESTING builds."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time

args = sys.argv[1:]
assert args[0] == '-c'
config = Path(args[1])
assert config.stat().st_mode & 0o077 == 0
command, args = args[2], args[3:]
if command == '--version':
    print('ossutil test double')
    sys.exit(0)
root = Path(os.environ['FAKE_OSS_ROOT'])
root.mkdir(exist_ok=True)
destination = args[1] if command == 'cp' else args[0]
key = hashlib.sha256(destination.encode()).hexdigest()
record = root / (key + '.json')
control = root / 'control.json'
rules = json.loads(control.read_text()) if control.exists() else {}
if rules.get('delay'):
    time.sleep(rules['delay'])
if command in rules.get('fail', []):
    print('Error: StatusCode=403, ErrorCode=AccessDenied')
    sys.exit(1)
if command == 'stat':
    if not record.exists():
        print('Error: StatusCode=404, ErrorCode=NoSuchKey')
        sys.exit(1)
    data = json.loads(record.read_text())
    print('Etag : "' + data['etag'] + '"')
    print('Last-Modified : 2026-10-27 00:00:00 +0000 UTC')
    for name, value in data['metadata'].items():
        print(name + ' : ' + value)
elif command == 'cp':
    content = Path(args[0]).read_bytes()
    headers = dict(pair.split(':', 1) for pair in args[args.index('--meta') + 1].split('#'))
    record.write_text(json.dumps(dict(destination=destination, etag=hashlib.md5(content).hexdigest(), metadata=headers,
                                     actual_sha256=hashlib.sha256(content).hexdigest(), uploads=1)))
    if rules.get('mutate_source'):
        with open(args[0], 'ab') as stream:
            stream.write(b'changed')
elif command == 'set-meta':
    data = json.loads(record.read_text())
    data['metadata'].update(pair.split(':', 1) for pair in args[1].split('#'))
    record.write_text(json.dumps(data))
else:
    raise AssertionError(command)
