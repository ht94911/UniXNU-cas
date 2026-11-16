#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the real installation sequence using synthetic transport replies."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import types
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("install_image", ROOT / "tools/baseband27/install-image-windows.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def head():
    data = bytearray(135168)
    data[:8] = b"SHNGDATA"
    struct.pack_into("<IIQ", data, 8, 1, 64, 1048576)
    data[24:56] = bytes.fromhex("c736c2c4cb886d4c57870f0112e1da3673858e352327ed0dbe60fc6394024114")
    struct.pack_into("<I", data, 56, 1)
    struct.pack_into("<I", data, 60, zlib.crc32(data[:60]))
    return bytes(data)

class Transport:
    def __init__(self, failure=None):
        self.calls = []
        self.failure = failure
        self.created = False
    def android(self):
        self.calls.append("android-readiness")
    def root(self, command, **kwargs):
        self.calls.append(command)
        if command.startswith("cat /proc/sys/kernel/random/boot_id"):
            return "synthetic-boot"
        if command.startswith("getprop"):
            return "none"
        if command.startswith("test "):
            return ""
        if command == "df -k /data":
            return "header\n/dev/dm 10000000 100 9999900 1% /data"
        if "pinfile create" in command:
            self.created = True
            return "created synthetic file"
        if command.startswith("chattr"):
            return ""
        if command.startswith("sha256sum"):
            return "c" * 64 + " " + module.TEMP
        if "pinfile rawwrite" in command:
            if self.failure == "rawwrite":
                raise RuntimeError("Synthetic uncertain write")
            return "wrote 53248 blocks (218103808 bytes); sha256 of what was written: " + "c" * 64
        if "pinfile rawhash" in command:
            return ("c" * 64 if module.TARGET in command else "b" * 64) + " file"
        if command.startswith("rm "):
            return ""
        raise AssertionError("Unexpected operation: " + command)
    def adb(self, *args, **kwargs):
        self.calls.append(("adb", args))
        assert args[0] == "push"

def exercise(output, failure=None):
    output.mkdir()
    task = object.__new__(module.Install)
    task.args = types.SimpleNamespace(out=output, image=output / "input.img", sha256="c" * 64, host="local5037")
    task.project = output
    task.gpt = b"synthetic"
    task.check = lambda: None
    task.claim = lambda: None
    task.transport = Transport(failure)
    prefix = head()
    task.prior = {"Data_prefix_sha256": {n: hashlib.sha256(prefix).hexdigest() for n in ("17", "27")},
                  "current_disk_payload_sha256_reference": "b" * 64}
    task.backend = types.SimpleNamespace(
        require=lambda value, message: None if value else (_ for _ in ()).throw(ValueError(message)),
        resolve_userdata_read_target=lambda *args: {"userdata_device": "/dev/block/sdb34"})
    task.guard = object()
    task.maps = lambda extra=None: {"old": "synthetic"}
    task.raw = lambda *args: prefix
    task.save = lambda *args: None
    task.transaction = lambda stage: task.transport.calls.append(("stage", stage))
    task.new_map_count = 0
    def mapping():
        task.new_map_count += 1
        return ({"size": module.IMAGE_BYTES, "extents": []}, "1:2:3:4" if
                failure != "extent" or task.new_map_count == 1 else "1:3:3:4")
    task.new_map = mapping
    released = []
    task.release = lambda *args: released.append(args)
    error = None
    try:
        task.install()
    except (RuntimeError, ValueError) as exc:
        error = str(exc)
    calls = task.transport.calls
    creates = [c for c in calls if isinstance(c, str) and "pinfile create " in c]
    writes = [c for c in calls if isinstance(c, str) and "pinfile rawwrite " in c]
    assert len(creates) == 1
    assert len(writes) == (0 if failure == "extent" else 1)
    assert all(c.startswith("/data/local/tmp/pinfile rawwrite " + module.TARGET + " /dev/block/sdb34 ")
               for c in writes)
    assert not any(isinstance(c, str) and any(v in c for v in ("reboot", "fastboot", "erase", "format", "reset")) for c in calls)
    if failure:
        assert error and not released
    else:
        assert not error and len(released) == 1
    return {"case": failure or "success", "creates": len(creates), "writes": len(writes),
            "released": bool(released), "phone_transport": False}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nb-install-contract-") as folder:
        root = Path(folder)
        results = [exercise(root / name, failure) for name, failure in (
            ("success", None), ("changed-map", "extent"), ("uncertain-write", "rawwrite"))]
        assert module.head_info(head())["generation"] == 1
        bad = bytearray(head()); struct.pack_into("<I", bad, 56, 2)
        try:
            module.head_info(bytes(bad))
        except ValueError:
            pass
        else:
            raise AssertionError("Generation change accepted")
    report = {"passed": True, "cases": results, "generation_checked": True, "phone_operations": False}
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
