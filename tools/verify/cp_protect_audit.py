#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit raw little-endian A840 PM4 for Type-4 writes into CP_PROTECT.

Type-7 payloads are stepped over according to their headers. Referenced
indirect buffers must be provided as separate streams.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from collections.abc import Collection
import json
import os
from pathlib import Path
import stat
import struct

from pm4_check import MAX_REGISTER, type4_header, type7_header

# NezhaGpu.c:3069-3088, in hardware slot order. The last number is the
# inclusive offset from the base, so a zero covers one register.
A840_PROTECT_RANGES = (
    ("RO", 0x00008, 0x039b), ("RO", 0x003b4, 0x008b), ("NA", 0x00440, 0x001f),
    ("RO", 0x00580, 0x005f), ("NA", 0x005e0, 0x011f), ("RO", 0x0074a, 0x0005),
    ("RO", 0x00759, 0x001b), ("NA", 0x00775, 0x000a), ("RO", 0x00789, 0x0000),
    ("RO", 0x0078c, 0x0013), ("NA", 0x00800, 0x0029), ("NA", 0x00837, 0x00af),
    ("RO", 0x008e7, 0x00c9), ("NA", 0x008ec, 0x00c3), ("NA", 0x009b1, 0x0250),
    ("NA", 0x00c07, 0x0008), ("RO", 0x00ce0, 0x0001), ("RO", 0x00df0, 0x0000),
    ("NA", 0x00df1, 0x0000), ("NA", 0x00e01, 0x0000), ("NA", 0x00e03, 0x1fff),
    ("NA", 0x03c00, 0x00c5), ("RO", 0x03cc6, 0x0039), ("NA", 0x03d00, 0x1fff),
    ("NA", 0x08600, 0x01ff), ("NA", 0x08e00, 0x00ff), ("RO", 0x08f00, 0x0000),
    ("NA", 0x08f01, 0x01be), ("NA", 0x09600, 0x01ff), ("RO", 0x0981a, 0x02e5),
    ("NA", 0x09e00, 0x01ff), ("NA", 0x0a600, 0x01ff), ("NA", 0x0a82e, 0x0000),
    ("NA", 0x0ae00, 0x0000), ("NA", 0x0ae02, 0x0004), ("NA", 0x0ae08, 0x0006),
    ("NA", 0x0ae10, 0x00bf), ("RO", 0x0aed0, 0x002f), ("NA", 0x0af00, 0x027f),
    ("NA", 0x0b600, 0x1fff), ("NA", 0x0dc00, 0x1fff), ("RO", 0x0fc00, 0x1fff),
    ("NA", 0x18400, 0x003f), ("RO", 0x18440, 0x013f), ("NA", 0x18580, 0x1fff),
    ("NA", 0x1b400, 0x1fff), ("NA", 0x1f400, 0x0477), ("RO", 0x1f878, 0x0507),
    ("NA", 0x1f930, 0x0329), ("NA", 0x1fd80, 0x1fff), ("NA", 0x27800, 0x007f),
    ("RO", 0x27880, 0x0385), ("NA", 0x27882, 0x0009), ("NA", 0x27c06, 0x0000),
)


def packet_header(header: int) -> tuple[int, int, int]:
    # The top nibble is the packet type. Type-4 and Type-7 share bits 31:30.
    kind = header >> 28
    if kind == 4:
        identifier, count = (header >> 8) & MAX_REGISTER, header & 0x7f
        reserved = 1 << 26
        expected = type4_header(identifier, count)
    elif kind == 7:
        identifier, count = (header >> 16) & 0x7f, header & 0x3fff
        reserved = (0xf << 24) | (1 << 14)
        expected = type7_header(identifier, count)
    else:
        raise ValueError(f"unsupported packet type {kind}")
    if header & reserved:
        raise ValueError(f"Type-{kind} reserved header bits are set")
    if header != expected:
        raise ValueError(f"Type-{kind} odd-parity error")
    return kind, identifier, count


