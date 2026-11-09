#!/usr/bin/env python3
"""Verify unchanged runtime identity; prior host tests are qualified separately."""
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
assert patches == list(expected['patch_hashes']) and len(patches) == 34
for name, digest in expected['patch_hashes'].items():
    assert sha(r / 'drivers/AdrenoMetal/bridge/patches' / name) == digest, name
for name, digest in expected['source_hashes'].items():
    assert sha(t / name) == digest, name
with tempfile.TemporaryDirectory(prefix='reserve016-') as work:
    env = dict(os.environ, GIT_INDEX_FILE=str(Path(work) / 'index'))
    git('read-tree', 'HEAD', env=env)
    git('add', '-A', env=env)
    assert git('write-tree', env=env).decode().strip() == expected['patched_tree']


reference=json.loads((Path(__file__).resolve().parent/'fixtures/unchanged-runtime-reference.json').read_text())
assert expected['patched_tree']==reference['runtime_tree']
assert sha(Path(__file__).resolve().parent/'fixtures/buffer-reserve-sources.json')==reference['runtime_sources_sha256']
assert sha(r/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm')==reference['frontend_sha256']
report=dict(passed=True,actual_runtime_source=True,source_tree_verified=True,
            current_run_host_tests_executed=False,prior_test_reference=reference,
            runtime_tree=expected['patched_tree'],frontend_sha256=reference['frontend_sha256'])
a.logs.mkdir(parents=True,exist_ok=True)
(a.logs/'unchanged-runtime-source.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
