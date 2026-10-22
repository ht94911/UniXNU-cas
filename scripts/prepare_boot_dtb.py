#!/usr/bin/env python3
"""Prepare the pinned stock SoC description without losing DTBO symbols.

This handles device-tree data, never executable firmware. The caller supplies
the private source explicitly. Only totalsize and trailing zero padding change;
no DTS text roundtrip, phandle rewrite or runtime/live tree is accepted.
"""
import argparse
import hashlib
import json
import struct
import tomllib
from pathlib import Path


def require(ok, message):
    if not ok:
        raise ValueError(message)


def prepare(data, policy, max_bytes=0x100000):
    require(policy.get("source") == "stock-soc-dtb", "unsupported DTB source policy")
    require(len(data) == policy["source_bytes"], "stock DTB source size mismatch")
    require(hashlib.sha256(data).hexdigest() == policy["source_sha256"], "stock DTB source hash mismatch")
    require(len(data) >= 56, "truncated stock DTB header")
    magic, total, structure, strings, reserve, version, compatible, _, strings_size, structure_size = struct.unpack_from(">10I", data)
    require(magic == 0xd00dfeed and total == len(data), "stock input must be exactly one complete DTB")
    require(version == 17 and compatible <= 17 and structure % 4 == 0 and reserve % 8 == 0,
            "unsupported stock DTB header")
    require(40 <= structure <= total and structure_size <= total - structure and
            40 <= strings <= total and strings_size <= total - strings and
            40 <= reserve <= min(structure, strings) - 16, "invalid stock DTB block bounds")
    require(structure + structure_size <= strings or strings + strings_size <= structure,
            "overlapping stock DTB blocks")
    # The hash pins all properties and reserve entries. Never discard live data
    # or a second appended tree under the guise of normalizing the padding.
    tree_end = max(structure + structure_size, strings + strings_size)
    require(not any(data[tree_end:]), "nonzero stock DTB trailing data")
    padding = policy["padding"]
    require(padding == 8192 and tree_end + padding <= max_bytes, "DTB padding or size exceeds policy")
    result = bytearray(data[:tree_end] + bytes(padding))
    struct.pack_into(">I", result, 4, len(result))
    require(hashlib.sha256(result).hexdigest() == policy["prepared_sha256"], "prepared DTB hash mismatch")
    return bytes(result)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("stock-dtb", "config", "map", "out"):
        ap.add_argument("--" + name, type=Path, required=True)
    a = ap.parse_args()
    try:
        config = tomllib.loads(a.config.read_text())
        memory = json.loads(a.map.read_text())
        data = prepare(a.stock_dtb.read_bytes(), config["boot_image_dtb"], memory["limits"]["dtb"]["max_bytes"])
        with a.out.open("xb") as stream:
            stream.write(data)
        print(json.dumps({"result": "pass", "source": str(a.stock_dtb.resolve()),
                          "source_sha256": config["boot_image_dtb"]["source_sha256"],
                          "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                          "hardware_proven": False}, indent=2))
        return 0
    except (OSError, ValueError, KeyError) as exc:
        print(json.dumps({"result": "fail", "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
