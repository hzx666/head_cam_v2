"""Embed deployment helpers and monitor defaults; never ship them separately."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
resources = {
    'embedded_boot': (root / 'scripts/boot_autostart.sh').read_text(encoding='utf-8'),
    'embedded_dhcp': (root / 'scripts/manual_dhcp.sh').read_text(encoding='utf-8'),
    'embedded_monitor': (root / 'config/monitor.json').read_text(encoding='utf-8'),
}
declarations = []
for name, value in resources.items():
    data = value.encode('utf-8') + b'\0'
    rows = [','.join(str(x) for x in data[i:i+32]) for i in range(0, len(data), 32)]
    declarations.append(f'static const char {name}[] = {{\n' + ',\n'.join(rows) + '\n};')
pathlib.Path(sys.argv[2]).write_text('\n'.join(declarations), encoding='ascii')
