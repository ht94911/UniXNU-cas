#!/usr/bin/env python3
"""Check the inherited task policy, own program path and exact cache signature."""
from pathlib import Path
import json
import plistlib
import sys
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/baseband27"))
from cellular_network_runtime import network_cache, ORIGINAL_PATH, HELPER_PATH, TASK_KEY
from cellular_runtime import detached_signature, verify_signature

jobs = {f"/System/Library/LaunchDaemons/fixture{i}.plist":
        dict(Label=f"fixture{i}", ProgramArguments=[f"/usr/libexec/fixture{i}"])
        for i in range(728)}
jobs[TASK_KEY] = dict(Label="com.apple.DumpPanic", ProgramArguments=[ORIGINAL_PATH],
                     RunAtLoad=True, KeepAlive={"Crashed": True},
                     ProcessType="Interactive", EnableTransactions=False)
original = dict(VersionNumber=7, LaunchDaemons=jobs, AppExtensions={})
with mock.patch.object(plistlib, "_count_to_size", return_value=4):
    source = plistlib.dumps(original, fmt=plistlib.FMT_BINARY, sort_keys=False)
changed, task = network_cache(source)
decoded = plistlib.loads(changed)
assert task["ProgramArguments"] == [HELPER_PATH]
assert task["KeepAlive"] == {"Crashed": True} and task["RunAtLoad"]
decoded["LaunchDaemons"][TASK_KEY]["ProgramArguments"] = [ORIGINAL_PATH]
assert decoded == original and len(decoded["LaunchDaemons"]) == 729
signed, cdhash = detached_signature(changed)
assert verify_signature(changed, signed) == cdhash
corrupt = bytearray(changed)
corrupt[100] ^= 1
try:
    verify_signature(corrupt, signed)
except ValueError:
    pass
else:
    raise AssertionError("A changed cache page was accepted")
for value in ([HELPER_PATH], ["/usr/libexec/other"]):
    rejected = plistlib.loads(source)
    rejected["LaunchDaemons"][TASK_KEY]["ProgramArguments"] = value
    with mock.patch.object(plistlib, "_count_to_size", return_value=4):
        raw = plistlib.dumps(rejected, fmt=plistlib.FMT_BINARY, sort_keys=False)
    try:
        network_cache(raw)
    except ValueError:
        pass
    else:
        raise AssertionError("An already changed task was accepted")
print(json.dumps(dict(passed=True, original_jobs=729,
                      original_task_policy_preserved=True,
                      only_program_argument_changed=True,
                      changed_page_rejected=True, changed_task_rejected=True,
                      phone_operations=False), indent=2))
