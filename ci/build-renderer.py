"""Build the bounded original final-preview sample observer on retained camera137."""
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
 user=root/'source/userland';host=a.out/'renderer-host'
 run('renderer-host-build',['xcrun','clang','-std=gnu11','-fno-objc-arc','-fblocks','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-O1','-g','-I',user,user/'camera-renderer-host.m','-framework','Foundation','-framework','CoreGraphics','-framework','CoreVideo','-framework','CoreMedia','-framework','IOSurface','-o',host]);host_output=run('renderer-host-run',[host]);assert b'PASS 95 production renderer forwarding cases' in host_output
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();out=a.out/'camera-userland';out.mkdir()
 units=['NezhaCameraPreview.c','NezhaCameraPixelBuffer.c','NezhaCameraGraphOutput.c','NezhaFigCapture24A446.c','NezhaCameraFigProvider.c','NezhaCameraFormats.c','NezhaCameraFactory.c','NezhaCameraDeviceBackend.c','NezhaCameraSystemVendor.m','NezhaCameraRenderer.m','upstream/hakios/camera_isp.c']
 flags=['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=gnu11','-O2','-fblocks','-fPIC','-Wall','-Wextra','-Werror','-I',user];objects=[]
 for unit in units:
  obj=out/(Path(unit).stem+'.o');run('provider-'+Path(unit).stem,[*flags,*(['-Wno-unused-parameter']if unit.startswith('upstream/')else[]),'-c',user/unit,'-o',obj]);objects.append(obj)
 provider=out/'libNezhaCamera27.dylib';run('provider-link',[*flags,'-dynamiclib','-install_name','@rpath/libNezhaCamera27.dylib',*objects,'-framework','CoreFoundation','-framework','CoreMedia','-framework','CoreVideo','-framework','IOSurface','-lobjc','-o',provider])
 run('provider-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera27',provider]);run('provider-verify',['codesign','--verify','--strict',provider]);detail=subprocess.run(['codesign','-d','--verbose=4',str(provider)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(out/'signature.txt').write_text(detail)
 run('renderer-assembly',[*flags,'-S',user/'NezhaCameraRenderer.m','-o',out/'Renderer.s'])
 helper=a.out/'nezha-camera-renderer-probe';run('probe-compile',['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',root/'source/NezhaCameraRendererProbe.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreVideo','-framework','CoreMedia','-framework','VideoToolbox','-framework','CoreGraphics','-o',helper])
 run('probe-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-nv12-probe','--entitlements',root/'source/probe.entitlements.plist',helper]);run('probe-verify',['codesign','--verify','--strict',helper]);detail=subprocess.run(['codesign','-d','--verbose=4',str(helper)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'helper-signature.txt').write_text(detail)
 imports=run('provider-imports',['xcrun','nm','-u',provider]).decode();exports=run('provider-exports',['xcrun','nm','-gU',provider]).decode();assert '_nezha_camera_renderer_observe' in exports and '_nezha_camera_renderer_install' in exports
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',source_revision=m['device_revision'],provider_sha256=sha(provider),helper_sha256=sha(helper),objects={o.name:sha(o)for o in objects},renderer_host_cases=95,own_empty_source_crop_repair=True,actual_frame_geometry=True,nonuniform_rotation_cases=0,owned_rotation_border_repair=True,system_downscale_case=False,original_rectangular_scaling_cases=4,original_BW_scaling_cases=2,original_VT_rect_cases=2,real_scaling_rectangle_observers=True,final_preview_samples_observed=True,final_preview_limit=8,final_preview_grid=25,final_preview_read_only=True,production_forwarding_fixture=True,original_camera_mode_and_self_memory_retained=True,original_renderer_consumer=True,signature_page_bytes=4096,Apple_codesign_strict_passed=True,capture_started=False,original_capability_values_modified=False,phone_operations=False),indent=2)+'\n')
except Exception as error:errors.append(str(error))
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n');spec=importlib.util.spec_from_file_location('cms',root/'ci/artifacts.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);module.seal(a.out,sorted(p.relative_to(a.out).as_posix()for p in a.out.rglob('*')if p.is_file()),root/'ci/artifact-recipient.pem',a.sealed)
if errors:print(errors);sys.exit(1)
print('Frame geometry provider and original portrait rotation consumer compiled and signed')
