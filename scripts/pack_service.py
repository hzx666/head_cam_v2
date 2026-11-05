"""Generate an XZ payload and assembly for the static ARM64 launcher."""
import hashlib
import lzma
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
payload = source.read_bytes()
compressed = output / "service.xz"
compressed.write_bytes(lzma.compress(payload, filters=[
    {"id": lzma.FILTER_LZMA2, "preset": 9, "dict_size": 8 * 1024 * 1024},
]))
(output / "packed_payload.h").write_text(
    f"#define PAYLOAD_SIZE {len(payload)}\n"
    f'#define PAYLOAD_TAG "{hashlib.sha256(payload).hexdigest()}"\n',
    encoding="ascii",
)
(output / "packed_payload.S").write_text(
    '.section .rodata\n.global payload_start,payload_end\npayload_start:\n'
    f'.incbin "{compressed}"\npayload_end:\n.section .note.GNU-stack,"",%progbits\n',
    encoding="utf-8",
)
