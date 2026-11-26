#!/usr/bin/env python3
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--unixnu',type=Path,required=True);p.add_argument('--logs',type=Path,required=True);a=p.parse_args();root=a.unixnu.resolve();runtime=root/'.cache/gpu-reference/metal2vulkan';host=root/'build/adreno-metal-frontend-host'
required={'crates/metal2vulkan-runtime/src/command.rs':['pub fn references_buffer','pub fn references_texture','resource_heaps','resources_unknown'], 'crates/metal2vulkan-runtime/src/memory.rs':['allocation_len','cfg!(target_os = "ios")','checked_next_power_of_two'], 'crates/metal2vulkan-runtime/src/times.rs':['CLOCK_UPTIME_RAW'], 'crates/metal2vulkan-runtime/src/device.rs':['QOS_CLASS_USER_INTERACTIVE','Role::Submit | Role::Complete','QOS_CLASS_UTILITY']}
for name,markers in required.items():
    text=(runtime/name).read_text()
    assert all(m in text for m in markers),name
frontend=(root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_text();assert 'NBSurfaceUse' in frontend and 'surfaceUses.clear()' in frontend and 'countUse;' not in frontend
for file in ['frontend_host.log','resource_wait_host.log']:
    text=(host/file).read_text();assert 'PASS' in text and 'FAIL' not in text,file
cpu=json.loads((a.logs/'lavapipe-host/report.json').read_text())
assert cpu['passed'] and cpu['existing_Mesa_CPU_ICD_built'] and cpu['source_runtime_floor_unchanged']
assert 'llvmpipe' in (host/'frontend_host.log').read_text().lower(), 'host regression did not use the selected CPU ICD'
sources={name:hashlib.sha256((runtime/name).read_bytes()).hexdigest() for name in required}
out=a.logs/'runtime-v2';out.mkdir(exist_ok=True);(out/'report.json').write_text(json.dumps({'passed':True,'actual_runtime_source':sources,'actual_frontend_source_sha256':hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest(),'actual_host_frontend_and_resource_wait_regressions_passed':True,'local_allocator_QoS_clock_alias_wait_and_surface_lifecycle_retained':True,'hardware_verified':False,'phone_operations':False},indent=2)+'\n')
