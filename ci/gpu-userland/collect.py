#!/usr/bin/env python3
"""Collect the GPU userland results of the runner into one directory and seal it for the owner.

  collect.py --unixnu DIR --device DIR --logs DIR --out DIR --sealed DIR

Outputs are the new arm64e/iOS27 plugin, bridge, Turnip library and diagnostics.
No device staging, OS input, shader priming data or original firmware is included.
All outputs leave through the existing CMS envelope.
"""
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
parser.add_argument('--host-only', action='store_true')
parser.add_argument('--debug-out', type=Path)
parser.add_argument('--debug-sealed', type=Path)
args = parser.parse_args()
if bool(args.debug_out) != bool(args.debug_sealed):
    parser.error('--debug-out and --debug-sealed must be paired')
ROOT, DEVICE, OUT = args.unixnu.resolve(), args.device.resolve(), args.out.resolve()
PLUGIN = ROOT / 'build/adreno-metal-plugin27'
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
    info['ok'] = (magic, cpu, info['subtype'], info['platform'], info['minos']) == (0xfeedfacf, 0x100000c, 2, 2, '27.0')
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
if not args.host_only and (PLUGIN / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal').is_file():
    shutil.copytree(PLUGIN, OUT / 'adreno-metal-plugin', symlinks=False,
                    ignore=shutil.ignore_patterns('*.o', '*.a', '*.dSYM'))
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
elif not args.host_only:
    problems.append('plugin not built')
turnip = ROOT / 'build/mesa-tu-ios27/src/freedreno/vulkan/libvulkan_freedreno.dylib'
if not args.host_only and turnip.is_file():
    (OUT / 'turnip-ios27').mkdir()
    shutil.copyfile(turnip, OUT / 'turnip-ios27/libvulkan_freedreno.dylib')
    shutil.copyfile(ROOT / 'build/mesa-tu-ios27/builtin-build-profile.json', OUT / 'turnip-ios27/builtin-build-profile.json')
    checks['turnip_ios_unstripped'] = dict(macho=macho(turnip), sha256=sha(turnip))

# Logs: the workflow's step logs, the build logs and every check log.
for source in sorted(args.logs.resolve().glob('*')):
    if source.is_file():
        shutil.copyfile(source, OUT / 'logs' / source.name)
    elif source.is_dir():
        shutil.copytree(source, OUT / 'logs' / source.name)
for pattern in ['.cache/mesa-tu-*.log', 'build/adreno-metal-bridge/build-*.log', 'build/turnip-darwin-host/*.log',
                'build/turnip-darwin-host/*.json', 'build/adreno-metal-frontend-host/*.log', 'build/priming-cache/*.json', 'build/adreno-metal-plugin27/*.json']:
    for source in sorted(ROOT.glob(pattern)):
        target = OUT / 'logs' / source.parent.name.lstrip('.') / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
host_debug = ROOT / 'build/host-debug'
if args.host_only and host_debug.is_dir():
    shutil.copytree(host_debug, OUT / 'host-debug')

outcomes = {k[len('OUTCOME_'):].lower(): v for k, v in os.environ.items() if k.startswith('OUTCOME_') and v}
record = json.loads((args.logs / 'sources.json').read_text()) if (args.logs / 'sources.json').is_file() else None
info = dict(
    purpose='GPU userland: Turnip with the Darwin kernel backend, the metal2vulkan bridge and the NativeAdrenoMetal plugin for '
            'iOS 27 arm64e, built at the pinned UniXNU sources; no GPU-bound cache or pack embedded',
    profile={k[len('PROFILE_'):].lower(): v for k, v in os.environ.items() if k.startswith('PROFILE_')},
    abi=record and record.get('abi'), sources=record,
    run={k: os.environ.get(k) for k in ['GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_REF_NAME', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
                                        'ImageOS', 'ImageVersion']},
    host=dict(macos=platform.mac_ver()[0], xcode=tool(['xcodebuild', '-version']),
              iphoneos_sdk=tool(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version']),
              clang=tool(['xcrun', 'clang', '--version']), rustc=tool(['rustc', '+' + os.environ.get('RUST_TOOLCHAIN', '1.98.0'), '-vV']),
              llvm_as=tool([shutil.which('llvm-as') or 'llvm-as', '--version']), glslc=tool(['glslc', '--version']),
              meson=tool([str(ROOT / '.cache/mesa-venv/bin/meson'), '--version'])),
    build=dict(turnip_replay='off (upstream --no-replay)', builtin_pack=None, prime_list=(ROOT / 'build/prime-list.txt').is_file(),
               priming_cache=None, m2vpack=None),
    outcomes=outcomes, checks=checks, problems=problems, hardware_verified=False,
    frontend_host_scope='host Vulkan instrument only; original QuartzCore fixture not provided')
if args.host_only:
    equality = args.logs / 'frontend-target-equality.json'
    info['purpose'] = 'macOS ' + platform.machine() + ' public-Vulkan frontend phase plus actual arm64 iOS17 and arm64e iOS27 signal-handler comparison. Private QuartzCore/selector fixtures and later Turnip software-CP/CoreImage phases are not covered. Does not replace the fixed Metal002 plugin.'
    public_phase = ROOT / 'build/adreno-metal-frontend-host/public-phase.json'
    info['checks']['public_vulkan_phase'] = json.loads(public_phase.read_text()) if public_phase.is_file() else None
    info['host_only'] = True
    info['host_device_base'] = 'default maintained _MTLDevice; target plugin unchanged'
    info['host_driver_configuration'] = {'MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS': os.environ.get('MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS'), 'scope': 'host MoltenVK only; no iPhone driver capability changed', 'prior_abort': '006 crash: MVKPhysicalDevice::initMetalFeatures -> MTLIOAccelDevice newIndirectArgumentEncoderWithLayout'}
    info['checks']['frontend_targets'] = json.loads(equality.read_text()) if equality.is_file() else None
    if not equality.is_file():
        problems.append('target equality qualification missing')
    for name, outcome in outcomes.items():
        if outcome != 'success':
            problems.append(name + ' ' + outcome)
files = sorted(p.relative_to(OUT).as_posix() for p in OUT.rglob('*') if p.is_file())
info['outputs'] = {name: dict(bytes=(OUT / name).stat().st_size, sha256=sha(OUT / name)) for name in files
                   if not name.startswith('logs/')}
(OUT / 'run-info.json').write_text(json.dumps(info, indent=2) + '\n')
files.append('run-info.json')

envelope = artifacts.seal(OUT, files, DEVICE / 'ci/xnu/artifact-recipient.pem', args.sealed.resolve())
debug_envelope = None
if args.debug_out and PLUGIN.is_dir():
    debug = args.debug_out.resolve()
    debug.mkdir(parents=True, exist_ok=False)
    for item in sorted(PLUGIN.iterdir()):
        if item.suffix == '.dSYM' and item.is_dir():
            shutil.copytree(item, debug / item.name, symlinks=False)
        elif item.suffix in ('.a', '.o') and item.is_file():
            shutil.copyfile(item, debug / item.name)
    debug_files = sorted(p.relative_to(debug).as_posix() for p in debug.rglob('*') if p.is_file())
    if debug_files:
        (debug / 'debug-identity.json').write_text(json.dumps(dict(
            run=info['run'], production_outputs=info['outputs'],
            purpose='Optional exact debug symbols and build archives;not needed for phone deployment',
            files={name: dict(bytes=(debug / name).stat().st_size, sha256=sha(debug / name)) for name in debug_files}
        ), indent=2) + '\n')
        debug_files.append('debug-identity.json')
        debug_envelope = artifacts.seal(debug, debug_files, DEVICE / 'ci/xnu/artifact-recipient.pem', args.debug_sealed.resolve())
summary = os.environ.get('GITHUB_STEP_SUMMARY')
if summary:
    with open(summary, 'a') as stream:
        stream.write('| Output | Bytes | SHA-256 |\n| --- | --- | --- |\n')
        for name, item in info['outputs'].items():
            if name.endswith('.ngpu') or name.startswith('adreno-metal-plugin/NativeAdrenoMetal.bundle/'):
                stream.write(f"| {name} | {item['bytes']} | `{item['sha256']}` |\n")
        stream.write('\nStep outcomes: ' + ', '.join(f'{k} {v}' for k, v in sorted(outcomes.items())) + '\n')
        stream.write('\nProblems: ' + (', '.join(problems) or 'none') + '\n\n```json\n' + json.dumps(envelope, indent=2) + '\n```\n')
print(json.dumps(dict(files=len(files), problems=problems, outcomes=outcomes, envelope=envelope,
                     encrypted_debug_retained_separately=debug_envelope is not None), indent=2))
