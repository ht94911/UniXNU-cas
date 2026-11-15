#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sign the target camera library and extend an existing local addon/trust pair.

Uses the retained rcodesign tool and GPU package readers, then the existing
UniXNU trust appender. No root image, boot policy, phone or network operation.
The native build may have failed after the userland build; only the sealed
target userland outputs are consumed here. A system boot remains separate.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
IDENTIFIER = 'org.unixnu.nezha.camera27'
LIBRARY = 'libNezhaCamera27.dylib'
ADDON_PATH = 'camera-media/' + LIBRARY
CACHE_SHA = '5ee8d115da8a913040ab65fa5f243096ef7e1939b407edd9f7416b71a3aff11e'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(path):
    return json.loads(path.read_text())


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def macho_info(data):
    assert struct.unpack_from('<3I', data)[:2] == (0xfeedfacf, 0x100000c)
    count, command_bytes = struct.unpack_from('<2I', data, 16)
    at, sections, identity, signature = 32, {}, None, None
    for _ in range(count):
        command, size = struct.unpack_from('<2I', data, at)
        assert size >= 8 and at + size <= 32 + command_bytes <= len(data)
        if command == 0x1b:
            assert size == 24 and identity is None
            identity = data[at + 8:at + 24].hex()
        elif command == 0x19:
            nsections = struct.unpack_from('<I', data, at + 64)[0]
            assert size == 72 + nsections * 80
            for i in range(nsections):
                s = at + 72 + i * 80
                section, segment, _, length, offset = struct.unpack_from('<16s16sQQI', data, s)
                flags = struct.unpack_from('<I', data, s + 64)[0]
                if flags & 0xff in (1, 0x0c, 0x12):  # zero-fill has no file contents
                    continue
                assert offset + length <= len(data)
                key = segment.rstrip(b'\0').decode() + '/' + section.rstrip(b'\0').decode()
                assert key not in sections
                sections[key] = sha(data[offset:offset + length])
        elif command == 0x1d:
            assert size == 16 and signature is None
            start, length = struct.unpack_from('<2I', data, at + 8)
            assert start + length <= len(data)
            signature = data[start:start + length]
        at += size
    assert at == 32 + command_bytes and identity and sections
    result = {'sha256': sha(data), 'bytes': len(data), 'macho': {'uuid': identity},
              'sections': sections}
    if signature:
        magic, length, count = struct.unpack_from('>3I', signature)
        assert magic == 0xfade0cc0 and 12 + count * 8 <= length <= len(signature)
        directories = []
        for i in range(count):
            _, start = struct.unpack_from('>2I', signature, 12 + i * 8)
            magic, size = struct.unpack_from('>2I', signature, start)
            assert 8 <= size <= length - start
            if magic == 0xfade0c02:
                cd = signature[start:start + size]
                ident = struct.unpack_from('>I', cd, 20)[0]
                directories.append({'cdhash': sha(cd)[:40],
                                    'identifier': cd[ident:].split(b'\0', 1)[0].decode()})
        assert len(directories) == 1
        result['signature'] = directories[0]
    return result


