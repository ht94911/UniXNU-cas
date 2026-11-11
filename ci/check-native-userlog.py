#!/usr/bin/env python3
"""Qualify the overlaid UserLog object, archive and XNU12377 startup records."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OID = '_sysctl__debug_qcom_usb_userlog'
SUBMIT = '_qcom_usb_userlog_submit'
HANDLER = '_qcom_usb_userlog_sysctl'
STARTUP = '___startup_SYSCTL_entry_sysctl__debug__qcom_usb_userlog'
REGISTER = '_sysctl_register_oid_early'
OID_KIND = 0x50e00005  # OPAQUE | WR | LOCKED | ANYBODY | OID2 | PERMANENT


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_source(component):
    expected = json.loads((ROOT / 'inputs.json').read_text())['userlog_target']['source_files']
    actual = {name: sha(Path(component) / name) for name in expected}
    require(actual == expected, 'Actual UserLog overlay source or unchanged wire differs')
    return actual


def macho(path):
    data = Path(path).read_bytes()
    require(struct.unpack_from('<I', data)[0] == 0xfeedfacf, 'Expected thin 64-bit Mach-O')
    sections, segments, table, chained = [], [], None, None
    at = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, length = struct.unpack_from('<II', data, at)
        if command == 0x19:
            name = data[at + 8:at + 24].rstrip(b'\0').decode()
            address, size, offset, file_size = struct.unpack_from('<4Q', data, at + 24)
            segments.append(dict(name=name, address=address, size=size, offset=offset, file_size=file_size))
            for index in range(struct.unpack_from('<I', data, at + 64)[0]):
                item = at + 72 + index * 80
                name, segment, address, size, offset, _, relocations, count = struct.unpack_from('<16s16sQQIIII', data, item)
                sections.append(dict(name=name.rstrip(b'\0').decode(), segment=segment.rstrip(b'\0').decode(),
                                     address=address, size=size, offset=offset, relocations=relocations, count=count))
        elif command == 2:
            table = struct.unpack_from('<4I', data, at + 8)
        elif command == 0x80000034:
            chained = struct.unpack_from('<II', data, at + 8)[0]
        at += length
    require(table is not None, 'Actual target must retain its native symbol table')
    symbol_at, count, strings, _ = table
    symbols = []
    for index in range(count):
        name_at, kind, section, description, value = struct.unpack_from('<IBBHQ', data, symbol_at + index * 16)
        name = data[strings + name_at:data.index(b'\0', strings + name_at)].decode()
        symbols.append(dict(name=name, kind=kind, section=section, description=description, value=value))
    formats = {}
    if chained is not None:
        _, starts_offset, _, _, imports, _, _ = struct.unpack_from('<7I', data, chained)
        require(imports == 0, 'Native target has chained imports')
        starts = chained + starts_offset
        count = struct.unpack_from('<I', data, starts)[0]
        require(count == len(segments), 'Native chained segment count differs')
        for index in range(count):
            offset = struct.unpack_from('<I', data, starts + 4 + index * 4)[0]
            if offset:
                formats[segments[index]['name']] = struct.unpack_from('<H', data, starts + offset + 6)[0]
    bases = [item['address'] for item in segments if item['offset'] == 0 and item['file_size']]
    return dict(data=data, sections=sections, symbols=symbols, formats=formats,
                base=bases[0] if bases else 0)


def definition(image, name, *, exported=False):
    found = [item for item in image['symbols'] if item['name'] == name and not item['kind'] & 0xe0]
    require(len(found) == 1 and found[0]['kind'] & 0x0e == 0x0e and found[0]['section'] and
            not found[0]['description'] & 0x80, 'Actual definition absent, weak or repeated: ' + name)
    require(not exported or found[0]['kind'] & 1, 'Object export is local: ' + name)
    return found[0]


def symbol_data(image, symbol, size):
    section = image['sections'][symbol['section'] - 1]
    delta = symbol['value'] - section['address']
    require(0 <= delta and delta + size <= section['size'], 'Symbol data escaped its actual section')
    return section, delta, image['data'][section['offset'] + delta:section['offset'] + delta + size]


def relocation(image, section, at, target):
    found = []
    for index in range(section['count']):
        address, info = struct.unpack_from('<iI', image['data'], section['relocations'] + index * 8)
        if address == at:
            require(info >> 27 & 1, 'Expected native symbol relocation for ' + target)
            found.append(image['symbols'][info & 0xffffff]['name'])
    require(found == [target], 'Actual initializer relocation differs: ' + target)


def native_pointer(image, section, raw):
    require(image['formats'].get(section['segment']) == 7,
            'Expected actual ARM64E_KERNEL chained startup pointer format')
    require(not raw >> 62 & 1, 'Native startup pointer is an unresolved bind')
    # XNU12377's ARM64E_KERNEL rebases use a 32-bit authenticated target or
    # a 43-bit unauthenticated target, relative to its preferred image base.
    bits = 32 if raw >> 63 else 43
    if bits == 43:
        require(raw >> 43 & 0xff == 0, 'Unexpected high-eight bits in native relative pointer')
    return image['base'] + (raw & ((1 << bits) - 1))


def initialization(obj, kernel):
    entry = definition(obj, STARTUP)
    section, delta, raw = symbol_data(obj, entry, 24)
    require(section['name'] == '__init_entry_set' and section['size'] == 24,
            'UserLog must retain its one original startup linker-set entry')
    require(struct.unpack_from('<II', raw) == (16, 1), 'Actual UserLog startup must be SYSCTL / SECOND')
    relocation(obj, section, delta + 8, REGISTER)
    relocation(obj, section, delta + 16, OID)
    oid = definition(obj, OID, exported=True)
    oid_section, oid_at, oid_raw = symbol_data(obj, oid, 80)
    require(struct.unpack_from('<I', oid_raw, 20)[0] == OID_KIND, 'Object OID permission flags differ')
    relocation(obj, oid_section, oid_at, '_sysctl__debug_children')
    relocation(obj, oid_section, oid_at + 48, HANDLER)
    final = definition(kernel, STARTUP)
    final_section, _, final_raw = symbol_data(kernel, final, 24)
    require(final_section['name'] == '__init_entry_set' and struct.unpack_from('<II', final_raw) == (16, 1),
            'Native kernel dropped or changed the actual UserLog startup entry')
    require(native_pointer(kernel, final_section, struct.unpack_from('<Q', final_raw, 8)[0]) ==
            definition(kernel, REGISTER)['value'], 'Native startup does not invoke sysctl_register_oid_early')
    native_oid = definition(kernel, OID)
    require(native_pointer(kernel, final_section, struct.unpack_from('<Q', final_raw, 16)[0]) == native_oid['value'],
            'Native startup does not register the actual UserLog OID')
    section, _, raw = symbol_data(kernel, native_oid, 80)
    require(struct.unpack_from('<I', raw, 20)[0] == OID_KIND, 'Native OID permission flags differ')
    for at, name in ((0, '_sysctl__debug_children'), (48, HANDLER)):
        require(native_pointer(kernel, section, struct.unpack_from('<Q', raw, at)[0]) == definition(kernel, name)['value'],
                'Native OID pointer differs: ' + name)
    return dict(startup_section=[final_section['segment'], final_section['name']], startup_record_bytes=24,
                startup_subsystem='SYSCTL', startup_rank='SECOND', startup_function=REGISTER,
                startup_argument=OID, object_startup_symbol=STARTUP, native_startup_symbol=STARTUP,
                startup_function_and_argument_relocations=True, native_initializer_pointers_verified=True,
                native_chained_pointer_format=7, native_oid_kind=hex(OID_KIND),
                runtime_registration_verified=False)


def qualify(tree, component, llvm, kernel, artifacts=None):
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    manifest = inputs['userlog_target']
    sources = check_source(component)
    helper = tree / 'scripts/check-keystore-source.py'
    if artifacts and not helper.is_file():
        helper = ROOT / 'source/device/scripts/check-keystore-source.py'
    require(sha(helper) == inputs['files']['source/device/scripts/check-keystore-source.py'],
            'Existing archive/symbol helper differs from the fixed Native014 source')
    spec = importlib.util.spec_from_file_location('existing_native_object_checks', helper)
    existing = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(existing)
    sys.path.insert(0, str(tree / 'scripts'))
    os.environ.update(UNIXNU_XNU_ARCH='arm64e', UNIXNU_IOS_VERSION='27.0')
    from kernel_target import check_object
    directory = artifacts / 'archives/qcom-usb-debug' if artifacts else tree / 'build/qcom-usb-debug'
    report = json.loads((directory / 'build-report.json').read_text())
    require(report['userlog_enabled'] is True, 'Native015 omitted the existing UserLog handler')
    record = artifacts / 'objects/bsd/DEVELOPMENT/subr_prf.o.json' if artifacts else tree / manifest['compile_record']
    original_record = Path(report['userlog_compile_record'])
    original_tree = original_record.parents[len(Path(manifest['compile_record']).parts) - 1]
    require(original_record == original_tree / manifest['compile_record'], 'Original target BSD record path differs')
    require(artifacts is not None or original_tree == tree, 'UserLog compile record belongs to another target tree')
    require(report['userlog_compile_record_sha256'] == sha(record),
            'UserLog did not use this target BSD compile record')
    require(report['component_source_tree'] == manifest['usb_tree'] and
            report['component_base_tree'] == manifest['usb_base_tree'], 'Compiled USB overlay/base tree differs')
    require(all(report['source_sha256'][name] == digest for name, digest in sources.items()
                if Path(name).suffix in ('.c', '.h')), 'Compiled UserLog C/header source differs')
    original_component = Path(report['component_source_directory'])
    require(artifacts is not None or original_component == component, 'UserLog compiled source directory differs')
    commands = [command for command in report['commands'] if str(original_component / 'kernel/QcomUsbUserLog.c') in command]
    require(len(commands) == 1, 'UserLog must have one actual standalone compile command')
    original_directory = Path(report['evidence_directory'])
    original_obj = original_directory / 'QcomUsbUserLog.o'
    require(original_directory.name == 'module' and original_directory.parent.name.startswith('archive-'),
            'Original USB standalone evidence directory differs')
    obj = (artifacts / 'usb-attempts' / original_directory.parent.name / original_obj.name) if artifacts else original_obj
    command = commands[0]
    require(command.count('-o') == 1 and Path(command[command.index('-o') + 1]) == original_obj,
            'UserLog compile command/object differs')
    build_spec = importlib.util.spec_from_file_location('existing_usb_build', component / 'scripts/build.py')
    builder = importlib.util.module_from_spec(build_spec)
    build_spec.loader.exec_module(builder)
    record_data = json.loads(record.read_text().rstrip().rstrip(','))
    require(Path(record_data['file']) == original_tree / '.cache/xnu-work/bsd/kern/subr_prf.c' and
            Path(record_data['directory']) == original_record.parent, 'UserLog record is not this native BSD translation unit')
    flags, _ = builder.compile_flags(record_data, check_paths=artifacts is None)
    require(command[:len(flags)] == flags, 'UserLog command did not preserve actual BSD compile flags')
    check_object(obj.read_bytes(), 'Native015 QcomUsbUserLog overlay')
    archive = directory / 'libQcomXnuUsbDebug.a'
    require(sha(archive) == report['archive_sha256'], 'Actual USB archive differs from its report')
    membership = existing.archive_member(llvm, archive, obj)
    imports = sorted({name for _, name in existing.symbols(llvm, obj, '--undefined-only')})
    dependencies = ['_cs_get_cdhash', '_PE_parse_boot_arg_str', '_proc_pid', REGISTER, '_sysctl__debug_children']
    require(all(name in imports for name in dependencies), 'Actual native UserLog dependency is missing')
    object_image, kernel_image = macho(obj), macho(kernel)
    for name in (SUBMIT, OID):
        definition(object_image, name, exported=True)
        definition(kernel_image, name)
    for name in dependencies:
        definition(kernel_image, name)
    startup = initialization(object_image, kernel_image)
    require(not existing.symbols(llvm, kernel, '--undefined-only'), 'Native015 kernel has unresolved imports')
    return dict(passed=True, target_compiled=True, hardware_verified=False, user_to_USB_verified=False,
                phone_operations=False, source_files=sources, BSD_compile_record_sha256=sha(record),
                standalone_compile_command=command, object_sha256=sha(obj), archive_sha256=sha(archive),
                archive_member=membership, additional_exports=[SUBMIT, OID], object_imports=imports,
                native_cdhash_api='_cs_get_cdhash', native_cdhash_definition_strong=True,
                original_evidence_directory=str(original_directory), original_compile_record=str(original_record),
                actual_object_path=str(obj), artifact_mode=artifacts is not None,
                source_overlay_sha256=sha(ROOT / manifest['source_overlay']),
                kernel_sha256=sha(kernel), checker_sha256=sha(__file__), **startup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--component', type=Path, required=True)
    parser.add_argument('--llvm', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, help='read collected native artifacts while preserving original paths/commands')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = qualify(args.tree.resolve(), args.component.resolve(), args.llvm.resolve(), args.kernel.resolve(),
                     args.artifacts.resolve() if args.artifacts else None)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
