#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a separate mode6 boot using a qualified C2 kernel and camera addon.

Keeps the fixed four-entry temporary boot, original root/seed and GPU content.
Only the iOS27 mode, camera launch policy, signed addon and trust pair change.
This prepares a candidate; live RAW and system-camera acceptance stay separate.
"""
import argparse
import hashlib
import hmac
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'EFI/UniXNU/'
KERNEL = PREFIX + 'Payloads/kernel.macho'
MENU = PREFIX + 'unixnu.toml'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(path):
    return json.loads(path.read_text())


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-work', type=Path, required=True)
    p.add_argument('--addon', type=Path, required=True)
    p.add_argument('--addon-native-work', type=Path, required=True)
    p.add_argument('--framework', type=Path, required=True)
    p.add_argument('--prelink-tools', type=Path, required=True)
    p.add_argument('--gpu-tools', type=Path, required=True)
    p.add_argument('--candidate', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    prior = a.native_work / 'candidate'
    package = load(prior / 'computer-package.json')
    dispatch = load(a.native_work / 'ci-dispatch.json')
    assert dispatch['conclusion'] == 'success' and package['camera_probe_mode'] == 5
    assert dispatch['head'] == package['native_head'] and dispatch['run_id'] == package['native_run']
    strict = load(prior / 'strict/qualified.json')
    assert strict['zero_missing'] and strict['zero_stubs'] and len(strict['closure']) == 38
    addon = load(a.addon / 'camera-addon.json')
    assert addon['code_and_PAC_sections_preserved'] and addon['objects_verified'] == 10
    assert not addon['phone_operations'] and not addon['system_camera_enabled']
    assert all(r['page_bytes'] == 4096 for r in addon['signed_library']['code_directories'])
    inputs = load(a.native_work / 'result/inputs.json')
    library_inputs = load(a.addon_native_work / 'result/inputs.json')
    assert addon['native_head'] == load(a.addon_native_work / 'ci-dispatch.json')['head']
    user_sources = [name for name in library_inputs['files']
                    if name.startswith('source/device/package/camera/userland/')]
    assert user_sources
    exact_userland = all(inputs['files'].get(name) == library_inputs['files'][name] for name in user_sources)
    if not exact_userland and addon.get('camera_model_sha256'):
        # A userland-only descendant of the selected C2 source may update
        # the factory/model, while every kernel source and ABI stays fixed.
        source = addon['source_revision']
        subprocess.run(['git', '-C', str(ROOT), 'merge-base', '--is-ancestor',
                        dispatch['device_source'], source], check=True)
        changed = subprocess.check_output(['git', '-C', str(ROOT), 'diff', '--name-only',
                                           dispatch['device_source'], source], text=True).splitlines()
        assert changed and all(name.startswith(('package/camera/userland/', 'tests/', 'scripts/'))
                               for name in changed)
        for name in user_sources:
            original = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                                 source + ':' + name.removeprefix('source/device/')])
            assert sha(original) == library_inputs['files'][name], name
    else:
        assert exact_userland
    a.out.mkdir(parents=True, exist_ok=False)
    stage = a.out / 'stage'
    for name, digest in package['members'].items():
        source = prior / 'stage' / name
        assert sha(source.read_bytes()) == digest, name
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for suffix in ('gpuaddon', 'trustcache'):
        source = a.addon / ('kernel.macho.' + suffix)
        assert sha(source.read_bytes()) == addon[suffix.replace('gpuaddon', 'addon') + '_sha256']
        shutil.copyfile(source, stage / (KERNEL + '.' + suffix))

    sys.path.insert(0, str(a.gpu_tools.resolve()))
    readers = module(a.gpu_tools / 'gpu007_inputs.py', 'camera_system_package_readers')
    old_addon = readers.addon_files((prior / 'stage' / (KERNEL + '.gpuaddon')).read_bytes())
    new_addon = readers.addon_files((stage / (KERNEL + '.gpuaddon')).read_bytes())
    additions = {'addons/libNezhaCamera27.dylib':
                 (0o644, (a.addon / 'libNezhaCamera27.dylib').read_bytes())}
    if addon.get('camera_model_sha256'):
        model = (a.addon / 'NezhaCameraModel.plist').read_bytes()
        assert sha(model) == addon['camera_model_sha256']
        parsed = plistlib.loads(model)
        assert len(parsed['AVCaptureDevices']) == 1
        additions['camera/NezhaCameraModel.plist'] = (0o644, model)
    assert new_addon == old_addon | additions
    old_trust = readers.trust_modules((prior / 'stage' / (KERNEL + '.trustcache')).read_bytes())
    new_trust = readers.trust_modules((stage / (KERNEL + '.trustcache')).read_bytes())
    assert len(new_trust) == 4 and new_trust[:-1] == old_trust

    args = package['kernel_args'].split()
    assert args.count('nezha-camera=5') == 1 and args.count('nezha-cusb=1') == 1
    args = [word for word in args if word != 'nezha-cusb=1' and not word.startswith('nezha-cj=')]
    args[args.index('nezha-camera=5')] = 'nezha-camera=6'
    assert not any(word.startswith('sheng-disable-jobs=') for word in args)
    args.append('sheng-disable-jobs=com.apple.CommCenter')
    line = ' '.join(args)
    assert len(line.encode()) <= 1000
    for word in ('sheng-data-persist=2', 'sheng-data-reset=0', 'native-ufs-root=1',
                 'nezha-reboot-after=0', 'nezha-panic-reset=0', 'nezha-usb-reboot=1'):
        assert args.count(word) == 1
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', UNIXNU_XNU_ARCH='arm64e',
               UNIXNU_IOS_VERSION='27.0', PYTHONPATH=str(a.prelink_tools))
    with (a.out / 'seal.log').open('w') as log:
        subprocess.run([sys.executable, str(a.framework / 'scripts/seal-source-crypto-kernel.py'),
                        str(stage / KERNEL), '--kernel-args', line], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    kernel = (stage / KERNEL).read_bytes()
    assert sha(kernel) == package['kernel_sha256'], 'mode change must retain existing kernel bytes'
    seal = load(stage / (KERNEL + '.crypto-hmac.json'))
    start, length, field = int(seal['code_file_offset'], 16), seal['code_bytes'], int(seal['hmac_file_offset'], 16)
    assert hmac.digest(b'\0', kernel[start:start + length], 'sha256') == kernel[field:field + 32]

    old_menu = (prior / 'stage' / MENU).read_text()
    menu = tomllib.loads(old_menu)
    assert [e['id'] for e in menu['entry']] == ['env17', 'env27', 'android', 'off']
    assert menu['entry'][1]['args'] == package['kernel_args']
    new_menu = old_menu.replace('args = ' + json.dumps(menu['entry'][1]['args']),
                                'args = ' + json.dumps(line), 1)
    title = 'iOS 27 - main camera system provider (' + a.candidate + ')'
    new_menu = new_menu.replace('title = ' + json.dumps(menu['entry'][1]['title']),
                               'title = ' + json.dumps(title), 1)
    expected = dict(menu)
    expected['entry'] = [dict(e) for e in menu['entry']]
    expected['entry'][1].update(args=line, title=title)
    assert tomllib.loads(new_menu) == expected
    (stage / MENU).write_text(new_menu)

    boot = (prior / 'boot.img').read_bytes()
    assert sha(boot) == package['boot_sha256']
    tag, fd, offset, size = struct.unpack_from('<8sQQQ', boot, 4096 + 0x40)
    assert tag == b'NZSHIM01' and fd < offset and offset + size <= len(boot) - 4096
    start, end = 4096 + offset, 4096 + offset + size
    payload = a.out / 'payload.img'
    payload.write_bytes(boot[start:end])
    changed = {MENU, KERNEL + '.crypto-hmac.json', KERNEL + '.gpuaddon', KERNEL + '.trustcache'}
    for name in changed:
        subprocess.run(['mcopy', '-o', '-i', str(payload), str(stage / name), '::/' + name], check=True)
    members = {}
    for name, digest in package['members'].items():
        actual = subprocess.check_output(['mtype', '-i', str(payload), '::/' + name])
        assert actual == (stage / name).read_bytes(), name
        members[name] = sha(actual)
        if name not in changed:
            assert members[name] == digest, name
    data = payload.read_bytes()
    assert len(data) == size
    complete = boot[:start] + data + boot[end:]
    (a.out / 'boot.img').write_bytes(complete)
    report = dict(package, candidate=a.candidate, boot_sha256=sha(complete),
                  payload_sha256=sha(data), members=members, kernel_args=line,
                  camera_probe_mode=6, camera_journal_token=None, camera_usb_gated=False,
                  camera_mode='native main camera system provider',
                  cameracaptured_launch_policy_enabled=True,
                  library_addon_sha256=addon['signed_library']['sha256'],
                  library_native_run=addon['native_run'], library_native_head=addon['native_head'],
                  library_source_revision=addon['source_revision'],
                  camera_model_sha256=addon.get('camera_model_sha256'),
                  original_Addon_files_preserved=len(old_addon), original_trust_modules_preserved=3,
                  original_17_menu_preserved=True, persistent_Data_reinitialized=False,
                  physical_frames_verified=False, system_camera_registered=False,
                  main_package_promoted=False, phone_operations=False)
    (a.out / 'computer-package.json').write_text(json.dumps(report, indent=2) + '\n')
    print('System camera candidate prepared', a.candidate, sha(complete), 'args', len(line.encode()))


if __name__ == '__main__':
    main()
