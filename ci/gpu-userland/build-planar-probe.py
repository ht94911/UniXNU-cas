"""Build the bounded planar storage/threadgroup helper using the existing SDK entry."""
import argparse,hashlib,importlib.util,json,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[2];source=root/'ci/gpu-userland/fixtures/planar-probe111';a.out.mkdir(parents=True,exist_ok=False);logs=a.out/'logs';logs.mkdir();calls=[];errors=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def run(name,cmd):
 cmd=list(map(str,cmd));calls.append(cmd);r=subprocess.run(cmd,capture_output=True,timeout=180);(logs/(name+'.stdout')).write_bytes(r.stdout);(logs/(name+'.stderr')).write_bytes(r.stderr)
 if r.returncode:raise RuntimeError(name+': '+r.stderr.decode(errors='replace')[-2500:])
 return r.stdout
try:
 prepare_spec=importlib.util.spec_from_file_location('probe_prepare',root/'ci/gpu-userland/prepare.py');prepare=importlib.util.module_from_spec(prepare_spec);prepare_spec.loader.exec_module(prepare);prepare.verify_ci_sources(root)
 shutil.copyfile(root/'source-sha256.json',a.out/'source-sha256.json')
 run('own-memory-host',['python3',root/'ci/gpu-userland/check-nv12-memory.py','--source',source/'NezhaCameraNv12Probe.mm','--out',a.out/'own-memory-host'])
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();exe=a.out/'nezha-camera-nv12-probe'
 run('probe-compile',['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',source/'NezhaCameraNv12Probe.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreVideo','-o',exe])
 air=a.out/'nv12.air';metallib=a.out/'nv12-sample.metallib'
 run('shader-compile',['xcrun','--sdk','iphoneos','metal','-target','air64-apple-ios27.0','-std=metal3.1','-c',source/'NezhaCameraNv12Probe.metal','-o',air]);run('shader-library',['xcrun','--sdk','iphoneos','metallib',air,'-o',metallib]);assert metallib.read_bytes()[:4]==b'MTLB'
 run('sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-nv12-probe','--entitlements',source/'NezhaCameraNv12Probe.entitlements.plist',exe]);run('verify',['codesign','--verify','--strict',exe]);detail=subprocess.run(['codesign','-d','--verbose=4',str(exe)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'signature.txt').write_text(detail)
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',binary_sha256=sha(exe),metallib_sha256=sha(metallib),signature_page_bytes=4096,Apple_codesign_strict_passed=True,source_sha256=sha(source/'NezhaCameraNv12Probe.mm'),shader_source_sha256=sha(source/'NezhaCameraNv12Probe.metal'),helper_delay_seconds=150,once_only=True,completion_deadline_seconds=15,camera_capture_started=False,phone_operations=False,planar_formats=[13,23],UV_format='R16Uint',threadgroup_lengths=[16384,32768],tested_shapes=[[64,16],[1200,2608]],pixel_comparison_independent_of_completion=True),indent=2)+'\n')
except Exception as e:errors.append(str(e))
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n');s=importlib.util.spec_from_file_location('cms',root/'ci/xnu/artifacts.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.seal(a.out,sorted(p.relative_to(a.out).as_posix()for p in a.out.rglob('*')if p.is_file()),root/'ci/xnu/artifact-recipient.pem',a.sealed)
if errors:print(errors);sys.exit(1)
print('native planar/TG helper compiled andsigned; device pixels pending')
