#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Observe the real DAS startup dependency of the system camera trial.

Reuse BootProcessProbe's existing focus3 and the original launch job. Only its
stdout/stderr destinations and the diagnostic focus change. No service is
disabled and no original executable, camera library or storage policy changes.
"""
import argparse
import hashlib
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
DAS_KEY = '/System/Library/LaunchDaemons/com.apple.dasd.plist'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def redirect_job(source, key, outputs, codec):
    """Append the two output fields using the retained four-byte cache codec."""
    original = plistlib.loads(source)
    offset_size, ref_size, count, root, table = struct.unpack('>6xBBQQQ', source[-32:])
    assert source[:8] == b'bplist00' and (offset_size, ref_size) == (4, 4)
    offsets = list(struct.unpack_from('>' + str(count) + 'I', source, table))
    root_keys, root_values, _ = codec.dictionary_refs(source, offsets[root], ref_size)
    jobs_index = root_values[list(original).index('LaunchDaemons')]
    keys, values, values_at = codec.dictionary_refs(source, offsets[jobs_index], ref_size)
    position = list(original['LaunchDaemons']).index(key)
    job_index = values[position]
    field_keys, field_values, _ = codec.dictionary_refs(source, offsets[job_index], ref_size)
    job = original['LaunchDaemons'][key]
    assert len(keys) == len(original['LaunchDaemons']) and len(field_keys) == len(job)
    assert not (outputs.keys() & job.keys())
    body = bytearray(source[:table])

    def append_string(text):
        raw = text.encode('ascii')
        index = len(offsets)
        offsets.append(len(body))
        body.extend(codec.counted(0x50, len(raw)) + raw)
        return index

    extra_keys = [append_string(k) for k in outputs]
    extra_values = [append_string(v) for v in outputs.values()]
    new_job = len(offsets)
    offsets.append(len(body))
    body.extend(codec.counted(0xd0, len(field_keys) + len(outputs)))
    body.extend(b''.join(struct.pack('>I', i) for i in
                        field_keys + extra_keys + field_values + extra_values))
    slot = values_at + (len(keys) + position) * ref_size
    body[slot:slot + ref_size] = struct.pack('>I', new_job)
    new_table = len(body)
    body.extend(b''.join(struct.pack('>I', i) for i in offsets))
    body.extend(struct.pack('>6xBBQQQ', 4, 4, len(offsets), root, new_table))
    result = bytes(body)
    expected = plistlib.loads(source)
    expected['LaunchDaemons'][key].update(outputs)
    assert plistlib.loads(result) == expected
    assert result[:slot] == source[:slot]
    assert result[slot + ref_size:table] == source[slot + ref_size:table]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--gpu-tools', type=Path, required=True)
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--prelink-tools', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    before = json.loads((args.source / 'ready.json').read_text())
    assert before['candidate'] == 'camera053-ui' and before['computer_qualified']
    assert before['boot_sha256'] == 'dd5ee8ecc14717a461f08257b0868315ec1f3b250db7d7b93eb1d77a2f61638f'
    assert before['camera_probe_mode'] == 6 and before['no_phone_payload_write']
    readers = module(args.gpu_tools / 'gpu007_inputs.py', 'das_package_readers')
    codec_path = ROOT / 'package/camera/upstream/service-cache/cellular_runtime.py'
    origin = json.loads((codec_path.parent / 'ORIGIN.json').read_text())
    assert origin['unmodified'] and sha(codec_path.read_bytes()) == origin['sha256']
    codec = module(codec_path, 'das_cache_codec')
    boot = (args.source / 'boot.img').read_bytes()
    assert sha(boot) == before['boot_sha256']
    tag, _, offset, size = struct.unpack_from('<8sQQQ', boot, 4096 + 0x40)
    assert tag == b'NZSHIM01' and 4096 + offset + size <= len(boot)
    start, end = 4096 + offset, 4096 + offset + size
    args.out.mkdir(parents=True, exist_ok=False)
    payload = args.out / 'payload.img'
    payload.write_bytes(boot[start:end])
    stage = args.out / 'stage'
    for name, digest in before['members'].items():
        raw = subprocess.check_output(['mtype', '-i', payload, '::/' + name], timeout=20)
        assert sha(raw) == digest, name
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    addon = stage / (KERNEL + '.gpuaddon')
    old_files = readers.addon_files(addon.read_bytes())
    cache = old_files['camera-xpc/launchd.plist'][1]
    old_signature = old_files['camera-xpc/launchd.plist.sig'][1]
    assert codec.verify_signature(cache, old_signature) == before['discovery_cache_cdhash']
    job = plistlib.loads(cache)['LaunchDaemons'][DAS_KEY]
    assert job['Program'] == '/usr/libexec/dasd' and job['Label'] == 'com.apple.dasd'
    assert job['MachServices']['com.apple.duetactivityscheduler'] is True
    outputs = {k: '/private/var/mobile/Library/Logs/nezha-dasd-054.' + suffix for k, suffix in
               [('StandardOutPath', 'stdout'), ('StandardErrorPath', 'stderr')]}
    revised = redirect_job(cache, DAS_KEY, outputs, codec)
    signature, cdhash = codec.detached_signature(revised)
    assert codec.verify_signature(revised, signature) == cdhash
    runtime = args.out / 'runtime/camera-xpc'
    runtime.mkdir(parents=True)
    (runtime / 'launchd.plist').write_bytes(revised)
    (runtime / 'launchd.plist.sig').write_bytes(signature)
    changed_addon = args.out / 'addon-with-das.bin'
    subprocess.run([sys.executable, args.framework / 'scripts/addon-add-dir.py', addon,
                    changed_addon, 'camera-xpc', runtime], check=True, timeout=30)
    addon.write_bytes(changed_addon.read_bytes())
    expected_files = dict(old_files)
    expected_files.update({'camera-xpc/launchd.plist': (0o644, revised),
                           'camera-xpc/launchd.plist.sig': (0o644, signature)})
    assert readers.addon_files(addon.read_bytes()) == expected_files
    trust = stage / (KERNEL + '.trustcache')
    old_modules = readers.trust_modules(trust.read_bytes())
    changed_trust = args.out / 'trust-with-das.bin'
    subprocess.run([sys.executable, args.framework / 'scripts/trust-append-module.py',
                    trust, changed_trust, cdhash], check=True, timeout=20)
    trust.write_bytes(changed_trust.read_bytes())
    assert readers.trust_modules(trust.read_bytes())[:-1] == old_modules
    line = before['kernel_args'].replace('boot-ui-follow=1', 'boot-ui-follow=0') + ' boot-ui-focus3=dasd'
    assert len(line.encode()) <= 1000 and line.count('boot-ui-focus3=dasd') == 1
    assert readers.decode_args(line) == readers.decode_args(before['kernel_args']) | {
        'boot-ui-follow': '0', 'boot-ui-focus3': 'dasd'}
    menu = (stage / MENU).read_text()
    decoded = tomllib.loads(menu)
    expected_menu = tomllib.loads(menu)
    expected_menu['entry'][1].update(args=line, title='iOS 27 - camera DAS startup (camera054-das)')
    menu = menu.replace('args = ' + json.dumps(decoded['entry'][1]['args']), 'args = ' + json.dumps(line), 1)
    menu = menu.replace('title = ' + json.dumps(decoded['entry'][1]['title']),
                        'title = ' + json.dumps(expected_menu['entry'][1]['title']), 1)
    assert tomllib.loads(menu) == expected_menu
    (stage / MENU).write_text(menu)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', UNIXNU_XNU_ARCH='arm64e',
               UNIXNU_IOS_VERSION='27.0', PYTHONPATH=str(args.prelink_tools))
    subprocess.run([sys.executable, args.framework / 'scripts/seal-source-crypto-kernel.py',
                    stage / KERNEL, '--kernel-args', line], env=env, check=True, timeout=25)
    assert sha((stage / KERNEL).read_bytes()) == before['kernel_sha256']
    changed = {MENU, KERNEL + '.gpuaddon', KERNEL + '.trustcache'}
    members = {}
    for name in changed:
        subprocess.run(['mcopy', '-o', '-i', payload, stage / name, '::/' + name], check=True, timeout=20)
    for name, digest in before['members'].items():
        raw = subprocess.check_output(['mtype', '-i', payload, '::/' + name], timeout=20)
        assert raw == (stage / name).read_bytes()
        members[name] = sha(raw)
        if name not in changed:
            assert members[name] == digest
    raw_payload = payload.read_bytes()
    assert len(raw_payload) == size
    full = boot[:start] + raw_payload + boot[end:]
    (args.out / 'boot.img').write_bytes(full)
    ready = dict(before, candidate='camera054-das', boot_sha256=sha(full), payload_sha256=sha(raw_payload),
        members=members, kernel_args=line, diagnostic_source_revision=subprocess.check_output(
            ['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
        original_system_jobs_preserved_except_camera_and_discovery=False,
        original_system_jobs_preserved_except_camera_discovery_and_DAS_outputs=True,
        DAS_daemon_redirected_paths=outputs, discovery_cache_cdhash=cdhash,
        qemu_result_reused_for_identical_kernel_and_arguments=False,
        qemu_result_reused_for_identical_kernel_and_diagnostic_focus_only=True,
        no_light_required=True, human_light_ready_required=False, human_interaction_required=False,
        eligible_while_unattended=True, system_camera_UI_trial=False, manual_tests_deferred=True,
        desktop_dependency_diagnostic=True, live_preview_verified=False, physical_still_verified=False,
        cache_codec_origin=origin)
    for filename in ('computer-package.json', 'ready.json'):
        (args.out / filename).write_text(json.dumps(ready, indent=2) + '\n')
    print('camera054 original DAS focus and two output paths prepared', ready['boot_sha256'], flush=True)


if __name__ == '__main__':
    main()
