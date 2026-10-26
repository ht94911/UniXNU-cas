#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Pack the cas boot image: CasBootShim + firmware volume, stock DTB, payload.

The ABL on cas takes an Android boot image header v2 with the DTB in the
header's dtb area (not appended to the kernel). The kernel part is
gzip(CasBootShim + SILICIUM_UEFI.fd). The ramdisk is the UniXNU payload FAT
from `./unixnu payload`; CasBootShim copies at most 64 MiB of it.
"""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_shim import build as build_shim  # noqa: E402
from prepare_boot_dtb import prepare as prepare_dtb  # noqa: E402

FD_BYTES = 0x800000
RAMDISK_MAX = 0x4000000  # CAS_PAYLOAD_SIZE in uefi-cas casPkg/Include/CasPlatform.h


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--uefi', type=Path, required=True, help='uefi-cas checkout')
    ap.add_argument('--fd', type=Path, required=True, help='SILICIUM_UEFI.fd from ./unixnu build firmware')
    ap.add_argument('--payload', type=Path, required=True, help='payload.img from ./unixnu payload')
    ap.add_argument('--stock-dtb', type=Path, required=True, help='stock SoC DTB index 2 (private)')
    ap.add_argument('--out', type=Path, required=True, help='output boot.img')
    a = ap.parse_args()

    config = tomllib.loads((a.uefi / 'Resources/Configs/cas.toml').read_text())
    image = config['boot_image']
    fd = a.fd.read_bytes()
    payload = a.payload.read_bytes()
    if len(fd) != FD_BYTES or fd[40:44] != b'_FVH':
        raise SystemExit(f'{a.fd}: expected an 8 MiB firmware volume')
    if not payload or len(payload) > RAMDISK_MAX:
        raise SystemExit(f'{a.payload}: payload must be 1..{RAMDISK_MAX} bytes, got {len(payload)}')
    dtb = prepare_dtb(a.stock_dtb.read_bytes(), config['boot_image_dtb'])

    with tempfile.TemporaryDirectory(prefix='cas-bootimg-') as scratch_name:
        scratch = Path(scratch_name)
        shim = build_shim(a.uefi, scratch / 'shim').read_bytes()
        kernel = gzip.compress(shim + fd, compresslevel=9, mtime=0)
        (scratch / 'kernel.gz').write_bytes(kernel)
        (scratch / 'cas.dtb').write_bytes(dtb)
        command = [sys.executable, str(a.uefi / 'Resources/Scripts/mkbootimg.py'),
                   '--kernel', str(scratch / 'kernel.gz'), '--ramdisk', str(a.payload.resolve()),
                   '--dtb', str(scratch / 'cas.dtb'), '--board', 'cas', '--cmdline', image['cmdline']]
        for key in ('header_version', 'pagesize', 'base', 'kernel_offset', 'ramdisk_offset', 'dtb_offset', 'tags_offset'):
            command += ['--' + key, str(image[key])]
        a.out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([*command, '--output', str(a.out)], check=True)

    data = a.out.read_bytes()
    kernel_size, _, ramdisk_size, _, _, _, _, page, version, _ = struct.unpack_from('<10I', data, 8)
    _, _, _, dtb_size, _ = struct.unpack_from('<IQIIQ', data, 1632)
    if data[:8] != b'ANDROID!' or version != 2 or page != image['pagesize']:
        raise SystemExit('boot image header mismatch')
    pages = lambda size: -(-size // page)
    ramdisk_at = page * (1 + pages(kernel_size))
    dtb_at = ramdisk_at + page * pages(ramdisk_size)
    unpacked = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(data[page:page + kernel_size])
    if unpacked != shim + fd or data[ramdisk_at:ramdisk_at + ramdisk_size] != payload \
            or data[dtb_at:dtb_at + dtb_size] != dtb:
        raise SystemExit('boot image parts do not round-trip')
    report = {'boot_image': str(a.out), 'sha256': sha(data), 'bytes': len(data), 'shim_sha256': sha(shim),
              'fd_sha256': sha(fd), 'payload_sha256': sha(payload), 'payload_bytes': len(payload),
              'dtb_sha256': sha(dtb), 'hardware_tested': False}
    a.out.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
