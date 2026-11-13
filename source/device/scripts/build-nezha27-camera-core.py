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
MODULES = ("Cci", "Ovx10500u", "Rdi1080", "Camss1080", "Csiphy24", "Dma", "Camcc", "Crm", "Cpas", "Profile", "MainCapture", "MainPins", "Rpmh", "IfeBandwidth", "IfeRsc", "CaptureResources", "FrameProbe", "IrqQueue")


def tool(env_name, mac_name, local_name):
    override = os.environ.get(env_name)
    if override:
        return [override]
    return ["xcrun", mac_name] if sys.platform == "darwin" else [local_name]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/nezha27-camera-core")
    parser.add_argument("--shared-soc", type=Path, default=os.environ.get("NEZHA_CAMERA_SHARED_SOC"),
                        help="selected SM8850 native owner source with include/ and src/")
    args = parser.parse_args()

    kit = ROOT / "kits/nezha/camera"
    smmu = ROOT / "kits/ArmSmmu4K"
    staged = kit.is_dir()
    modules = MODULES
    if staged:
        from kernel_target import check_object

        helper = runpy.run_path(str(ROOT / "scripts/build-sheng-source-drivers.py"))
        objects_dir = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
        records = {}
        for suffix, relative in ((".c", "bsd/DEVELOPMENT/bsd_init.o.json"),
                                 (".cpp", "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json")):
            records[suffix] = json.loads((objects_dir / relative).read_text().strip().rstrip(","))
        modules += ("Journal", "Probe")
    else:
        kit = ROOT / "package/camera"
        smmu = ROOT / "package/gpu/adreno"

    cc = tool("CAMERA_TARGET_CC", "clang", "clang")
    ar = tool("CAMERA_ARCHIVE_TOOL", "ar", "llvm-ar")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    objects = []
    sources = [kit / ("NezhaCamera" + module + ".c") for module in modules]
    sources.append(kit / "upstream/QcomRpmh.c")
    if staged:
        sources.append(kit / "NezhaCameraNativeMemory.cpp")
        if args.shared_soc is not None:
            shared = args.shared_soc.resolve()
            sources.extend(shared / "src" / name for name in ("Sm8850Resources.c", "Sm8850ResourcesNative.cpp"))
            sources.extend(kit / ("NezhaCameraNative" + name + ".cpp") for name in ("Smmu", "Rpmh", "Gcc", "Irq", "Capture"))
    elif args.shared_soc is not None:
        parser.error("C2 shared-SMMU adapter requires the native staged build")
    for source in sources:
        name = source.stem
        obj = out / (name + ".arm64e.o")
        if staged:
            includes = ["-I", str(smmu)]
            if name == "NezhaCameraProbe":
                includes += ["-I", str(ROOT / "drivers/QcomXnuUsbDebug/include"),
                             "-I", str(ROOT / "kits/nezha/nezha27-input")]
                if args.shared_soc is not None:
                    includes += ["-DNEZHA_CAMERA_NATIVE_CAPTURE=1"]
            if source.suffix == ".cpp":
                includes += ["-I", str(ROOT / "refer/Adreno740"), "-I", str(ROOT / ".cache/xnu-work/osfmk")]
            if args.shared_soc is not None and name in ("NezhaCameraNativeSmmu", "NezhaCameraNativeRpmh",
                    "NezhaCameraNativeGcc", "Sm8850Resources", "Sm8850ResourcesNative"):
                includes += ["-I", str(shared / "include")]
            compile_record = records[source.suffix]
            command = helper["compile_command"](compile_record, source, obj, includes, [])
            subprocess.run(command, cwd=compile_record["directory"], check=True)
            check_object(obj.read_bytes(), name)
        else:
            subprocess.run(cc + ["-target", "arm64e-apple-ios27.0", "-std=c11", "-O2",
                "-ffreestanding", "-mgeneral-regs-only", "-fno-stack-protector",
                "-Wall", "-Wextra", "-Wconversion", "-Werror", "-I", str(smmu),
                "-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))

    slot = kit / 'NezhaCameraProfileSlot.S'
    slot_object = out / 'NezhaCameraProfileSlot.arm64e.o'
    subprocess.run(cc + ['-target', 'arm64e-apple-ios27.0', '-c', str(slot), '-o', str(slot_object)], check=True)
    if staged:
        check_object(slot_object.read_bytes(), slot.name)
    objects.append(str(slot_object))

    archive = out / "libNezha27CameraCore.a"
    temporary = out / ".libNezha27CameraCore.a"
    temporary.unlink(missing_ok=True)
    subprocess.run(ar + ["rcs", str(temporary), *objects], check=True)
    temporary.replace(archive)
    subprocess.run(ar + ["t", str(archive)], check=True)


if __name__ == "__main__":
    main()
