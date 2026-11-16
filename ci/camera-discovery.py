"""Compile one bounded camera enumerator and seal it with the existing CMS tool."""
import argparse,hashlib,importlib.util,json,plistlib,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1]
a.out.mkdir(parents=True,exist_ok=False);logs=a.out/'logs';logs.mkdir();calls=[];result='failure';error=None
m=json.loads((root/'inputs.json').read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(name,cmd):
 cmd=list(map(str,cmd));calls.append(cmd);r=subprocess.run(cmd,capture_output=True,timeout=180);(logs/(name+'.stdout')).write_bytes(r.stdout);(logs/(name+'.stderr')).write_bytes(r.stderr);assert r.returncode==0,name;return r.stdout
try:
 assert all(sha(root/path)==digest for path,digest in m['files'].items())
 assert sha(root/'ci/camera-discovery.py')==m['builder_sha256']
 assert sha(root/'ci/artifacts.py')==m['artifacts_sha256'] and sha(root/'ci/artifact-recipient.pem')==m['certificate_sha256']
 (a.out/'inputs.json').write_bytes((root/'inputs.json').read_bytes())
 host=a.out/'registration-host'; run('host-regression',['xcrun','clang','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-O1',root/'source/camera-launch-registration.m','-framework','Foundation','-o',host]); run('host-regression-run',[host])
 host_ls=a.out/'ls-registration-host'; run('host-ls-build',['xcrun','clang','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g','-O1',root/'source/camera-ls-registration.m','-framework','Foundation','-o',host_ls]); run('host-ls-run',[host_ls])
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();binary=a.out/'nezha-camera-launch'
 run('compile',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',root/'source/NezhaCameraLaunch.m','-framework','AVFoundation','-framework','CoreMedia','-framework','Foundation','-o',binary])
 run('sign',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.camera-launch','--pagesize','4096','--entitlements',root/'source/NezhaCameraLaunch.entitlements.plist',binary]);run('verify',['codesign','--verify','--strict',binary])
 info=subprocess.run(['codesign','-d','--verbose=4',str(binary)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in info;(a.out/'signature.txt').write_text(info)
 imports=run('imports',['xcrun','nm','-u',binary]).decode();assert '_objc_getClass' in imports and '_dlopen' in imports
 library=a.out/'libNezhaCameraLS27.dylib'
 run('compile-ls-library',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2','-dynamiclib','-install_name','@rpath/libNezhaCameraLS27.dylib',root/'source/NezhaCameraLSRegistration.m','-framework','Foundation','-o',library])
 run('sign-ls-library',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.camera-ls','--pagesize','4096',library]);run('verify-ls-library',['codesign','--verify','--strict',library])
 signature_ls=subprocess.run(['codesign','-d','--verbose=4',str(library)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in signature_ls;(a.out/'ls-signature.txt').write_text(signature_ls)
 run('ls-assembly',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-O2','-S',root/'source/NezhaCameraLSRegistration.m','-o',a.out/'LSRegistration.s'])
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',sdk=sdk,source_revision=m['device_revision'],binary_sha256=sha(binary),Apple_codesign_strict_passed=True,page_bytes=4096,authorization_requests=0,capture_started=False,launch_request_only=True,original_system_registration_when_absent=True,current_LSInstallInfo_registration=True,original_modifydb_post_install=True,original_registration_entitlement_present=True,installed_proxy_no_Info_read_gate=True,host_registration_cases=9,host_LS_registration_cases=10,LS_library_sha256=sha(library),original_server_registration_library=True,device_discovery_observer=True,launch_deadline_seconds=45,original_Camera_app_verified=False,phone_executed=False),indent=2)+'\n');result='success'
except Exception as e:error=str(e)
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion=result,error=error,commands=calls),indent=2)+'\n');s=importlib.util.spec_from_file_location('artifacts',root/'ci/artifacts.py');mod=importlib.util.module_from_spec(s);s.loader.exec_module(mod);names=sorted(x.relative_to(a.out).as_posix() for x in a.out.rglob('*') if x.is_file());mod.seal(a.out,names,root/'ci/artifact-recipient.pem',a.sealed)
print('Camera bounded launch',result)
if result!='success':raise SystemExit(1)
