#!/usr/bin/env python3
"""Collect the GPU userland results of the runner into one directory and seal it for the owner.

  collect.py --unixnu DIR --device DIR --logs DIR --out DIR --sealed DIR

OUT receives:
  adreno-metal-plugin/   build/adreno-metal-plugin as build-adreno-metal-plugin.py left it (NativeAdrenoMetal.bundle with the
                         Turnip library, adrenoprobed, the launchd plists, trust.json, the dSYM of the plugin)
  turnip-ios/            the unstripped iOS Turnip library and builtin-build-profile.json (symbols for crash reports)
  gpu-addon-test.ngpu    scripts/make-gpu-addon.py without --publish: every process the Metal discovery loads the plugin into runs
                         the user test, the Turnip smoke and the bridge self-test, then keeps the software renderer
  gpu-addon-publish.ngpu scripts/make-gpu-addon.py --publish: backboardd returns the native Metal device (GPU compositing)
  logs/                  build and check logs
  run-info.json          pins, ABI header, toolchain, step outcomes, Mach-O and signature checks, every file's sha256
and SEALED the CMS envelope of all of it for ci/xnu/artifact-recipient.pem (ci/xnu/artifacts.py). Nothing leaves in clear."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--unixnu', type=Path, required=True)
parser.add_argument('--device', type=Path, required=True)
parser.add_argument('--logs', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--sealed', type=Path, required=True)
args = parser.parse_args()
ROOT, DEVICE, OUT = args.unixnu.resolve(), args.device.resolve(), args.out.resolve()
PLUGIN = ROOT / 'build/adreno-metal-plugin'
REPLAY_MARK = b'F %u/%u o%x c%u e%c'   # NativeAdrenoUserTest.h freshScan, compiled only with TurnipReplay.h
sys.path.insert(0, str(DEVICE / 'ci/xnu'))
import artifacts  # noqa: E402


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tool(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout.strip() or None
    except OSError:
        return None


def macho(path):
    data = Path(path).read_bytes()
    magic, cpu, subtype, _, count, size = struct.unpack_from('<6I', data)
    info = dict(magic=hex(magic), cpu=hex(cpu), subtype=subtype & 0xffffff, uuid=None, platform=None, minos=None, sdk=None)
    at = 32
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, at)
        if command == 0x1b:
            info['uuid'] = data[at + 8:at + 24].hex()
        if command == 0x32:
            plat, minos, sdk = struct.unpack_from('<III', data, at + 8)
            info.update(platform=plat, minos=f'{minos >> 16}.{(minos >> 8) & 0xff}', sdk=f'{sdk >> 16}.{(sdk >> 8) & 0xff}')
        at += length
    info['ok'] = (magic, cpu, info['subtype'], info['platform'], info['minos']) == (0xfeedfacf, 0x100000c, 0, 2, '17.0')
    return info


def signature(path):
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    page = re.search(r'^Page size=(\d+)$', detail, re.M)
    ident = re.search(r'^Identifier=(\S+)$', detail, re.M)
    return dict(cdhash=cdhash and cdhash[1], page_size=page and int(page[1]), identifier=ident and ident[1])


if OUT.exists():
    shutil.rmtree(OUT)
(OUT / 'logs').mkdir(parents=True)
checks, problems = {}, []

# The plugin as built, the Turnip library with its symbols, and the GPU addons.
if (PLUGIN / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal').is_file():
    shutil.copytree(PLUGIN, OUT / 'adreno-metal-plugin', symlinks=False,
                    ignore=shutil.ignore_patterns('AdrenoPM4.o'))
    bundle = PLUGIN / 'NativeAdrenoMetal.bundle'
    trust = json.loads((PLUGIN / 'trust.json').read_text())
    for name, path, cdhash in [('NativeAdrenoMetal', bundle / 'NativeAdrenoMetal', trust['cdhash']),
                               ('libvulkan_freedreno.dylib', bundle / 'libvulkan_freedreno.dylib', trust['turnip_cdhash']),
                               ('adrenoprobed', PLUGIN / 'adrenoprobed', trust['daemon_cdhash'])]:
        item = dict(macho=macho(path), signature=signature(bundle if name == 'NativeAdrenoMetal' else path),
                    sha256=sha(path), bytes=path.stat().st_size,
                    libraries=tool(['otool', '-L', str(path)]))
        item['turnip_replay_compiled_in'] = REPLAY_MARK in path.read_bytes()
        item['ok'] = (item['macho']['ok'] and item['signature']['page_size'] == 4096 and item['signature']['cdhash'] == cdhash
                      and not item['turnip_replay_compiled_in'])
        if not item['ok']:
            problems.append(name)
        checks[name] = item
    exports = tool(['nm', '-gU', str(bundle / 'libvulkan_freedreno.dylib')]) or ''
    checks['turnip_exports'] = {s: (' T ' + s) in exports for s in ['_vk_icdGetInstanceProcAddr', '_vkGetInstanceProcAddr']}
    if not all(checks['turnip_exports'].values()):
        problems.append('turnip exports')
    for name, flags in [('gpu-addon-test.ngpu', []), ('gpu-addon-publish.ngpu', ['--publish'])]:
        listing = subprocess.run([sys.executable, str(ROOT / 'scripts/make-gpu-addon.py'), *flags, str(OUT / name)],
                                 capture_output=True, text=True)
        (OUT / 'logs' / (name + '.txt')).write_text(listing.stdout + listing.stderr)
        if listing.returncode:
            problems.append(name)
else:
    problems.append('plugin not built')
turnip = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
if turnip.is_file():
    (OUT / 'turnip-ios').mkdir()
    shutil.copyfile(turnip, OUT / 'turnip-ios/libvulkan_freedreno.dylib')
    shutil.copyfile(ROOT / 'build/mesa-tu-ios/builtin-build-profile.json', OUT / 'turnip-ios/builtin-build-profile.json')
    checks['turnip_ios_unstripped'] = dict(macho=macho(turnip), sha256=sha(turnip))

# Logs: the workflow's step logs, the build logs and every check log.
for source in sorted(args.logs.resolve().glob('*')):
    if source.is_file():
        shutil.copyfile(source, OUT / 'logs' / source.name)
    elif source.is_dir():
        shutil.copytree(source, OUT / 'logs' / source.name)
for pattern in ['.cache/mesa-tu-*.log', 'build/adreno-metal-bridge/build-*.log', 'build/turnip-darwin-host/*.log',
                'build/turnip-darwin-host/*.json', 'build/adreno-metal-frontend-host/*.log', 'build/priming-cache/*.json']:
    for source in sorted(ROOT.glob(pattern)):
        target = OUT / 'logs' / source.parent.name.lstrip('.') / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

outcomes = {k[len('OUTCOME_'):].lower(): v for k, v in os.environ.items() if k.startswith('OUTCOME_')}
record = json.loads((args.logs / 'sources.json').read_text()) if (args.logs / 'sources.json').is_file() else None
info = dict(
    purpose='GPU userland: Turnip with the Darwin kernel backend, the metal2vulkan bridge and the NativeAdrenoMetal plugin for '
            'iOS 17 arm64, built at the pinned UniXNU sources; no GPU-bound cache or pack embedded',
    profile={k[len('PROFILE_'):].lower(): v for k, v in os.environ.items() if k.startswith('PROFILE_')},
    abi=record and record.get('abi'), sources=record,
    run={k: os.environ.get(k) for k in ['GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_REF_NAME', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
                                        'ImageOS', 'ImageVersion']},
    host=dict(macos=platform.mac_ver()[0], xcode=tool(['xcodebuild', '-version']),
              iphoneos_sdk=tool(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version']),
              clang=tool(['xcrun', 'clang', '--version']), rustc=tool(['rustc', '+' + os.environ.get('RUST_TOOLCHAIN', '1.98.0'), '-vV']),
              llvm_as=tool(['/opt/homebrew/opt/llvm/bin/llvm-as', '--version']), glslc=tool(['glslc', '--version']),
              meson=tool([str(ROOT / '.cache/mesa-venv/bin/meson'), '--version'])),
    build=dict(turnip_replay='off (UNIXNU_TURNIP_REPLAY=0)', builtin_pack=None, prime_list=(ROOT / 'build/prime-list.txt').is_file(),
               priming_cache=None, m2vpack=None),
    outcomes=outcomes, checks=checks, problems=problems)
files = sorted(p.relative_to(OUT).as_posix() for p in OUT.rglob('*') if p.is_file())
info['outputs'] = {name: dict(bytes=(OUT / name).stat().st_size, sha256=sha(OUT / name)) for name in files
                   if not name.startswith('logs/')}
(OUT / 'run-info.json').write_text(json.dumps(info, indent=2) + '\n')
files.append('run-info.json')

envelope = artifacts.seal(OUT, files, DEVICE / 'ci/xnu/artifact-recipient.pem', args.sealed.resolve())
summary = os.environ.get('GITHUB_STEP_SUMMARY')
if summary:
    with open(summary, 'a') as stream:
        stream.write('| Output | Bytes | SHA-256 |\n| --- | --- | --- |\n')
        for name, item in info['outputs'].items():
            if name.endswith('.ngpu') or name.startswith('adreno-metal-plugin/NativeAdrenoMetal.bundle/'):
                stream.write(f"| {name} | {item['bytes']} | `{item['sha256']}` |\n")
        stream.write('\nStep outcomes: ' + ', '.join(f'{k} {v}' for k, v in sorted(outcomes.items())) + '\n')
        stream.write('\nProblems: ' + (', '.join(problems) or 'none') + '\n\n```json\n' + json.dumps(envelope, indent=2) + '\n```\n')
print(json.dumps(dict(files=len(files), problems=problems, outcomes=outcomes, envelope=envelope), indent=2))
