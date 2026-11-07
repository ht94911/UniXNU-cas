#!/usr/bin/env python3
"""Verify the compiled runtime source and run its actual allocator CPU cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
r = a.unixnu.resolve()
t = r / '.cache/gpu-reference/metal2vulkan'
expected = json.loads((Path(__file__).resolve().parent / 'fixtures/buffer-reserve-sources.json').read_text())
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
git = lambda *args, **kwargs: subprocess.check_output(['git', '-C', str(t), *args], **kwargs)
assert git('rev-parse', 'HEAD').decode().strip() == expected['original_pin']
builder = r / 'scripts/build-adreno-metal-bridge.py'
assert sha(builder) == expected['builder_sha256']
patches = re.findall(r"'([0-9]{4}[^']+\.patch)'", builder.read_text())
assert patches == list(expected['patch_hashes']) and len(patches) == 32
for name, digest in expected['patch_hashes'].items():
    assert sha(r / 'drivers/AdrenoMetal/bridge/patches' / name) == digest, name
for name, digest in expected['source_hashes'].items():
    assert sha(t / name) == digest, name
with tempfile.TemporaryDirectory(prefix='reserve016-') as work:
    env = dict(os.environ, GIT_INDEX_FILE=str(Path(work) / 'index'))
    git('read-tree', 'HEAD', env=env)
    git('add', '-A', env=env)
    assert git('write-tree', env=env).decode().strip() == expected['patched_tree']

a.logs.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, CARGO_INCREMENTAL='0', CARGO_BUILD_JOBS='3',
           CARGO_TARGET_DIR=str(r / 'build/buffer-reserve-host'), RUSTFLAGS='-Dwarnings')
command = ['cargo', '+1.98.0', 'test', '--release', '--manifest-path', str(t / 'Cargo.toml'),
           '-p', 'metal2vulkan-runtime', '--all-features', '--lib',
           'memory::latency_tests::', '--', '--nocapture']
with (a.logs / 'buffer-reserve-cargo.log').open('w') as log:
    result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1200)
log = (a.logs / 'buffer-reserve-cargo.log').read_text()
passed = result.returncode == 0 and '7 passed; 0 failed' in log
report = dict(passed=passed, command=command, exit_code=result.returncode,
              sources=expected, actual_runtime_source=True, fake_Vulkan_callbacks=True,
              physical_GPU=False, ios_runtime_qualified=False)
(a.logs / 'buffer-reserve-host.json').write_text(json.dumps(report, indent=2) + '\n')
print(log[-8000:])
assert passed, 'actual allocator CPU regression failed; inspect sealed compiler/test log'
