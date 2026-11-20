#!/usr/bin/env python3
"""Verify actual target artifacts contain the intended safe full-chain path."""
import argparse
import hashlib
import json
import plistlib
import re
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import importlib.util

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
p.add_argument('--collected',type=Path,
               help='Verify an already collected target without rebuilding it')
p.add_argument('--signature-checker',type=Path,
               help='Existing local CodeDirectory checker for collected arm64e targets')
a=p.parse_args();root=a.unixnu.resolve()
if a.signature_checker:
    assert a.collected, 'The local signature checker requires collected targets'
bundle=(a.collected/'adreno-metal-plugin/NativeAdrenoMetal.bundle' if a.collected
        else root/'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle')
data=(bundle/'NativeAdrenoMetal').read_bytes()
profile=json.loads((bundle/'gpu-build.json').read_text())
assert profile['target']=='ios27' and profile['iphone_24a446'] and not profile['ipad_private_layout_enabled']
for text in (b'PUB iPhone24A446: public policy; iPad raw offsets disabled',
             b'FULL begin: raw,Turnip,Metal checks; UI remains independent',
             b'FULL returned CP=%d; T/M/F are separate verdicts'):
    assert text in data,text
for text in (b'ca virtualized key %.*s = %u',b'ca virtualized key ? flags %llx = %u'):
    assert text not in data,'iPad-only raw reader survived in the target artifact'
if a.signature_checker:
    signature_spec=importlib.util.spec_from_file_location('target_signature',a.signature_checker)
    signature_checker=importlib.util.module_from_spec(signature_spec)
    signature_spec.loader.exec_module(signature_checker)
    target_info=json.loads((a.collected/'run-info.json').read_text())
    trust=json.loads((a.collected/'adreno-metal-plugin/trust.json').read_text())
    for name in ('NativeAdrenoMetal','libvulkan_freedreno.dylib'):
        expected=target_info['checks'][name]
        trust_key='cdhash' if name=='NativeAdrenoMetal' else 'turnip_cdhash'
        assert expected['ok'] and expected['signature']['cdhash']==trust[trust_key]
        special={1:(bundle/'Info.plist').read_bytes(),
                 3:(bundle/'_CodeSignature/CodeResources').read_bytes()} if name=='NativeAdrenoMetal' else None
        signature_checker.signature(bundle/name,expected,special)
    resources=plistlib.loads((bundle/'_CodeSignature/CodeResources').read_bytes())
    for name,rule in resources['files2'].items():
        relative=Path(name)
        assert not relative.is_absolute() and '..' not in relative.parts
        path=bundle/relative
        assert path.is_file() and not path.is_symlink()
        if isinstance(rule,dict) and 'cdhash' in rule:
            assert name=='libvulkan_freedreno.dylib' and rule['cdhash'].hex()==trust['turnip_cdhash']
        elif isinstance(rule,dict) and 'hash2' in rule:
            assert hashlib.sha256(path.read_bytes()).digest()==rule['hash2']
        elif isinstance(rule,bytes):
            assert hashlib.sha1(path.read_bytes()).digest()==rule
        else:
            raise ValueError('Unqualified bundle resource '+name)
else:
    subprocess.run(['codesign','--verify','--strict',str(bundle)],check=True)
assert b'M cache: reuse published runtime writable directory' in data
assert b'M cache rejected; test not started; UI remains independent' in data
library=(a.collected/'turnip-ios27/libvulkan_freedreno.dylib' if a.collected
         else root/'build/mesa-tu-ios27/src/freedreno/vulkan/libvulkan_freedreno.dylib')
producer_path=(a.collected/'turnip-ios27/builtin-build-profile.json' if a.collected
               else root/'build/mesa-tu-ios27/builtin-build-profile.json')