def cache_paths(header):
    assert sha(header) == CACHE_SHA and header[:16] == b'dyld_v1  arm64e\0'
    start, count = struct.unpack_from('<2Q', header, 136)
    assert start + count * 32 <= len(header)
    paths = {}
    for i in range(count):
        uuid, _, _, offset = struct.unpack_from('<16sQ2I', header, start + i * 32)
        assert offset < len(header)
        name = header[offset:header.index(b'\0', offset)].decode()
        assert name not in paths
        paths[name] = uuid.hex()
    assert paths['/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture'] == \
        '995fc22b41bd37b4812f2686b81c2694'
    return paths


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-work', type=Path, required=True)
    p.add_argument('--base-package', type=Path, required=True)
    p.add_argument('--gpu-tools', type=Path, required=True, help='retained GPU notes directory')
    p.add_argument('--framework', type=Path, required=True)
    p.add_argument('--rcodesign', type=Path, help='needed only for an unsigned target library')
    p.add_argument('--cache-header', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    dispatch = load(a.native_work / 'ci-dispatch.json')
    result = a.native_work / 'result'
    manifest = load(result / 'manifest.json')
    assert manifest['run']['GITHUB_REPOSITORY'] == 'ht94911/UniXNU-cas'
    assert manifest['run']['GITHUB_SHA'] == dispatch['head']
    assert int(manifest['run']['GITHUB_RUN_ID']) == dispatch['run_id']
    assert int(manifest['run']['GITHUB_RUN_ATTEMPT']) == dispatch['run_attempt']
    records = {f['path']: f for f in manifest['files']}

    def sealed(relative):
        data = (result / relative).read_bytes()
        record = records[relative]
        assert len(data) == record['bytes'] and sha(data) == record['sha256'], relative
        return data

    inputs = json.loads(sealed('inputs.json'))
    assert inputs['device_revision'] == dispatch['device_source']
    sdk = json.loads(sealed('camera-userland/sdk-qualified.json'))
    assert sdk['actual_CF_CM_CV_headers'] and sdk['library_linked']
    assert sdk['target'] == 'arm64e-apple-ios27.0' and len(sdk['objects']) == 10
    for name, digest in sdk['objects'].items():
        assert sha(sealed('camera-userland/' + name)) == digest
    library = sealed('camera-userland/' + LIBRARY)
    assert sha(library) == sdk['library_sha256']
    original = macho_info(library)
    cache = cache_paths(a.cache_header.read_bytes())

    sys.path.insert(0, str(a.gpu_tools.resolve()))
    readers = module(a.gpu_tools / 'gpu007_inputs.py', 'camera_gpu_package_readers')
    base = load(a.base_package)
    stage = a.base_package.parent / 'stage'
    prefix = 'EFI/UniXNU/Payloads/kernel.macho.'
    addon = (stage / (prefix + 'gpuaddon')).read_bytes()
    trust = (stage / (prefix + 'trustcache')).read_bytes()
    assert sha(addon) == base['members'][prefix + 'gpuaddon']
    assert sha(trust) == base['members'][prefix + 'trustcache']
    old_files, old_modules = readers.addon_files(addon), readers.trust_modules(trust)
    assert ADDON_PATH not in old_files and len(old_modules) == 3
    a.out.mkdir(parents=True, exist_ok=False)
    unsigned = a.out / ('unsigned-' + LIBRARY)
    unsigned.write_bytes(library)
    signed = a.out / LIBRARY
    if sdk.get('owner_4K_signed'):
        assert sdk['Apple_codesign_strict_passed'] and 'signature' in original
        signed.write_bytes(library)  # preserve the actual Apple-signed CI bytes
    else:
        assert a.rcodesign and a.rcodesign.is_file()
        # rcodesign verify rejects an empty ad-hoc CMS wrapper in this
        # retained version. The target auditor verifies every signed page.
        with (a.out / 'sign.log').open('w') as log:
            subprocess.run([str(a.rcodesign), 'sign', '-C', '/dev/null', '--digest', 'sha256',
                            '--binary-identifier', IDENTIFIER, str(unsigned), str(signed)],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    signed_info = macho_info(signed.read_bytes())
    assert signed_info['sections'] == original['sections']
    assert signed_info['macho'] == original['macho']
    assert signed_info['signature']['identifier'] == IDENTIFIER
    auditor = module(ROOT / 'scripts/check-adreno840-userland.py', 'camera_signature_auditor')
    qualified = auditor.signature(signed, signed_info)
    assert all(d['weak'] or d['path'] in cache for d in qualified['dependencies'])
    additions = {ADDON_PATH: (0o644, signed.read_bytes())}
    model_sha = None
    if (result / 'NezhaCameraModel.plist').is_file():
        model = sealed('NezhaCameraModel.plist')
        described = plistlib.loads(model)
        assert len(described['AVCaptureDevices']) == 1
        sensor = described['AVCaptureDevices'][0]['MultiStream']['FigCaptureStreamFormats']['Photo']
        assert (sensor['SensorWidth'], sensor['SensorHeight'], sensor['MaxFrameRate']) == (4096, 3072, 30)
        additions['camera-media/NezhaCameraModel.plist'] = (0o644, model)
        (a.out / 'NezhaCameraModel.plist').write_bytes(model)
        model_sha = sha(model)
    entry = b''
    for filename, (mode, content) in additions.items():
        assert filename not in old_files
        name = filename.encode() + b'\0'
        packed = zlib.compress(content, 9)
        entry += struct.pack('<IIQQ', len(name), mode, len(content), len(packed))
        entry += name + bytes(-len(name) % 8) + packed + bytes(-len(packed) % 8)
    count = struct.unpack_from('<I', addon, 8)[0]
    revised = b'NGPUADD2' + struct.pack('<IIQ', count + len(additions), 0, len(addon) + len(entry)) + addon[24:] + entry
    new_files = readers.addon_files(revised)
    assert new_files == old_files | additions
    (a.out / 'kernel.macho.gpuaddon').write_bytes(revised)
    original_trust = a.out / 'original.trustcache'
    original_trust.write_bytes(trust)
    revised_trust = a.out / 'kernel.macho.trustcache'
    cdhash = qualified['code_directories'][0]['cdhash']
    subprocess.run([sys.executable, str(a.framework / 'scripts/trust-append-module.py'),
                    str(original_trust), str(revised_trust), cdhash], check=True, timeout=30)
    modules = readers.trust_modules(revised_trust.read_bytes())
    assert len(modules) == 4 and modules[:-1] == old_modules
    assert modules[-1][24:] == bytes.fromhex(cdhash) + b'\x02\0\0\0'
    report = {'source_revision': dispatch['device_source'], 'native_run': dispatch['run_id'],
              'native_head': dispatch['head'], 'native_conclusion': dispatch['conclusion'],
              'target_SDK': sdk['sdk'], 'objects_verified': len(sdk['objects']),
              'unsigned_library_sha256': sha(library), 'signed_library': qualified,
              'code_and_PAC_sections_preserved': True,
              'signer_sha256': sha(a.rcodesign.read_bytes()) if not sdk.get('owner_4K_signed') else None,
              'Apple_signed_CI_library_preserved': bool(sdk.get('owner_4K_signed')),
              'camera_model_sha256': model_sha, 'cache_header_sha256': CACHE_SHA,
              'addon_sha256': sha(revised), 'addon_original_files_preserved': len(old_files),
              'addon_original_compressed_bytes_preserved': True,
              'trustcache_sha256': sha(revised_trust.read_bytes()),
              'original_trust_modules_preserved': [sha(m) for m in old_modules],
              'camera_cdhash': cdhash, 'library_addon_path': ADDON_PATH,
              'model_addon_path': 'camera-media/NezhaCameraModel.plist' if model_sha else None,
              'phone_operations': False,
              'system_camera_enabled': False, 'system_camera_registered': False,
              'physical_frames_verified': False, 'system_boot_package_required': True}
    (a.out / 'camera-addon.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Camera addon prepared:', len(old_files), 'original files, 3 original trust modules retained;',
          'camera 4K CodeDirectory', cdhash)


if __name__ == '__main__':
    main()
