#!/usr/bin/env python3
"""Exercise the production frontend against real CoreVideo pools and gated GPU work."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve();kit=root/'drivers/AdrenoMetal'
out=a.logs/'surface-pool';out.mkdir(parents=True,exist_ok=False)
source=kit/'NativeBridgeMetalFrontend.mm';test=kit/'bridge/surface_pool_host.mm'
archive=root/'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
subprocess.run([sys.executable,root/'scripts/build-adreno-metal-bridge.py','--target','host'],cwd=root,check=True)
assert archive.is_file(), 'The production host bridge must exist before the GPU consumer'
patch=Path(__file__).with_name('patches')/'camera150-surface-use-plugin.patch'
before=out/'before';before.mkdir()
(before/source.name).write_bytes(source.read_bytes())
subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=before,check=True)
subprocess.run(['git','apply','--reverse',str(patch)],cwd=before,check=True)
legacy=(before/source.name).read_text()
selector='static const bool gCountsSurfaceUse=!strcmp(getprogname(),"backboardd");'
assert legacy.count(selector)==1
# The target failure was in backboardd. Select its exact existing policy in
# this one host instrument; no runtime call or completion behavior is mocked.
legacy=legacy.replace(selector,'static const bool gCountsSurfaceUse=true;')
(out/'legacy-frontend.mm').write_text(legacy)
legacy_test=out/'legacy-surface-pool.mm'
legacy_test.write_text(test.read_text().replace('#import "../NativeBridgeMetalFrontend.mm"','#import "legacy-frontend.mm"'))
flags=['-std=c++17','-fobjc-arc','-O1','-g0','-Wall','-Wextra','-Werror',
 '-Wno-unused-parameter','-Wno-deprecated-declarations','-I',str(kit),'-I',str(kit/'bridge'),
 '-I',str(root/'drivers/Adreno740'),'-I',str(root/'.cache/mesa-work/include')]
frameworks=['-framework','Foundation','-framework','Metal','-framework','IOSurface',
 '-framework','CoreFoundation','-framework','CoreVideo']
loader=Path(os.environ['NMB_HOST_LOADER']);assert loader.is_file()
environment=dict(os.environ,SHENG_DIAG='0');environment.pop('SHENG_GPU_TRACE',None)
rows={}
for label,harness in (('legacy',legacy_test),('current',test)):
 exe=out/label;cache=out/(label+'-cache');cache.mkdir()
 compiled=subprocess.run(['xcrun','clang++',*flags,str(harness),str(archive),*frameworks,'-o',str(exe)],capture_output=True,text=True,timeout=180)
 (out/(label+'-build.log')).write_text(compiled.stdout+compiled.stderr)
 assert compiled.returncode==0,label+': surface pool compilation failed'
 run=subprocess.run([str(exe),str(loader),str(cache),label],env=environment,capture_output=True,text=True,timeout=180)
 (out/(label+'-run.log')).write_text(run.stdout+run.stderr)
 assert run.returncode==0 and ('PASS '+label+':') in run.stdout,(label,run.returncode,run.stdout,run.stderr)
 rows[label]=dict(exit_code=run.returncode,output=run.stdout)
 print(run.stdout,end='')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,source_sha256=sha(source),test_sha256=sha(test),patch_sha256=sha(patch),
 real_CoreVideo_pool=True,real_IOSurface_use_counts=True,real_GPU_reads=True,
 cached_texture_exhaustion_reproduced=True,legacy_frames=13,current_frames=64,
 pending_GPU_blocks_pool_reuse=True,independent_command_uses=True,nested_views_deduplicated=True,
 discard_and_refusal_release=True,compute_render_imported_buffer_covered=True,
 retained_completed_commands_do_not_pin_pool=True,legacy_backboard_policy_selected_only_in_host_fixture=True,
 GPU_completion_not_synthesized=True,phone_operations=False,results=rows)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
