#!/usr/bin/env python3
"""Deterministic 16 MiB FAT16 superfloppy for cas EFI-only RAM boot."""
import argparse
import hashlib
import json
import struct
import tomllib
from pathlib import Path

SECTOR = 512
SECTORS = 32768
FAT_SECTORS = 128
ROOT_SECTORS = 32
ROOT = 1 + FAT_SECTORS * 2
DATA = ROOT + ROOT_SECTORS
DATE = ((2026 - 1980) << 9) | (10 << 5) | 22

def require(condition):
    if not condition:
        raise ValueError("EFI-only FAT input/configuration invariant failed")

def validate_config(data):
    config = tomllib.loads(data.decode("utf-8"))
    require(config["version"] == 1)
    require(set(config) == {"version", "boot", "ui", "entry"})
    require(config["boot"] == {"default": "smoke", "timeout": -1, "rotate": 0, "fallback_timeout": 10, "connect_all": False})
    require(config["ui"]["layout"] == "portrait")
    require([(e["id"], e["type"]) for e in config["entry"]] == [("smoke", "efi"), ("missing", "efi"), ("android", "action"), ("firmware", "action")])
    require(config["entry"][0] == {"id": "smoke", "title": "Run CasSmoke EFI test", "type": "efi", "path": "\\EFI\\Tools\\CasSmoke.efi"})
    require(config["entry"][1]["path"] == "\\EFI\\Tools\\Missing.efi")
    require(config["entry"][2]["action"] == "reset" and config["entry"][3]["action"] == "exit")
    require(all(set(e) <= {"id", "title", "type", "path", "action"} for e in config["entry"]))
    return config

def build(config, program):
    validate_config(config)
    if program[:2] != b"MZ" or len(program) < 64:
        raise ValueError("CasSmoke must be a PE executable")
    pe = struct.unpack_from("<I", program, 60)[0]
    if pe + 94 > len(program) or program[pe:pe+4] != b"PE\0\0" or struct.unpack_from("<H", program, pe+4)[0] != 0xaa64 or struct.unpack_from("<H", program, pe+92)[0] != 10:
        raise ValueError("CasSmoke must be AARCH64 EFI_APPLICATION")
    image = bytearray(SECTORS * SECTOR)
    image[:3] = b"\xeb\x3c\x90"
    image[3:11] = b"CASRAM16"
    struct.pack_into("<HBHBHHBHHHII", image, 11, 512, 1, 1, 2, 512, SECTORS, 0xf8, FAT_SECTORS, 32, 64, 0, 0)
    struct.pack_into("<BBBI", image, 36, 0x80, 0, 0x29, 0x43415301)
    image[43:54] = b"CAS_EFI    "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xaa"
    fat = bytearray(FAT_SECTORS * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xfff8, 0xffff)
    next_cluster = 2
    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + 511) // 512)
        start = next_cluster
        if start + count > SECTORS - DATA + 2:
            raise ValueError("payload exceeds FAT capacity")
        for n in range(count):
            c = start + n
            struct.pack_into("<H", fat, c * 2, 0xffff if n == count - 1 else c + 1)
            pos = (DATA + c - 2) * 512
            chunk = data[n*512:(n+1)*512]
            image[pos:pos+len(chunk)] = chunk
        next_cluster += count
        return start
    def entry(name, attr, cluster, size=0):
        require(len(name) == 11)
        item = bytearray(32);item[:11] = name;item[11] = attr
        struct.pack_into("<HHHH", item, 14, 0, DATE, DATE, 0)
        struct.pack_into("<HHHI", item, 22, 0, DATE, cluster, size)
        return item
    def long_entry(name, short):
        checksum = 0
        for c in short:checksum = (((checksum & 1) << 7) + (checksum >> 1) + c) & 255
        chars = [ord(c) for c in name] + [0]
        require(len(chars) <= 13)
        chars += [0xffff] * (13-len(chars))
        item = bytearray(32);item[0]=0x41;item[11]=0x0f;item[13]=checksum
        for offset,c in zip([1,3,5,7,9,14,16,18,20,22,24,28,30],chars):struct.pack_into("<H",item,offset,c)
        return item
    # Reserve three directory clusters before data files.
    efi, unixnu, tools = [allocate(b"") for _ in range(3)]
    cfg, app = allocate(config), allocate(program)
    def directory(cluster, parent, children):
        data = entry(b".          ", 0x10, cluster) + entry(b"..         ", 0x10, parent) + b"".join(children)
        require(len(data) < 512)
        pos = (DATA + cluster - 2) * 512
        image[pos:pos+len(data)] = data
    directory(efi, 0, [entry(b"UNIXNU     ",0x10,unixnu),entry(b"TOOLS      ",0x10,tools)])
    short = b"UNIXNU~1TOM"
    directory(unixnu,efi,[long_entry("unixnu.toml",short),entry(short,0x20,cfg,len(config))])
    directory(tools,efi,[entry(b"CASSMOKEEFI",0x20,app,len(program))])
    root = entry(b"CAS_EFI    ",8,0) + entry(b"EFI        ",0x10,efi)
    image[ROOT*512:ROOT*512+len(root)] = root
    image[512:(1+FAT_SECTORS)*512] = fat
    image[(1+FAT_SECTORS)*512:ROOT*512] = fat
    return bytes(image)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config",type=Path,default=Path(__file__).resolve().parents[1]/"config/unixnu.toml")
    ap.add_argument("--efi",type=Path,required=True)
    ap.add_argument("--out",type=Path,required=True)
    args=ap.parse_args()
    data=build(args.config.read_bytes(),args.efi.read_bytes())
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with args.out.open("xb") as f:f.write(data)
    print(json.dumps({"path":str(args.out.resolve()),"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"format":"FAT16 superfloppy","timestamp":"2026-10-22T00:00:00","hardware_proven":False},indent=2))

if __name__=="__main__":main()
