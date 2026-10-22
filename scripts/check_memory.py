#!/usr/bin/env python3
"""Validate the cas candidate map and optional actual build/runtime sizes.

Offline only. This proves interval constraints, not ABL placement or hardware
accessibility. Firmware must enforce the matching entry-time constraints.
"""
import argparse
import json
from pathlib import Path

LIMIT64 = 1 << 64


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError("addresses/sizes must be integer or integer text")
    return int(value, 0) if isinstance(value, str) else int(value)


def interval(item):
    base, size = number(item["base"]), number(item["size"])
    if base < 0 or size <= 0 or base >= LIMIT64 or size > LIMIT64 - base:
        raise ValueError(f"invalid/overflowing interval: {item.get('name', item)}")
    return base, base + size


def inside(a, b):
    return b[0] <= a[0] and a[1] <= b[1]


def overlaps(a, b):
    return max(a[0], b[0]) < min(a[1], b[1])


def validate_map(config):
    if config.get("schema_version") != 1 or config.get("board") != "cas":
        raise ValueError("unsupported map schema or wrong board")
    banks = [interval(x) for x in config["ram_banks"]]
    protected = [(x["name"], interval(x)) for x in config["protected"]]
    regions = []
    names = set()
    for item in config["regions"]:
        name, span = item["name"], interval(item)
        alignment = number(item.get("alignment", 4096))
        if alignment < 4096 or alignment & (alignment - 1) or span[0] % alignment or span[1] % alignment:
            raise ValueError(f"unaligned region: {name}")
        if name in names:
            raise ValueError("duplicate region name")
        names.add(name)
        if not any(inside(span, bank) for bank in banks):
            raise ValueError(f"region outside a RAM bank: {name}")
        if span[1] > 1 << 32:
            raise ValueError(f"cas first-stage region exceeds the audited low-4GB window: {name}")
        for protected_name, blocked in protected:
            if overlaps(span, blocked):
                raise ValueError(f"{name} overlaps protected {protected_name}")
        for previous_name, previous in regions:
            if overlaps(span, previous):
                raise ValueError(f"{name} overlaps {previous_name}")
        regions.append((name, span))
    required = {"dxe_heap", "uefi_fd", "dtb_copy", "uefi_stack", "boot_log", "payload_copy", "kernel_input", "upper_input"}
    if names != required:
        raise ValueError("missing/unexpected candidate region")
    for item in config["mmio"]:
        span = interval(item)
        if span[0] % 4096 or span[1] % 4096 or any(overlaps(span, bank) for bank in banks):
            raise ValueError("MMIO alignment/RAM overlap")
    for item in config["input_windows"].values():
        span = interval(item)
        if not any(inside(span, bank) for bank in banks):
            raise ValueError("input window outside RAM")
        if any(overlaps(span, blocked) for _, blocked in protected):
            raise ValueError("input window intersects a protected region")
    return dict(regions)


def validate_sizes(config, sizes):
    regions = validate_map(config)
    for name, size in sizes.items():
        size = number(size)
        if size < 0:
            raise ValueError("negative input size")
        limits = config["limits"][name]
        if size < number(limits.get("min_bytes", 0)) or size > number(limits["max_bytes"]) or (limits.get("exact") and size != number(limits["max_bytes"])):
            raise ValueError(f"{name} size violates its build bound: {size}")
    for part, region in (("fd", "uefi_fd"), ("dtb", "dtb_copy"), ("ramdisk", "payload_copy")):
        if part in sizes and number(sizes[part]) > regions[region][1] - regions[region][0]:
            raise ValueError(f"{part} exceeds its destination")
    if "fd" in sizes and "shim" in sizes:
        span = regions["kernel_input"]
        if number(sizes["fd"]) + number(sizes["shim"]) + 0x80000 > span[1] - span[0]:
            raise ValueError("decompressed kernel exceeds its allowed corridor")


def validate_runtime(config, actual, sizes=None):
    regions = validate_map(config)
    spans = {}
    for name in ("kernel", "dtb", "ramdisk"):
        if name == "ramdisk" and number(actual[name]["size"]) == 0:
            continue
        span = interval(actual[name])
        if not inside(span, interval(config["input_windows"][name])):
            raise ValueError(f"runtime {name} is outside its audited input window")
        if name == "kernel" and span[0] % 4096:
            raise ValueError("unaligned kernel entry")
        if name == "dtb" and (span[0] % 8 or span[1] - span[0] < 40):
            raise ValueError("invalid FDT address/length")
        for role in ("uefi_fd", "dxe_heap", "dtb_copy", "uefi_stack", "boot_log"):
            if overlaps(span, regions[role]):
                raise ValueError(f"runtime {name} would be overwritten by {role}")
        for previous, occupied in spans.items():
            if overlaps(span, occupied):
                raise ValueError(f"runtime {name} overlaps {previous}")
        spans[name] = span
        if name in ("dtb", "ramdisk") and span[1] - span[0] > number(config["limits"][name]["max_bytes"]):
            raise ValueError(f"runtime {name} exceeds destination capacity")
    if sizes and "ramdisk" in sizes and number(actual["ramdisk"]["size"]) != number(sizes["ramdisk"]):
        raise ValueError("runtime initrd size differs from packaged FAT")
    return spans


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", type=Path, default=Path(__file__).resolve().parents[1] / "config/memory-map.json")
    for name in ("fd", "dtb", "ramdisk", "shim", "boot_image"):
        ap.add_argument("--" + name.replace("_", "-"), type=Path)
    ap.add_argument("--runtime-json", type=Path)
    args = ap.parse_args()
    config = json.loads(args.map.read_text())
    sizes = {name: getattr(args, name).stat().st_size for name in ("fd", "dtb", "ramdisk", "shim", "boot_image") if getattr(args, name)}
    validate_sizes(config, sizes)
    if args.runtime_json:
        validate_runtime(config, json.loads(args.runtime_json.read_text()), sizes)
    print(json.dumps({"result": "pass", "board": "cas", "regions": len(config["regions"]), "protected": len(config["protected"]),
                      "actual_sizes": sizes, "runtime_record_checked": bool(args.runtime_json), "hardware_proven": False}, indent=2))


if __name__ == "__main__":
    main()
