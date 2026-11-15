#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Apply the original task sandbox declaration to the finite cellular helper."""
import argparse
import json
from pathlib import Path
import plistlib
import struct

from cellular_cached_runtime import (
    CACHE_SHA256, HELPER_SHA256, SIGNATURE_SHA256, HELPER_PATH, TASK_KEY, sha,
)
from cellular_runtime import merge_cache, detached_signature, verify_signature

REFERENCE_KEY = "/System/Library/LaunchDaemons/com.apple.replayd.plist"
DECLARATION = "com.apple.private.security.no-sandbox"


def permission_cache(source):
    original = plistlib.loads(source)
    jobs = original["LaunchDaemons"]
    if original.get("VersionNumber") != 7 or len(jobs) != 729:
        raise ValueError("Expected the selected original729 iOS27 tasks")
    if jobs[REFERENCE_KEY].get(DECLARATION) is not True:
        raise ValueError("The original OS task does not carry the reference declaration")
    task = dict(jobs[TASK_KEY])
    if task["ProgramArguments"] != [HELPER_PATH] or DECLARATION in task:
        raise ValueError("Expected the original diagnostic task")
    task[DECLARATION] = True
    cache = merge_cache(source, replacement_key=TASK_KEY, replacement_job=task)
    decoded = plistlib.loads(cache)
    if decoded["LaunchDaemons"][TASK_KEY].pop(DECLARATION) is not True or decoded != original:
        raise ValueError("Task changes extend beyond the reference declaration")
    return cache, task


def stage(cache, signature, helper, output):
    raw, signed, binary = cache.read_bytes(), signature.read_bytes(), helper.read_bytes()
    if (sha(raw), sha(signed), sha(binary)) != (
        CACHE_SHA256, SIGNATURE_SHA256, HELPER_SHA256
    ):
        raise ValueError("Expected original cache/signature and qualified028 helper")
    verify_signature(raw, signed)
    merged, task = permission_cache(raw)
    detached, cdhash = detached_signature(merged)
    if verify_signature(merged, detached) != cdhash:
        raise ValueError("Changed cache signature differs")
    if struct.unpack(">6xBBQQQ", merged[-32:])[:2] != (4, 4):
        raise ValueError("Original cache reference widths changed")
    output.mkdir(parents=True, exist_ok=False)
    files = {
        "cellular-xpc/launchd.plist": (0o644, merged),
        "cellular-xpc/launchd.plist.sig": (0o644, detached),
        "cellular-bin/DumpPanic": (0o755, binary),
    }
    for name, (mode, content) in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
    result = dict(
        candidate="baseband030", original_jobs=729, original_jobs_preserved=True,
        original_task_only_delta=DECLARATION, reference_task=REFERENCE_KEY,
        reference_from_same_original_iOS27=True, diagnostic_task=task,
        diagnostic_entry=HELPER_PATH, original_FDR_preserved=True,
        independent_owner_task=False, helper_sha256=sha(binary),
        helper_byte_identical028=True, helper_build_reused=43689118984,
        cdhash=cdhash, source_cache_sha256=sha(raw), cache_sha256=sha(merged),
        signature_sha256=sha(detached), original_reference_widths=[4, 4],
        global_kernel_or_modem_policy_changed=False,
        chosen_launch=29, chosen_demand=21,
        one_request_per_invocation=True, request_timeout_seconds=180,
        helper_alarm_seconds=400, actual_task_permissions_pending=True,
        hardware_verified=False, phone_operations=False,
        files={name: dict(mode=mode, bytes=len(content), sha256=sha(content))
               for name, (mode, content) in files.items()},
    )
    (output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


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
