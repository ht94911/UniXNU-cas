#!/usr/bin/env python3
"""Cross-check actual cas C/DSC/FDF/shim/config constants against the memory map."""
import argparse
import json
import re
import tomllib
from pathlib import Path

from check_memory import number, validate_map
from generate_memory_map import render


def check(uefi, device):
    pkg = uefi / "Platforms/Xiaomi/casPkg"
    memory = json.loads((device / "config/memory-map.json").read_text())
    validate_map(memory)
    config = tomllib.loads((uefi / "Resources/Configs/cas.toml").read_text())
    macros = dict(re.findall(r"^#define\s+(CAS_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$", (pkg / "Include/CasPlatform.h").read_text(), re.M))
    regions = {r["name"]: r for r in memory["regions"]}
    names = {"HEAP": "dxe_heap", "FD": "uefi_fd", "FDT": "dtb_copy", "STACK": "uefi_stack", "LOG": "boot_log", "KERNEL": "kernel_input", "PAYLOAD": "payload_copy"}
    for macro, role in names.items():
        for key, field in (("BASE", "base"), ("SIZE", "size")):
            if number(macros.get(f"CAS_{macro}_{key}", "-1")) != number(regions[role][field]):
                raise ValueError(f"C header disagrees with memory design: CAS_{macro}_{key}")
    if number(macros["CAS_TEXT_OFFSET"]) != config["boot_shim"]["text_offset"] or config["boot_shim"]["text_offset"] != 0x80000:
        raise ValueError("ARM64 text_offset mismatch")
    if number(macros["CAS_INPUT_END"]) != number(memory["input_windows"]["dtb"]["base"]) + number(memory["input_windows"]["dtb"]["size"]):
        raise ValueError("input window end mismatch")
    if (pkg / "Library/MemoryMapLib/MemoryMapLib.c").read_text() != render(memory):
        raise ValueError("C memory descriptors differ from the accepted generated map")
    dsc = (pkg / "cas.dsc").read_text()
    pcds = dict(re.findall(r"^\s*(g\w+\.Pcd\w+)\|([^#\n]+)", dsc, re.M))
    expected_pcds = {
        "gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase": number(regions["uefi_stack"]["base"]),
        "gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize": number(regions["uefi_stack"]["size"]),
        "gArmTokenSpaceGuid.PcdGicDistributorBase": 0x17a00000,
        "gArmTokenSpaceGuid.PcdGicRedistributorsBase": 0x17a60000,
        "gArmPlatformTokenSpaceGuid.PcdCoreCount": 8,
    }
    for key, value in expected_pcds.items():
        if number(pcds.get(key, "-1").strip()) != value:
            raise ValueError(f"DSC board constant mismatch: {key}")
    if not re.search(r"DEFINE\s+SOC_TYPE\s*=\s*0\s*$", dsc, re.M):
        raise ValueError("cas requires SOC_TYPE=0 (SM8250)")
    linker = (uefi / "BootShim/CasBootShim.lds").read_text()
    value = re.search(r"_ImageSize\s*=\s*_Payload\s*\+\s*(0x[0-9a-fA-F]+)", linker)
    if not value or int(value[1], 16) != config["uefi_fd"]["size"]:
        raise ValueError("shim linker image size differs from FD size")
    fdf = (pkg / "cas.fdf").read_text()
    for required in ("BaseAddress = $(FD_BASE)|gArmTokenSpaceGuid.PcdFdBaseAddress",
                     "Size = $(FD_SIZE)|gArmTokenSpaceGuid.PcdFdSize", "NumBlocks = $(FD_BLOCKS)",
                     "INF casPkg/Sec/Sec.inf"):
        if required not in fdf:
            raise ValueError("FDF layout does not bind the validated FD/SEC values")
    return {"result": "pass", "board": "cas", "soc": "SM8250", "design_id": memory["design_id"],
            "hardware_proven": False, "header_macros": macros, "pcds_checked": expected_pcds,
            "boot_header_version": config["boot_image"]["header_version"], "text_offset": "0x80000"}


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uefi", type=Path, required=True)
    args = ap.parse_args()
    print(json.dumps(check(args.uefi, Path(__file__).resolve().parents[1]), indent=2))
