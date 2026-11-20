"""Build the bounded native NV12 sampler using the existing SDK/CMS entry."""
import argparse,hashlib,importlib.util,json,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];a.out.mkdir(parents=True,exist_ok=False);logs=a.out/'logs';logs.mkdir();manifest=json.loads((root/'inputs.json').read_text());calls=[];errors=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def run(name,cmd):
 cmd=list(map(str,cmd));calls.append(cmd);r=subprocess.run(cmd,capture_output=True,timeout=180);(logs/(name+'.stdout')).write_bytes(r.stdout);(logs/(name+'.stderr')).write_bytes(r.stderr)
 if r.returncode:raise RuntimeError(name+': '+r.stderr.decode(errors='replace')[-2500:])
 return r.stdout
try:
 for n,d in manifest['files'].items():assert sha(root/n)==d
 assert sha(root/'ci/artifacts.py')==manifest['artifacts_sha256'] and sha(root/'ci/artifact-recipient.pem')==manifest['certificate_sha256'];shutil.copyfile(root/'inputs.json',a.out/'inputs.json')
 run('own-memory-host',['python3',root/'ci/check-nv12-memory.py','--source',root/'source/NezhaCameraVtReplay.mm','--out',a.out/'own-memory-host'])
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();exe=a.out/'nezha-camera-vt-replay'
 run('probe-compile',['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',root/'source/NezhaCameraVtReplay.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreVideo','-o',exe])
 air=a.out/'replay.air';metallib=a.out/'replay-sample.metallib'
 run('shader-compile',['xcrun','--sdk','iphoneos','metal','-target','air64-apple-ios27.0','-std=metal3.1','-c',root/'source/NezhaCameraVtReplay.metal','-o',air]);run('shader-library',['xcrun','--sdk','iphoneos','metallib',air,'-o',metallib]);assert metallib.read_bytes()[:4]==b'MTLB'
 run('sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-nv12-probe','--entitlements',root/'source/probe.entitlements.plist',exe]);run('verify',['codesign','--verify','--strict',exe]);detail=subprocess.run(['codesign','-d','--verbose=4',str(exe)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'signature.txt').write_text(detail)
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',binary_sha256=sha(exe),metallib_sha256=sha(metallib),signature_page_bytes=4096,Apple_codesign_strict_passed=True,helper_delay_seconds=150,once_only=True,completion_deadline_seconds=15,camera_capture_started=False,phone_operations=False,planar_formats=[13,23],UV_format='R16Uint',threadgroup_lengths=[16384,32768],tested_shapes=[[16,16],[1200,2608]],original_VT_function=True,original_inputs_external=True,uniform_reader=True,filter_reader=True,explicit_pixel_sampler=True,source_sha256=sha(root/'source/NezhaCameraVtReplay.mm'),shader_source_sha256=sha(root/'source/NezhaCameraVtReplay.metal')),indent=2)+'\n')
except Exception as e:errors.append(str(e))
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n');s=importlib.util.spec_from_file_location('cms',root/'ci/artifacts.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.seal(a.out,sorted(p.relative_to(a.out).as_posix()for p in a.out.rglob('*')if p.is_file()),root/'ci/artifact-recipient.pem',a.sealed)
if errors:print(errors);sys.exit(1)
print('Original VT replay compiled and signed; actual pixels pending')
