#!/usr/bin/env python3
"""Check030's single task delta and detached signing with native host consumers."""
import ctypes
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/baseband27"))
from cellular_task_permissions import (
    DECLARATION, HELPER_PATH, REFERENCE_KEY, TASK_KEY, permission_cache,
)
from cellular_runtime import detached_signature, merge_cache, JOB_KEY, JOB, verify_signature

tasks = {
    f"/System/Library/LaunchDaemons/org.unixnu.fixture{i}.plist": {
        "Label": f"org.unixnu.fixture{i}", "ProgramArguments": ["/usr/libexec/fixture"],
    } for i in range(727)
}
tasks[REFERENCE_KEY] = {
    "Label": "com.apple.replayd", DECLARATION: True,
    "ProgramArguments": ["/usr/libexec/replayd"],
}
tasks[TASK_KEY] = {
    "Label": "com.apple.DumpPanic", "RunAtLoad": True,
    "KeepAlive": {"Crashed": True}, "ProgramArguments": [HELPER_PATH],
}
original = {"VersionNumber": 7, "LaunchDaemons": tasks, "AppExtensions": {}}
with mock.patch.object(plistlib, "_count_to_size", return_value=4):
    source = plistlib.dumps(original, fmt=plistlib.FMT_BINARY, sort_keys=False)
changed, task = permission_cache(source)
decoded = plistlib.loads(changed)
assert decoded["LaunchDaemons"][TASK_KEY].pop(DECLARATION) is True
assert decoded == original and len(decoded["LaunchDaemons"]) == 729
# Existing add-job users retain their previous output and semantics.
added = plistlib.loads(merge_cache(source))
assert added["LaunchDaemons"].pop(JOB_KEY) == JOB and added == original
signature, cdhash = detached_signature(changed)
assert verify_signature(changed, signature) == cdhash
corrupt = bytearray(changed)
corrupt[100] ^= 1
try:
    verify_signature(corrupt, signature)
except ValueError:
    pass
else:
    raise AssertionError("Changed page accepted")
native = False
if sys.platform == "darwin":
    xpc = ctypes.CDLL("/usr/lib/system/libxpc.dylib")
    xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_create_from_plist.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_count.argtypes = [ctypes.c_void_p]
    xpc.xpc_dictionary_get_count.restype = ctypes.c_size_t
    xpc.xpc_dictionary_get_bool.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_bool.restype = ctypes.c_bool
    xpc.xpc_release.argtypes = [ctypes.c_void_p]
    buffer = ctypes.create_string_buffer(changed)
    parsed = xpc.xpc_create_from_plist(buffer, len(changed))
    assert parsed
    jobs = xpc.xpc_dictionary_get_value(parsed, b"LaunchDaemons")
    assert xpc.xpc_dictionary_get_count(jobs) == 729
    selected = xpc.xpc_dictionary_get_value(jobs, TASK_KEY.encode())
    assert xpc.xpc_dictionary_get_bool(selected, DECLARATION.encode())
    xpc.xpc_release(parsed)
    with tempfile.TemporaryDirectory() as folder:
        cache = Path(folder) / "launchd.plist"
        detached = Path(folder) / "launchd.plist.sig"
        cache.write_bytes(changed)
        detached.write_bytes(signature)
        subprocess.run(["/usr/bin/codesign", "--verify", "--strict", "--detached",
                        str(detached), str(cache)], check=True)
    native = True
print(json.dumps(dict(
    passed=True, synthetic_input=True, original_jobs=729,
    only_changed_task=TASK_KEY, only_changed_field=DECLARATION,
    original_add_job_contract_preserved=True, changed_page_rejected=True,
    native_macOS_consumers_executed=native, phone_operations=False,
), indent=2))