producer=json.loads(producer_path.read_text())['profile']
mesa=root/'.cache/mesa-work'
actual_diff=subprocess.check_output(['git','-C',str(mesa),'diff','HEAD','--binary'])
assert producer['source_diff_sha256']==hashlib.sha256(actual_diff).hexdigest()
for name,digest in producer['extra_sources'].items():
    assert hashlib.sha256((mesa/name).read_bytes()).hexdigest()==digest,name
symbols=subprocess.check_output(['llvm-nm' if a.collected else 'nm',str(library)],text=True)
assert not re.search(r'_+ZL2dw(?:\b|\.)',symbols), 'old process-global connection survived'
for marker in (b'M-body cleanup-pending', b'SWAP begin: upstream GMEM/sysmem device+host colour checks',
               b'bad%u/%u', b'metal-work cb=', b'metal-device %s reg='):
    assert marker in data,marker
# Local data names can be stripped and local functions can have type t. Verify
# the actual lifecycle instructions/import stubs, never a discarded data label.
text_symbols=[line.split()[-1] for line in symbols.splitlines()
              if len(line.split())>=3 and line.split()[-2] in ('t','T') and '.cold' not in line]
def one(needle):
    found=[name for name in text_symbols if needle in name]
    assert len(found)==1,(needle,found)
    return found[0]
names=[one(n) for n in ['tu_knl_darwin_load','darwin_device_init','darwin_device_finish',
                       'dw_open_connection','tu_cmd_buffer_begin','tu_darwin_bo_cache_stats']]
names+= [name for name in text_symbols if 'darwin_va_finish' in name or 'darwin_va_del' in name]
assembly=subprocess.check_output(['llvm-objdump','--disassemble-symbols='+','.join(names),'--no-show-raw-insn',str(library)],text=True)
blocks={};current=None
for line in assembly.splitlines():
    match=re.match(r'^([0-9a-fA-F]+) <(.+)>:$',line)
    if match:current=match[2];blocks[current]=[];continue
    match=re.match(r'^\s*([0-9a-fA-F]+):\s+(.+)$',line)
    if match and current:blocks[current].append((int(match[1],16),re.sub(r'\s+',' ',match[2]).split(' ;')[0]))
assert all(name in blocks for name in names)
indirect=subprocess.check_output(['llvm-objdump','--macho','--indirect-symbols',str(library)],text=True)
stubs={}
for address,_,name in re.findall(r'^(0x[0-9a-fA-F]+)\s+(\d+)\s+(\S+)$',indirect,re.M):
    stubs.setdefault(name,set()).add(int(address,16))
def target(op):
    match=re.match(r'(?:bl|b) (0x[0-9a-fA-F]+)',op)
    return int(match[1],16) if match else None
def sites(block,name):return [i for i,(_,op) in enumerate(block) if target(op) in stubs[name]]
enum,init,finish,opener,begin,stats=[blocks[name] for name in names[:6]]
ei=next(i for i,(_,op) in enumerate(enum) if '<'+names[3]+'>' in op)
stack=re.match(r'add x0, sp, #(0x[0-9a-f]+|\d+)',enum[ei-2][1]);assert stack
enum_offset=int(stack[1],0)
for site in sites(enum,'_IOServiceClose'):
    match=re.match(r'ldr w0, \[sp, #(0x[0-9a-f]+|\d+)\]',enum[site-1][1]);assert match and int(match[1],0)==enum_offset
assert len(sites(enum,'_IOServiceClose'))==2
ii=next(i for i,(_,op) in enumerate(init) if '<'+names[3]+'>' in op)
owner=re.match(r'mov x0, (x\d+)',init[ii-1][1]);assert owner
owner=owner[1]
assert any(op=='mov '+owner+', x0' for _,op in init[:ii])
assert any(re.fullmatch(r'mov w1, #(?:0x1f8|504)',op) for _,op in init[:ii])
assert any(re.match(r'str '+owner+r', \[x\d+, #(?:0x1b00|6912)\]',op) for _,op in init[ii:])
state=[re.match(r'ldr (x\d+), \[x0, #(?:0x1b00|6912)\]',op) for _,op in finish[:25]]
state=[m[1] for m in state if m];assert len(state)==1;state=state[0]
closes=sites(finish,'_IOServiceClose');assert len(closes)==1
assert finish[closes[0]-1][1]=='ldr w0, ['+state+']'
lock=sites(finish,'_pthread_mutex_lock')[0];unlock=sites(finish,'_pthread_mutex_unlock')[0]
assert lock<unlock<closes[0]
next_registers=[m[1] for _,op in finish[lock:unlock]
                if (m:=re.match(r'ldr (x\d+), \['+state+r', #(?:0xd0|208)\]',op))]
