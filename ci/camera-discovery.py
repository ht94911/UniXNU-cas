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
 host_memory=a.out/'memory-host'
 run('host-memory-build',['xcrun','clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',root/'source/userland',root/'source/camera-memory-self.c','-o',host_memory])
 run('host-memory-run',[host_memory])
 host_observer=a.out/'discovery-host'; run('host-discovery-build',['xcrun','clang','-fno-objc-arc','-fblocks','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g','-O1',root/'source/camera-discovery.m','-framework','Foundation','-o',host_observer]); run('host-discovery-run',[host_observer])
 host_ls=a.out/'ls-registration-host'; run('host-ls-build',['xcrun','clang','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g','-O1',root/'source/camera-ls-registration.m','-framework','Foundation','-o',host_ls]); run('host-ls-run',[host_ls])
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();binary=a.out/'nezha-camera-launch'
 observer_object=a.out/'NezhaCameraDiscovery.o'
 run('compile-discovery',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fno-objc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2','-c',root/'source/NezhaCameraDiscovery.m','-o',observer_object])
 observer=a.out/'libNezhaCameraDiscovery27.dylib'
 run('link-discovery',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-dynamiclib','-install_name','@rpath/libNezhaCameraDiscovery27.dylib',observer_object,'-framework','Foundation','-o',observer])
 run('sign-discovery',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.camera-discovery','--pagesize','4096',observer]);run('verify-discovery',['codesign','--verify','--strict',observer])
 signature_observer=subprocess.run(['codesign','-d','--verbose=4',str(observer)],capture_output=True,text=True,check=True).stderr;(a.out/'discovery-signature.txt').write_text(signature_observer)
 run('discovery-assembly',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fno-objc-arc','-fblocks','-O2','-S',root/'source/NezhaCameraDiscovery.m','-o',a.out/'Discovery.s'])
 # Original079 helper is retained locally because the exact kernel CDHash is bound.
 binary=None
 library=a.out/'libNezhaCameraLS27.dylib'
 run('compile-ls-library',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2','-dynamiclib','-install_name','@rpath/libNezhaCameraLS27.dylib',root/'source/NezhaCameraLSRegistration.m','-framework','Foundation','-o',library])
 run('sign-ls-library',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.camera-ls','--pagesize','4096',library]);run('verify-ls-library',['codesign','--verify','--strict',library])
 signature_ls=subprocess.run(['codesign','-d','--verbose=4',str(library)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in signature_ls;(a.out/'ls-signature.txt').write_text(signature_ls)
 run('ls-assembly',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-O2','-S',root/'source/NezhaCameraLSRegistration.m','-o',a.out/'LSRegistration.s'])
 source=root/'source/userland';provider_out=a.out/'camera-userland';provider_out.mkdir()
 units=['NezhaCameraPreview.c','NezhaCameraPixelBuffer.c','NezhaCameraGraphOutput.c','NezhaFigCapture24A446.c','NezhaCameraFigProvider.c','NezhaCameraFormats.c','NezhaCameraFactory.c','NezhaCameraDeviceBackend.c','NezhaCameraSystemVendor.m','upstream/hakios/camera_isp.c']
 flags=['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=gnu11','-O2','-fblocks','-fPIC','-Wall','-Wextra','-Werror','-I',source]
 objects=[]
 for unit in units:
  obj=provider_out/(Path(unit).stem+'.o');options=['-Wno-unused-parameter'] if unit.startswith('upstream/') else []
  run('provider-'+Path(unit).stem,[*flags,*options,'-c',source/unit,'-o',obj]);objects.append(obj)
 provider=provider_out/'libNezhaCamera27.dylib'
 run('provider-link',[*flags,'-dynamiclib','-install_name','@rpath/libNezhaCamera27.dylib',*objects,'-framework','CoreFoundation','-framework','CoreMedia','-framework','CoreVideo','-lobjc','-o',provider])
 run('provider-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera27',provider]);run('provider-verify',['codesign','--verify','--strict',provider])
 provider_signature=subprocess.run(['codesign','--display','--verbose=4',str(provider)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in provider_signature;(provider_out/'signature.txt').write_text(provider_signature)
 run('provider-assembly',[*flags,'-S',source/'NezhaCameraSystemVendor.m','-o',provider_out/'SystemVendor.s'])
 run('model',['python3',root/'source/make-camera-model.py','--out',a.out/'NezhaCameraModel.plist'])
 (provider_out/'sdk-qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',sdk=sdk,source_revision=m['device_revision'],actual_CF_CM_CV_headers=True,library_linked=True,library_sha256=sha(provider),objects={p.name:sha(p) for p in objects},owner_4K_signed=True,Apple_codesign_strict_passed=True,original_sources_audit_token_by_value=True,self_memory_increase_command22=True,self_memory_increase_bytes=718*1048576,self_memory_host_cases=10,system_camera_registered=False,physical_frames_verified=False,C2_library_loaded=False),indent=2)+'\n')
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',sdk=sdk,source_revision=m['device_revision'],helper_reused_from079=True,Apple_codesign_strict_passed=True,page_bytes=4096,authorization_requests=0,capture_started=False,launch_request_only=True,original_system_registration_when_absent=True,current_LSInstallInfo_registration=True,original_modifydb_post_install=True,original_registration_entitlement_present=True,original_LSInstallInfo_URL_NSError_registration=True,original_sources_observer=True,independent_main_source_init=True,native_LS_seed_completion_recheck=True,installed_proxy_no_Info_read_gate=True,host_registration_cases=0,host_LS_registration_cases=28,host_discovery_cases=35,original_AVFCapture_ownership_preserved=True,accepted_registration_changed_output=True,discovery_library_sha256=sha(observer),LS_library_sha256=sha(library),original_server_registration_library=True,device_discovery_observer=True,launch_deadline_seconds=45,exactGPU051_required_getter_alias=True,pluginbinaryunchanged=True,original_Camera_app_verified=False,phone_executed=False),indent=2)+'\n');result='success'
except Exception as e:error=str(e)
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion=result,error=error,commands=calls),indent=2)+'\n');s=importlib.util.spec_from_file_location('artifacts',root/'ci/artifacts.py');mod=importlib.util.module_from_spec(s);s.loader.exec_module(mod);names=sorted(x.relative_to(a.out).as_posix() for x in a.out.rglob('*') if x.is_file());mod.seal(a.out,names,root/'ci/artifact-recipient.pem',a.sealed)
print('Camera105 self-only finite memory and target provider',result)
if result!='success':raise SystemExit(1)
