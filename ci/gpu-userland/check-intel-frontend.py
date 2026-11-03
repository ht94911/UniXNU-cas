#!/usr/bin/env python3
"""Compile the actual frontend on Intel first, then prove the host edit leaves ARM objects identical."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
root = a.unixnu.resolve()
kit = root / 'drivers/AdrenoMetal'
source = kit / 'NativeBridgeMetalFrontend.mm'
original = subprocess.check_output(['git', '-C', str(kit), 'show', '816022307852de7e1741ac05ae9111982318763f:NativeBridgeMetalFrontend.mm'])
flags = ['-std=c++17', '-fobjc-arc', '-O2', '-g0', '-fstack-protector-strong',
         '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
         '-I', str(kit), '-I', str(root / 'drivers/Adreno740'), '-I', str(root / '.cache/mesa-work/include')]
subprocess.run(['xcrun', 'clang++', *flags, '-fsyntax-only', str(source)], check=True)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
report = {'Intel_frontend_syntax_passed': True, 'original_source_sha256': hashlib.sha256(original).hexdigest(),
          'updated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'targets': []}
with tempfile.TemporaryDirectory(prefix='frontend-target-') as temp:
    work = Path(temp)
    before = work / 'original/NativeBridgeMetalFrontend.mm'
    before.parent.mkdir()
    before.write_bytes(original)
    for target in ('arm64-apple-ios17.0', 'arm64e-apple-ios27.0'):
        hashes = []
        for label, path in (('before', before), ('after', source)):
            output = work / (target + '-' + label + '.o')
            cmd = ['xcrun', 'clang++', '-target', target, '-isysroot', sdk, *flags,
                   '-ffile-prefix-map=' + str(before.parent) + '=' + str(kit), '-c', str(path), '-o', str(output)]
            subprocess.run(cmd, check=True)
            hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
        if hashes[0] != hashes[1]:
            raise SystemExit('Host change altered the actual ' + target + ' object: ' + str(hashes))
        report['targets'].append({'target': target, 'object_sha256': hashes[0], 'before_after_identical': True})
a.report.parent.mkdir(parents=True, exist_ok=True)
a.report.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
