#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fill SHA256 page slots in a verified, same-size Apple ad hoc file template.

The existing Mac codesign command produces the generic file's metadata from
public bytes. Only page hashes change locally; the private cache stays local.
The Mac fixture checks the resulting detached signature with real codesign.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

IDENTIFIER = 'org.unixnu.wifi.launchd-cache'


def directories(blob):
    result = []

    def walk(start, available):
        magic, size = struct.unpack_from('>II', blob, start)
        if size < 8 or size > available:
            raise ValueError('Invalid detached signature blob range')
        if magic == 0xfade0c02:
            result.append((start, size))
        elif magic in (0xfade0cc0, 0xfade0cc1):
            count = struct.unpack_from('>I', blob, start + 8)[0]
            if 12 + count * 8 > size:
                raise ValueError('Invalid signature index')
            for index in range(count):
                _, offset = struct.unpack_from('>II', blob, start + 12 + index * 8)
                if offset < 12 + count * 8 or offset >= size:
                    raise ValueError('Invalid signature child offset')
                walk(start + offset, size - offset)

    walk(0, len(blob))
    if len(result) != 1:
        raise ValueError('Require one SHA256 generic CodeDirectory')
    return result


def metadata(blob, data_size):
    start, size = directories(blob)[0]
    version, flags, hashes, name, special, pages, limit = struct.unpack_from('>7I', blob, start + 8)
    hash_size, hash_type, platform, power = struct.unpack_from('4B', blob, start + 36)
    if version >= 0x20300 and not limit:
        limit = struct.unpack_from('>Q', blob, start + 56)[0]
    if flags != 2 or hash_size != 32 or hash_type != 2 or power != 12:
        raise ValueError('Require the upstream ad hoc SHA256/4KiB file signature')
    if not 0 < name < size or blob[start + name:start + size].split(b'\0', 1)[0].decode() != IDENTIFIER:
        raise ValueError('Launchd cache signature identifier changed')
    if limit != data_size or pages != (data_size + 4095) // 4096:
        raise ValueError('Template and private file sizes differ')
    if hashes < special * 32 or hashes + pages * 32 > size:
        raise ValueError('Invalid signature hash table')
    return dict(start=start, size=size, hash_offset=hashes, code_pages=pages,
                code_limit=limit, flags=flags, platform=platform, page_bytes=4096)


def verify(blob, data):
    item = metadata(blob, len(data))
    begin = item['start'] + item['hash_offset']
    for page in range(item['code_pages']):
        expected = hashlib.sha256(data[page * 4096:(page + 1) * 4096]).digest()
        if blob[begin + page * 32:begin + (page + 1) * 32] != expected:
            raise ValueError('Detached file page hash mismatch')
    return item


def rewrite(template, data):
    item = metadata(template, len(data))
    output = bytearray(template)
    begin = item['start'] + item['hash_offset']
    for page in range(item['code_pages']):
        output[begin + page * 32:begin + (page + 1) * 32] = hashlib.sha256(
            data[page * 4096:(page + 1) * 4096]).digest()
    output = bytes(output)
    verify(output, data)
    cd = output[item['start']:item['start'] + item['size']]
    return output, dict(identifier=IDENTIFIER, cdhash=hashlib.sha256(cd).hexdigest()[:40],
        data_sha256=hashlib.sha256(data).hexdigest(), signature_sha256=hashlib.sha256(output).hexdigest(),
        template_sha256=hashlib.sha256(template).hexdigest(), metadata_unchanged=True,
        Apple_trust_chain_claimed=False, **{k:v for k,v in item.items() if k not in ('start','size','hash_offset')})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--template', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    signature, report = rewrite(args.template.read_bytes(), args.data.read_bytes())
    args.output.write_bytes(signature)
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
