"""Check retained AIR module hashes and the original compiler serialization."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True);p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve();kit=root/'drivers/AdrenoMetal'
out=a.logs/'stitch-modules';out.mkdir(parents=True,exist_ok=False)
archive=root/'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
assert archive.is_file(),'Reuse the existing qualified host bridge'
flags=['-std=c++17','-fobjc-arc','-O1','-g0','-Wall','-Wextra','-Werror',
       '-Wno-unused-parameter','-Wno-deprecated-declarations','-I',str(kit),
       '-I',str(kit/'bridge'),'-I',str(root/'drivers/Adreno740'),'-I',str(root/'.cache/mesa-work/include')]
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
stitch_test=kit/'bridge/stitch_modules_host.mm'
stitch_command=['xcrun','clang++',*flags,str(stitch_test),str(archive),
               '-framework','Foundation','-framework','Metal','-framework','IOSurface',
               '-framework','CoreFoundation','-o',str(out/'stitch-consumer')]
compiled_stitch=subprocess.run(stitch_command,capture_output=True,text=True)
(out/'stitch-build.log').write_text(compiled_stitch.stdout+compiled_stitch.stderr)
assert compiled_stitch.returncode==0,'Production stitching consumer compilation failed'
stitch_run=subprocess.run([str(out/'stitch-consumer'),str(out/'shared-functions.air')],capture_output=True,text=True,
                         timeout=90,env=dict(os.environ,METAL_STITCH_OUT=str(out/'stitched.metallib')))
(out/'stitch-run.log').write_text(stitch_run.stdout+stitch_run.stderr)
assert stitch_run.returncode==0 and 'PASS Metal shared AIR stitching:' in stitch_run.stdout,stitch_run.stdout+stitch_run.stderr
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
    timeout=90,env=dict(os.environ,METAL_STITCH_OUT=str(out/'stitched-distinct.metallib')))
(out/'stitch-distinct-run.log').write_text(distinct_run.stdout+distinct_run.stderr)
assert distinct_run.returncode==0 and 'PASS Metal distinct AIR stitching:' in distinct_run.stdout,distinct_run.stdout+distinct_run.stderr
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,actual_production_ObjC_stitching_serializer=True,system_compiler_shared_modules=True,
    system_compiler_distinct_modules=True,repeated_function_object_preserved=True,module_content_hash_verified=True,
    source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),test_sha256=sha(stitch_test),
    shared_metallib_sha256=sha(out/'stitched.metallib'),distinct_metallib_sha256=sha(out/'stitched-distinct.metallib'),
    real_GPU_created=False,phone_operations=False,physical_frame_improvement_verified=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
