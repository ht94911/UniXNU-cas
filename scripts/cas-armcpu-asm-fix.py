#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage ArmCpu with the Darwin assembler separator fix in the generated tree.

Darwin's assembler treats ';' as a comment, so multi-instruction inline asm in
ArmCpu.cpp needs explicit separators. The fix comes from UniXNU-umi
(patches/umi-armcpu/darwin-asm-separators.patch, BSD-2-Clause-Patent). The
module checkout stays untouched: the patched copy lives in build/.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'kits/cas/armcpu/darwin-asm-separators.patch'
LINK = ROOT / 'drivers/ArmCpu'
OUTPUT = ROOT / 'build/cas-armcpu'

if not LINK.is_symlink():
    raise SystemExit('drivers/ArmCpu must be the generated-tree module symlink')
if LINK.resolve() == OUTPUT.resolve():
    raise SystemExit(0)
source = LINK.resolve()
if OUTPUT.exists():
    shutil.rmtree(OUTPUT)
shutil.copytree(source, OUTPUT, ignore=shutil.ignore_patterns('.git', 'build', '__pycache__'))
for flags in (['--check'], []):
    subprocess.run(['git', 'apply', *flags, str(PATCH)], cwd=OUTPUT, check=True)
LINK.unlink()
LINK.symlink_to(OUTPUT, target_is_directory=True)
print('cas ArmCpu: Darwin assembler separator fix staged from', source)
