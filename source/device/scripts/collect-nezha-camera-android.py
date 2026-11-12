#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect camera topology and module names during an assigned Android window."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


MODULE_DIRS = (
    "/vendor/lib64/camera", "/odm/lib64/camera", "/vendor/lib64/hw",
    "/odm/lib64/hw", "/vendor/etc/camera", "/odm/etc/camera",
    "/vendor_dlkm/lib/modules",
)
CAMERA_EVENT = re.compile(rb"cam_sensor|cam_cci|cam_csiphy|cam_ife|cam_smmu|camx|ovx10500|s5kjn5|s5khpe", re.I)
DEBUG_ROOTS = ("/sys/kernel/debug", "/dev/_nungsd_/viickvzr")


def collect(args):
    adb = [str(args.adb), "-H", args.host, "-P", str(args.port), "-s", args.serial]

    def read(*command, optional=False):
        try:
            result = subprocess.run(adb + list(command), capture_output=True, timeout=15)
        except subprocess.TimeoutExpired:
            if optional:
                return None
            raise
        if result.returncode:
            if optional:
                return None
            raise RuntimeError(f"ADB returned {result.returncode} for {command[0]}")
        return result.stdout

    if read("get-state").strip() != b"device":
        raise RuntimeError("ADB device is unavailable")
    if read("exec-out", "getprop", "sys.boot_completed").strip() != b"1":
        raise RuntimeError("Android has not finished booting")
    if read("exec-out", "getprop", "ro.product.device").strip() != b"nezha":
        raise RuntimeError("Device is not nezha")

    # Use a fresh private directory. The full media.camera diagnostic can
    # contain recent client activity and stays with the other device evidence.
    args.output.mkdir(parents=True, exist_ok=False)
    saved = {}

    def save(name, data):
        (args.output / name).write_bytes(data)
        saved[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    build = read("exec-out", "getprop", "ro.vendor.build.fingerprint")
    save("vendor-build.txt", build)
    service = read("exec-out", "dumpsys", "-t", "10", "media.camera")
    if not service.strip() or b"DUMP TIMEOUT" in service or b"Can't find service" in service:
        raise RuntimeError("The Android camera service did not return a complete dump")
    save("media-camera.txt", service)

    listing = []
    for directory in MODULE_DIRS:
        names = read("exec-out", "ls", "-1", directory, optional=True)
        if names is None or names.startswith(b"ls:"):
            continue
        for name in names.decode("utf-8", errors="replace").splitlines():
            if directory.endswith("/hw") and not any(x in name.lower() for x in ("cam", "isp")):
                continue
            if directory.endswith("/modules") and "cam" not in name.lower():
                continue
            listing.append(f"{directory}/{name}")
    save("camera-module-paths.txt", ("\n".join(listing) + "\n").encode("utf-8"))

    def root_read(command):
        return read("exec-out", "su", "-c", shlex.quote(command), optional=True)

    if (root_read("id -u") or b"").strip() == b"0":
        # These stock debugfs reports identify inherited clock and rail votes.
        for leaf, name in (("clk/clk_summary", "android-clocks.txt"),
                           ("regulator/regulator_summary", "android-regulators.txt")):
            for debug in DEBUG_ROOTS:
                data = root_read(f"cat {debug}/{leaf} 2>/dev/null")
                if data and data.strip():
                    save(name, data)
                    break
        dmesg = root_read("dmesg")
        if dmesg:
            camera = [line for line in dmesg.splitlines() if CAMERA_EVENT.search(line)]
            if camera:
                save("camera-dmesg.txt", b"\n".join(camera) + b"\n")
    (args.output / "receipt.json").write_text(json.dumps(saved, indent=2) + "\n", encoding="utf-8")
    print(f"Saved Camera2 diagnostic and {len(listing)} module paths to {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, required=True, help="verified platform-tools adb")
    parser.add_argument("--serial", required=True, help="project's authorized device serial")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5037)
    parser.add_argument("--output", type=Path, required=True, help="new local private directory")
    args = parser.parse_args()
    try:
        collect(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Camera collection stopped: {error}\n")


if __name__ == "__main__":
    main()
