#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Read the local 24A446 donor and prepare entries for the existing text-fixups parser.

This is a source-stage inspection tool. It does not patch the donor, install
anything, compile, package an image or touch the phone. After a test package is
selected, append these entries to that isolated package's existing ADT property.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import uuid

DONOR_SHA256 = "d00659f0aef2475c0cb1f5358fff411229cbcbd0883b51fc470d40a87a300b18"
DONOR_UUID = "1479ae4d-23f4-32da-85f6-82c2a31afb7f"
# Actual control flow, independently traced to the original Eureka constructor
# and QMI parsed-position reader. Branching to the existing constructor preserves
# its authenticated vtable, callbacks, original location privacy and app path.
FIXUPS = (
    (0x176E0E0, bytes.fromhex("20020034"), struct.pack("<I", 0x14000013)),
    # Original setEventReport takes numerous hardware reports absent on this
    # board. Its mask->TLV builder receives only parsedPosition (bit 1).
    (0x1773374, bytes.fromhex("0111941a"), struct.pack("<I", 0x52800041)),
)
GUARDS = (
    (0x176E0D8, "6b6ea2979674a897"),  # original device probes before selection
    (0x176E12C, "e81b0191e81300f9"),  # existing Eureka constructor argument path
    (0x176E5E8, "41cffd97"),         # original authenticated Eureka constructor
)


def entries(image):
    if hashlib.sha256(image).hexdigest() != DONOR_SHA256:
        raise ValueError("Expected the inspected original iPhone14,4/24A446 locationd")
    magic, cpu = struct.unpack_from("<II", image)
    if magic != 0xFEEDFACF or cpu != 0x100000C:
        raise ValueError("Expected the thin arm64 Mach-O donor")
    at, found_uuid = 32, None
    for _ in range(struct.unpack_from("<I", image, 16)[0]):
        command, length = struct.unpack_from("<II", image, at)
        if length < 8 or at + length > len(image):
            raise ValueError("Malformed Mach-O load command")
        if command == 0x1B:
            found_uuid = str(uuid.UUID(bytes=image[at + 8:at + 24]))
        at += length
    if found_uuid != DONOR_UUID:
        raise ValueError("locationd UUID differs")
    for offset, expected in GUARDS:
        raw = bytes.fromhex(expected)
        if image[offset:offset + len(raw)] != raw:
            raise ValueError(f"Original consumer control flow differs at {offset:x}")
    out = []
    for offset, original, replacement in FIXUPS:
        if image[offset:offset + len(original)] != original:
            raise ValueError("Original consumer instruction differs")
        # File page substitution already verifies original signed source pages.
        row = f"locationd,{offset:x},{original.hex()},{replacement.hex()}".encode("ascii")
        if len(row) >= 160 or offset % 4096 + len(original) > 4096:
            raise ValueError("Entry exceeds the existing signed-page parser")
        out.append(row + b"\0")
    return b"".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locationd", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="New local file containing the two NUL-terminated entries")
    args = parser.parse_args()
    try:
        result = entries(args.locationd.read_bytes())
        if args.output:
            with args.output.open("xb") as stream:
                stream.write(result)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, str(error) + "\n")
    print(result.replace(b"\0", b"\n").decode("ascii"), end="")


if __name__ == "__main__":
    main()
