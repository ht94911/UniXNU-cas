#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Route indirect branches inside linked Rust objects of an arm64e Mach-O image
through the stripping trampolines in A840RustPacBoundary.c.

Usage:
  rust_pac_rewrite.py IMAGE LINK_MAP ARCHIVE_NAME
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19


def rewrite_rust_indirect_branches(image_path: Path, link_map_path: Path, archive_name: str) -> dict:
    if not archive_name or not archive_name.strip():
        raise RuntimeError("archive_name must be non-empty")
    link_map = link_map_path.read_text(encoding="utf-8", errors="replace")
    data = bytearray(image_path.read_bytes())

    if len(data) < 32:
        raise RuntimeError("Mach-O image truncated (< 32 bytes)")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MH_MAGIC_64:
        raise RuntimeError(f"unsupported Mach-O magic {magic:#x} (expected MH_MAGIC_64)")

    files: set[int] = set()
    text: tuple[int, int] | None = None
    symbols: list[tuple[int, int, int, str]] = []
    part: str | None = None

    for line in link_map.splitlines():
        if line.startswith("# Object files:"):
            part = "files"
            continue
        if line.startswith("# Sections:"):
            part = "sections"
            continue
        if line.startswith("# Symbols:"):
            part = "symbols"
            continue
        if line.startswith("# Dead Stripped"):
            part = None
            continue
        if line.startswith("#") or not line.strip():
            continue
        if part == "files":
            m = re.match(r"\[\s*(\d+)\]\s+(.*)$", line)
            if m and archive_name in m.group(2):
                files.add(int(m.group(1)))
        elif part == "sections":
            fields = line.split()
            if len(fields) >= 4 and fields[2] == "__TEXT" and fields[3] == "__text":
                text = (int(fields[0], 16), int(fields[1], 16))
        elif part == "symbols":
            m = re.match(r"(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+\[\s*(\d+)\]\s+(.*)$", line)
            if m:
                symbols.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), m.group(4)))

    if not files or text is None:
        raise RuntimeError(f"link map names no object of {archive_name} or missing __TEXT,__text")

    tramp: dict[int, int] = {}
    for address, _size, _index, name in symbols:
        m = re.fullmatch(r"_nmb_pac_call_x(\d+)", name)
        if m:
            tramp[int(m.group(1))] = address
    if sorted(tramp) != list(range(30)):
        missing = sorted(set(range(30)) - set(tramp))
        raise RuntimeError(f"missing PAC trampolines: {missing}")

    ncmds = struct.unpack_from("<I", data, 16)[0]
    at = 32
    segment = None
    for _ in range(ncmds):
        if at + 8 > len(data):
            raise RuntimeError("truncated Mach-O load commands")
        cmd, size = struct.unpack_from("<II", data, at)
        if size < 8 or at + size > len(data):
            raise RuntimeError(f"invalid Mach-O load command size {size} at {at:#x}")
        if cmd == LC_SEGMENT_64 and size >= 48 and data[at + 8 : at + 24].rstrip(b"\0") == b"__TEXT":
            vmaddr, vmsize, fileoff = struct.unpack_from("<QQQ", data, at + 24)
            segment = (vmaddr, vmsize, fileoff)
            break
        at += size
    if segment is None:
        raise RuntimeError("missing __TEXT LC_SEGMENT_64 in Mach-O image")

    vmaddr, _vmsize, fileoff = segment
    lo, hi = text[0], text[0] + text[1]
    counts = {"blr": 0, "br": 0, "x30": 0, "objects": len(files)}
    for address, size, index, _name in symbols:
        if index not in files or not (lo <= address < hi):
            continue
        for pc in range(address, min(address + size, hi), 4):
            off = pc - vmaddr + fileoff
            if off < 0 or off + 4 > len(data):
                raise RuntimeError(f"symbol PC {pc:#x} maps outside Mach-O file (off={off:#x})")
            word = struct.unpack_from("<I", data, off)[0]
            kind = {0xD63F0000: "blr", 0xD61F0000: "br"}.get(word & 0xFFFFFC1F)
            if not kind:
                continue
            reg = (word >> 5) & 31
            if reg == 30:
                counts["x30"] += 1
                continue
            delta = (tramp[reg] - pc) >> 2
            if not (-(1 << 25) <= delta < (1 << 25)):
                raise RuntimeError(f"trampoline out of branch range at {pc:#x}")
            opcode = 0x94000000 if kind == "blr" else 0x14000000
            struct.pack_into("<I", data, off, opcode | (delta & 0x3FFFFFF))
            counts[kind] += 1

    image_path.write_bytes(data)
    return counts


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        sys.stderr.write(__doc__ or "")
        return 2
    counts = rewrite_rust_indirect_branches(Path(argv[1]), Path(argv[2]), argv[3])
    print(
        f"PASS: Rust indirect branches routed through PAC trampolines: "
        f"{counts['blr']} blr, {counts['br']} br, {counts['x30']} x30 left ({counts['objects']} objects)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
