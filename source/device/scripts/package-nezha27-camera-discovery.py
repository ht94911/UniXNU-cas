#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Add a qualified one-shot enumerator to a mode6 system-camera candidate.

The native input must include the scoped camera cache mounts. Preserve the
existing kernel, root/Data policy, four-entry menu and addon/trust contents.
"""
import argparse
import hashlib
import importlib.util
import json
import plistlib
from pathlib import Path
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


def read(path):
    return json.loads(path.read_text())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--system-candidate', type=Path, required=True)
    p.add_argument('--native-work', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--gpu-tools', type=Path, required=True)
    p.add_argument('--framework', type=Path, required=True)
    p.add_argument('--candidate', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    before = read(a.system_candidate / 'computer-package.json')
    inputs = read(a.native_work / 'result/inputs.json')
    dispatch = read(a.native_work / 'ci-dispatch.json')
    patch = 'patches/xnu-12377/zzzzzz-nezha-camera-discovery-launch.patch'
    assert inputs['files'].get('source/device/' + patch) == sha((ROOT / patch).read_bytes())
    assert dispatch['conclusion'] == 'success' and before['native_head'] == dispatch['head']
    assert before['native_run'] == dispatch['run_id'] and before['camera_probe_mode'] == 6
    assert before['cameracaptured_launch_policy_enabled'] and not before['camera_usb_gated']
    runtime = read(a.runtime / 'camera-discovery.json')
    assert runtime['original_object_bytes_preserved']
    if not runtime['original_jobs_preserved']:
        # A diagnostic descendant records the original camera daemon's
        # stderr without changing its code, arguments or service identity.
        delayed = runtime.get('discovery_job_timing')
        if delayed:
            assert runtime['original_jobs_except_camera_and_discovery_preserved']
            assert delayed == {'RunAtLoad': False, 'StartInterval': 150, 'LaunchOnlyOnce': True}
        else:
            assert runtime['original_jobs_except_camera_outputs_preserved']
        reference = runtime['camera_cache_reference']
        original = Path(reference['path']).read_bytes()
        assert sha(original) == reference['sha256']
        expected = plistlib.loads(original)
        key = '/System/Library/LaunchDaemons/com.apple.cameracaptured.plist'
        outputs = runtime['camera_daemon_redirected_paths']
        assert set(outputs) == {'StandardOutPath', 'StandardErrorPath'}
        for field, path in outputs.items():
            assert field not in expected['LaunchDaemons'][key]
            assert path.startswith('/private/var/mobile/Library/Logs/nezha-camera-daemon-')
        expected['LaunchDaemons'][key].update(outputs)
        if delayed:
            job_key = '/System/Library/LaunchDaemons/org.unixnu.nezha.camera-enumerate.plist'
            expected['LaunchDaemons'][job_key].update(delayed)
            assert expected['LaunchDaemons'][job_key] == runtime['camera_job']
        assert plistlib.loads((a.runtime / 'camera-xpc/launchd.plist').read_bytes()) == expected
    assert not runtime['phone_operations'] and not runtime['system_discovery_executed']
    expected_files = {'camera-xpc/launchd.plist', 'camera-xpc/launchd.plist.sig',
        'camera-bin/nezha-camera-enumerate', 'daemons/org.unixnu.nezha.camera-enumerate.plist'}
    assert set(runtime['files']) == expected_files
    for name, record in runtime['files'].items():
        data = (a.runtime / name).read_bytes()
        assert len(data) == record['bytes'] and sha(data) == record['sha256']
    job = runtime['camera_job']
    assert job['UserName'] == 'mobile' and job['LaunchOnlyOnce'] and not job['KeepAlive']
    spec = importlib.util.spec_from_file_location('discovery_readers', a.gpu_tools / 'gpu007_inputs.py')
    readers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(readers)
    a.out.mkdir(parents=True, exist_ok=False)
    stage = a.out / 'stage'
    for name, digest in before['members'].items():
        source = a.system_candidate / 'stage' / name
        assert sha(source.read_bytes()) == digest, name
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    addon = stage / (KERNEL + '.gpuaddon')
    old_addon = readers.addon_files(addon.read_bytes())
    assert not (expected_files & old_addon.keys())
    changed_addon = a.out / 'addon-with-discovery.bin'
    subprocess.run([sys.executable, a.framework / 'scripts/addon-add-dir.py', addon,
        changed_addon, 'camera-xpc', a.runtime / 'camera-xpc',
        'camera-bin', a.runtime / 'camera-bin', 'daemons', a.runtime / 'daemons'], check=True)
    addon.write_bytes(changed_addon.read_bytes())
    added = {n:(record['mode'],(a.runtime / n).read_bytes()) for n,record in runtime['files'].items()}
    assert readers.addon_files(addon.read_bytes()) == old_addon | added
    trust = stage / (KERNEL + '.trustcache')
    old_trust = readers.trust_modules(trust.read_bytes())
    changed_trust = a.out / 'trust-with-discovery.bin'
    subprocess.run([sys.executable, a.framework / 'scripts/trust-append-module.py', trust,
        changed_trust, runtime['client_cdhash'], runtime['cache_cdhash']], check=True)
    trust.write_bytes(changed_trust.read_bytes())
    modules = readers.trust_modules(trust.read_bytes())
    assert modules[:-1] == old_trust and len(modules) == len(old_trust) + 1
    assert struct.unpack_from('<I', modules[-1], 20)[0] == 2
    assert sha((stage / KERNEL).read_bytes()) == before['kernel_sha256']
    menu = (stage / MENU).read_text()
    decoded = tomllib.loads(menu)
    assert [entry['id'] for entry in decoded['entry']] == ['env17', 'env27', 'android', 'off']
    title = 'iOS 27 - main camera system discovery (' + a.candidate + ')'
    revised = menu.replace('title = ' + json.dumps(decoded['entry'][1]['title']),
        'title = ' + json.dumps(title), 1)
    expected = dict(decoded, entry=[dict(entry) for entry in decoded['entry']])
    expected['entry'][1]['title'] = title
    assert tomllib.loads(revised) == expected
    (stage / MENU).write_text(revised)
    boot = (a.system_candidate / 'boot.img').read_bytes()
    assert sha(boot) == before['boot_sha256']
    tag, fd, offset, size = struct.unpack_from('<8sQQQ', boot, 4096 + 0x40)
    assert tag == b'NZSHIM01' and fd < offset and offset + size <= len(boot) - 4096
    start, end = 4096 + offset, 4096 + offset + size
    payload = a.out / 'payload.img'
    payload.write_bytes(boot[start:end])
    changed = {MENU, KERNEL + '.gpuaddon', KERNEL + '.trustcache'}
    for name in changed:
        subprocess.run(['mcopy', '-o', '-i', payload, stage / name, '::/' + name], check=True)
    members = {}
    for name, digest in before['members'].items():
        data = subprocess.check_output(['mtype', '-i', payload, '::/' + name])
        assert data == (stage / name).read_bytes()
        members[name] = sha(data)
        if name not in changed:
            assert members[name] == digest
    contents = payload.read_bytes()
    assert len(contents) == size
    full = boot[:start] + contents + boot[end:]
    (a.out / 'boot.img').write_bytes(full)
    report = dict(before, candidate=a.candidate, boot_sha256=sha(full), payload_sha256=sha(contents),
        members=members, discovery_client_run=runtime['client_run'],
        discovery_client_cdhash=runtime['client_cdhash'], discovery_cache_cdhash=runtime['cache_cdhash'],
        original_system_jobs_preserved=729, discovery_launch_once=True,
        discovery_system_executed=False, physical_frames_verified=False,
        phone_operations=False, manual_tests_deferred=True)
    if runtime.get('camera_daemon_redirected_paths'):
        report.update(camera_daemon_redirected_paths=runtime['camera_daemon_redirected_paths'],
                      original_system_jobs_preserved_except_camera_outputs=not bool(runtime.get('discovery_job_timing')))
    if runtime.get('discovery_job_timing'):
        report.update(discovery_job_timing=runtime['discovery_job_timing'],
                      original_system_jobs_preserved_except_camera_and_discovery=True)
    (a.out / 'computer-package.json').write_text(json.dumps(report, indent=2) + '\n')
    print('System discovery candidate prepared', a.candidate, sha(full))


if __name__ == '__main__':
    main()
