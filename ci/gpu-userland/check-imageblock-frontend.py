"""Check real compute imageblock calls and pixels through the production frontend."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
r = a.unixnu.resolve()
kit = r / 'drivers/AdrenoMetal'
out = a.logs / 'imageblock-frontend'
out.mkdir(parents=True, exist_ok=False)
fixture = r / '.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/engine/kernel_imageblock_wide_access.ll'
subprocess.run(['llvm-as', str(fixture), '-o', str(out / 'wide.air')], check=True)
archive = r / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
harness = kit / 'bridge/imageblock_frontend_host.mm'
flags = ['-std=c++17', '-fobjc-arc', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', str(kit),
         '-I', str(kit / 'bridge'), '-I', str(r / 'drivers/Adreno740'),
         '-I', str(r / '.cache/mesa-work/include')]
compiled = subprocess.run(['xcrun', 'clang++', *flags, str(harness), str(archive),
                           '-framework', 'Foundation', '-framework', 'Metal',
                           '-framework', 'IOSurface', '-framework', 'CoreFoundation',
                           '-o', str(out / 'consumer')], capture_output=True, text=True)
(out / 'build.log').write_text(compiled.stdout + compiled.stderr)
assert compiled.returncode == 0, 'Production imageblock consumer compilation failed'
cache = out / 'cache'
cache.mkdir()
executed = subprocess.run([str(out / 'consumer'), os.environ['NMB_HOST_LOADER'],
                           str(cache), str(out / 'wide.air')], capture_output=True, text=True,
                          timeout=120, env=dict(os.environ, SHENG_DIAG='0'))
(out / 'run.log').write_text(executed.stdout + executed.stderr)
assert executed.returncode == 0 and 'PASS imageblock frontend: 2 kernels, 128 pixels;' in executed.stdout, executed.stdout + executed.stderr
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = dict(passed=True, production_imageblock_size_selector=True,
              production_pipeline_memory_query=True, kernels=2, pixels=128,
              real_host_Vulkan_dispatch=True, source_sha256=sha(kit / 'NativeBridgeMetalFrontend.mm'),
              bridge_source_sha256=sha(kit / 'bridge/NativeMetalObjects.rs'),
              header_sha256=sha(kit / 'bridge/NativeMetalObjects.h'),
              test_sha256=sha(harness), owned_fixture_sha256=sha(fixture),
              physical_camera_preview_verified=False, phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
