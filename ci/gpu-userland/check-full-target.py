#!/usr/bin/env python3
"""Verify actual target artifacts contain the intended safe full-chain path."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve()
bundle=root/'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle'
data=(bundle/'NativeAdrenoMetal').read_bytes()
profile=json.loads((bundle/'gpu-build.json').read_text())
assert profile['target']=='ios27' and profile['iphone_24a446'] and not profile['ipad_private_layout_enabled']
for text in (b'PUB iPhone24A446: public policy; iPad raw offsets disabled',
             b'FULL begin: raw,Turnip,Metal checks; UI remains independent',
             b'FULL returned CP=%d; T/M/F are separate verdicts'):
    assert text in data,text
for text in (b'ca virtualized key %.*s = %u',b'ca virtualized key ? flags %llx = %u'):
    assert text not in data,'iPad-only raw reader survived in the target artifact'
subprocess.run(['codesign','--verify','--strict',str(bundle)],check=True)
assert b'M cache: reuse published runtime writable directory' in data
assert b'M cache rejected; test not started; UI remains independent' in data
library=root/'build/mesa-tu-ios27/src/freedreno/vulkan/libvulkan_freedreno.dylib'
producer=json.loads((root/'build/mesa-tu-ios27/builtin-build-profile.json').read_text())['profile']
mesa=root/'.cache/mesa-work'
actual_diff=subprocess.check_output(['git','-C',str(mesa),'diff','HEAD','--binary'])
assert producer['source_diff_sha256']==hashlib.sha256(actual_diff).hexdigest()
for name,digest in producer['extra_sources'].items():
    assert hashlib.sha256((mesa/name).read_bytes()).hexdigest()==digest,name
symbols=subprocess.check_output(['nm',str(library)],text=True)
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
report=dict(actual_display_profile=display_profile,actual_sync_profile=sync_profile,initial_tile_store_error_propagation_source_bound=True,actual_target_checked=True,plugin_sha256=hashlib.sha256(data).hexdigest(),
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
(a.logs/'full-target-qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='profile'},indent=2))
