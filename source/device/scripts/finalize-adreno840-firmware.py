#!/usr/bin/env python3
"""Insert verified local A840 firmware into a NEW native kernel before prelink.

No phone access and no network. The source-only CI archive deliberately carries
an empty, file-backed slot. Never upload this tool's resource or native output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

CAPACITY = 256 * 1024
FILES = (
    ('gen80200_gmu.bin', 112570, '7e2f9381dbae6ba3eda632e51e07377a90a9549e2e164a033255ab4ecd431ba8'),
    ('gen80200_sqe.fw', 121172, '1f1c7bae8050a9910902ec42b3d741ff3a68bc21114dea966a502ade2a9b606d'),
    ('gen80200_zap.mbn', 12288, '3f919701004082d9ea58b21fb9ee3a646ad42554a16eb2de8b0f7dfffedfdb4a'),
)


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_pack(directory):
    pack = bytearray(CAPACITY)
    struct.pack_into('<8sII', pack, 0, b'PANDFW01', 3, 0)
    struct.pack_into('<8sII', pack, 512, b'NGFW001\0', 1, 0)
    offset, records = 4096, []
    for i, (name, size, digest) in enumerate(FILES):
        path = Path(directory) / name
        require(path.is_file() and not path.is_symlink(), 'missing ordinary firmware file: ' + name)
        data = path.read_bytes()
        require(len(data) == size and sha(data) == digest, 'unqualified firmware identity: ' + name)
        require(offset + size <= CAPACITY, 'firmware exceeds reserved slot')
        struct.pack_into('<48sQQ', pack, 16 + i * 64, name.encode(), offset, size)
        pack[544 + i * 32:576 + i * 32] = bytes.fromhex(digest)
        pack[offset:offset + size] = data
        records.append(dict(name=name, bytes=size, sha256=digest, offset=offset))
        offset += (size + 4095) & ~4095
    struct.pack_into('<Q', pack, 528, offset)
    return bytes(pack), dict(files=records, used_bytes=offset, capacity_bytes=CAPACITY, sha256=sha(pack))


def finalize_native(raw, expected_sha, pack, image_type, *, capacity=CAPACITY,
                    section_name='__nezgfw', owner_symbol='_nezha_gpu_fwpack',
                    size_symbol='_nezha_gpu_fwpack_size'):
    require(sha(raw) == expected_sha, 'native kernel input SHA256 differs')
    require(len(pack) == capacity, 'resource length differs')
    require(len(raw) >= 32, 'truncated Mach-O header')
    magic, cpu, subtype, kind = struct.unpack_from('<4I', raw)
    require((magic, cpu, subtype & 0xffffff, kind) == (0xfeedfacf, 0x100000c, 2, 2),
            'expected standalone arm64e native executable, before fileset prelink')
    image = image_type(raw)
    slots = [s for s in image.sections if s['segment'] == '__DATA_CONST' and s['name'] == section_name]
    require(len(slots) == 1, 'native firmware section is missing or ambiguous')
    slot = slots[0]
    require(slot['size'] == capacity and slot['va'] % 4096 == 0, 'firmware slot size/alignment differs')
    offset = image.offset(slot['va'], capacity)
    names = {}
    for address, symbols in image.symbols().items():
        for name in symbols:
            names.setdefault(name, []).append(address)
    require(names.get(owner_symbol) == [slot['va']], 'firmware symbol is not the unique slot owner')
    size_symbols = names.get(size_symbol, [])
    require(len(size_symbols) == 1, 'firmware size symbol is missing or ambiguous')
    require(struct.unpack('<Q', image.read(size_symbols[0], 8))[0] == capacity, 'runtime size differs from slot')
    require(raw[offset:offset + capacity] == bytes(capacity), 'refuse an already-filled firmware slot')
    result = raw[:offset] + pack + raw[offset + capacity:]
    require(len(raw) == len(result) and result[:offset] == raw[:offset] and
                        result[offset + capacity:] == raw[offset + capacity:], 'nonresource bytes changed')
    return result, dict(input_native_sha256=expected_sha, output_native_sha256=sha(result),
                        native_bytes=len(result), section_va=hex(slot['va']), section_file_offset=offset,
                        resource_bytes=capacity, nonresource_bytes_unchanged=True,
                        firmware_present=True, fresh_strict_prelink_required=True,
                        hardware_verified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--expected-native-sha256', required=True)
    parser.add_argument('--prelink-tools', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    require(args.kernel.resolve() != args.out.resolve(), 'output must be a new kernel')
    report_path = args.out.with_suffix(args.out.suffix + '.resource.json')
    require(not args.out.exists() and not report_path.exists(), 'output or report already exists')
    sys.path.insert(0, str(args.prelink_tools.resolve()))
    from kc_image import Image
    pack, resource = make_pack(args.firmware_dir)
    result, report = finalize_native(args.kernel.read_bytes(), args.expected_native_sha256, pack, Image)
    report['resource'] = resource
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('xb') as stream:
        stream.write(result)
    require(sha(args.out.read_bytes()) == report['output_native_sha256'], 'output readback differs')
    with report_path.open('x') as stream:
        stream.write(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
