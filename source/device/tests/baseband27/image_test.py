#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import hashlib
import importlib.util
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('packer', ROOT / 'tools/baseband27/pack-image.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)

def rejects(call):
    try:
        call()
    except ValueError:
        return
    raise AssertionError('invalid input was accepted')

with tempfile.TemporaryDirectory(prefix='nb-image-test-') as temp:
    base = Path(temp)
    source = base / 'inputs'
    (source / 'tree').mkdir(parents=True)
    (source / 'empty').write_bytes(b'')
    (source / 'tree/sample.bin').write_bytes(bytes(i % 251 for i in range(9000)))
    for wide in [False, True]:
        h, p = (64,56) if wide else (52,32)
        header = bytearray(h + 3*p)
        header[:6] = b'\x7fELF' + bytes([2 if wide else 1, 1])
        struct.pack_into('<Q' if wide else '<I', header, 32 if wide else 28, h)
        struct.pack_into('<HH', header, 54 if wide else 42, p, 3)
        for i in range(3):
            off, size = (0,len(header)) if i==0 else (4096,32) if i==1 else (8192,17)
            kind, flags, addr, mem = (1,0,0x8aa00000,4096) if i==2 else (0,2<<24 if i==1 else 0,0,size)
            if wide:
                struct.pack_into('<II6Q',header,h+i*p,kind,flags,off,0,addr,size,mem,4096)
            else:
                struct.pack_into('<8I',header,h+i*p,kind,off,0,addr,size,mem,flags,4096)
        name = 'fw64' if wide else 'fw32'
        (source/(name+'.mdt')).write_bytes(header + bytes([0x35])*32)
        (source/(name+'.b02')).write_bytes(bytes([0x72])*17)
    result = packer.pack(source, base / 'a.img', 0)
    second = packer.pack(source, base / 'b.img', 0)
    assert result == second
    assert result['sha256'] == hashlib.sha256((base / 'a.img').read_bytes()).hexdigest()
    assert (base / 'a.img.sha256').read_text() == result['sha256'] + '  a.img\n'
    rejects(lambda: packer.pack(source, base / 'a.img', 0))
    rejects(lambda: packer.pack(source, source / 'inside.img', 0))
    (source / 'link').symlink_to(source / 'empty')
    rejects(lambda: packer.pack(source, base / 'link.img', 0))
    (source / 'link').unlink()
    # Physical readers are compiled from production C, with a fragmented fake medium.
    exe = base / 'reader'
    subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
        str(ROOT / 'tests/baseband27/archive_test.c'), str(ROOT / 'package/baseband27/platform/NbArchive.c'),
        str(ROOT / 'package/baseband27/platform/NbFirmware.c'), str(ROOT / 'package/baseband27/upstream/QcomMdt.c'),
        '-lcrypto', '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(base / 'a.img')], check=True)
print('PASS local image tool: deterministic output, SHA-256, rejected invalid inputs')
