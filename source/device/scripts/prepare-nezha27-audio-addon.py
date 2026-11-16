#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare an independent audio addon/trust pair using the selected framework.

Uses an already compiled and signed audio HAL plus an explicitly pinned base
addon and trust bundle. No compiler, network, phone or Data/seed operation. A new output
directory is required. This does not enable playback or qualify a boot image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import struct
import sys

TOOLS = {'addon-add-dir.py': 'f7038c80ac859ab48bfc6ffeac95d746b4b4ba4e2c7f50b9d56ab78af419acd8',
         'trust-append-module.py': '5de6c87ea926e2c9b6eefba8ecf643dec656e693d24aa51d7c06f7fb0e57fa4d'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(script, *args):
    saved = sys.argv
    try:
        sys.argv = [str(script), *map(str, args)]
        return runpy.run_path(str(script), run_name='__main__')
    finally:
        sys.argv = saved


def modules(path):
    data = path.read_bytes()
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count < 1024 or 4 + 4 * count > len(data):
        raise ValueError('invalid trust bundle count')
    offsets = list(struct.unpack_from(f'<{count}I', data, 4)) + [len(data)]
    if offsets[0] != 4 + 4 * count or any(a >= b for a, b in zip(offsets, offsets[1:])):
        raise ValueError('invalid trust bundle offsets')
    return [data[a:b] for a, b in zip(offsets, offsets[1:])]


def prepare(args):
    for name, digest in TOOLS.items():
        if sha(args.framework / 'scripts' / name) != digest:
            raise ValueError('selected framework helper changed: ' + name)
    for path, digest in ((args.base_addon, args.expected_base_addon_sha256),
                         (args.base_trust, args.expected_base_trust_sha256)):
        if not re.fullmatch(r'[0-9a-f]{64}', digest) or sha(path) != digest:
            raise ValueError('selected base addon/trust identity differs: ' + str(path))
    build = json.loads((args.native_output / 'build-report.json').read_text())
    bundle = args.native_output / 'NezhaAudio27.driver'
    hal = build['HAL']
    if hal.get('status') != 'compiled-signed' or hal.get('returncode') != 0 or sha(bundle / 'NezhaAudio27') != hal['sha256']:
        raise ValueError('HAL is not the exact successful signed native output')
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    if info['CFBundleExecutable'] != 'NezhaAudio27' or info['CFBundleIdentifier'] != 'org.unixnu.nezha.audio27':
        raise ValueError('HAL bundle identity mismatch')
    signature = (args.native_output / 'HAL-signature.txt').read_text()
    matches = re.findall(r'^CDHash=([0-9a-f]{40})$', signature, re.M)
    if len(matches) != 1 or not re.search(r'^Page size=4096$', signature, re.M) or 'Format=bundle with Mach-O thin (arm64e)' not in signature:
        raise ValueError('missing matching native arm64e/4K signature record')
    cdhash = matches[0]
    paths = list(bundle.rglob('*'))
    expected = {'Info.plist', 'NezhaAudio27', '_CodeSignature/CodeResources'}
    if any(p.is_symlink() for p in paths) or {p.relative_to(bundle).as_posix() for p in paths if p.is_file()} != expected:
        raise ValueError('unexpected HAL members')
    args.output.mkdir(parents=True, exist_ok=False)
    staged = args.output / 'stage' / bundle.name
    shutil.copytree(bundle, staged)
    for path in staged.rglob('*'):
        if path.is_file():
            path.chmod(0o755 if path.name == 'NezhaAudio27' else 0o644)
    addon = args.output / 'audio.addon'
    helper = invoke(args.framework / 'scripts/addon-add-dir.py', args.base_addon, addon, 'audio', staged.parent)
    before = helper['read'](args.base_addon)
    after = helper['read'](addon)
    if len({n for n, _, _ in after}) != len(after) or any(n.startswith('audio/') for n, _, _ in before):
        raise ValueError('duplicate addon members or an existing audio bundle')
    retained = [(n, m, b) for n, m, b in after if not n.startswith('audio/')]
    if retained != before:
        raise ValueError('selected base addon member changed')
    added = [(n, m, b) for n, m, b in after if n.startswith('audio/')]
    if {n for n, _, _ in added} != {'audio/NezhaAudio27.driver/' + p for p in expected}:
        raise ValueError('wrong new audio paths')
    for name, mode, body in added:
        source = bundle / name.removeprefix('audio/NezhaAudio27.driver/')
        if body != source.read_bytes() or mode != (0o755 if source.name == 'NezhaAudio27' else 0o644):
            raise ValueError('HAL bytes or executable mode changed')
    trust = args.output / 'audio.trust'
    invoke(args.framework / 'scripts/trust-append-module.py', args.base_trust, trust, cdhash)
    old, new = modules(args.base_trust), modules(trust)
    if new[:-1] != old or len(new[-1]) != 48 or struct.unpack_from('<I', new[-1])[0] != 2 or struct.unpack_from('<I', new[-1], 20)[0] != 1 or new[-1][24:44].hex() != cdhash or new[-1][44:] != bytes([2, 0, 0, 0]):
        raise ValueError('trust module preservation/append mismatch')
    result = {'kind': 'computer-only-audio-addon-pair', 'base_addon_members_preserved': len(before),
              'base_addon_sha256': args.expected_base_addon_sha256,
              'base_trust_sha256': args.expected_base_trust_sha256,
              'added_HAL_files': len(added), 'original_trust_modules_preserved': len(old),
              'HAL_sha256': hal['sha256'], 'HAL_cdhash': cdhash,
              'addon_sha256': sha(addon), 'trust_sha256': sha(trust),
              'existing_native_signature_record_reused': True, 'new_codesign_run': False,
              'boot_image_created': False, 'phone_accessed': False, 'seed_changed': False,
              'audio_hardware_accepted': False, 'main_package_promoted': False}
    (args.output / 'ready.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('framework', 'native-output', 'base-addon', 'base-trust', 'output'):
        p.add_argument('--' + name, required=True, type=Path)
    for name in ('expected-base-addon-sha256', 'expected-base-trust-sha256'):
        p.add_argument('--' + name, required=True)
    print(json.dumps(prepare(p.parse_args()), indent=2))
