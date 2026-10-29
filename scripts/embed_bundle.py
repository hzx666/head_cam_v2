#!/usr/bin/env python3
"""Generate private build inputs. Never prints credentials; does not import reference code."""
import argparse
import ast
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess


def decrypt(value, password):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    data = base64.b64decode(value, validate=True)
    if data[:8] != b'Salted__':
        raise ValueError('Invalid embedded credential format')
    derived = hashlib.pbkdf2_hmac('sha256', password.encode(), data[8:16], 10000, 48)
    decryptor = Cipher(algorithms.AES(derived[:32]), modes.CBC(derived[32:])).decryptor()
    plain = decryptor.update(data[16:]) + decryptor.finalize()
    padding = plain[-1]
    if not 1 <= padding <= 16 or plain[-padding:] != bytes([padding]) * padding:
        raise ValueError('Invalid credential padding')
    return plain[:-padding].decode()


def main():
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--credentials-source', type=Path, help='Existing teleop upload_core.py')
    source.add_argument('--credentials-json', type=Path, help='Private build-only JSON')
    parser.add_argument('--ossutil', required=True, type=Path)
    parser.add_argument('--ca', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--arch', choices=['arm64', 'x86_64'], default='arm64')
    args = parser.parse_args()
    os.umask(0o077)
    if args.credentials_source:
        values = {}
        for node in ast.parse(args.credentials_source.read_text(encoding='utf-8')).body:
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name) and target.id in {'_K', '_T_AK_ENC', '_T_SK_ENC', 'OSS_BUCKET', 'OSS_ENDPOINT'}:
                        values[target.id] = ast.literal_eval(node.value)
        credentials = dict(endpoint=values['OSS_ENDPOINT'], bucket=values['OSS_BUCKET'],
                           access_key_id=decrypt(values['_T_AK_ENC'], values['_K']),
                           access_key_secret=decrypt(values['_T_SK_ENC'], values['_K']), security_token='')
    else:
        credentials = json.loads(args.credentials_json.read_text())
    for key in ['endpoint', 'bucket', 'access_key_id', 'access_key_secret']:
        if not isinstance(credentials.get(key), str) or not credentials[key] or any(c in credentials[key] for c in '\r\n'):
            raise ValueError('Missing or invalid build credential field: ' + key)
    credentials['endpoint'] = credentials['endpoint'].removeprefix('https://')
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.ossutil.read_bytes()
    expected = b'\xb7\x00' if args.arch == 'arm64' else b'\x3e\x00'
    if binary[:6] != b'\x7fELF\x02\x01' or binary[18:20] != expected:
        raise ValueError('ossutil architecture does not match release target')
    # Refuse a dynamically linked helper; the deployed artifact must be self-contained.
    elf = subprocess.check_output(['readelf', '-l', str(args.ossutil)], text=True)
    if 'INTERP' in elf:
        raise ValueError('ossutil must be a static executable')
    (args.out / 'ossutil.bin').write_bytes(binary)
    cert = args.ca.read_bytes()
    if b'BEGIN CERTIFICATE' not in cert:
        raise ValueError('Invalid CA bundle')
    (args.out / 'ca.pem').write_bytes(cert)
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    key, iv = os.urandom(32), os.urandom(16)
    encryptor = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    encrypted = encryptor.update(json.dumps(credentials, separators=(',', ':')).encode()) + encryptor.finalize()
    def array(name, data):
        return 'static const unsigned char ' + name + '[] = {' + ','.join(str(b) for b in data) + '};\n'
    (args.out / 'credentials.h').write_text(array('credential_key', key) + array('credential_iv', iv) + array('credential_cipher', encrypted))
    assembly = '.section .rodata\n'
    for name, file in [('ossutil', 'ossutil.bin'), ('ca', 'ca.pem')]:
        path = str((args.out / file).resolve())
        if '"' in path or '\n' in path:
            raise ValueError('Invalid build path')
        assembly += f'.global bundled_{name}_start\n.global bundled_{name}_end\nbundled_{name}_start:\n.incbin "{path}"\nbundled_{name}_end:\n'
    assembly += '.section .note.GNU-stack,"",%progbits\n'
    (args.out / 'bundle_data.S').write_text(assembly)
    print('Prepared encrypted build credentials, static ossutil and CA bundle (no secrets printed).')


if __name__ == '__main__':
    main()
