#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Reuse the original iOS27 diagnostic task for the selected cellular helper."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib

from cellular_runtime import CACHE_SHA256, verify_signature

HELPER_SHA256 = "34edc272b82a580a5c30d490190772afaa5223977fb039b37000408c40b67d1a"
SIGNATURE_SHA256 = "0c67b44d8fb35f3080cd8e20f028fca8ad9a992c5c03acc33e206a44ed343d48"
TASK_KEY = "/System/Library/LaunchDaemons/com.apple.DumpPanic.plist"
HELPER_PATH = "/usr/libexec/DumpPanic"


def sha(content):
    return hashlib.sha256(content).hexdigest()


def stage(cache, original_signature, helper, output):
    raw, signature, binary = (
        cache.read_bytes(), original_signature.read_bytes(), helper.read_bytes()
    )
    if (sha(raw), sha(signature), sha(binary)) != (
        CACHE_SHA256, SIGNATURE_SHA256, HELPER_SHA256
    ):
        raise ValueError("Expected the original cache/signature and qualified028 helper")
    cdhash = verify_signature(raw, signature)
    jobs = plistlib.loads(raw)["LaunchDaemons"]
    task = jobs[TASK_KEY]
    if len(jobs) != 729 or task["ProgramArguments"] != [HELPER_PATH]:
        raise ValueError("Expected the original729 task cache and diagnostic entry")
    if task.get("RunAtLoad") is not True or task.get("KeepAlive") != {"Crashed": True}:
        raise ValueError("Original diagnostic launch policy differs")
    if any(job.get("Label") == "org.unixnu.cellular-demand021" for job in jobs.values()):
        raise ValueError("The independent owner task must not be installed")
    output.mkdir(parents=True, exist_ok=False)
    files = {
        "cellular-xpc/launchd.plist": (0o644, raw),
        "cellular-xpc/launchd.plist.sig": (0o644, signature),
        "cellular-bin/DumpPanic": (0o755, binary),
    }
    for name, (mode, content) in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
    report = {
        "candidate": "baseband029",
        "original_jobs": len(jobs),
        "original_jobs_preserved": True,
        "original_cache_and_signature_byte_identical": True,
        "independent_owner_task": False,
        "diagnostic_task": task,
        "diagnostic_entry": HELPER_PATH,
        "original_FDR_preserved": True,
        "helper_sha256": sha(binary),
        "helper_build_reused": 43689118984,
        "helper_byte_identical028": True,
        "cdhash": cdhash,
        "chosen_launch": 29,
        "chosen_demand": 21,
        "one_request_per_invocation": True,
        "request_timeout_seconds": 180,
        "helper_alarm_seconds": 400,
        "normal_nonzero_exit_does_not_match_Crashed": True,
        "crash_relaunch_must_be_checked_on_device": True,
        "hardware_verified": False,
        "phone_operations": False,
        "files": {
            name: {"mode": mode, "bytes": len(content), "sha256": sha(content)}
            for name, (mode, content) in files.items()
        },
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-cache", type=Path, required=True)
    parser.add_argument("--original-signature", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.original_cache, args.original_signature,
                           args.helper, args.output), indent=2))


if __name__ == "__main__":
    main()
