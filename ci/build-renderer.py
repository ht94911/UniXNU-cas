"""Build155 software HEVC registration and photo delivery; retain152 native and150 Metal."""
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
 run('renderer-host-build',['xcrun','clang','-std=gnu11','-fno-objc-arc','-fblocks','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-O1','-g','-I',user,user/'camera-renderer-host.m','-framework','Foundation','-framework','CoreGraphics','-framework','CoreVideo','-framework','CoreMedia','-framework','IOSurface','-o',host]);host_output=run('renderer-host-run',[host]);assert b'PASS 736 production renderer forwarding cases' in host_output
 preview_host=a.out/'camera-preview-host'
 run('preview-host-build',['xcrun','clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-I',user,user/'NezhaCameraPreview.c',root/'source/tests/camera-preview.c',user/'upstream/hakios/camera_isp.c','-o',preview_host]);preview_output=run('preview-host-run',[preview_host]);assert b'600 saturated frames without repeated CCI requests' in preview_output
 fixture=root/'source/tests/camera-platform'
 graph_host=a.out/'camera-graph-host'
 graph_units=['NezhaCameraFigProvider.c','NezhaCameraGraphOutput.c','NezhaCameraPreview.c','NezhaCameraPixelBuffer.c','NezhaCameraFormats.c','NezhaCameraFactory.c','upstream/hakios/camera_isp.c']
 run('graph-host-build',['xcrun','clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fblocks','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-I',fixture,'-I',user,*[user/unit for unit in graph_units],root/'source/tests/camera-fig-provider.c','-o',graph_host])
 graph_output=run('graph-host-run',[graph_host]);assert b'complete release passed' in graph_output
 run('graph-AE-host-run',[graph_host,'ae'])
 still_host=a.out/'still-observer-host'
 # This C-only ObjC fixture uses the required generic IMP conversions.
 # Exact runtime encodings and96 calls validate their typed ABI. Apple
 # Clang21 warns on the C shim; production.m and all other warnings retain
 # their original flags. No original call or signature gate is bypassed.
 run('still-observer-host-build',['xcrun','clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-cast-function-type-mismatch','-fblocks','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-I',fixture/'vendor-fixture','-I',fixture,root/'source/tests/camera-still-observers.c','-o',still_host])
 still_output=run('still-observer-host-run',[still_host]);assert b'96 original forwards' in still_output
 photo_codec_host=a.out/'photo-codec-host'
 run('photo-codec-host-build',['xcrun','clang','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-I',user,root/'source/tests/camera-photo-codec.c','-framework','CoreFoundation','-o',photo_codec_host])
 photo_codec_output=run('photo-codec-host-run',[photo_codec_host]);assert b'4 registration outcomes preserved' in photo_codec_output
 photo_host=a.out/'photo-observer-host'
 run('photo-observer-host-build',['xcrun','clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-cast-function-type-mismatch','-fblocks','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-I',fixture/'vendor-fixture','-I',fixture,'-I',user,root/'source/tests/camera-photo-observers.c','-o',photo_host])
 photo_output=run('photo-observer-host-run',[photo_host]);assert b'96 original forwards' in photo_output
 discovery_host=a.out/'discovery-host'
 discovery_fixture=a.out/'discovery-fixture.m'
 fixture_text=(root/'source/camera-discovery.m').read_text()
 old_include='#include "../package/camera/userland/NezhaCameraDiscovery.m"'
 assert fixture_text.count(old_include)==1
 discovery_fixture.write_text(fixture_text.replace(old_include,'#include "'+(root/'source/NezhaCameraDiscovery.m').as_posix()+'"'))
 run('discovery-host-build',['xcrun','clang','-fno-objc-arc','-fblocks','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g','-O1',discovery_fixture,'-framework','Foundation','-framework','CoreMedia','-o',discovery_host])
 discovery_output=run('discovery-host-run',[discovery_host]);assert b'24 original calls' in discovery_output
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip();out=a.out/'camera-userland';out.mkdir()
 units=['NezhaCameraPreview.c','NezhaCameraPixelBuffer.c','NezhaCameraGraphOutput.c','NezhaFigCapture24A446.c','NezhaCameraFigProvider.c','NezhaCameraFormats.c','NezhaCameraFactory.c','NezhaCameraDeviceBackend.c','NezhaCameraSystemVendor.m','NezhaCameraRenderer.m','upstream/hakios/camera_isp.c']
 flags=['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=gnu11','-O2','-fblocks','-fPIC','-Wall','-Wextra','-Werror','-I',user];objects=[]
 for unit in units:
  obj=out/(Path(unit).stem+'.o');run('provider-'+Path(unit).stem,[*flags,*(['-Wno-unused-parameter']if unit.startswith('upstream/')else[]),'-c',user/unit,'-o',obj]);objects.append(obj)
 provider=out/'libNezhaCamera27.dylib';run('provider-link',[*flags,'-dynamiclib','-install_name','@rpath/libNezhaCamera27.dylib',*objects,'-framework','CoreFoundation','-framework','CoreMedia','-framework','CoreVideo','-framework','IOSurface','-framework','CoreGraphics','-lobjc','-o',provider])
 run('provider-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera27',provider]);run('provider-verify',['codesign','--verify','--strict',provider]);detail=subprocess.run(['codesign','-d','--verbose=4',str(provider)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(out/'signature.txt').write_text(detail)
 run('renderer-assembly',[*flags,'-S',user/'NezhaCameraRenderer.m','-o',out/'Renderer.s'])
 run('provider-assembly',[*flags,'-S',user/'NezhaCameraFigProvider.c','-o',out/'FigProvider.s'])
 run('vendor-assembly',[*flags,'-S',user/'NezhaCameraSystemVendor.m','-o',out/'SystemVendor.s'])
 discovery=a.out/'libNezhaCameraDiscovery27.dylib'
 run('discovery-compile',[*flags,'-fno-objc-arc','-dynamiclib','-install_name','@rpath/libNezhaCameraDiscovery27.dylib',root/'source/NezhaCameraDiscovery.m','-framework','Foundation','-framework','CoreMedia','-o',discovery])
 run('discovery-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-discovery',discovery]);run('discovery-verify',['codesign','--verify','--strict',discovery])
 detail=subprocess.run(['codesign','-d','--verbose=4',str(discovery)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'discovery-signature.txt').write_text(detail)
 run('discovery-assembly',[*flags,'-fno-objc-arc','-S',root/'source/NezhaCameraDiscovery.m','-o',a.out/'Discovery.s'])
 helper=a.out/'nezha-camera-renderer-probe';run('probe-compile',['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',root/'source/NezhaCameraRendererProbe.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreVideo','-framework','CoreMedia','-framework','VideoToolbox','-framework','CoreGraphics','-o',helper])
 run('probe-sign',['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.camera-nv12-probe','--entitlements',root/'source/probe.entitlements.plist',helper]);run('probe-verify',['codesign','--verify','--strict',helper]);detail=subprocess.run(['codesign','-d','--verbose=4',str(helper)],capture_output=True,text=True,check=True).stderr;assert 'Page size=4096' in detail;(a.out/'helper-signature.txt').write_text(detail)
 imports=run('provider-imports',['xcrun','nm','-u',provider]).decode();exports=run('provider-exports',['xcrun','nm','-gU',provider]).decode();assert '_nezha_camera_renderer_observe' in exports and '_nezha_camera_renderer_install' in exports
 (a.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',source_revision=m['device_revision'],provider_sha256=sha(provider),helper_sha256=sha(helper),objects={o.name:sha(o)for o in objects},renderer_host_cases=736,own_empty_source_crop_repair=True,actual_frame_geometry=True,nonuniform_rotation_cases=0,owned_rotation_border_repair=True,system_downscale_case=False,original_rectangular_scaling_cases=4,original_BW_scaling_cases=2,original_VT_rect_cases=2,real_scaling_rectangle_observers=True,final_preview_samples_observed=True,final_preview_limit=64,final_preview_first=8,final_preview_early_sequence=24,final_preview_interval=300,final_preview_grid=25,final_preview_read_only=True,preview_flow_snapshots=True,preview_flow_stages=5,preview_flow_report_limit=40,preview_flow_no_new_worker=True,preview_flow_original_calls_forwarded_once=True,transfer_pool_nil_observed=True,transfer_emitted_and_dropped_counted=True,original_drop_reason_observed=True,transfer_scope_thread_local=True,reference_ownership_preserved=True,production_forwarding_fixture=True,original_camera_mode_and_self_memory_retained=True,original_renderer_consumer=True,signature_page_bytes=4096,Apple_codesign_strict_passed=True,capture_started=False,original_capability_values_modified=False,phone_operations=False,stitcher_geometry_repair=True,stitcher_geometry_consumer=True,stitcher_geometry_cases=2,nil_intermediate_pool=True,real_pixel_transfer_node_crop_repair=True,node_state_read_only=True,node_sample_copy=True,node_crop_modes=4,node_invalid_and_partial_preserved=True,unchanged_AE_control_groups_skipped=True,saturated600_frames_host_passed=True,digital_only_AE_retained=True,frame_length_ack_hold_retained=True,node_mode1_all_NaN_normalized=True,node_mixed_nonfinite_and_other_modes_retained=True,empty_request_normalized_before_original=True,original_node_output_prepare=True,original_transfer_caller=True,payload_free_still_command=True,typed_still_payload_log=True,original_null_command_cases_passed=True,incremental_output_configurations=True,nested_still_buffer_count_update=True,all_scaler_pools_and_handlers_retained=True,still_request_observers=True,still_observer_host_forwards=96,still_observer_host_install_refusals=12,photo_observer_host_forwards=24,graph_fixture_still_delta_passed=True,retained_native152_kernel=True,retained_Metal150_plugin=True,software_hevc_registration=True,software_hevc_host_registration_outcomes=4,photo_delivery_observers=True,photo_delivery_host_forwards=96,photo_delivery_install_refusals=12,Camera_photo_verified=False),indent=2)+'\n')
except Exception as error:errors.append(str(error))
finally:
 (a.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n');spec=importlib.util.spec_from_file_location('cms',root/'ci/artifacts.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);module.seal(a.out,sorted(p.relative_to(a.out).as_posix()for p in a.out.rglob('*')if p.is_file()),root/'ci/artifact-recipient.pem',a.sealed)
if errors:print(errors);sys.exit(1)
print('Frame geometry provider and original portrait rotation consumer compiled and signed')
