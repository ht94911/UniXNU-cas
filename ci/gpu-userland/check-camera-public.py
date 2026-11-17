"""Run production public parameter parsing before the long runtime build."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--unixnu',required=True,type=Path);p.add_argument('--logs',required=True,type=Path);a=p.parse_args()
root=a.unixnu.resolve();kit=root/'drivers/AdrenoMetal';out=a.logs/'camera-public';out.mkdir(parents=True,exist_ok=False)
# Keep production classes/parser/getters; unused Vulkan code is discarded here.
# The later linked consumer covers both caches and actual AIR reflection.
text=(kit/'bridge/camera_function_spi_host.mm').read_text();text=text[:text.index('\nint main(')]+'''\nint main() {@autoreleasepool {
 setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
 fprintf(stderr,"camera-public-consumer: entered\\n");
 unsigned checks=publicArgumentCases();printf("PASS Camera public metadata: %u cases\\n",checks);return 0;
}}\n'''
harness=out/'public.mm';harness.write_text(text.replace('#import "../NativeBridgeMetalFrontend.mm"','#import "'+str(kit/'NativeBridgeMetalFrontend.mm')+'"'))
flags=['-std=c++17','-fobjc-arc','-O1','-g0','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-deprecated-declarations','-I',str(kit),'-I',str(kit/'bridge'),'-I',str(root/'drivers/Adreno740'),'-I',str(root/'.cache/mesa-work/include')]
compiled=subprocess.run(['xcrun','clang++',*flags,str(harness),'-Wl,-dead_strip','-Wl,-undefined,dynamic_lookup','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreFoundation','-o',str(out/'consumer')],capture_output=True,text=True)
(out/'build.log').write_text(compiled.stdout+compiled.stderr);assert compiled.returncode==0,'Public consumer compile failed'
run=subprocess.run([str(out/'consumer')],capture_output=True,text=True,timeout=30,env=dict(os.environ,SHENG_DIAG='0',NMB_LOG_STDERR='1'))
(out/'run.log').write_text(run.stdout+run.stderr);(out/'exit.json').write_text(json.dumps(dict(returncode=run.returncode,stdout_bytes=len(run.stdout),stderr_bytes=len(run.stderr)))+'\n')
if run.returncode:
 debug=subprocess.run(['xcrun','lldb','--batch','-o','run','-o','bt','--',str(out/'consumer')],capture_output=True,text=True,timeout=45,env=dict(os.environ,SHENG_DIAG='0',NMB_LOG_STDERR='1'))
 (out/'failure-backtrace.log').write_text(debug.stdout+debug.stderr)
match=re.search(r'PASS Camera public metadata: (\d+) cases',run.stdout)
assert run.returncode==0 and match and int(match[1])==14,str(run.returncode)+' '+run.stdout+run.stderr
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,checks=14,production_public_parameters_executed=True,production_return_type_executed=True,source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),metadata_source_sha256=sha(kit/'NativeMetalFunctionMetadata.h'),test_sha256=sha(kit/'bridge/camera_function_spi_host.mm'),GPU_submissions=False,phone_operations=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
