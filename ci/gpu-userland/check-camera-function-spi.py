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
stitch_fixture=root/'.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/engine/stitched_caller.ll'
assert stitch_fixture.is_file()
subprocess.run(['llvm-as',str(stitch_fixture),'-o',str(out/'stitch.air')],check=True)
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
executed = subprocess.run([str(out/'consumer'), str(out/'tables.air'),str(out/'stitch.air')],
                          capture_output=True, text=True, timeout=90,
                          env=dict(os.environ, SHENG_DIAG='0'))
(out/'run.log').write_text(executed.stdout+executed.stderr)
matched = re.search(r'PASS Camera function SPI: (\d+) ', executed.stdout)
(out/'exit.json').write_text(json.dumps(dict(returncode=executed.returncode, stdout_bytes=len(executed.stdout), stderr_bytes=len(executed.stderr)))+'\n')
if executed.returncode:
    debug=subprocess.run(['xcrun','lldb','--batch','-o','run','-o','thread backtrace all','--',str(out/'consumer'),str(out/'tables.air'),str(out/'stitch.air')],capture_output=True,text=True,timeout=90,env=dict(os.environ,SHENG_DIAG='0'))
    (out/'failure-backtrace.log').write_text(debug.stdout+debug.stderr)
assert executed.returncode == 0 and matched and int(matched[1]) == 37, str(executed.returncode)+' '+executed.stdout+executed.stderr
# The current Xcode compiler cannot read the runtime fixture's AIRv29.
# Compile the same authored stitchable functions with its own AIR version.
compiler_source=out/'shared-functions.metal'
compiler_source.write_text('#include <metal_stdlib>\nusing namespace metal;\n'
    '[[stitchable]] float sum2(float a,float b) { return a+b; }\n'
    '[[stitchable]] float product2(float a,float b) { return a*b; }\n')
compiler_fixture=subprocess.run(['xcrun','--sdk','macosx','metal','-std=metal3.2','-c',str(compiler_source),
                                 '-o',str(out/'shared-functions.air')],capture_output=True,text=True)
(out/'stitch-fixture-build.log').write_text(compiler_fixture.stdout+compiler_fixture.stderr)
assert compiler_fixture.returncode==0,'Current system compiler could not build the public stitching fixture'
stitch_test=kit/'bridge/camera_stitch_host.mm'
stitch_command=['xcrun','clang++',*flags,str(stitch_test),str(archive),
               '-framework','Foundation','-framework','Metal','-framework','IOSurface',
               '-framework','CoreFoundation','-o',str(out/'stitch-consumer')]
compiled_stitch=subprocess.run(stitch_command,capture_output=True,text=True)
(out/'stitch-build.log').write_text(compiled_stitch.stdout+compiled_stitch.stderr)
assert compiled_stitch.returncode==0,'Production stitching consumer compilation failed'
stitch_run=subprocess.run([str(out/'stitch-consumer'),str(out/'shared-functions.air')],capture_output=True,text=True,
                         timeout=90,env=dict(os.environ,CAMERA_STITCH_OUT=str(out/'stitched.metallib')))
(out/'stitch-run.log').write_text(stitch_run.stdout+stitch_run.stderr)
assert stitch_run.returncode==0 and 'PASS Camera shared AIR stitching:' in stitch_run.stdout,stitch_run.stdout+stitch_run.stderr
# Distinct AIR modules must also retain both functions after deduplication.
distinct_air=[]
for name,body in [('sum2','a+b'),('product2','a*b')]:
    source=out/(name+'.metal')
    source.write_text('#include <metal_stdlib>\nusing namespace metal;\n'
        '[[stitchable]] float '+name+'(float a,float b) { return '+body+'; }\n')
    air=out/(name+'.air')
    built=subprocess.run(['xcrun','--sdk','macosx','metal','-std=metal3.2','-c',str(source),'-o',str(air)],capture_output=True,text=True)
    (out/(name+'-build.log')).write_text(built.stdout+built.stderr)
    assert built.returncode==0,'Distinct public AIR fixture failed: '+name
    distinct_air.append(air)
distinct_run=subprocess.run([str(out/'stitch-consumer'),*map(str,distinct_air)],capture_output=True,text=True,
    timeout=90,env=dict(os.environ,CAMERA_STITCH_OUT=str(out/'stitched-distinct.metallib')))
(out/'stitch-distinct-run.log').write_text(distinct_run.stdout+distinct_run.stderr)
assert distinct_run.returncode==0 and 'PASS Camera distinct AIR stitching:' in distinct_run.stdout,distinct_run.stdout+distinct_run.stderr
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
report = dict(passed=True, checks=37, production_cache_selectors_executed=True,
              name_constants_and_error_preserved=True, public_ARGR_cases=14,
              public_parameters_independent_of_RFLT=True, actual_AIR_reflection_executed=True,
              untagged_reflection_returns_nil=True, repeated_arguments_cached=True,
              actual_stitchable_value_parameters_executed=True, actual_stitchable_return_type_executed=True,
              stitchable_public_fixture_sha256=sha(stitch_fixture),
              source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),
              metadata_source_sha256=sha(kit/'NativeMetalFunctionMetadata.h'),
              bridge_source_sha256=sha(kit/'bridge/NativeMetalObjects.rs'),
              test_sha256=sha(harness), public_fixture_sha256=sha(fixture),
              host_runtime_device_opened=False, native_Metal_device_registered=False,
              GPU_submissions=False, phone_operations=False)
report.update(production_stitch_serialization_executed=True,system_compiler_stitch_passed=True,
              shared_AIR_functions_same_hash=True,distinct_AIR_stitch_passed=True,repeated_function_object_stitch_passed=True,stitch_test_sha256=sha(stitch_test),
              stitched_output_sha256=sha(out/'stitched.metallib'),
              current_compiler_public_fixture=True,current_compiler_fixture_sha256=sha(out/'shared-functions.air'),
              distinct_stitched_output_sha256=sha(out/'stitched-distinct.metallib'))
(out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