def audit_bytes(data: bytes) -> dict:
    violations, errors = [], []
    packet_index, offset, register_writes = 0, 0, 0
    dwords = len(data) // 4
    if not data or len(data) % 4:
        errors.append({"packet_index": 0, "dword_offset": 0,
                       "error": f"expected a nonempty aligned dword stream ({len(data)} bytes)"})
    else:
        while offset < dwords:
            header, = struct.unpack_from("<I", data, offset * 4)
            try:
                kind, identifier, count = packet_header(header)
                available = dwords - offset - 1
                if count > available:
                    raise ValueError(f"truncated Type-{kind}: expected {count} payload dwords, have {available}")
            except ValueError as exc:
                errors.append({"packet_index": packet_index, "dword_offset": offset,
                               "header": header, "error": str(exc)})
                break
            if kind == 4:
                for write_index, register in enumerate(range(identifier, identifier + count)):
                    for segment_index, (access, start, length) in enumerate(A840_PROTECT_RANGES):
                        if start <= register <= start + length:
                            violations.append({
                                "packet_index": packet_index, "header_dword_offset": offset,
                                "dword_offset": offset + 1 + write_index, "register": register,
                                "register_hex": f"0x{register:05x}", "segment_index": segment_index,
                                "access": access, "segment_start": start, "segment_end": start + length,
                            })
                register_writes += count
            offset += 1 + count
            packet_index += 1
    return {"status": "FAIL" if violations or errors else "PASS", "dwords": dwords,
            "packet_count": packet_index, "register_writes": register_writes,
            "violation_count": len(violations), "violations": violations, "errors": errors}


def discover_files(path: Path, suffixes: set[str], *,
                   excluded_dirs: Collection[Path] = ()) -> list[Path]:
    """Enumerate artifact names without following directory links or Git internals."""
    if path.is_symlink():
        raise ValueError(f"symlinked scan input: {path}")
    if path.is_file() and path.suffix.lower() in suffixes:
        return [path]
    if not path.is_dir():
        raise ValueError(f"expected a directory or {sorted(suffixes)} file: {path}")

    def walk_error(error: OSError) -> None:
        raise error

    matches = []
    for directory, dirs, files in os.walk(path, onerror=walk_error):
        current = Path(directory)
        dirs[:] = [name for name in dirs if name != ".git" and current / name not in excluded_dirs]
        matches.extend(current / name for name in files
                       if Path(name).suffix.lower() in suffixes)
    return sorted(matches)


def audit_paths(inputs) -> dict:
    files, errors = set(), []
    roots = sorted({Path(path).absolute() for path in inputs})
    for root in roots:
        try:
            files.update(discover_files(root, {".pm4"}))
        except (OSError, ValueError) as exc:
            errors.append({"path": str(root), "error": str(exc)})

    rows = []
    for path in sorted(files):
        try:
            if not stat.S_ISREG(path.lstat().st_mode):
                raise ValueError("PM4 input must be a regular file")
            rows.append({"path": str(path), **audit_bytes(path.read_bytes())})
        except (OSError, ValueError) as exc:
            errors.append({"path": str(path), "error": str(exc)})
    if not rows and not errors:
        errors.append({"error": "no .pm4 streams found"})
    passed = not errors and all(row["status"] == "PASS" for row in rows)
    return {"status": "PASS" if passed else "FAIL", "chip": "A840",
            "protection_range_count": len(A840_PROTECT_RANGES), "scan_roots": list(map(str, roots)),
            "file_count": len(rows), "packet_count": sum(row["packet_count"] for row in rows),
            "register_writes": sum(row["register_writes"] for row in rows),
            "violation_count": sum(row["violation_count"] for row in rows),
            "files": rows, "errors": errors}


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--input", type=Path, action="append", required=True,
                     help="raw .pm4 file or directory to scan recursively")
    args = cli.parse_args(argv)
    report = audit_paths(args.input)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
