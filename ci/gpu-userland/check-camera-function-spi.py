"""Exercise production function creation, public parameters and AIR reflection."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
root = a.unixnu.resolve()
kit = root/'drivers/AdrenoMetal'
out = a.logs/'camera-function-spi'
out.mkdir(parents=True, exist_ok=False)
archive = root/'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
harness = kit/'bridge/camera_function_spi_host.mm'
fixture = root/'.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/reflection/tables.ll'
assert archive.is_file() and fixture.is_file()
assembled = subprocess.run(['llvm-as', str(fixture), '-o', str(out/'tables.air')], capture_output=True, text=True)
(out/'assemble.log').write_text(assembled.stdout+assembled.stderr)
assert assembled.returncode == 0, 'Public AIR fixture assembly failed'
flags = ['-std=c++17', '-fobjc-arc', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', str(kit),
         '-I', str(kit/'bridge'), '-I', str(root/'drivers/Adreno740'),
         '-I', str(root/'.cache/mesa-work/include')]
command = ['xcrun', 'clang++', *flags, str(harness), str(archive),
           '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
           '-framework', 'CoreFoundation', '-o', str(out/'consumer')]
compiled = subprocess.run(command, capture_output=True, text=True)
(out/'build.log').write_text(compiled.stdout+compiled.stderr)
assert compiled.returncode == 0, 'Production Camera function consumer compilation failed'
loader = os.environ['NMB_HOST_LOADER']
executed = subprocess.run([str(out/'consumer'), loader, str(out/'cache'), str(out/'tables.air')],
                          capture_output=True, text=True, timeout=90,
                          env=dict(os.environ, SHENG_DIAG='0'))
(out/'run.log').write_text(executed.stdout+executed.stderr)
matched = re.search(r'PASS Camera function SPI: (\d+) ', executed.stdout)
(out/'exit.json').write_text(json.dumps(dict(returncode=executed.returncode, stdout_bytes=len(executed.stdout), stderr_bytes=len(executed.stderr)))+'\n')
if executed.returncode:
    debug=subprocess.run(['xcrun','lldb','--batch','-o','run','-o','bt','--',str(out/'consumer'),loader,str(out/'cache'),str(out/'tables.air')],capture_output=True,text=True,timeout=90,env=dict(os.environ,SHENG_DIAG='0'))
    (out/'failure-backtrace.log').write_text(debug.stdout+debug.stderr)
assert executed.returncode == 0 and matched and int(matched[1]) == 32, str(executed.returncode)+' '+executed.stdout+executed.stderr
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
report = dict(passed=True, checks=32, production_cache_selectors_executed=True,
              name_constants_and_error_preserved=True, public_ARGR_cases=14,
              public_parameters_independent_of_RFLT=True, actual_AIR_reflection_executed=True,
              untagged_reflection_returns_nil=True, repeated_arguments_cached=True,
              source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),
              metadata_source_sha256=sha(kit/'NativeMetalFunctionMetadata.h'),
              bridge_source_sha256=sha(kit/'bridge/NativeMetalObjects.rs'),
              test_sha256=sha(harness), public_fixture_sha256=sha(fixture),
              host_runtime_device_opened=True, native_Metal_device_registered=False,
              GPU_submissions=False, phone_operations=False)
(out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
