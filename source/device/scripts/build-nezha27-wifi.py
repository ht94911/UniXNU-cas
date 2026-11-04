#!/usr/bin/env python3
"""Build only source for the selected iPhone27 target; no blobs or packaging.

Uses the framework's actual XNU C/C++ compilation records and header recipe.
The private SMMU symbol namespace avoids mixing its current context layout with
the independently selected GPU version. The network bridge registers late.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tomllib

from kernel_target import ARCH, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/nezha27-wifi'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(argv, **kwargs):
    return subprocess.check_output([str(v) for v in argv], timeout=300, **kwargs)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    if ARCH != 'arm64e' or not TRIPLE.startswith('arm64e-apple-ios27.'):
        raise SystemExit('WCN7860 recipe requires the selected iPhone27 arm64e target')
    OUT.mkdir(parents=True, exist_ok=True)
    helper = load('wifi_source_driver_compile', ROOT / 'scripts/build-sheng-source-drivers.py')
    headers_helper = load('wifi_public_storage_headers', ROOT / 'scripts/prepare-storage-headers.py')
    headers = headers_helper.prepare(ROOT / 'refer/IOStorageFamily',
                                    '7edb88fbae296fb7c8ce2f64e115e116e566d51c', OUT / 'headers')
    include = OUT / 'headers/include'
    sdk = Path(headers['sdk'])
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    override = tomllib.loads((device / 'local.toml').read_text())['source']
    soc = next(v for v in override if v['kind'] == 'soc' and v['name'] == 'sm8850')
    soc = (device / soc['local']).resolve()
    usb = next(v for v in override if v['name'] == 'QcomXnuUsbDebug')
    usb = (device / usb['local']).resolve()
    sources = ROOT / 'kits/nezha/NezhaWcn7860'
    base = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    records = {
        '.c': json.loads((base / 'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(',')),
        '.cpp': json.loads((base / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(',')),
        'platform': json.loads((base / 'osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(',')),
    }
    include_flags = ['-I', str(include), '-idirafter',
                     str(sdk / 'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
    extra = ['-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(sources),
             '-I', str(soc / 'include'), '-I', str(usb / 'include')]
    selected = [*sorted(sources.glob('*.c')), *sorted((sources / 'qcom').glob('*.c')),
                *sorted((sources / 'adreno').glob('*.c')), sources / 'PeachWLAN.cpp', soc / 'Sm8850Rpmh.c']
    objects, details = [], []
    for source in selected:
        # Device mappings/allocations require the real Mach-private interface;
        # VFS and the network frontend retain this target's BSD build contract.
        record = records['platform' if source.name == 'NezhaWifiMemory.c' else source.suffix]
        obj = OUT / (source.stem + '.raw.o')
        command = helper.compile_command(record, source, obj, include_flags, extra)
        # XNU's socket header names an Apple-internal warning group unavailable
        # in this qualified Xcode. Keep Werror and all supported diagnostics.
        command += ['-Wno-unknown-warning-option']
        (OUT / (source.stem + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
        (OUT / (source.stem + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            print(result.stderr)
            details.append({'source': str(source), 'error': result.returncode})
            continue
        check_object(obj.read_bytes(), source.name)
        deps = {}
        for filename in shlex.split(obj.with_suffix('.d').read_text().replace('\\\n', '').split(':', 1)[1]):
            path = Path(filename)
            if not path.is_absolute(): path = Path(record['directory']) / path
            deps[str(path.resolve())] = sha(path)
        objects.append(obj)
        details.append({'source': str(source), 'source_sha256': sha(source),
                        'object': str(obj), 'object_sha256': sha(obj), 'headers': deps})
    (OUT / 'compile-report.json').write_text(json.dumps(details, indent=2) + '\n')
    if len(objects) != len(selected):
        raise SystemExit('Wi-Fi compile failed; all visible compiler errors saved, no archive written')

    llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    names = set()
    for obj in objects:
        for line in run([llvm / 'llvm-nm', '-g', '--defined-only', obj], text=True).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2].startswith('_ArmSmmu4K'):
                names.add(parts[2])
    if not names: raise SystemExit('No real SMMU implementation definitions')
    renames = OUT / 'smmu-namespace.txt'
    renames.write_text(''.join(f'{n} _NezhaWifi{n[1:]}\n' for n in sorted(names)))
    final = []
    for raw in objects:
        obj = raw.with_name(raw.name.replace('.raw.o', '.o'))
        run([llvm / 'llvm-objcopy', '--redefine-syms=' + str(renames), raw, obj])
        if raw.name == 'PeachWLAN.raw.o':
            late = OUT / 'PeachWLAN.late.o'
            run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                 '--globalize-symbol=__GLOBAL__sub_I_PeachWLAN.cpp', obj, late])
            obj = late
            section_text = run([llvm / 'llvm-objdump', '--macho', '--section-headers', obj], text=True)
            if '__mod_init_func' in section_text: raise SystemExit('Unexpected early network metaclass initializer')
        check_object(obj.read_bytes(), obj.name)
        final.append(obj)
    archive = OUT / 'libNezha27Wifi.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, *final])
    (OUT / 'build-report.json').write_text(json.dumps({
        'target': TRIPLE, 'objects': len(final), 'sources': details,
        'private_smmu_symbols': sorted(names), 'late_registry_classes': True,
        'uses_donor_networking_cpp_abi': False, 'no_old_objects': True,
        'archive_sha256': sha(archive), 'firmware_included': False,
        'phone_package': False, 'hardware_verified': False,
    }, indent=2) + '\n')
    print(f'Built {len(final)} real iPhone27 Wi-Fi objects; firmware and hardware verification separate')


if __name__ == '__main__':
    main()
