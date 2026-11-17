#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the local stock camera.ko tables through LLVM's ELF symbol reader."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def require(value, message):
    if not value:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--control', default='ctrl_reg_2_4_0_1')
    parser.add_argument('--data-rate', type=int, default=8000000000)
    parser.add_argument('--output', type=Path, required=True, help='local private JSON')
    args = parser.parse_args()
    binary = args.kernel.read_bytes()
    require(binary.startswith(b'\x7fELF\x02\x01'), 'Expected a little-endian ELF64 module')
    rows = json.loads(subprocess.check_output(['llvm-readobj', '--elf-output-style=JSON',
                       '--sections', '--symbols', str(args.kernel)], timeout=12))
    require(len(rows) == 1 and rows[0]['FileSummary']['Format'] == 'elf64-littleaarch64',
            'Expected one ARM64 Android module')
    sections = [row['Section'] for row in rows[0]['Sections']]
    symbols = [row['Symbol'] for row in rows[0]['Symbols']]
    named = {symbol['Name']['Name']: symbol for symbol in symbols if symbol['Size'] > 0}
    relocations = {}
    for section in sections:
        if section['Type']['Name'] != 'SHT_RELA':
            continue
        target = section['Info']
        start, size = section['Offset'], section['Size']
        for offset, info, addend in struct.iter_unpack('<QQq', binary[start:start + size]):
            if info & 0xffffffff == 257:  # R_AARCH64_ABS64
                relocations[target, offset] = symbols[info >> 32], addend

    def data(symbol):
        require(symbol['Section']['Value'] < len(sections), 'Unresolved table section')
        section = sections[symbol['Section']['Value']]
        offset, size = symbol['Value'], symbol['Size']
        require(offset <= section['Size'] and size <= section['Size'] - offset, 'Table exceeds its ELF section')
        at = section['Offset'] + offset
        return binary[at:at + size]

    def pointer(symbol, offset):
        require(offset + 8 <= symbol['Size'], 'Pointer exceeds its table')
        target, addend = relocations[symbol['Section']['Value'], symbol['Value'] + offset]
        address = target['Value'] + addend
        matches = [entry for entry in named.values() if entry['Section']['Value'] == target['Section']['Value'] and
                   entry['Value'] == address]
        require(len(matches) == 1, 'Relocated pointer does not identify a unique table')
        return matches[0]

    def registers(symbol):
        content = data(symbol)
        require(len(content) % 16 == 0, 'Invalid register table length')
        result = []
        for offset, value, delay, kind in struct.iter_unpack('<IIII', content):
            require(offset < 0x2000 and offset % 4 == 0 and value <= 0xff and delay <= 1000000,
                    'Invalid register table entry')
            result.append(dict(offset=offset, value=value, delay_us=delay, kind=kind))
        return dict(symbol=symbol['Name']['Name'], sha256=hashlib.sha256(content).hexdigest(), registers=result)

    control = named[args.control]
    require(control['Size'] == 128, 'Unexpected stock CSIPHY control size')
    table = dict(source_sha256=hashlib.sha256(binary).hexdigest(), control=args.control,
                 lane_offsets=struct.unpack_from('<8I', data(control), 0x50),
                 requested_data_rate=args.data_rate)
    for name, offset in (('common', 8), ('irq', 16), ('reset_enter', 24), ('reset_exit', 32),
                         ('lane_enable', 40), ('dphy', 56), ('cphy', 64), ('dphy_clock', 72)):
        table[name] = registers(pointer(control, offset))
    delta = pointer(control, 120)
    count = struct.unpack_from('<Q', data(delta))[0]
    profiles = pointer(delta, 8)
    require(count > 0 and profiles['Size'] == count * 416, 'Unexpected stock data-rate array size')
    records = data(profiles)
    rate_rows = []
    selected = None
    for index in range(count):
        at = index * 416
        flags, rate, reg_count = struct.unpack_from('<3Q', records, at)
        symbol = pointer(profiles, at + 224)  # CSIPHY0, variant0.
        require(symbol['Size'] == reg_count * 16, 'Data-rate table length differs')
        common_count = struct.unpack_from('<Q', records, at + 216)[0]
        rate_rows.append(dict(index=index, flags=flags, max_data_rate=rate, symbol=symbol['Name']['Name']))
        if selected is None and flags == 0 and rate >= args.data_rate:
            selected = registers(symbol)
            selected.update(index=index, max_data_rate=rate)
            if common_count:
                common = pointer(profiles, at + 24)
                require(common['Size'] == common_count * 16, 'Common control table length differs')
                selected['common'] = registers(common)
    require(selected is not None, 'No stock C-PHY profile covers the requested data rate')
    table.update(profiles=rate_rows, selected=selected)
    args.output.write_text(json.dumps(table, indent=2) + '\n', encoding='utf-8')
    print('Stock CSIPHY', args.control, 'selected', selected['symbol'], 'for', args.data_rate)


if __name__ == '__main__':
    main()
