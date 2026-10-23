"""Read the versioned cas diagnostic RAM record without touching a device.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import struct
import zlib

MAGIC=b"CASLOG01"
HEADER=96
CAPACITY=65536

def decode(raw, expected_build="cas-diag-001", current_boot=None):
    if len(raw)>CAPACITY:
        raise ValueError("record exceeds reserved slot")
    if len(raw)<8:
        raise ValueError("truncated magic")
    if raw[:8]!=MAGIC:
        raise ValueError("no diagnostic magic")
    if len(raw)<HEADER:
        raise ValueError("truncated header")
    version,header,total,length=struct.unpack_from("<IIII",raw,8)
    sequence,boot,flags,checksum=struct.unpack_from("<QQII",raw,24)
    if version!=1 or header!=HEADER or total<HEADER or total>CAPACITY or length!=total-HEADER:
        raise ValueError("unsupported version or invalid lengths")
    if total>len(raw):
        raise ValueError("truncated payload")
    if not sequence or not boot or flags&~7 or any(raw[80:96]):
        raise ValueError("invalid sequence, boot stamp, flags or reserved bytes")
    name,sep,padding=raw[48:80].partition(b"\0")
    if not sep or not name or any(padding) or any(c<32 or c>126 for c in name):
        raise ValueError("invalid build identity")
    zeroed=raw[:44]+bytes(4)+raw[48:total]
    if zlib.crc32(zeroed)!=checksum:
        raise ValueError("CRC32 mismatch")
    data=raw[HEADER:total]
    if any(c not in (9,10,13) and not 32<=c<=126 for c in data):
        raise ValueError("non-ASCII diagnostic payload")
    build=name.decode("ascii")
    return {"result":"valid" if expected_build is None or build==expected_build else "foreign_build",
            "version":version,"build_id":build,"sequence":sequence,"boot_stamp":boot,
            "flags":flags,"warm_reset_requested":bool(flags&1),"probe_passed_in_record":bool(flags&2),
            "probe_refused_in_record":bool(flags&4),"payload_bytes":length,"crc32":f"{checksum:08x}",
            "boot_relation":"unknown" if current_boot is None else ("same_boot" if boot==current_boot else "prior_boot_candidate"),
            "persistence_verified":False,"payload":data.decode("ascii")}
