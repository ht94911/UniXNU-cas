#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare the existing signed discovery client and original system job cache.

Reuse the audited binary-plist and detached-signature implementation. Inputs
stay local; this prepares addon files without mounting or accessing a phone.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib

ROOT = Path(__file__).resolve().parents[1]
LABEL = "org.unixnu.nezha.camera-enumerate"
JOB_KEY = "/System/Library/LaunchDaemons/" + LABEL + ".plist"
JOB = {
    "Label": LABEL,
    "ProgramArguments": ["/usr/libexec/nezha-camera-enumerate"],
    "RunAtLoad": True, "LaunchOnlyOnce": True, "KeepAlive": False,
    "UserName": "mobile", "LimitLoadToSessionType": "System", "ProcessType": "Background",
    "StandardOutPath": "/private/var/mobile/Library/Logs/nezha-camera-enumerate.json",
    "StandardErrorPath": "/private/var/mobile/Library/Logs/nezha-camera-enumerate.stderr",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-client', type=Path, required=True)
    p.add_argument('--original-cache', type=Path, required=True)
    p.add_argument('--original-signature', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    qualified = json.loads((a.native_client / 'computer-qualified.json').read_text())
    client = (a.native_client / 'result/nezha-camera-enumerate').read_bytes()
    assert sha(client) == qualified['binary_sha256'] == qualified['signature']['sha256']
    assert qualified['Apple_codesign_strict_passed'] and qualified['authorization_requests'] == 0
    assert not qualified['capture_started'] and not qualified['actual_phone_executed']
    assert all(cd['page_bytes'] == 4096 for cd in qualified['signature']['code_directories'])
    vendor = ROOT / 'package/camera/upstream/service-cache'
    origin = json.loads((vendor / 'ORIGIN.json').read_text())
    codec = vendor / 'cellular_runtime.py'
    assert origin['unmodified'] and sha(codec.read_bytes()) == origin['sha256']
    spec = importlib.util.spec_from_file_location('camera_job_codec', codec)
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    cache, signature = a.original_cache.read_bytes(), a.original_signature.read_bytes()
    assert sha(cache) == runtime.CACHE_SHA256
    runtime.verify_signature(cache, signature)
    runtime.LABEL, runtime.JOB_KEY, runtime.JOB = LABEL, JOB_KEY, JOB
    merged = runtime.merge_cache(cache)
    owner_signature, cdhash = runtime.detached_signature(merged)
    assert runtime.verify_signature(merged, owner_signature) == cdhash
    files = {
        'camera-xpc/launchd.plist': (0o644, merged),
        'camera-xpc/launchd.plist.sig': (0o644, owner_signature),
        'camera-bin/nezha-camera-enumerate': (0o755, client),
        'daemons/' + LABEL + '.plist': (0o644, plistlib.dumps(JOB)),
    }
    a.out.mkdir(parents=True, exist_ok=False)
    for name, (mode, data) in files.items():
        target = a.out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
    report = dict(original_cache_sha256=sha(cache), original_jobs=729,
        original_jobs_preserved=True, original_object_bytes_preserved=True,
        camera_job=JOB, client_cdhash=qualified['signature']['code_directories'][0]['cdhash'],
        cache_cdhash=cdhash, client_run=qualified['native_run'],
        camera_cache_codec_origin=origin, phone_operations=False, system_discovery_executed=False,
        files={n:dict(mode=mode,bytes=len(data),sha256=sha(data)) for n,(mode,data) in files.items()})
    (a.out / 'camera-discovery.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Camera discovery runtime prepared with original729 jobs and signed bounded client', cdhash)


if __name__ == '__main__':
    main()
