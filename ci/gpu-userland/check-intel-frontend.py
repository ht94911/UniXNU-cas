#!/usr/bin/env python3
"""Compile on Intel and both iOS targets; compare the actual ARM signal-handler code and relocations.

The rest of the translation unit contains ENCODE(__LINE__). Inserting the
host branch changes those diagnostic line constants, so whole-object identity
is recorded, not claimed. The deployed Metal002 plugin remains unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import struct


def signal_handler(path):
    data = path.read_bytes()
    magic, cpu, subtype, kind, count = struct.unpack_from('<5I', data)
    assert (magic, cpu, kind) == (0xfeedfacf, 0x100000c, 1)
    sections, symtab, at = [], None, 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, at)
        assert size >= 8 and at + size <= len(data)
        if command == 0x19:
            for k in range(struct.unpack_from('<I', data, at + 64)[0]):
                p = at + 72 + k * 80
                name = data[p:p + 16].rstrip(b'\0').decode()
                va, length, off = struct.unpack_from('<QQI', data, p + 32)
                rel, nrel = struct.unpack_from('<II', data, p + 56)
                sections.append((name, va, length, off, rel, nrel))
        elif command == 2:
            symtab = struct.unpack_from('<4I', data, at + 8)
        at += size
    assert symtab
    symoff, nsyms, stroff, strsize = symtab
    symbols = []
    for i in range(nsyms):
        name, typ, sec, _, va = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
        assert name < strsize
        name = data[stroff + name:data.index(b'\0', stroff + name)].decode()
        symbols.append((name, typ, sec, va))
    found = [s for s in symbols if s[0].startswith('__ZL10abortProbe') and s[1] & 14 == 14]
    assert len(found) == 1, found
    name, _, sec, begin = found[0]
    text, va, length, off, rel, nrel = sections[sec - 1]
    assert text == '__text'
    end = min([s[3] for s in symbols if s[2] == sec and s[3] > begin and s[1] & 14 == 14] + [va + length])
    code = data[off + begin - va:off + end - va]
    relocations = []
    for i in range(nrel):
        address, flags = struct.unpack_from('<II', data, rel + i * 8)
        if not begin <= va + address < end:
            continue
        index, external, rtype = flags & 0xffffff, (flags >> 27) & 1, flags >> 28
        target = symbols[index][0] if external else (index if rtype == 10 else sections[index - 1][0])
        relocations.append((va + address - begin, flags >> 24, target))
    return {'symbol': name, 'code_sha256': hashlib.sha256(code).hexdigest(),
            'bytes': len(code), 'relocations': relocations}

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
root = a.unixnu.resolve()
kit = root / 'drivers/AdrenoMetal'
source = kit / 'NativeBridgeMetalFrontend.mm'
original = subprocess.check_output(['git', '-C', str(kit), 'show', '816022307852de7e1741ac05ae9111982318763f:NativeBridgeMetalFrontend.mm'])
flags = ['-std=c++17', '-fobjc-arc', '-O2', '-g0', '-fstack-protector-strong',
         '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
         '-I', str(kit), '-I', str(root / 'drivers/Adreno740'), '-I', str(root / '.cache/mesa-work/include')]
subprocess.run(['xcrun', 'clang++', *flags, '-fsyntax-only', str(source)], check=True)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
report = {'Intel_frontend_syntax_passed': True, 'original_source_sha256': hashlib.sha256(original).hexdigest(),
          'updated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'targets': []}
with tempfile.TemporaryDirectory(prefix='frontend-target-') as temp:
    work = Path(temp)
    before = work / 'original/NativeBridgeMetalFrontend.mm'
    before.parent.mkdir()
    before.write_bytes(original)
    for target in ('arm64-apple-ios17.0', 'arm64e-apple-ios27.0'):
        hashes, handlers = [], []
        for label, path in (('before', before), ('after', source)):
            output = work / (target + '-' + label + '.o')
            cmd = ['xcrun', 'clang++', '-target', target, '-isysroot', sdk, *flags,
                   '-ffile-prefix-map=' + str(before.parent) + '=' + str(kit), '-c', str(path), '-o', str(output)]
            subprocess.run(cmd, check=True)
            hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
            handlers.append(signal_handler(output))
            saved = a.report.parent / 'frontend-target-objects' / output.name
            saved.parent.mkdir(parents=True, exist_ok=True)
            saved.write_bytes(output.read_bytes())
        if handlers[0] != handlers[1]:
            raise SystemExit('ARM signal-handler code or relocations changed: ' + str(handlers))
        report['targets'].append({'target': target, 'before_object_sha256': hashes[0], 'after_object_sha256': hashes[1],
                                  'whole_objects_identical': hashes[0] == hashes[1],
                                  'signal_handler_code_and_relocations_identical': True, 'signal_handler': handlers[0]})
report['whole_translation_unit_claimed_identical'] = False
report['fixed_Metal002_target_plugin_replaced'] = False
report['line_number_effect'] = 'ENCODE(__LINE__) diagnostic constants change with the host source edit; original target plugin remains fixed.'
a.report.parent.mkdir(parents=True, exist_ok=True)
a.report.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
