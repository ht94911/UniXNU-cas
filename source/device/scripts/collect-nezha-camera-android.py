#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect camera topology and module names during an assigned Android window."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


MODULE_DIRS = (
    "/vendor/lib64/camera", "/odm/lib64/camera", "/vendor/lib64/hw",
    "/odm/lib64/hw", "/vendor/etc/camera", "/odm/etc/camera",
)


def collect(args):
    adb = [str(args.adb), "-H", args.host, "-P", str(args.port), "-s", args.serial]

    def read(*command, optional=False):
        result = subprocess.run(adb + list(command), capture_output=True, timeout=15)
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
    save("media-camera.txt", read("exec-out", "dumpsys", "media.camera"))

    listing = []
    for directory in MODULE_DIRS:
        names = read("exec-out", "ls", "-1", directory, optional=True)
        if names is None or names.startswith(b"ls:"):
            continue
        for name in names.decode("utf-8", errors="replace").splitlines():
            if directory.endswith("/hw") and not any(x in name.lower() for x in ("cam", "isp")):
                continue
            listing.append(f"{directory}/{name}")
    save("camera-module-paths.txt", ("\n".join(listing) + "\n").encode("utf-8"))
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
