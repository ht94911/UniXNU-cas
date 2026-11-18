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
env = dict(os.environ, RUSTC_BOOTSTRAP='1', RUSTFLAGS='-Dwarnings',
           CARGO_BUILD_JOBS='3', CARGO_TARGET_DIR=str(r / 'build/m2v-target'),
           NMB_RUNTIME_ID='camera090-heap-options-host')
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

archive = r / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
harness = kit / 'bridge/heap_options_frontend_host.mm'
flags = ['-std=c++17', '-fobjc-arc', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', str(kit),
         '-I', str(kit / 'bridge'), '-I', str(r / 'drivers/Adreno740'),
         '-I', str(r / '.cache/mesa-work/include')]
compiled = subprocess.run(['xcrun', 'clang++', *flags, str(harness), str(archive),
                           '-framework', 'Foundation', '-framework', 'Metal',
                           '-framework', 'IOSurface', '-framework', 'CoreFoundation',
                           '-o', str(out / 'consumer')], capture_output=True, text=True)
(out / 'build.log').write_text(compiled.stdout + compiled.stderr)
assert compiled.returncode == 0, 'Production heap consumer compilation failed'
cache = out / 'cache'
cache.mkdir()
executed = subprocess.run([str(out / 'consumer'), os.environ['NMB_HOST_LOADER'],
                           str(cache)], capture_output=True, text=True, timeout=90,
                          env=dict(os.environ, SHENG_DIAG='0'))
(out / 'run.log').write_text(executed.stdout + executed.stderr)
match = re.search(r'PASS heap frontend: (\d+) production calls;', executed.stdout)
assert executed.returncode == 0 and match and int(match[1]) == 7, executed.stdout + executed.stderr
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = dict(passed=True, production_decode_cases=16, production_frontend_calls=7,
              real_host_Vulkan_heaps=True, hints_removed_from_reported_options=True,
              storage_cache_hazard_and_private_fields_preserved=True,
              source_sha256=sha(kit / 'NativeBridgeMetalFrontend.mm'),
              bridge_source_sha256=sha(kit / 'bridge/NativeMetalObjects.rs'),
              test_sha256=sha(harness),
              decode_test_sha256=sha(kit / 'bridge/heap_options_host.rs'),
              physical_camera_preview_verified=False, phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
