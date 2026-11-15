#!/usr/bin/env python3
"""Build and sign just the source camera provider in the established CMS flow."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UNITS = ['NezhaCameraPreview.c', 'NezhaCameraPixelBuffer.c', 'NezhaCameraGraphOutput.c',
         'NezhaFigCapture24A446.c', 'NezhaCameraFigProvider.c', 'NezhaCameraFormats.c',
         'NezhaCameraFactory.c', 'NezhaCameraDeviceBackend.c', 'NezhaCameraSystemVendor.m',
         'upstream/hakios/camera_isp.c']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sealed', type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((ROOT / 'inputs.json').read_text())
    for path, digest in manifest['files'].items():
        assert sha(ROOT / path) == digest, path
    for path, key in [('ci/camera-userland.py', 'builder_sha256'),
                      ('ci/artifacts.py', 'artifacts_sha256'),
                      ('ci/artifact-recipient.pem', 'certificate_sha256')]:
        assert sha(ROOT / path) == manifest[key], path
    shutil.copyfile(ROOT / 'inputs.json', a.out / 'inputs.json')
    source = ROOT / 'source/device/package/camera/userland'
    output = a.out / 'camera-userland'
    output.mkdir()
    logs = a.out / 'logs'
    logs.mkdir()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    flags = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios27.0',
             '-isysroot', sdk, '-std=gnu11', '-O2', '-fblocks', '-fPIC',
             '-Wall', '-Wextra', '-Werror', '-I', str(source)]
    calls, objects, errors = [], [], []
    outcome = 'failure'

    def run(command, name):
        command = [str(x) for x in command]
        calls.append(command)
        with (logs / (name + '.log')).open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        return result.returncode

    try:
        for unit in UNITS:
            obj = output / (Path(unit).stem + '.o')
            options = ['-Wno-unused-parameter'] if unit.startswith('upstream/') else []
            if run([*flags, *options, '-c', source / unit, '-o', obj], Path(unit).stem):
                errors.append(unit)
            else:
                objects.append(obj)
        if errors:
            raise ValueError('Target units failed: ' + ', '.join(errors))
        library = output / 'libNezhaCamera27.dylib'
        if run([*flags, '-dynamiclib', '-install_name', '@rpath/libNezhaCamera27.dylib',
                *objects, '-framework', 'CoreFoundation', '-framework', 'CoreMedia',
                '-framework', 'CoreVideo', '-lobjc', '-o', library], 'link'):
            raise ValueError('Target dylib link failed')
        if run([*flags, '-S', source / 'NezhaCameraFigProvider.c', '-o', output / 'FigProvider.s'], 'PAC'):
            raise ValueError('Provider PAC assembly failed')
        unsigned = sha(library)
        if run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
                '--identifier', 'org.unixnu.nezha.camera27', library], 'sign'):
            raise ValueError('Owner library signing failed')
        if run(['codesign', '--verify', '--strict', library], 'verify'):
            raise ValueError('Owner library signature verification failed')
        detail = subprocess.run(['codesign', '--display', '--verbose=4', library],
                                capture_output=True, text=True, check=True).stderr
        assert 'Page size=4096' in detail
        (output / 'signature.txt').write_text(detail)
        (output / 'sdk-qualified.json').write_text(json.dumps({
            'target': 'arm64e-apple-ios27.0', 'sdk': sdk, 'actual_CF_CM_CV_headers': True,
            'library_linked': True, 'library_sha256': sha(library),
            'unsigned_library_sha256': unsigned, 'objects': {p.name: sha(p) for p in objects},
            'owner_4K_signed': True, 'Apple_codesign_strict_passed': True,
            'system_camera_registered': False, 'physical_frames_verified': False,
            'C2_library_loaded': False,
        }, indent=2) + '\n')
        if run([sys.executable, ROOT / 'source/device/scripts/make-nezha27-camera-model.py',
                '--out', a.out / 'NezhaCameraModel.plist'], 'model'):
            raise ValueError('Measured sensor model creation failed')
        outcome = 'success'
    except Exception as error:
        errors.append(str(error))
    finally:
        (a.out / 'build-result.json').write_text(json.dumps({
            'conclusion': outcome, 'errors': errors, 'commands': calls,
            'source_revision': manifest['device_revision'], 'kernel_rebuilt': False,
            'phone_operations': False,
        }, indent=2) + '\n')
        spec = importlib.util.spec_from_file_location('camera_artifacts', ROOT / 'ci/artifacts.py')
        artifacts = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(artifacts)
        names = sorted(path.relative_to(a.out).as_posix() for path in a.out.rglob('*') if path.is_file())
        artifacts.seal(a.out, names, ROOT / 'ci/artifact-recipient.pem', a.sealed)
    print('Camera userland', outcome, 'objects', len(objects))
    if outcome != 'success':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
