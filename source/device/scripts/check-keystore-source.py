#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the selected public KeyStore source and its existing normal/late chain."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_inputs(source, probe, lock_path):
    lock = json.loads(Path(lock_path).read_text())
    actual = {name: sha(Path(source) / name) for name in lock['source_files']}
    expected = {name: item['sha256'] for name, item in lock['source_files'].items()}
    require(actual == expected, 'Actual selected AppleKeyStoreState five-file source differs')
    require(sha(probe) == lock['device_callback_source_sha256'], 'Actual KeyStoreProbe source differs')
    return dict(component_revision=lock['source_revision'], source=str(Path(source).resolve()),
                source_files=actual, source_sha256=actual['NativeKeyStoreEvents.cpp'],
                callback_source_sha256=sha(probe), source_lock_sha256=sha(lock_path))


def check_command(tree):
    directory = Path(tree) / 'build/apple-key-store-state'
    command = json.loads((directory / 'compile-command.json').read_text())
    source = Path(tree) / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
    inputs = [value for value in command if value.endswith('/NativeKeyStoreEvents.cpp')]
    require(len(inputs) == 1 and Path(inputs[0]).resolve() == source.resolve(),
            'Normal KeyStore command did not select the resolved source')
    require(command.count('-o') == 1 and
            Path(command[command.index('-o') + 1]).resolve() ==
            (directory / 'NativeKeyStoreEvents.o').resolve(), 'Normal KeyStore command output differs')
    return sha(directory / 'compile-command.json')


def sections(data):
    # Same section comparison used by the existing Bluetooth native qualifier.
    result, offset = {}, 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', data, offset + 64)[0]):
                at = offset + 72 + index * 80
                name, segment, _, size, begin = struct.unpack_from('<16s16sQQI', data, at)
                flags = struct.unpack_from('<I', data, at + 64)[0]
                key = (segment.rstrip(b'\0').decode(), name.rstrip(b'\0').decode())
                raw = None if flags & 255 in (1, 12, 18) else data[begin:begin + size]
                result[key] = (size, raw)
        offset += length
    return result


def symbols(llvm, path, mode):
    text = subprocess.check_output([str(Path(llvm) / 'llvm-nm'), '--no-demangle', '-g', mode, str(path)], text=True)
    result = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[-2]) == 1:
            result.append((parts[-2], parts[-1]))
    return result


def check_objects(tree, llvm, inputs, kernel=None):
    from kernel_target import check_object
    tree, llvm = Path(tree), Path(llvm)
    normal_dir, legacy = tree / 'build/apple-key-store-state', tree / 'build/native-legacy-display'
    normal_path, late_path = [normal_dir / name for name in ('NativeKeyStoreEvents.o', 'NativeKeyStoreEvents.late.o')]
    probe = legacy / 'KeyStoreProbe.o'
    normal, late = normal_path.read_bytes(), late_path.read_bytes()
    for path in (normal_path, late_path, probe):
        check_object(path.read_bytes(), path.name)
    normal_sections, late_sections = sections(normal), sections(late)
    require(('__DATA', '__mod_init_func') in normal_sections, 'Normal KeyStore initializer section missing')
    require(('__DATA', '__mod_init_func') not in late_sections, 'Late KeyStore initializer section was retained')
    normal_sections.pop(('__DATA', '__mod_init_func'))
    require(normal_sections == late_sections, 'Normal/late KeyStore code or data differs')
    getter, init = '_native_aks_device_id', '__GLOBAL__sub_I_NativeKeyStoreEvents.cpp'
    for path in (normal_path, late_path):
        require(getter in {name for _, name in symbols(llvm, path, '--undefined-only')},
                'Actual KeyStore object lacks the required C ABI getter')
    require(any(kind.isupper() and name == init for kind, name in symbols(llvm, late_path, '--defined-only')),
            'Explicit late KeyStore initializer missing')
    getter_defs = [kind for kind, name in symbols(llvm, probe, '--defined-only') if name == getter]
    require(len(getter_defs) == 1 and getter_defs[0].isupper() and getter_defs[0] not in ('W', 'V'),
            'KeyStoreProbe must provide one strong C ABI getter')
    require(init in {name for _, name in symbols(llvm, probe, '--undefined-only')},
            'KeyStoreProbe explicit initializer reference missing')
    normal_archive, late_archive = normal_dir / 'libAppleKeyStoreState.a', legacy / 'libNativeLegacyDisplay.late.a'
    for archive, path in ((normal_archive, normal_path), (late_archive, late_path), (late_archive, probe)):
        members = subprocess.check_output([str(llvm / 'llvm-ar'), 't', str(archive)], text=True).splitlines()
        require(members.count(path.name) == 1, 'KeyStore archive member count differs: ' + path.name)
        actual = subprocess.check_output([str(llvm / 'llvm-ar'), 'p', str(archive), path.name])
        require(actual == path.read_bytes(), 'KeyStore archive member differs: ' + path.name)
    result = dict(inputs, compile_command_sha256=check_command(tree),
                  normal_archive_sha256=sha(normal_archive), late_archive_sha256=sha(late_archive),
                  normal_object_sha256=sha(normal_path), late_object_sha256=sha(late_path),
                  getter_object_sha256=sha(probe), normal_late_code_and_data_identical=True,
                  late_initializer_explicit=True, identity_getter_defined_once_in_probe=True,
                  actual_archive_members_verified=True, target_objects=3, hardware_verified=False)
    if kernel:
        definitions = [kind for kind, name in symbols(llvm, kernel, '--defined-only') if name == getter]
        require(len(definitions) == 1 and definitions[0].isupper() and definitions[0] not in ('W', 'V'),
                'Native kernel must provide one strong C ABI getter')
        require(not symbols(llvm, kernel, '--undefined-only'), 'Native kernel has unresolved imports')
        result.update(identity_getter_resolved_in_native=True, native_kernel_sha256=sha(kernel))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--llvm', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = check_inputs(args.source, args.probe, args.lock)
    if args.tree:
        if args.llvm is None:
            parser.error('--tree needs --llvm for the existing target/archive checks')
        result = check_objects(args.tree, args.llvm, result, args.kernel)
    result.update(passed=True, phone_operations=False, target_compiled=bool(args.tree), keyboard_pairing_fixed=False)
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
