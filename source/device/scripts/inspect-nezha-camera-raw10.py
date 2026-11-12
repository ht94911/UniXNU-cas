#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect concatenated CSI-2 packed RAW10 frames and export a Bayer PGM."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def write_pgm(path, frame, width, height, stride):
    row = bytearray(width * 2)
    with path.open("xb") as output:
        output.write(f"P5\n{width} {height}\n1023\n".encode("ascii"))
        for y in range(height):
            start = y * stride
            for x in range(0, width, 4):
                index = start + (x // 4) * 5
                a, b, c, d, low = frame[index:index + 5]
                struct.pack_into(
                    ">4H", row, x * 2,
                    (a << 2) | (low & 3),
                    (b << 2) | ((low >> 2) & 3),
                    (c << 2) | ((low >> 4) & 3),
                    (d << 2) | (low >> 6),
                )
            output.write(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--stride", type=int, required=True, help="captured bytes per row")
    parser.add_argument("--pgm", type=Path, help="new PGM file for a selected frame")
    parser.add_argument("--frame", type=int, default=0, help="zero-based frame to export")
    args = parser.parse_args()

    width, height, stride = args.width, args.height, args.stride
    if width <= 0 or width % 4 or height <= 0 or stride < (width // 4) * 5:
        parser.error("RAW10 requires groups of four pixels and a stride covering every group")
    frame_bytes = height * stride
    count, remainder = divmod(args.capture.stat().st_size, frame_bytes)
    if remainder or count == 0:
        parser.error("capture is empty or contains a partial frame for this geometry")
    if args.pgm and not 0 <= args.frame < count:
        parser.error("selected frame is outside the capture")

    hashes = []
    with args.capture.open("rb") as capture:
        for index in range(count):
            frame = capture.read(frame_bytes)
            if len(frame) != frame_bytes:
                parser.error("capture was truncated during read")
            hashes.append(hashlib.sha256(frame).hexdigest())
            if args.pgm and index == args.frame:
                write_pgm(args.pgm, frame, width, height, stride)
        if capture.read(1):
            parser.error("capture grew during read")

    print(json.dumps({"width": width, "height": height, "stride": stride,
                      "frame_bytes": frame_bytes, "frame_count": count,
                      "distinct_frame_hashes": len(set(hashes)), "sha256_per_frame": hashes},
                     indent=2))


if __name__ == "__main__":
    main()
