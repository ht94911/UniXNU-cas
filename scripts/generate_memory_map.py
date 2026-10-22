#!/usr/bin/env python3
"""Generate/check the cas C map from the accepted machine-readable design."""
import argparse
import json
from pathlib import Path
from check_memory import number, validate_map


def render(config):
    validate_map(config)
    lines = ["// SPDX-License-Identifier: BSD-2-Clause-Patent", "// Generated from " + config["design_id"] + "; do not hand-edit.",
             "#include <Library/MemoryMapLib.h>", "", "STATIC EFI_MEMORY_REGION_DESCRIPTOR mRegions[] = {"]
    for region in config["regions"]:
        role = region["name"]
        name = {"dxe_heap": "DXE Heap", "uefi_fd": "UEFI FD", "uefi_stack": "UEFI Stack"}.get(role, role)
        resource = "SYS_MEM" if role in ("dxe_heap", "uefi_fd", "uefi_stack") else "MEM_RES"
        types = {"dxe_heap": "Conv", "uefi_fd": "BsData", "uefi_stack": "BsData"}
        attr = "ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP"
        if role in ("dxe_heap", "uefi_fd"):
            # Heap must execute the decompressed DXE Core before its protection
            # policy takes effect. Do not pretend WRITE_BACK_XN is real NX.
            attr = "ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK"
        if role == "boot_log":
            attr = "ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED"
        lines.append(f'  {{"{name}", 0x{number(region["base"]):x}, 0x{number(region["size"]):x}, AddMem, {resource}, SYS_MEM_CAP, {types.get(role, "Reserv")}, {attr}}},')
    for index, region in enumerate(config["protected"]):
        splash = number(region["base"]) == 0x9c000000 and number(region["size"]) == 0x2300000
        name = "Display Reserved" if splash else "protected_%02d" % index
        hob = "AddMem" if splash else "HobOnlyNoCacheSetting"
        lines.append(f'  {{"{name}", 0x{number(region["base"]):x}, 0x{number(region["size"]):x}, {hob}, MEM_RES, UNCACHEABLE, Reserv, ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED}},')
    for region in config["mmio"]:
        lines.append(f'  {{"{region["name"]}", 0x{number(region["base"]):x}, 0x{number(region["size"]):x}, AddDev, MMAP_IO, UNCACHEABLE, MmIO, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE}},')
    lines += ["};", "", "VOID GetMemoryMap (EFI_MEMORY_REGION_DESCRIPTOR **Map, UINT8 *Count) {",
              "  *Map = mRegions;", "  *Count = ARRAY_SIZE (mRegions);", "}", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uefi", type=Path, required=True)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    cfg = json.loads((Path(__file__).resolve().parents[1] / "config/memory-map.json").read_text())
    content = render(cfg)
    target = args.uefi / "Platforms/Xiaomi/casPkg/Library/MemoryMapLib/MemoryMapLib.c"
    if args.write:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    elif not target.is_file() or target.read_text() != content:
        raise SystemExit("cas C memory map differs from the accepted JSON; regenerate/review before building")
    print(json.dumps({"result": "pass", "mode": "write" if args.write else "compare", "design_id": cfg["design_id"], "regions": 37}))
