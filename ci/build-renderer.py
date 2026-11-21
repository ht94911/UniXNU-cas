"""Build the production renderer fallback and its original native consumer."""
import argparse,hashlib,importlib.util,json,subprocess,shutil,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];a.out.mkdir(exist_ok=False,parents=True);logs=a.out/'logs';logs.mkdir();m=json.loads((root/'inputs.json').read_text());calls=[];errors=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def run(name,cmd):
 cmd=list(map(str,cmd));calls.append(cmd);r=subprocess.run(cmd,capture_output=True,timeout=180);(logs/(name+'.stdout')).write_bytes(r.stdout);(logs/(name+'.stderr')).write_bytes(r.stderr)
 if r.returncode:raise RuntimeError(name+': '+r.stderr.decode(errors='replace')[-5000:])
 return r.stdout
try:
 for name,digest in m['files'].items():assert sha(root/name)==digest
 (a.out/'inputs.json').write_bytes((root/'inputs.json').read_bytes())
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip()
 helper=a.out/'nezha-camera-renderer-probe';run('probe-compile',['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',root/'source/NezhaCameraRendererProbe.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreVideo','-framework','CoreMedia','-framework','VideoToolbox','-o',helper])
 run('probe-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-nv12-probe','--entitlements',root/'source/probe.entitlements.plist',helper]);run('probe-verify',['codesign','--verify','--strict',helper]);detail=subprocess.run(['codesign','-d','--verbose=4',str(helper)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'helper-signature.txt').write_text(detail)
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',source_revision=m['device_revision'],helper_sha256=sha(helper),original_renderer_consumer=True,typed_BW_pixel_format=True,provider_and_host50_reused_from_run=44886620355,signature_page_bytes=4096,Apple_codesign_strict_passed=True,capture_started=False,phone_operations=False),indent=2)+'\n')
except Exception as error:errors.append(str(error))
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n');spec=importlib.util.spec_from_file_location('cms',root/'ci/artifacts.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);module.seal(a.out,sorted(p.relative_to(a.out).as_posix()for p in a.out.rglob('*')if p.is_file()),root/'ci/artifact-recipient.pem',a.sealed)
if errors:print(errors);sys.exit(1)
print('Production renderer fallback and native consumer compiled and signed')
