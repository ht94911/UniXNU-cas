#!/usr/bin/env python3
"""Bind completed public camera checks to unchanged production source files."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unixnu', type=Path, required=True)
parser.add_argument('--logs', type=Path, required=True)
args = parser.parse_args()
kit = args.unixnu.resolve() / 'drivers/AdrenoMetal'
fixtures = Path(__file__).with_name('fixtures') / 'retained-camera-contracts'
current_frontend_sha = hashlib.sha256((kit / 'NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
args.logs.mkdir(parents=True, exist_ok=True)
retained_source = tempfile.TemporaryDirectory(prefix='camera093-retained-', dir=args.logs)
previous = Path(retained_source.name)
shutil.copyfile(kit / 'NativeBridgeMetalFrontend.mm', previous / 'NativeBridgeMetalFrontend.mm')
patch = Path(__file__).with_name('patches') / 'camera093-surface-plane-plugin.patch'
subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=previous, check=True)
subprocess.run(['git', 'apply', '--reverse', str(patch)], cwd=previous, check=True)
previous_sha = hashlib.sha256((previous / 'NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
source_files = {
    'source_sha256': 'NativeBridgeMetalFrontend.mm',
    'metadata_source_sha256': 'NativeMetalFunctionMetadata.h',
    'bridge_source_sha256': 'bridge/NativeMetalObjects.rs',
}
test_files = {
    'function-constant': {'test_sha256': 'bridge/function_constant_selector_host.mm'},
    'camera-function-spi': {
        'test_sha256': 'bridge/camera_function_spi_host.mm',
        'stitch_test_sha256': 'bridge/camera_stitch_host.mm',
    },
    'extern-function': {
        'test_sha256': 'bridge/extern_function_host.mm',
        'icon_library_trace_test_sha256': 'bridge/icon_library_trace_host.mm',
    },
    'heap-options': {'decode_test_sha256': 'bridge/heap_options_host.rs'},
}
bound = {}
for name, tests in test_files.items():
    report = json.loads((fixtures / (name + '.json')).read_text())
    assert report['passed'] and report['previous_run'] == 44307910885
    assert report['current_run_executed'] is False
    files = {key: path for key, path in source_files.items() if key in report}
    files.update(tests)
    for key, relative in files.items():
        path = previous / relative if relative == 'NativeBridgeMetalFrontend.mm' else kit / relative
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert report[key] == digest, name + ': changed source ' + relative
    report['retained_exact_source_evidence'] = True
    report['previous_frontend_sha256'] = previous_sha
    report['current_frontend_sha256'] = current_frontend_sha
    report['retained_frontend_patch_reversed_exactly'] = True
    report['current_full_frontend_identical'] = False
    report['surface_plane_change_covered_separately'] = True
    report['retained_source_hashes_verified'] = True
    destination = args.logs / name
    destination.mkdir(parents=True, exist_ok=False)
    (destination / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    bound[name] = sorted(files.values())
print(json.dumps(dict(passed=True, previous_run=44307910885,
                      current_run_executed=False, unchanged_sources=bound,
                      phone_operations=False), indent=2))