assert any(re.fullmatch(r'str '+reg+r', \[x\d+\]',op) for reg in next_registers for _,op in finish[lock:unlock])
assert any(re.match(r'str xzr, \[x\d+, #(?:0x1b00|6912)\]',op) for _,op in finish)
def mutex_address(block):
    site=sites(block,'_pthread_mutex_lock')[0]
    page=re.match(r'adrp x0, (0x[0-9a-f]+)',block[site-2][1])
    offset=re.match(r'add x0, x0, #(0x[0-9a-f]+|\d+)',block[site-1][1])
    assert page and offset,block[max(0,site-5):site]
    return int(page[1],16)+int(offset[1],0)
registry=[mutex_address(block) for block in (init,finish,stats)]
assert len(set(registry))==1,'lifecycle and diagnostic callback guards differ'
# The deletion helper can be inlined or out of line. Inspect the actual reachable
# selected helper bodies; neither path may iterate through a freed parent.
reachable={names[2]}
for _ in range(len(names)):
    for name in list(reachable):
        for _,op in blocks[name]:
            for child in names[6:]:
                if '<'+child+'>' in op:reachable.add(child)
clear='\n'.join(op for name in reachable for _,op in blocks[name])
assert 'rb_node_next' not in clear
assert 'rb_tree_first' in clear and clear.count('rb_augmented_tree_remove')>=2
reserve=next(i for i,(_,op) in enumerate(begin) if 'tu_cs_reserve_space' in op)
assert re.fullmatch(r'mov w1, #(?:0x1000|4096)',begin[reserve-1][1])
branch=re.match(r'cbz w0, (0x[0-9a-f]+)',begin[reserve+1][1]);assert branch
assert re.match(r'str w0, \[x\d+, #(?:0x5a4|1444)\]',begin[reserve+2][1])
success=int(branch[1],16)
assert any(op=='retab' and pc<success for pc,op in begin[reserve+2:])
(a.logs/'actual-device-lifetimes-and-command-guard.asm').write_text(assembly)
(a.logs/'actual-import-stubs.txt').write_text(indirect)
contexts=json.loads((a.logs/'device-context-qualified.json').read_text())
assert contexts['actual_prepared_sources_match'] and contexts['new_ownership_and_guard_cases_passed']
assert contexts['real_Mesa_RB_used'] and contexts['old_VA_UAF_reproduced'] and contexts['actual_swap_API_and_lifetime_guards_passed']
tile_errors=json.loads((a.logs/'tile-store-error-host.json').read_text())
assert tile_errors['passed'] and tile_errors['actual_prepared_sources_match']
assert tile_errors['source_sha256']==hashlib.sha256((mesa/'src/freedreno/vulkan/tu_cmd_buffer.cc').read_bytes()).hexdigest()
sync_profile=json.loads((a.logs/'sync-profile/report.json').read_text())
assert sync_profile['passed'] and sync_profile['actual_producer_to_decoder_checked']
assert sync_profile['real_Mach_query']['executed'] and sync_profile['real_Mach_query']['passed']
assert sync_profile['CPU_query']=='thread_info/THREAD_BASIC_INFO' and sync_profile['CPU_wire_precision_ns']==1000
assert sync_profile['actual_source_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
display_profile=json.loads((a.logs/'display-profile/report.json').read_text())
assert display_profile['passed'] and display_profile['actual_reader'] and display_profile['actual_frame_producer'] and display_profile['actual_decoder']
tested_display_sources={str(Path(path).resolve()):digest for path,digest in display_profile['source_sha256'].items()}
for path in (root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm',root/'drivers/GopFramebuffer/NativeGOPTiming.h',
             Path(__file__).resolve().parent/'fixtures/display/NezhaDpuTiming.h',Path(__file__).resolve().parent/'decode-display-timing.py'):
    assert tested_display_sources[str(path.resolve())]==hashlib.sha256(path.read_bytes()).hexdigest()
assert b'ndt=1' in data and b'NATIVE_METAL_DISPLAY_TIMING' in data
wait_profile=json.loads((a.logs/'render-wait/report.json').read_text())
assert wait_profile['passed'] and wait_profile['actual_capture_and_fragment_code']
assert wait_profile['real_Mach_wait']['executed'] and wait_profile['real_Mach_wait']['passed']
assert wait_profile['source_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert wait_profile['header_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeRenderWaitSample.h').read_bytes()).hexdigest()
for marker in (b'_thread_get_state',b'_mach_vm_read_overwrite',b'_mach_thread_self',b'render-wait-sample'):
    assert marker in data,marker
observer_profile=json.loads((a.logs/'frame-observers/report.json').read_text())
client_profile=json.loads((a.logs/'ui-main/report.json').read_text())
assert observer_profile['passed'] and observer_profile['actual_source_reader_serializer_batch']
assert client_profile['passed'] and client_profile['real_Mac_main_thread']['executed'] and client_profile['real_Mac_main_thread']['passed']
for report in (observer_profile,client_profile):
    assert report['source_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
    assert report['display_state_header_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeDisplayWaitState.h').read_bytes()).hexdigest()
    assert report['observer_include_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeFrameObservers.inc').read_bytes()).hexdigest()
for marker in (b'pthread_main_thread_np',b'ui-main-wait-sample',b'springboard-waits.trace',b'NATIVE_METAL_DRIVER_CPU_PROFILE',b'kern.bootsessionuuid'):
    assert marker in data,marker
discovery_profile=json.loads((a.logs/'display-discovery/report.json').read_text())
assert discovery_profile['passed'] and discovery_profile['real_Mac_executed']
assert discovery_profile['actual_result']['cases']==7
assert discovery_profile['source_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert discovery_profile['observer_include_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeFrameObservers.inc').read_bytes()).hexdigest()
for marker in (b'display-wait discovery=',b'ui-wait begin',b'ui-wait end',b'budget_s=1800 max=90000'):
    assert marker in data,marker
cache_profile=json.loads((a.logs/'pipeline-cache/report.json').read_text())
assert cache_profile['passed'] and cache_profile['actual_Turnip_functions']
assert cache_profile['actual_prepared_source_sha256']==hashlib.sha256((a.unixnu/'.cache/mesa-work/src/freedreno/vulkan/tu_pipeline.cc').read_bytes()).hexdigest()
assert cache_profile['patch_sha256']==hashlib.sha256((a.unixnu/'patches/mesa/turnip-shaders-shared-across-pipeline-caches.patch').read_bytes()).hexdigest()
assert [row['objects_for_three_caches'] for row in cache_profile['rows']]==[3,1]
assert wait_profile['ARM64_integer_values_from_actual_reader'] and wait_profile['wire_version']==2
assert wait_profile['wire_bytes']==576 and wait_profile['stack_frames']==16
assert client_profile['required_read_bound_bytes']==83886080
icon_limits=json.loads((a.logs/'icon-limits/report.json').read_text())
assert icon_limits['passed'] and icon_limits['actual']['cases']==23
assert icon_limits['real_Mac_MTLDevice_getters'] and icon_limits['real_ARC_pressure_lifetime_test']
assert icon_limits['old_ARC_disposal_reproduced']
assert icon_limits['source_sha256']==hashlib.sha256((a.unixnu/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert b'base limits: %u fields of %zu bytes filled from the device' in data
extern_function=json.loads((a.logs/'extern-function/report.json').read_text())
assert extern_function['passed'] and extern_function['checks']==12
assert extern_function['production_ObjectiveC_selector_executed'] and extern_function['previous_missing_selector_reproduced']
assert extern_function['exact_kernel_bitcode_and_library_lifetime_verified']
assert extern_function['retained_frontend_patch_reversed_exactly']
assert extern_function['current_frontend_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert extern_function['test_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/bridge/extern_function_host.mm').read_bytes()).hexdigest()
multi_level=json.loads((a.logs/'multi-level-output/report.json').read_text())
assert multi_level['passed'] and multi_level['exact_runtime_source_verified'] and multi_level['prior_Mac_runtime_checks_passed']
assert multi_level['retained_exact_runtime_sources'] and multi_level['current_Mac_allocator_cases']==8
extern_spec=importlib.util.spec_from_file_location('extern_target',Path(__file__).with_name('check-extern-target.py'))
extern_target=importlib.util.module_from_spec(extern_spec)
extern_spec.loader.exec_module(extern_target)
map_path=(a.collected/'adreno-metal-plugin/NativeAdrenoMetal.map' if a.collected
          else root/'build/adreno-metal-plugin27/NativeAdrenoMetal.map')
target_extern=extern_target.verify(bundle/'NativeAdrenoMetal',map_path)
assert target_extern['passed'] and target_extern['actual_method_list_verified'] and target_extern['authenticated_message_branch']
(a.logs/'actual-extern-target.json').write_text(json.dumps(target_extern,indent=2)+'\n')
assert extern_function['icon_library_trace_methods_executed']
assert extern_function['icon_library_trace_budget']==96 and extern_function['icon_library_trace_disabled_records']==0
assert extern_function['icon_library_trace_test_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/bridge/icon_library_trace_host.mm').read_bytes()).hexdigest()
for marker in (b'ci-library read=0 file=',b'ci-names lib=',b'ci-kernel lib=',b'ci-stitch graphs='):
    assert marker in data,marker
report=dict(actual_icon_limits_host=icon_limits,actual_pipeline_cache_host=cache_profile,actual_display_discovery=discovery_profile,actual_frame_observers=observer_profile,actual_UI_main_thread=client_profile,actual_render_wait_profile=wait_profile,actual_display_profile=display_profile,actual_sync_profile=sync_profile,initial_tile_store_error_propagation_source_bound=True,actual_target_checked=True,plugin_sha256=hashlib.sha256(data).hexdigest(),
            profile=profile,required_markers_present=True,iPad_raw_reader_markers_absent=True,
            bundle_signature_verified=True,host_pixel_result='legacy host010 failure retained;host GUI path not run by this build;A840 acceptance remains separate',
            actual_device_registry_compiled=True,old_singleton_symbol_absent=True,
            actual_initial_command_reserve_compiled=True,fulltest_runtime_cache_handoff_compiled=True,
            actual_VA_tree_unlink_before_free_compiled=True,
            actual_registry_mutex_address=hex(registry[0]),static_local_data_names_not_required=True,
            actual_Mesa_producer_sources_verified=True,
            actual_enum_stack_connection_offset=enum_offset,
            actual_Mac_source_bound_context_tests=contexts,
            real_A840_shader_or_UI_verified=False,phone_operations=False)
report.update(collected_target=bool(a.collected),
              signature_verification='CodeDirectory pages and CodeResources' if a.signature_checker else 'macOS codesign')
report.update(actual_extern_function=extern_function,actual_extern_target=target_extern,actual_multi_level_output=multi_level)
function_constant = json.loads((a.logs/'function-constant/report.json').read_text())
assert function_constant['passed'] and function_constant['checks'] == 10
assert function_constant['retained_frontend_patch_reversed_exactly']
assert function_constant['current_frontend_sha256'] == hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert target_extern['required_function_constant_method_list_verified'] and target_extern['required_forwards_to_original_getter']
report['actual_function_constant'] = function_constant
camera_function = json.loads((a.logs/'camera-function-spi/report.json').read_text())
assert camera_function['passed'] and camera_function['checks'] == 37
assert camera_function['production_stitch_serialization_executed'] and camera_function['system_compiler_stitch_passed']
assert camera_function['shared_AIR_functions_same_hash']
assert camera_function['stitch_test_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/bridge/camera_stitch_host.mm').read_bytes()).hexdigest()
assert camera_function['retained_frontend_patch_reversed_exactly']
assert camera_function['current_frontend_sha256'] == hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert camera_function['metadata_source_sha256'] == hashlib.sha256((root/'drivers/AdrenoMetal/NativeMetalFunctionMetadata.h').read_bytes()).hexdigest()
assert camera_function['bridge_source_sha256'] == hashlib.sha256((root/'drivers/AdrenoMetal/bridge/NativeMetalObjects.rs').read_bytes()).hexdigest()
assert target_extern['camera_function_SPI_method_metadata_verified']
report['actual_camera_function_SPI'] = camera_function

compute_dump=json.loads((a.logs/'compute-dump/report.json').read_text())
assert compute_dump['passed'] and compute_dump['checks']==28
assert compute_dump['actual_ObjectiveC_bodies_executed'] and compute_dump['disabled_dispatch_arguments_unchanged']
assert compute_dump['compute_filter_only_matching_commands'] and compute_dump['compute_filter_budget']==2
assert compute_dump['repeated_pipeline_does_not_spend_budget'] and compute_dump['readbacks_do_not_spend_budget']
assert compute_dump['threadgroup_lengths_forwarded_unchanged'] and compute_dump['threadgroup_before_filter_selection_retained']
assert compute_dump['threadgroup_updates_and_unset_logged']
assert b'NATIVE_METAL_DUMP_COMPUTE_MATCH' in data
assert compute_dump['source_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
for marker in (b'kernel %u dispatch %s', b'CPU center %u/%u/%u', b'compute %@ k=%s', b'after %s'):
    assert marker in data, marker
report['actual_compute_dump'] = compute_dump
copy_pitch=json.loads((a.logs/'copy-pitch/report.json').read_text())
assert copy_pitch['passed'] and copy_pitch['checks']==37
assert copy_pitch['actual_Mesa_helper_executed'] and copy_pitch['actual_Mesa_register_fields']
assert copy_pitch['A8XX_pitch_in_bits'] and copy_pitch['A6XX_A7XX_unchanged']
assert copy_pitch['source_sha256']==hashlib.sha256((root/'.cache/mesa-work/src/freedreno/vulkan/tu_descriptor_set.h').read_bytes()).hexdigest()
report['actual_copy_pitch'] = copy_pitch

planes=json.loads((a.logs/'surface-plane-forwarding/report.json').read_text())
assert planes['passed'] and planes['checks']==15 and planes['actual_ObjectiveC_bodies_executed']
assert planes['source_sha256']==hashlib.sha256((root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_bytes()).hexdigest()
assert target_extern['actual_surface_plane_method_verified'] and target_extern['surface_plane_constant_zero_getter_absent']
report.update(actual_surface_plane_forwarding=planes, actual_surface_plane_target=target_extern['surface_plane_IMP'])
small_yuv=json.loads((a.logs/'small-yuv-layout/report.json').read_text())
assert small_yuv['passed'] and small_yuv['checks']==300
assert small_yuv['actual_image_selection_block'] and small_yuv['actual_fdl_fallback_block']
assert small_yuv['source_sha256']==hashlib.sha256((root/'.cache/mesa-work/src/freedreno/vulkan/tu_image.cc').read_bytes()).hexdigest()
report['actual_small_yuv_layout']=small_yuv

(a.logs/'full-target-qualified.json').write_text(json.dumps(report,indent=2)+'\n')

print(json.dumps({k:v for k,v in report.items() if k!='profile'},indent=2))
