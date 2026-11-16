#!/usr/bin/env python3
"""Offline SDK fixture; never touches hardware."""
import json
import os
import pathlib
import sys

state = json.loads(pathlib.Path(os.environ['TEST_EGO_STATE']).read_text())
if sys.argv[2] == 'query':
    print(json.dumps(state), flush=True)
else:
    pathlib.Path(os.environ['TEST_EGO_INSTALLED']).write_bytes(pathlib.Path(sys.argv[3]).read_bytes())
    print(json.dumps({'status': 3, 'progress': 60, 'result': 0}), flush=True)
    print(json.dumps({'status': 4, 'progress': 100, 'result': 0}), flush=True)
