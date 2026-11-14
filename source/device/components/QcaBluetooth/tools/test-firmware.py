#!/usr/bin/env python3
"""Check PERI NVM preparation with synthetic fixtures and optional SHA-verified local stock inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', action='append', type=Path, default=[], help='optional captured741-byte PERI NVM; kept in memory')
parser.add_argument('--stock-sha256', help='required original digest for every optional stock input')
args = parser.parse_args()
if args.stock and (not args.stock_sha256 or len(args.stock_sha256) != 64 or
                   any(c not in '0123456789abcdef' for c in args.stock_sha256)):
    parser.error('--stock requires an exact lowercase --stock-sha256')
out = root / 'out/firmware-test'
out.mkdir(parents=True, exist_ok=True)
sources = ['src/qbt_firmware.c', 'tests/firmware.c']
inputs = sources + ['include/qbt_firmware.h', 'tools/test-firmware.py']
command = [os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-I' + str(root / 'include'),
           *[str(root / source) for source in sources], '-o', str(out / 'firmware')]
subprocess.run(command, check=True)
result = subprocess.run([out / 'firmware'], text=True, capture_output=True)
(out / 'output.txt').write_text(result.stdout + result.stderr)
report = dict(passed=result.returncode == 0, synthetic_firmware=True, controller_simulated=False,
              hardware_verified=False, device_operations=False, private_firmware_used=bool(args.stock),
              private_bytes_saved=False, command=command,
              inputs={source: hashlib.sha256((root / source).read_bytes()).hexdigest() for source in inputs},
              output=result.stdout + result.stderr, stock_checks=[])
if result.returncode:
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    raise SystemExit((result.stdout + result.stderr)[-5000:] or str(result.returncode))
for path in args.stock:
    original = path.read_bytes()
    source_sha = hashlib.sha256(original).hexdigest()
    if source_sha != args.stock_sha256:
        raise SystemExit(f'Original SHA mismatch: {path}')
    prepared = subprocess.run([out / 'firmware', '--prepare-stdin'], input=original, capture_output=True)
    if prepared.returncode:
        raise SystemExit(prepared.stderr.decode('utf-8', 'replace')[-5000:] or str(prepared.returncode))
    copy = prepared.stdout
    assert len(copy) == len(original) == 741
    changed = [i for i, (a, b) in enumerate(zip(original, copy)) if a != b]
    assert changed == [93] and original[93] == 0xfd and copy[93] == 0x11
    assert path.read_bytes() == original
    report['stock_checks'].append(dict(path=str(path.resolve()), bytes=len(original),
        original_sha256=source_sha, prepared_sha256=hashlib.sha256(copy).hexdigest(),
        original_preserved=True, changed_offsets=changed, prepared_baud_code='0x11'))
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(result.stdout, end='')
for item in report['stock_checks']:
    print(f"SHA-verified stock {Path(item['path']).name}: only offset93 changed; original preserved; no private bytes saved")
