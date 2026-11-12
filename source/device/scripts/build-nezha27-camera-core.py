#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the freestanding camera core for the selected XNU arm64e port."""

import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MODULES = ("Cci", "Ovx10500u", "Rdi1080", "Camss1080", "Dma", "Camcc", "MainCapture")


def tool(env_name, mac_name, local_name):
    override = os.environ.get(env_name)
    if override:
        return [override]
    return ["xcrun", mac_name] if sys.platform == "darwin" else [local_name]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/nezha27-camera-core")
    args = parser.parse_args()

    kit = ROOT / "kits/nezha/camera"
    smmu = ROOT / "kits/ArmSmmu4K"
    staged = kit.is_dir()
    modules = MODULES
    if staged:
        from kernel_target import check_object

        helper = runpy.run_path(str(ROOT / "scripts/build-sheng-source-drivers.py"))
        record = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json"
        compile_record = json.loads(record.read_text().strip().rstrip(","))
        modules += ("Probe",)
    else:
        kit = ROOT / "package/camera"
        smmu = ROOT / "package/gpu/adreno"

    cc = tool("CAMERA_TARGET_CC", "clang", "clang")
    ar = tool("CAMERA_ARCHIVE_TOOL", "ar", "llvm-ar")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    objects = []
    for module in modules:
        name = "NezhaCamera" + module
        source = kit / (name + ".c")
        obj = out / (name + ".arm64e.o")
        if staged:
            command = helper["compile_command"](compile_record, source, obj, ["-I", str(smmu)], [])
            subprocess.run(command, cwd=compile_record["directory"], check=True)
            check_object(obj.read_bytes(), name)
        else:
            subprocess.run(cc + ["-target", "arm64e-apple-ios27.0", "-std=c11", "-O2",
                "-ffreestanding", "-mgeneral-regs-only", "-fno-stack-protector",
                "-Wall", "-Wextra", "-Wconversion", "-Werror", "-I", str(smmu),
                "-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))

    archive = out / "libNezha27CameraCore.a"
    temporary = out / ".libNezha27CameraCore.a"
    temporary.unlink(missing_ok=True)
    subprocess.run(ar + ["rcs", str(temporary), *objects], check=True)
    temporary.replace(archive)
    subprocess.run(ar + ["t", str(archive)], check=True)


if __name__ == "__main__":
    main()
