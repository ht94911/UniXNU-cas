#!/usr/bin/env python3
"""Bind SoftwareAES to the measured iPhone family using the existing HID recipe.

The generic component build must run first. Refuse an unaudited OS, any vtable
or PAC mismatch, and every unresolved private import. Ordinary ARM64 recorded
addresses are never consulted. Original method adapters use the prelinker's
existing .original relocation contract.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import shlex
import shutil
import struct
import subprocess

from kernel_target import ARCH, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/software-aes'
SOURCE = ROOT / 'drivers/SoftwareAES'

def require(value, reason):
    if not value:
        raise SystemExit(reason)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command):
    return subprocess.check_output([str(v) for v in command], text=True, timeout=300).strip()

def object_info(data):
    magic, cpu, subtype, kind, count = struct.unpack_from('<5I', data)
    require((magic, cpu, subtype & 255, kind) == (0xfeedfacf, 0x100000c, 2, 1), 'Expected arm64e Mach-O object')
    sections, symbols, offset, symtab = [], [], 32, None
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            nsects, = struct.unpack_from('<I', data, offset + 64)
            for i in range(nsects):
                at = offset + 72 + i * 80
                va, size, fileoff = struct.unpack_from('<QQI', data, at + 32)
                ro, rc = struct.unpack_from('<II', data, at + 56)
                sections.append((va, size, fileoff, ro, rc))
        elif command == 2:
            symtab = struct.unpack_from('<4I', data, offset + 8)
        offset += length
    require(symtab is not None, 'Object has no symbol table')
    sy, count, strings, _ = symtab
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, sy + i * 16)
        name = data[strings + ix:data.index(b'\0', strings + ix)].decode()
        symbols.append((name, va, kind))
    return sections, symbols

def vtable(data, name, count):
    sections, symbols = object_info(data)
    candidates = [v for n, v, k in symbols if n == name and k & 0xe == 0xe]
    require(len(candidates) == 1, 'Expected one concrete NativeAES virtual table')
    table, entries = candidates[0], {}
    for va, size, fileoff, ro, rc in sections:
        if not va <= table < va + size:
            continue
        for i in range(rc):
            offset, bits = struct.unpack_from('<II', data, ro + i * 8)
            if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                entries[va + offset] = (symbols[bits & 0xffffff][0],
                                       struct.unpack_from('<Q', data, fileoff + offset)[0])
    require(table + 16 + count * 8 not in entries, 'Unexpected extra NativeAES virtual method')
    return [entries.get(table + 16 + i * 8) for i in range(count)]

require(ARCH == 'arm64e' and TRIPLE == 'arm64e-apple-ios27.0', 'Select the qualified iPhone27 source profile')
abi_path = Path(__file__).with_name('aes-iphone-24A446.json')
abi = json.loads(abi_path.read_text())
require(abi['kernelcache_sha256'] == 'd99b7719ad4b367db264a7682806aecaf6e0621c6761d07325a58337319d4238' and
        abi['virtual_entries'] == 181 and abi['instance_size'] == 0x120 and
        abi['parent_instance_size'] == 0x88, 'Unqualified original AES fixture')
raw = (OUT / 'NativeAES.o').read_bytes()
check_object(raw, 'SoftwareAES component')
entries = vtable(raw, '__ZTV9NativeAES', 181)
require(all(entries), 'Missing authenticated NativeAES virtual slot')
overrides = {0, 1, 7, 86, 111, 117, 168, 169, 170, 171, 172}
for i, (name, word) in enumerate(entries):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and
            word >> 32 & 65535 == int(abi['slots'][i][1], 16), f'NativeAES slot {i} PAC differs from original')
    expected = abi['slot_names'][i]
    if i in overrides:
        expected = expected.replace('16IOAESAccelerator', '9NativeAES')
    require(name == expected, f'NativeAES slot {i} symbol differs: {name} != {expected}')

spec = importlib.util.spec_from_file_location('aes_source_compile', ROOT / 'scripts/build-sheng-source-drivers.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(record_path.read_text().strip().rstrip(','))
source = ROOT / 'kits/nezha/software-aes/NativeAESBootstrap.cpp'
obj = OUT / 'NativeAESBootstrap.o'
command = helper.compile_command(record, source, obj, ['-I', str(SOURCE)],
                                 ['-I', str(ROOT / '.cache/xnu-work/osfmk')])
(OUT / 'bootstrap-command.json').write_text(json.dumps(command, indent=2) + '\n')
compiled = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
(OUT / 'bootstrap.log').write_text(compiled.stdout + compiled.stderr)
require(not compiled.returncode, compiled.stdout + compiled.stderr)
check_object(obj.read_bytes(), 'AES original-parent bootstrap')
headers = {}
for name in shlex.split(obj.with_suffix('.d').read_text().replace('\\\n', '').split(':', 1)[1]):
    path = Path(name)
    if not path.is_absolute():
        path = Path(record['directory']) / path
    headers[str(path.resolve())] = sha(path)
(OUT / 'bootstrap-headers.json').write_text(json.dumps(headers, indent=2) + '\n')

private = set()
for data in (raw, obj.read_bytes()):
    _, symbols = object_info(data)
    private.update(n for n, _, k in symbols if k == 1 and
                   ('16IOAESAccelerator' in n or '19IOCryptoAccelerator' in n))
require(private and private <= abi['bindings'].keys(), 'Unqualified original AES import: ' +
        str(sorted(private - abi['bindings'].keys())))
bindings = {n: int(abi['bindings'][n], 16) for n in sorted(private)}
text, addresses = '.text\n.p2align 2\n', ''
for name, address in bindings.items():
    if name.endswith('10gMetaClassE'):
        addresses += f'.globl {name}\n.set {name}, {address:#x}\n'
    else:
        addresses += f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
        text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
for stem, contents in (('aes-bindings', text), ('aes-addresses', addresses)):
    path = OUT / (stem + '.S')
    path.write_text(contents)
    run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', path,
         '-o', OUT / (stem + '.o')])
llvm = Path(run(['brew', '--prefix', 'llvm'])) / 'bin'
late = OUT / 'NativeAES.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeAES.cpp', OUT / 'NativeAES.o', late])
archive = OUT / 'libSoftwareAES.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, obj, OUT / 'aes-bindings.o', OUT / 'aes-addresses.o'])
report = dict(component='SoftwareAES', target=TRIPLE, fixture_sha256=sha(abi_path),
              kernelcache_sha256=abi['kernelcache_sha256'], compiled_PAC_slots=181,
              compiled_symbols_match=True, original_bindings='fixed-collection-addresses',
              bindings={n: hex(v) for n, v in bindings.items()}, late_no_mod_init=True,
              source_sha256={n: sha(SOURCE / n) for n in ('NativeAES.cpp', 'OriginalAESABI.h')},
              bootstrap_sha256=sha(source), native_object_sha256=hashlib.sha256(raw).hexdigest(),
              bootstrap_object_sha256=sha(obj), archive_sha256=sha(archive),
              software_keys_only=True, runtime_KAT_required=True, hardware_qualified=False)
(OUT / 'iphone-build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: SoftwareAES 181 actual iPhone PAC/symbol slots; original C2/D2 adapters; post-IOBSD parent/bootstrap, runtime KAT pending')
