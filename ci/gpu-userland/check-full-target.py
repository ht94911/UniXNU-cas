#!/usr/bin/env python3
import argparse,hashlib,json,re,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--unixnu',type=Path,required=True);p.add_argument('--logs',type=Path,required=True);a=p.parse_args();root=a.unixnu.resolve();bundle=root/'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle';binary=bundle/'NativeAdrenoMetal';data=binary.read_bytes();profile=json.loads((bundle/'gpu-build.json').read_text());assert profile['target']=='ios27' and profile['iphone_24a446'] and not profile['ipad_private_layout_enabled']
subprocess.run(['codesign','--verify','--strict',str(bundle)],check=True)
for forbidden in [b'ca virtualized key %.*s = %u',b'ca virtualized key ? flags %llx = %u']:assert forbidden not in data
for required in [b'PUB iPhone24A446: public policy; iPad raw offsets disabled',b'App Library current item index installed',b'metal-work cb=']:assert required in data,required
library=root/'build/mesa-tu-ios27/src/freedreno/vulkan/libvulkan_freedreno.dylib';symbols=subprocess.check_output(['nm',str(library)],text=True);assert not re.search(r'_+ZL2dw(?:\b|\.)',symbols),'old singleton native connection'
mesa=root/'.cache/mesa-work';producer=json.loads((root/'build/mesa-tu-ios27/builtin-build-profile.json').read_text())['profile'];diff=subprocess.check_output(['git','-C',str(mesa),'diff','HEAD','--binary']);assert producer['source_diff_sha256']==hashlib.sha256(diff).hexdigest()
for name,digest in producer['extra_sources'].items():assert hashlib.sha256((mesa/name).read_bytes()).hexdigest()==digest
reports={}
for dirname in ['icon-list-index','icon-limits','extern-function','stitch-modules','submit-room-host','sync-profile','display-profile','render-wait','ui-main','long-latency','frame-observers','ui-update-wait','display-discovery','codegen-cache','display-preference','pipeline-cache','runtime-v2']:
    report=a.logs/dirname/'report.json'
    if not report.is_file():raise AssertionError('missing required actual check: '+dirname)
    x=json.loads(report.read_text());assert x.get('passed',True),dirname;reports[dirname]=x
contexts=json.loads((a.logs/'device-context-qualified.json').read_text());assert contexts['actual_prepared_sources_match'] and contexts['new_ownership_and_guard_cases_passed'];reports['device-contexts']=contexts
runtime=reports['runtime-v2'];assert runtime['actual_host_frontend_and_resource_wait_regressions_passed']
frontend=root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm';assert runtime['actual_frontend_source_sha256']==hashlib.sha256(frontend.read_bytes()).hexdigest()
import importlib.util
spec=importlib.util.spec_from_file_location('extern_target',Path(__file__).with_name('check-extern-target.py'));target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target);extern=target.verify(binary,root/'build/adreno-metal-plugin27/NativeAdrenoMetal.map');assert extern['passed']
result={'passed':True,'actual_target_checked':True,'profile':profile,'plugin_sha256':hashlib.sha256(data).hexdigest(),'bundle_signature_verified':True,'iPad_raw_reader_markers_absent':True,'actual_Mesa_producer_sources_verified':True,'actual_runtime_v2_host_regressions':runtime,'actual_extern_target':extern,'required_check_reports':reports,'real_A840_shader_or_UI_verified':False,'phone_operations':False}
(a.logs/'full-target.json').write_text(json.dumps(result,indent=2)+'\n')

