#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage a "late" Mach-O object: drop __DATA,__mod_init_func and make the C++ static initializer global.

The sheng/umi builders do this with `llvm-objcopy --remove-section=__DATA,__mod_init_func
--globalize-symbol=__GLOBAL__sub_I_<file>.cpp`; the kernel then runs the initializer explicitly between
OSMetaClass::preModLoad and postModLoad instead of at boot. The LLVM 18 llvm-objcopy of the Linux host
refuses --globalize-symbol for Mach-O, so this module performs the same two edits on an MH_OBJECT:

  * the section header is removed (later section ordinals in symbols and non-extern relocations are
    renumbered); its data and relocation bytes stay in the file but are no longer referenced, and the
    load commands keep their file offsets (the freed 80 bytes become padding after them);
  * the initializer symbol gets N_EXT and moves from the LC_DYSYMTAB local range into the sorted external
    range; every extern relocation and indirect symbol index is renumbered.

The __text bytes are never touched; callers compare them before and after, as the upstream builders do.
"""
import argparse
from pathlib import Path
import struct
import subprocess

MH_MAGIC_64, MH_OBJECT = 0xfeedfacf, 1
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB = 0x19, 0x2, 0xb
N_STAB, N_TYPE, N_EXT, N_SECT, N_ABS = 0xe0, 0x0e, 0x01, 0x0e, 0x02
S_MOD_INIT_FUNC_POINTERS, SECTION_TYPE = 0x9, 0xff
ARM64_RELOC_ADDEND = 10
INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS = 0x80000000, 0x40000000


class MachOError(Exception):
    pass


def _cstr(blob):
    return blob.split(b'\0', 1)[0].decode()


def parse(data):
    magic, cpu, subtype, kind, ncmds, sizeofcmds = struct.unpack_from('<6I', data)
    if magic != MH_MAGIC_64 or kind != MH_OBJECT:
        raise MachOError('expected a 64-bit Mach-O MH_OBJECT')
    commands, sections, at = [], [], 32
    symtab = dysymtab = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        commands.append((cmd, at, size))
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            for i in range(nsects):
                header = at + 72 + i * 80
                sectname, segname = _cstr(data[header:header + 16]), _cstr(data[header + 16:header + 32])
                offset, align, reloff, nreloc, flags = struct.unpack_from('<5I', data, header + 48)
                sections.append(dict(segment=at, header=header, sectname=sectname, segname=segname,
                                     reloff=reloff, nreloc=nreloc, flags=flags))
        elif cmd == LC_SYMTAB:
            symtab = at
        elif cmd == LC_DYSYMTAB:
            dysymtab = at
        at += size
    if at != 32 + sizeofcmds:
        raise MachOError('load command sizes disagree with the header')
    if symtab is None or dysymtab is None:
        raise MachOError('object lacks LC_SYMTAB/LC_DYSYMTAB')
    return dict(commands=commands, sections=sections, symtab=symtab, dysymtab=dysymtab, sizeofcmds=sizeofcmds)


def stage_late(source, destination, initializer, remove=('__DATA', '__mod_init_func')):
    data = bytearray(Path(source).read_bytes())
    info = parse(data)
    symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, info['symtab'] + 8)
    dy = list(struct.unpack_from('<18I', data, info['dysymtab'] + 8))
    ilocal, nlocal, iextdef, nextdef, iundef, nundef = dy[:6]
    indirectoff, nindirect = dy[12], dy[13]
    if dy[14:18] != [0, 0, 0, 0] or ilocal != 0 or iextdef != nlocal or iundef != iextdef + nextdef or \
            iundef + nundef != nsyms:
        raise MachOError('unexpected LC_DYSYMTAB layout for an object file')
    entries = []
    for index in range(nsyms):
        strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        name = data[stroff + strx:data.index(b'\0', stroff + strx)].decode()
        entries.append([strx, ntype, nsect, ndesc, value, name])

    # 1. Remove the initializer pointer section.
    matches = [i for i, s in enumerate(info['sections']) if (s['segname'], s['sectname']) == tuple(remove)]
    if len(matches) != 1:
        raise MachOError(f'expected exactly one {remove[0]},{remove[1]} section, found {len(matches)}')
    removed = matches[0] + 1     # 1-based section ordinal
    section = info['sections'][matches[0]]
    if section['flags'] & SECTION_TYPE != S_MOD_INIT_FUNC_POINTERS:
        raise MachOError('the section to remove is not a module initializer pointer section')
    dropped = set()
    for index, entry in enumerate(entries):
        # Section symbols carry the ordinal in n_sect; so do debugger (STAB) entries that name a section.
        if not entry[1] & N_STAB and entry[1] & N_TYPE != N_SECT:
            continue
        if entry[2] == removed:
            if entry[1] & (N_EXT | N_STAB):
                raise MachOError(f'symbol {entry[5]} of the removed section cannot be dropped')
            dropped.add(index)
        elif entry[2] > removed:
            entry[2] -= 1

    # 2. Globalize the initializer and keep the local/defined/undefined ranges ordered.
    target = [i for i, e in enumerate(entries) if e[5] == initializer]
    if len(target) != 1:
        raise MachOError(f'expected exactly one symbol {initializer}')
    target = target[0]
    if not ilocal <= target < ilocal + nlocal or entries[target][1] & (N_STAB | N_EXT) or \
            entries[target][1] & N_TYPE != N_SECT:
        raise MachOError(f'{initializer} is not a local section symbol')
    entries[target][1] |= N_EXT
    locals_ = [i for i in range(ilocal, ilocal + nlocal) if i != target and i not in dropped]
    extdefs = sorted(list(range(iextdef, iextdef + nextdef)) + [target], key=lambda i: entries[i][5].encode())
    undefs = list(range(iundef, iundef + nundef))
    order = locals_ + extdefs + undefs
    mapping = {old: new for new, old in enumerate(order)}

    # Relocations of the remaining sections: extern ones name symbols, the others name section ordinals.
    for index, sect in enumerate(info['sections']):
        if index + 1 == removed:
            continue
        for k in range(sect['nreloc']):
            at = sect['reloff'] + k * 8
            address, word = struct.unpack_from('<iI', data, at)
            if address < 0:
                raise MachOError('scattered relocations are not expected in an ARM64 object')
            symbolnum, extern, rtype = word & 0xffffff, word >> 27 & 1, word >> 28
            if extern:
                if symbolnum in dropped:
                    raise MachOError('a relocation refers to a symbol of the removed section')
                symbolnum = mapping[symbolnum]
            elif rtype != ARM64_RELOC_ADDEND:
                if symbolnum == removed:
                    raise MachOError('a relocation refers to the removed section')
                if symbolnum > removed:
                    symbolnum -= 1
            struct.pack_into('<I', data, at + 4, (word & ~0xffffff) | symbolnum)
    for k in range(nindirect):
        at = indirectoff + k * 4
        value = struct.unpack_from('<I', data, at)[0]
        if not value & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
            struct.pack_into('<I', data, at, mapping[value])

    table = bytearray()
    for old in order:
        strx, ntype, nsect, ndesc, value, _ = entries[old]
        table += struct.pack('<IBBHQ', strx, ntype, nsect, ndesc, value)
    data[symoff:symoff + nsyms * 16] = table + bytes((nsyms - len(order)) * 16)
    struct.pack_into('<I', data, info['symtab'] + 12, len(order))
    struct.pack_into('<6I', data, info['dysymtab'] + 8, 0, len(locals_), len(locals_), len(extdefs),
                     len(locals_) + len(extdefs), len(undefs))

    # Remove the section header last: shift the following load commands, keep file offsets.
    segment, header = section['segment'], section['header']
    nsects, cmdsize = struct.unpack_from('<I', data, segment + 64)[0], struct.unpack_from('<I', data, segment + 4)[0]
    end = 32 + info['sizeofcmds']
    data[header:end] = data[header + 80:end] + bytes(80)
    struct.pack_into('<I', data, segment + 64, nsects - 1)
    struct.pack_into('<I', data, segment + 4, cmdsize - 80)
    struct.pack_into('<I', data, 20, info['sizeofcmds'] - 80)
    Path(destination).write_bytes(bytes(data))
    return dict(removed_section=f'{remove[0]},{remove[1]}', globalized=initializer, dropped_local_symbols=len(dropped))


def text_bytes(path):
    data = Path(path).read_bytes()
    info = parse(data)
    found = [s for s in info['sections'] if (s['segname'], s['sectname']) == ('__TEXT', '__text')]
    if len(found) != 1:
        raise MachOError('expected one __TEXT,__text section')
    header = found[0]['header']
    size = struct.unpack_from('<Q', data, header + 40)[0]
    offset = struct.unpack_from('<I', data, header + 48)[0]
    return data[offset:offset + size]


def check(source, late, initializer):
    """The upstream invariant (instructions unchanged) plus what the edit must achieve, read back with cctools nm/otool."""
    if text_bytes(source) != text_bytes(late):
        raise MachOError('late staging changed the instructions')
    symbols = subprocess.check_output(['xcrun', 'nm', '-m', str(late)], text=True)
    if not any(line.split()[-1:] == [initializer] and '(__TEXT,__text) external' in line for line in symbols.splitlines()):
        raise MachOError(f'{initializer} is not an external __text symbol in {late}')
    commands = subprocess.check_output(['xcrun', 'otool', '-l', str(late)], text=True)
    if '__mod_init_func' in commands:
        raise MachOError(f'{late} still has a module initializer section')
    # The kernel linker must accept the result.
    subprocess.run(['xcrun', 'ld', '-r', '-arch', 'arm64', str(late), '-o', str(Path(late).with_suffix('.ldcheck.o'))],
                   check=True, capture_output=True)
    Path(late).with_suffix('.ldcheck.o').unlink()


def stage(source, destination, initializer):
    result = stage_late(source, destination, initializer)
    check(source, destination, initializer)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--initializer', required=True, help='for example __GLOBAL__sub_I_NativeAES.cpp')
    args = parser.parse_args()
    print(stage(args.source, args.destination, args.initializer))
