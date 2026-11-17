"""Exercise the production Metal function-constant object with original consumers."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', required=True, type=Path)
p.add_argument('--logs', required=True, type=Path)
a = p.parse_args()
root = a.unixnu.resolve()
kit = root/'drivers/AdrenoMetal'
out = a.logs/'function-constant'
out.mkdir(parents=True, exist_ok=False)
source = kit/'NativeBridgeMetalFrontend.mm'
harness = kit/'bridge/function_constant_selector_host.mm'
archive = root/'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
assert archive.is_file(), 'The preceding extern consumer builds this same host bridge'
flags = ['-std=c++17', '-fobjc-arc', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', str(kit),
    '-I', str(kit/'bridge'), '-I', str(root/'drivers/Adreno740'),
    '-I', str(root/'.cache/mesa-work/include')]
command = ['xcrun', 'clang++', *flags, str(harness), str(archive),
    '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
    '-framework', 'CoreFoundation', '-o', str(out/'consumer')]
compiled = subprocess.run(command, capture_output=True, text=True)
(out/'build.log').write_text(compiled.stdout+compiled.stderr)
assert compiled.returncode == 0, 'Production selector consumer compilation failed'
executed = subprocess.run([str(out/'consumer')], capture_output=True, text=True,
    timeout=30, env=dict(os.environ, SHENG_DIAG='0'))
(out/'run.log').write_text(executed.stdout+executed.stderr)
assert executed.returncode == 0 and 'PASS function constant selector: 10 ' in executed.stdout
report = dict(passed=True, checks=10, production_ObjectiveC_selector_executed=True,
    required_and_isRequired_share_live_flag=True, real_GPU_created=False,
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    test_sha256=hashlib.sha256(harness.read_bytes()).hexdigest(), phone_operations=False)
(out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
