#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Reuse the original finite diagnostic task with the cellular helper's own name."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct

from cellular_cached_runtime import CACHE_SHA256, SIGNATURE_SHA256, TASK_KEY
from cellular_runtime import merge_cache, detached_signature, verify_signature

ORIGINAL_PATH = "/usr/libexec/DumpPanic"
HELPER_PATH = "/usr/libexec/cellular-demand021"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def network_cache(source):
    original = plistlib.loads(source)
    jobs = original["LaunchDaemons"]
    task = dict(jobs[TASK_KEY])
    if (original.get("VersionNumber") != 7 or len(jobs) != 729
            or task["ProgramArguments"] != [ORIGINAL_PATH]
            or task.get("RunAtLoad") is not True
            or task.get("KeepAlive") != {"Crashed": True}
            or "Program" in task):
        raise ValueError("Expected the original729 diagnostic task and launch policy")
    task["ProgramArguments"] = [HELPER_PATH]
    merged = merge_cache(source, replacement_key=TASK_KEY, replacement_job=task)
    restored = plistlib.loads(merged)
    if restored["LaunchDaemons"][TASK_KEY]["ProgramArguments"] != [HELPER_PATH]:
        raise ValueError("The selected task does not use the cellular helper")
    restored["LaunchDaemons"][TASK_KEY]["ProgramArguments"] = [ORIGINAL_PATH]
    if restored != original or struct.unpack(">6xBBQQQ", merged[-32:])[:2] != (4, 4):
        raise ValueError("Task fields or original binary cache representation changed")
    return merged, task


def stage(cache, original_signature, helper, output, *, helper_sha256):
    source, signature, binary = cache.read_bytes(), original_signature.read_bytes(), helper.read_bytes()
    if (len(helper_sha256) != 64 or sha(binary) != helper_sha256
            or sha(source) != CACHE_SHA256 or sha(signature) != SIGNATURE_SHA256):
        raise ValueError("Original cache/signature or qualified helper digest differs")
    verify_signature(source, signature)
    merged, task = network_cache(source)
    detached, cdhash = detached_signature(merged)
    if verify_signature(merged, detached) != cdhash:
        raise ValueError("Cellular cache signing failed")
    output.mkdir(parents=True, exist_ok=False)
    files = {
        "cellular-xpc/launchd.plist": (0o644, merged),
        "cellular-xpc/launchd.plist.sig": (0o644, detached),
        "cellular-bin/cellular-demand021": (0o755, binary),
    }
    for name, (mode, content) in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
    report = dict(
        candidate="baseband033", original_jobs=729, original_jobs_preserved=True,
        independent_owner_task=False, diagnostic_task=task, diagnostic_entry=HELPER_PATH,
        original_task_only_delta="ProgramArguments[0]", original_FDR_preserved=True,
        original_DumpPanic_program_preserved=True,
        source_cache_sha256=sha(source), cache_sha256=sha(merged),
        original_cache_and_signature_byte_identical=False,
        cdhash=cdhash, helper_sha256=helper_sha256,
        original_reference_widths=[4, 4], chosen_launch=29, chosen_demand=21,
        one_request_per_invocation=True, request_timeout_seconds=180,
        helper_alarm_seconds=400, normal_nonzero_exit_does_not_match_Crashed=True,
        crash_relaunch_must_be_checked_on_device=True, hardware_verified=False,
        original_named_Autobox_not_selected_by_helper_name=True,
        actual_network_permissions_pending=True, phone_operations=False,
        files={name: dict(mode=mode, bytes=len(content), sha256=sha(content))
               for name, (mode, content) in files.items()},
    )
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-cache", type=Path, required=True)
    parser.add_argument("--original-signature", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--helper-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.original_cache, args.original_signature, args.helper,
                           args.output, helper_sha256=args.helper_sha256), indent=2))


if __name__ == "__main__":
    main()
