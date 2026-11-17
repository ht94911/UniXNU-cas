#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Adapted from 57UU/UniXNU-sirius f269452c build-sirius-pmgr.py.
"""Build the reused PMGR publisher using this kernel's actual C++ ABI."""
import json
from pathlib import Path
from kernel_target import check_object
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)
source = ROOT / 'kits/nezha/NezhaIOS27Pmgr.cpp'
obj = OUT / 'NezhaIOS27Pmgr.o'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
                '-Wno-missing-designated-field-initializers', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
check_object(obj.read_bytes(), 'Nezha iOS27 PMGR publisher')
archive = OUT / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print('nezha iOS27 PMGR publisher: recorded XNU ABI, -Werror and kernel target object audit passed')
