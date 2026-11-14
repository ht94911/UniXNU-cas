#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the cache and detached signature through macOS platform consumers."""
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cellular_runtime", ROOT / "tools/baseband27/cellular_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
original = {
    "VersionNumber": 7,
    "LaunchDaemons": {
        f"/System/Library/LaunchDaemons/org.unixnu.fixture{i}.plist": {
            "Label": f"org.unixnu.fixture{i}", "ProgramArguments": ["/usr/libexec/fixture"],
        } for i in range(729)
    },
    "AppExtensions": {},
    "SystemLibraryTreeState": {"fixture": True},
}
with mock.patch.object(plistlib, "_count_to_size", return_value=4):
    source = plistlib.dumps(original, fmt=plistlib.FMT_BINARY, sort_keys=False)
merged = runtime.merge_cache(source)
decoded = plistlib.loads(merged)
assert decoded["LaunchDaemons"].pop(runtime.JOB_KEY) == runtime.JOB and decoded == original
signature, cdhash = runtime.detached_signature(merged)
assert runtime.verify_signature(merged, signature) == cdhash

result = dict(passed=True, synthetic_input=True, original_fixture_jobs=729,
              added_owner_job=runtime.LABEL, cdhash=cdhash, phone_operations=False,
              target_iOS_cache_not_uploaded=True, macOS_consumers_executed=False)
if sys.platform == "darwin":
    xpc = ctypes.CDLL("/usr/lib/system/libxpc.dylib")
    xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_create_from_plist.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_count.argtypes = [ctypes.c_void_p]
    xpc.xpc_dictionary_get_count.restype = ctypes.c_size_t
    xpc.xpc_array_get_string.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_array_get_string.restype = ctypes.c_char_p
    xpc.xpc_release.argtypes = [ctypes.c_void_p]
    buffer = ctypes.create_string_buffer(merged)
    parsed = xpc.xpc_create_from_plist(buffer, len(merged))
    assert parsed
    jobs = xpc.xpc_dictionary_get_value(parsed, b"LaunchDaemons")
    assert xpc.xpc_dictionary_get_count(jobs) == 730
    job = xpc.xpc_dictionary_get_value(jobs, runtime.JOB_KEY.encode())
    arguments = xpc.xpc_dictionary_get_value(job, b"ProgramArguments")
    assert xpc.xpc_array_get_string(arguments, 0) == b"/usr/libexec/cellular-demand021"
    xpc.xpc_release(parsed)
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "launchd.plist"
        detached = path.with_suffix(".plist.sig")
        path.write_bytes(merged)
        detached.write_bytes(signature)
        subprocess.run(["/usr/bin/codesign", "--verify", "--strict", "--detached",
                        str(detached), str(path)], check=True)
        detail = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", "--detached",
                                 str(detached), str(path)], check=True, capture_output=True, text=True)
        assert "CDHash=" + cdhash in detail.stderr
        changed = bytearray(merged)
        changed[100] ^= 1
        path.write_bytes(changed)
        rejected = subprocess.run(["/usr/bin/codesign", "--verify", "--strict", "--detached",
                                   str(detached), str(path)], capture_output=True)
        assert rejected.returncode != 0
    result.update(macOS_consumers_executed=True, host_libxpc_parsed=True,
                  actual_codesign_detached_verified=True, actual_codesign_changed_page_rejected=True)
else:
    changed = bytearray(merged)
    changed[100] ^= 1
    try:
        runtime.verify_signature(changed, signature)
    except ValueError:
        pass
    else:
        raise AssertionError("Changed synthetic cache was accepted")
    result["local_page_check_only"] = True
print(json.dumps(result, indent=2))
