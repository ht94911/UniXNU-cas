#!/usr/bin/env python3
"""Package a verified cas full build into a new, audited base boot image.

No adb/fastboot, network, repository update, persistent-variable or partition
operation exists here. This F-04 base profile always has an empty ramdisk.
"""
import argparse
import gzip
import json
import os
import platform
import shutil
import subprocess
import sys
import tomllib
import zlib
from datetime import datetime, timezone
from pathlib import Path

from build_shim import build as build_shim
from check_boot import inspect
from provenance import check_ref, file_ref, require_clean, source_state

DEVICE = Path(__file__).resolve().parents[1]
EPOCH = "1792643741"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uefi", type=Path, required=True)
    ap.add_argument("--firmware-build", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--environment", type=Path, required=True)
    a = ap.parse_args()
    uefi, build, out = a.uefi.resolve(), a.firmware_build.resolve(), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(SOURCE_DATE_EPOCH=EPOCH, PYTHONHASHSEED="0", LC_ALL="C.UTF-8", TZ="UTC")
    run = {"started_at": datetime.now(timezone.utc).isoformat(), "result": "running", "board": "cas",
           "build_id": out.name, "hardware_proven": False, "commands": [], "profile": "base-empty-ramdisk"}
    run_path = out / "package-run.json"

    def save_run():
        run_path.write_text(json.dumps(run, indent=2) + "\n")

    def command(argv, log_name):
        argv = [str(arg) for arg in argv]
        item = {"argv": argv, "cwd": str(uefi), "log": log_name}
        run["commands"].append(item)
        save_run()
        with (out / log_name).open("x") as stream:
            result = subprocess.run(argv, cwd=uefi, env=env, stdout=stream, stderr=subprocess.STDOUT)
        item["returncode"] = result.returncode
        if result.returncode:
            raise RuntimeError(log_name + " failed; preserved output is not a usable image")

    try:
        sources = source_state(uefi, DEVICE)
        require_clean(sources)
        build_run_path = build / "run.json"
        build_run = json.loads(build_run_path.read_text())
        if build_run["phase"] != "build" or build_run["result"] != "pass":
            raise ValueError("a successful full-FD build is required")
        for key in ("source_state", "fd", "build_report"):
            check_ref(build_run[key])
        if json.loads(Path(build_run["source_state"]["path"]).read_text()) != sources:
            raise ValueError("current sources differ from the successful full build")
        command([sys.executable, DEVICE / "scripts/preflight.py", "--uefi", uefi, "--out", out / "preflight-target.json"], "preflight-target.log")
        command([sys.executable, DEVICE / "scripts/check_target.py", "--uefi", uefi], "target-constants.log")
        config_path = uefi / "Resources/Configs/cas.toml"
        memory_path = DEVICE / "config/memory-map.json"
        config = tomllib.loads(config_path.read_text())
        memory = json.loads(memory_path.read_text())
        versions = {"python": sys.version, "platform": platform.platform(), "zlib": zlib.ZLIB_RUNTIME_VERSION}
        for program in ("clang", "lld-link", "ld.lld", "llvm-objcopy", "dtc", "git"):
            versions[program] = subprocess.check_output([program, "--version"], env=env, text=True).strip()
        if "version 18.1.3" not in versions["clang"] or "1.7.0" not in versions["dtc"]:
            raise ValueError("expected the fixed LLVM18.1.3 and dtc1.7.0 environment")
        command([sys.executable, "-m", "pip", "freeze"], "python-packages.txt")

        fd_path = out / "SILICIUM_UEFI.fd"
        shutil.copyfile(build_run["fd"]["path"], fd_path)
        dtb_path, dts_path = out / "cas.dtb", uefi / "Resources/DTBs/cas.dts"
        command(["dtc", "-I", "dts", "-O", "dtb", "-p", str(config["boot_image_dtb"]["padding"]), "-o", dtb_path, dts_path], "dtc-build.log")
        command(["dtc", "-I", "dtb", "-O", "dts", "-o", out / "cas-unpacked.dts", dtb_path], "dtc-readback.log")
        shim_path = build_shim(uefi, out / "shim")
        ramdisk_path = out / "ramdisk.empty"
        ramdisk_path.write_bytes(b"")
        kernel_path = out / "kernel.gz"
        kernel_path.write_bytes(gzip.compress(shim_path.read_bytes() + fd_path.read_bytes(), compresslevel=9, mtime=0))
        image_path = out / "boot.img"
        b = config["boot_image"]
        cmd = [sys.executable, uefi / "Resources/Scripts/mkbootimg.py", "--kernel", kernel_path,
               "--ramdisk", ramdisk_path, "--dtb", dtb_path, "--board", "cas", "--cmdline", b["cmdline"]]
        for key in ("header_version", "pagesize", "base", "kernel_offset", "ramdisk_offset", "dtb_offset", "tags_offset"):
            cmd += ["--" + key, str(b[key])]
        command([*cmd, "--output", image_path, "--id"], "mkbootimg.log")
        check, parts = inspect(image_path.read_bytes(), fd_path.read_bytes(), shim_path.read_bytes(), dtb_path.read_bytes(), b"", config, memory)
        (out / "unpacked").mkdir()
        for name, data in parts.items():
            (out / "unpacked" / (name + ".bin")).write_bytes(data)
        (out / "image-check.json").write_text(json.dumps(check, indent=2) + "\n")
        command([sys.executable, DEVICE / "scripts/preflight.py", "--uefi", uefi, "--stage", "artifacts",
                 "--fd", fd_path, "--dtb", dtb_path, "--shim", shim_path, "--boot-image", image_path,
                 "--ramdisk", ramdisk_path, "--out", out / "preflight-artifacts.json"], "preflight-artifacts.log")
        (out / "firmware").mkdir()
        for source in build.iterdir():
            if source.is_file() and (source.suffix in (".json", ".log") or source.name == "BuildReport.txt"):
                shutil.copyfile(source, out / "firmware" / source.name)
        for source in (build / "Build/DEBUG_CLANGPDB/FV").iterdir():
            if source.is_file() and source.suffix.lower() in (".map", ".txt", ".xref", ".fv"):
                shutil.copyfile(source, out / "firmware" / source.name)
        (out / "modules").mkdir()
        for source in (build / "Build/DEBUG_CLANGPDB/AARCH64").glob("*.efi"):
            shutil.copyfile(source, out / "modules" / source.name)
        for source in (build / "Build/DEBUG_CLANGPDB/AARCH64").rglob("DEBUG/*.map"):
            relative = source.relative_to(build / "Build/DEBUG_CLANGPDB/AARCH64")
            target = out / "maps" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        if sources != source_state(uefi, DEVICE):
            raise ValueError("sources changed during packaging")
        input_files = [a.environment.resolve(), build_run_path, Path(build_run["source_state"]["path"]),
                       config_path, dts_path, memory_path, uefi / "Resources/Scripts/mkbootimg.py"]
        input_files += sorted((uefi / "Platforms/Xiaomi/casPkg").rglob("*"))
        input_files += sorted((uefi / "BootShim").glob("CasBoot*"))
        input_files += sorted((DEVICE / "config").glob("*.json"))
        input_files += sorted((DEVICE / "scripts").glob("*.py"))
        manifest = {"schema_version": 1, "board": "cas", "build_id": out.name, "profile": run["profile"],
                    "result": "pass", "hardware_proven": False, "repositories": sources,
                    "source_date_epoch": EPOCH, "gzip_mtime": 0, "tools": versions,
                    "build_run": build_run, "pack_commands": run["commands"],
                    "inputs": [file_ref(p) for p in sorted(set(input_files)) if p.is_file()],
                    "vendor_inputs": [], "efi_payload": None, "ramdisk": file_ref(ramdisk_path),
                    "image": file_ref(image_path), "image_checks": check,
                    "outputs": [file_ref(p) for p in sorted(out.rglob("*")) if p.is_file() and p != run_path]}
        manifest_path = out / "source-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        run.update(result="pass", image=file_ref(image_path), source_manifest=file_ref(manifest_path))
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        run.update(result="fail", error=str(exc))
    run["ended_at"] = datetime.now(timezone.utc).isoformat()
    save_run()
    print(json.dumps({k: run[k] for k in ("result", "image", "source_manifest", "error") if k in run}, indent=2))
    return 0 if run["result"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
