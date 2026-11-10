#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Reuse the public Mac detached fixture and check the added console strings.

Inputs are public Python tools and one byte count. No private cache is read.
The original fixture supplies codesign/page/tamper and large binary-XPC checks.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys

FIXTURE_SHA = '3596fbb421e2d223c9d01b6280af68ca7e83595c99d3d96e481450f187201d72'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bytes', type=int, required=True)
    for name in ('fixture-script', 'tool', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('Requires real Mac codesign and libxpc')
    if hashlib.sha256(args.fixture_script.read_bytes()).hexdigest() != FIXTURE_SHA:
        parser.error('Original public fixture source differs')
    args.out.mkdir(parents=True, exist_ok=False)
    subprocess.run([sys.executable, str(args.fixture_script.resolve()), '--bytes', str(args.bytes),
                    '--tool', str(args.tool.resolve()), '--out', str(args.out.resolve())], check=True)
    fields = dict(UserName='mobile', StandardOutPath='/dev/console', StandardErrorPath='/dev/console')
    job = '/System/Library/LaunchDaemons/com.apple.bluetoothd.plist'
    public = {'LaunchDaemons': {job: dict(Label='com.apple.bluetoothd',
              ProgramArguments=['/public/fixture'], **fields)}}
    raw = plistlib.dumps(public, fmt=plistlib.FMT_BINARY, sort_keys=True)
    xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
    xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_create_from_plist.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_string.restype = ctypes.c_char_p
    xpc.xpc_release.argtypes = [ctypes.c_void_p]
    buffer = ctypes.create_string_buffer(raw)
    root = xpc.xpc_create_from_plist(buffer, len(raw))
    if not root:
        raise ValueError('Public console fixture XPC parsing failed')
    try:
        jobs = xpc.xpc_dictionary_get_value(root, b'LaunchDaemons')
        entry = xpc.xpc_dictionary_get_value(jobs, job.encode()) if jobs else None
        if not entry or any(xpc.xpc_dictionary_get_string(entry, key.encode()) != value.encode()
                            for key, value in fields.items()):
            raise ValueError('Public console fixture fields differ in real XPC')
    finally:
        xpc.xpc_release(root)
    report_path = args.out / 'cache-template-report.json'
    report = json.loads(report_path.read_text())
    report.update(real_XPC_console_fixture_fields_verified=True, console_fixture_fields=fields,
        console_fixture_jobs=1, actual_private_cache_XPC_parsed=False,
        original_fixture_sha256=FIXTURE_SHA,
        console_fixture_tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
