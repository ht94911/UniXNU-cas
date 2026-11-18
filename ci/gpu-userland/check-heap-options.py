"""Run production heap option decoding and real frontend heaps on the host."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
r = a.unixnu.resolve()
kit = r / 'drivers/AdrenoMetal'
out = a.logs / 'heap-options'
out.mkdir(parents=True, exist_ok=False)
expected=json.loads((Path(__file__).parent/'fixtures/multi-level-output-sources.json').read_text())
runtime_id=hashlib.sha256((expected['original_pin']+'\n'+expected['patched_tree']).encode()).hexdigest()[:32]
env = dict(os.environ, RUSTC_BOOTSTRAP='1',
           CARGO_BUILD_JOBS='3', CARGO_TARGET_DIR=str(r / 'build/m2v-target'),
           NMB_RUNTIME_ID=runtime_id)
for key, name in [('NMB_FIXTURE_AIR', 'runtime_examples.air'),
                  ('NMB_FC_AIR', 'function_constant.air'),
                  ('NMB_SCANOUT_AIR', 'scanout_convert.air')]:
    env[key] = str(r / 'build/adreno-metal-bridge' / name)
    assert Path(env[key]).is_file()
command = ['cargo', '+1.98.0', 'test', '--release', '--manifest-path',
           str(r / 'build/adreno-metal-bridge/Cargo.toml'), '--lib',
           'heap_options_tests', '--', '--nocapture']
with (out / 'decode.log').open('w') as log:
    decoded = subprocess.run(command, cwd=r, env=env, stdout=log,
                             stderr=subprocess.STDOUT, timeout=1500)
text = (out / 'decode.log').read_text()
assert decoded.returncode == 0 and 'PASS heap options: 16 production cases;' in text

sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report=dict(passed=True,production_decode_cases=16,source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),
            bridge_source_sha256=sha(kit/'bridge/NativeMetalObjects.rs'),
            decode_test_sha256=sha(kit/'bridge/heap_options_host.rs'),host_Vulkan_device_opened=False,phone_operations=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
