#!/usr/bin/env python3
"""Qualify cancellation-specific admission through the original refresh body.

Executes patched selector/thunk/frame/gate instructions. PAC and the kdebug SVC
boundary are modeled, and Swift task allocation is an explicit bounded model.
The original query/publish body, timers and private credentials are unchanged.
"""
import argparse,hashlib,importlib.util,itertools,json,re,struct,subprocess
from pathlib import Path
import capstone,unicorn
from unicorn import arm64_const as reg
IMAGE='/System/Library/PrivateFrameworks/WiFiSettingsKit.framework/WiFiSettingsKit'
UUID='8b098e4613a83ed68b75ffdc063460aa'
CAVE=0x2b72aa8a0
WRAPPER=0x2b74156d0
TASK=0x2b738ffb8
ENTRY=0x2b742355c
BODY=0x2b74236d4
ORIGINAL_DESCRIPTOR=0x2b74d3678
FRAME_DESCRIPTOR=0x2b74d3508
CANCEL_RETURN=0x2b7315e4c
SELECT_RESUME=0x2b74157dc
TASK_RESUME=TASK+4
ENTRY_RESUME=0x2b7423588
ADMIT,COALESCED,QUERY=0x2b74237d4,0x2b7423750,0x2b74239dc
ALLOC=0x2b809dc30
IVAR=0x2cdce0770
MODE_OFFSET=0x5d8
FRAME_BYTES=0x5e0
PATCHES=[(0x2b74157d4,0x2b74157d4,'wifi27_refresh_descriptor_select'),
         (TASK,TASK,'wifi27_normal_refresh_entry'),
         (0x2b7423584,0x2b7423584,'wifi27_store_refresh_mode'),
         (0x2b7423740,0x2b742374c,'wifi27_refresh_window_gate')]
def digest(v):return hashlib.sha256(v).hexdigest()
def branch(at,to):
 d=to-at;assert d%4==0 and -(1<<27)<=d<(1<<27);return struct.pack('<I',0x14000000|((d//4)&0x3ffffff))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache-reader',required=True,type=Path);p.add_argument('--evidence-dir',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--llvm',type=Path,default=Path('/usr/lib/llvm-18/bin'));a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 sp=importlib.util.spec_from_file_location('stock_owned',a.cache_reader);m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m);cache=m.Cache();im=cache.images[IMAGE];assert im['uuid']==UUID
 md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN);md.detail=True
 originals={}
 for f in [WRAPPER,TASK,ENTRY,BODY,0x2b7424634,0x2b7424728,0x2b7315ba0]:
  e=json.loads((a.evidence_dir/(hex(f)+'.json')).read_text());data=cache.read(f,int(e['end'],16)-f);assert digest(data)==e['sha256'] and e['image']==IMAGE;originals[f]=(data,e)
 assert struct.unpack('<iI',cache.read(FRAME_DESCRIPTOR,8))==(ENTRY-FRAME_DESCRIPTOR,FRAME_BYTES)
 assert struct.unpack('<iI',cache.read(ORIGINAL_DESCRIPTOR,8))==(TASK-ORIGINAL_DESCRIPTOR,32)
 # The existing coroutine's highest direct x22 field ends exactly at0x5d8.
 # Its allocated0x5e0 frame has an unused8-byte tail, not an added ABI field.
 accesses=[]
 for f in [ENTRY,BODY]:
  for i in md.disasm(originals[f][0],f):
   for op in i.operands:
    if op.type==capstone.arm64.ARM64_OP_MEM and md.reg_name(op.mem.base)=='x22':
     first=next((o for o in i.operands if o.type==capstone.arm64.ARM64_OP_REG),None)
     name=md.reg_name(first.reg) if first else ''
     width=16 if name.startswith('q') else 4 if name.startswith('w') else 8
     if i.mnemonic.startswith(('strb','ldrb','ldurb','sturb')):width=1
     if i.mnemonic.startswith(('stp','ldp')):width*=2
     start=op.mem.disp;end=start+width;assert end<=MODE_OFFSET,(hex(i.address),i.mnemonic,i.op_str)
     accesses.append((hex(i.address),start,end))
   if i.mnemonic in ['add','sub'] and len(i.operands)>2 and i.operands[1].type==capstone.arm64.ARM64_OP_REG and md.reg_name(i.operands[1].reg)=='x22' and i.operands[2].type==capstone.arm64.ARM64_OP_IMM:
    assert i.operands[2].imm<MODE_OFFSET
 assert max(t[2] for t in accesses)==MODE_OFFSET
 prefix=list(md.disasm(cache.read(ENTRY,0x30),ENTRY))
 assert not any(md.reg_name(r) in ('x0','w0') for i in prefix for r in i.regs_access()[0]), 'x0 must be dead before its first overwrite'
 wp=[(i.mnemonic,i.op_str) for i in md.disasm(cache.read(WRAPPER,24),WRAPPER)]
 assert wp==[('pacibsp',''),('stp','x22, x21, [sp, #-0x30]!'),('stp','x20, x19, [sp, #0x10]'),('stp','x29, x30, [sp, #0x20]'),('add','x29, sp, #0x20'),('mov','x19, x20')]
 # Query-prefix live inputs and window-reset creation are kept on their original
 # paths. Forced queries reproduce x28 from the original x27 string constant.
 qprefix=[(i.mnemonic,i.op_str) for i in md.disasm(cache.read(QUERY,0x38),QUERY)]
 assert qprefix[3]==('ldr','x8, [x26, #0xd70]') and qprefix[6]==('add','x23, x22, #0xb8')
 assert not any(c.get('name') and '_swift_task_switch' in c['name'] for c in originals[BODY][1]['calls'])
 assert any(c.get('name') and '_swift_task_create' in c['name'] and ADMIT<=int(c['pc'],16)<QUERY for c in originals[BODY][1]['calls'])
 assert any(int(c['pc'],16)==CANCEL_RETURN-4 and int(c['target'],16)==WRAPPER for c in originals[0x2b7315ba0][1]['calls'])
 header=cache.read(im['va'],32);assert im['va']+32+struct.unpack_from('<I',header,20)[0]==CAVE and not any(cache.read(CAVE,328))
 source=Path(__file__).with_name('wifi27-cancel-force-refresh.S');obj=a.out/'force.o';elf=a.out/'force.elf';binary=a.out/'force.bin'
 defs={'wifi27_cancel_refresh_return':CANCEL_RETURN,'wifi27_original_refresh_descriptor':ORIGINAL_DESCRIPTOR,'wifi27_descriptor_resume':SELECT_RESUME,'wifi27_original_task_resume':TASK_RESUME,'wifi27_refresh_entry_resume':ENTRY_RESUME,'wifi27_original_query_body':QUERY,'wifi27_original_window_admit':ADMIT,'wifi27_original_coalesced_return':COALESCED}
 cmds=[[a.llvm/'llvm-mc','-triple=aarch64-none-elf','-filetype=obj',source,'-o',obj],[a.llvm/'ld.lld','-m','aarch64elf','-Ttext='+hex(CAVE),'-e','wifi27_refresh_descriptor_select',*[f'--defsym={k}={v:#x}' for k,v in defs.items()],obj,'-o',elf],[a.llvm/'llvm-objcopy','-O','binary','--only-section=.text',elf,binary]]
 for cmd in cmds:subprocess.run(list(map(str,cmd)),check=True,capture_output=True)
 code=binary.read_bytes();assert len(code)<=328 and len(code)%4==0
 symbols={}
 for line in subprocess.check_output([str(a.llvm/'llvm-nm'),'-n',str(elf)],text=True).splitlines():
  value,kind,name=line.split();symbols[name]=int(value,16)
 desc=symbols['wifi27_forced_refresh_descriptor'];off=desc-CAVE
 assert struct.unpack_from('<iI',code,off)==(symbols['wifi27_forced_refresh_entry']-desc,32)
 assert off+8==len(code)
 ins=list(md.disasm(code[:off],CAVE));assert len(ins)*4==off and sum(i.mnemonic=='svc' for i in ins)==1
 (a.out/'force.asm').write_text('\n'.join(f'{i.address:#x}: {i.mnemonic} {i.op_str}' for i in ins)+'\n')
 patches=[(CAVE+k,bytes(min(16,len(code)-k)),code[k:k+16]) for k in range(0,len(code),16)];patched={}
 for start,site,name in PATCHES:
  before=cache.read(start,16);delta=site-start;after=before[:delta]+branch(site,symbols[name])+before[delta+4:];patches.append((start,before,after));patched[start]=after
 expected={0x2b74157d4:('adrp','x16, #0x2b74d3000'),TASK:('pacibsp',''),0x2b7423584:('str','x4, [x22, #0x590]'),0x2b742374c:('tbz','w9, #0, #0x2b74237d4')}
 for at,value in expected.items():i=next(md.disasm(cache.read(at,4),at));assert (i.mnemonic,i.op_str)==value
 assert cache.read(0x2b74157d8,4).hex()=='10e21991' # add descriptor low offset; selector supplies the full value
 entries=[]
 for va,before,after in patches:
  mp=next(x for x in cache.mappings if x['va']<=va<x['end']);offset=mp['offset']+va-mp['va'];assert mp['initprot']==mp['maxprot']==5 and offset%4096+len(before)<=4096 and cache.file_read(mp['name'],offset,len(before))==before
  text=f"{mp['name']},{offset:x},{before.hex()},{after.hex()}";assert len(text)<160;entries.append({'va':hex(va),'file':mp['name'],'offset':hex(offset),'original':before.hex(),'replacement':after.hex(),'entry':text})
 assert len(entries)<=32
 def machine(slide):
  u=unicorn.Uc(unicorn.UC_ARCH_ARM64,unicorn.UC_MODE_ARM);mapped=set()
  def page(at):
   base=at&~4095
   if base not in mapped:u.mem_map(base,4096);mapped.add(base)
  for f,(raw,e) in originals.items():
   for base in range(f&~4095,(f+len(raw)+4095)&~4095,4096):page(base+slide)
   u.mem_write(f+slide,raw)
  page(CAVE+slide);u.mem_write(CAVE+slide,code)
  for start,data in patched.items():page(start+slide);u.mem_write(start+slide,data)
  page(FRAME_DESCRIPTOR+slide);u.mem_write(FRAME_DESCRIPTOR+slide,cache.read(FRAME_DESCRIPTOR,16))
  page(IVAR+slide);u.mem_write(IVAR+slide,struct.pack('<Q',0x80))
  u.mem_map(0x70000000,0x10000);u.mem_map(0x71000000,0x30000)
  return u
 def rr(u,n):return u.reg_read(getattr(reg,'UC_ARM64_REG_X'+str(n)))
 def wr(u,n,v):u.reg_write(getattr(reg,'UC_ARM64_REG_X'+str(n)),v)
 def init(u):
  for n in range(31):wr(u,n,0x12340000+n*16)
  u.reg_write(reg.UC_ARM64_REG_SP,0x70008000);u.reg_write(reg.UC_ARM64_REG_NZCV,0xa0000000)
  for n in range(32):u.reg_write(getattr(reg,'UC_ARM64_REG_Q'+str(n)),(1<<100)+n)
 def selector(caller,valid,slide):
  u=machine(slide);init(u);fp=0x70009000;wr(u,29,fp);saved=(caller+slide)|(0x51ab<<48);u.mem_write(fp+8,struct.pack('<Q',saved));regs=[rr(u,n) for n in range(31)];done=[]
  def hook(u,pc,size,_):
   if pc==SELECT_RESUME+slide:done.append(True);u.emu_stop();return
   i=next(md.disasm(bytes(u.mem_read(pc,4)),pc))
   if i.mnemonic=='autib':
    assert i.op_str=='x8, x9' and rr(u,9)==fp+16
    wr(u,8,caller+slide if valid else (caller+slide)|(1<<55));u.reg_write(reg.UC_ARM64_REG_PC,pc+4)
  u.hook_add(unicorn.UC_HOOK_CODE,hook);u.emu_start(0x2b74157d4+slide,SELECT_RESUME+slide+4,count=100)
  force=valid and caller==CANCEL_RETURN;want=(desc if force else ORIGINAL_DESCRIPTOR)+slide
  assert done and rr(u,16)==want and struct.unpack('<Q',u.mem_read(fp+8,8))[0]==saved
  assert all(rr(u,n)==v for n,v in enumerate(regs) if n not in [8,9,16])
  assert u.reg_read(reg.UC_ARM64_REG_SP)==0x70008000 and u.reg_read(reg.UC_ARM64_REG_NZCV)==0xa0000000
  return {'caller':hex(caller),'auth_valid_modeled':valid,'slide':hex(slide),'forced_descriptor':force,'saved_authenticated_return_untouched':True}
 def task(mode,incoming,slide):
  u=machine(slide);init(u);parent=0x71001000;capture=0x71003000;frame=0x71008000;selfptr=0x71015000;meta=0x71016000
  wr(u,0,incoming);wr(u,20,capture);wr(u,22,parent);u.mem_write(capture+0x10,struct.pack('<QQQQ',0x71014000,0x71014800,selfptr,meta));u.mem_write(frame,bytes([0xcc])*FRAME_BYTES);initial=bytes(u.mem_read(frame,FRAME_BYTES));done=[];calls=[]
  def hook(u,pc,size,_):
   if pc==ENTRY_RESUME+slide:done.append(True);u.emu_stop();return
   i=next(md.disasm(bytes(u.mem_read(pc,4)),pc))
   if i.mnemonic.startswith(('pac','aut')):u.reg_write(reg.UC_ARM64_REG_PC,pc+4);return
   if i.mnemonic=='bl' and i.operands[0].imm==ALLOC+slide:
    assert rr(u,0)==FRAME_BYTES;calls.append(FRAME_BYTES)
    for n in range(19):wr(u,n,0xbad000+n)
    wr(u,0,frame);wr(u,30,pc+4);u.reg_write(reg.UC_ARM64_REG_PC,pc+4)
  u.hook_add(unicorn.UC_HOOK_CODE,hook);start=(symbols['wifi27_forced_refresh_entry'] if mode else TASK)+slide;u.emu_start(start,ENTRY_RESUME+slide+4,count=400)
  assert done and calls==[FRAME_BYTES] and rr(u,22)==frame and rr(u,3)==selfptr and rr(u,4)==meta
  assert struct.unpack('<Q',u.mem_read(frame+MODE_OFFSET,8))[0]==mode and struct.unpack('<Q',u.mem_read(frame+0x590,8))[0]==meta
  actual=bytes(u.mem_read(frame,FRAME_BYTES));allowed=set(range(16))|set(range(0x590,0x598))|set(range(MODE_OFFSET,FRAME_BYTES));assert all(actual[i]==initial[i] for i in range(FRAME_BYTES) if i not in allowed)
  return {'mode':mode,'incoming_dead_void_argument':hex(incoming),'slide':hex(slide),'original_task_forwarder_executed':True,'original_frame_allocation_bytes':FRAME_BYTES,'only_unused_tail_mode_added':True}
 def gate(mode,window,error,slide):
  u=machine(slide);init(u);frame=0x71008000;selfptr=0x71015000;sp0=0x70008000;u.mem_write(frame,bytes([0xa5])*FRAME_BYTES);u.mem_write(frame+MODE_OFFSET,struct.pack('<Q',mode));u.mem_write(selfptr,bytes([0x5a])*512);u.mem_write(selfptr+0x80,bytes([window]));wr(u,8,IVAR+slide-0x770);wr(u,22,frame);wr(u,23,selfptr);wr(u,24,0xeb000000005d4543);wr(u,25,0x41465245544e495b);wr(u,27,0xd00000000000002f);wr(u,28,0x12345555)
  fm=bytes(u.mem_read(frame,FRAME_BYTES));obj=bytes(u.mem_read(selfptr,512));parent=b'parent-frame'.ljust(128,b'!');u.mem_write(sp0,parent);events=[];done=[];writes=[];dest=QUERY if mode and window&1 else COALESCED if window&1 else ADMIT
  before={n:rr(u,n) for n in range(19,31)}
  def hook(u,pc,size,_):
   if pc in [v+slide for v in [QUERY,COALESCED,ADMIT]]:done.append(pc);u.emu_stop();return
   i=next(md.disasm(bytes(u.mem_read(pc,4)),pc))
   if i.mnemonic=='svc':
    assert rr(u,16)==179;events.append([rr(u,n) for n in range(5)])
    for n in range(19):wr(u,n,0xbad000+n)
    wr(u,0,error);wr(u,30,0xbad30);u.reg_write(reg.UC_ARM64_REG_NZCV,0x20000000 if error else 0);u.reg_write(reg.UC_ARM64_REG_PC,pc+4)
  u.hook_add(unicorn.UC_HOOK_CODE,hook);u.hook_add(unicorn.UC_HOOK_MEM_WRITE,lambda u,access,addr,size,value,_:writes.append((addr,size)));u.emu_start(0x2b7423740+slide,dest+slide+4,count=400)
  assert done==[dest+slide]
  assert events==([[0x2b28e300,selfptr,1,window,0]] if mode else [])
  assert bytes(u.mem_read(frame,FRAME_BYTES))==fm and bytes(u.mem_read(selfptr,512))==obj and bytes(u.mem_read(sp0,128))==parent
  assert u.reg_read(reg.UC_ARM64_REG_SP)==sp0 and u.reg_read(reg.UC_ARM64_REG_NZCV)==0xa0000000
  for n,v in before.items():assert rr(u,n)==(0x2cedc6000+slide if n==26 else before[27] if n==28 and dest==QUERY else v),(n,hex(rr(u,n)),hex(v))
  assert all(sp0-0xb0<=at and at+n<=sp0 for at,n in writes)
  for n in range(32):assert u.reg_read(getattr(reg,'UC_ARM64_REG_Q'+str(n)))==(1<<100)+n
  return {'mode':mode,'window':window,'modeled_trace_errno':error,'slide':hex(slide),'route':{QUERY:'original-query-only',COALESCED:'original-coalesced-return',ADMIT:'original-new-window-and-query'}[dest],'existing_window_unchanged':True,'additional_reset_timer_created':False if dest==QUERY else None,'event':events,'original_query_live_values_initialized':True}
 cases=[]
 for caller,valid,slide in itertools.product([CANCEL_RETURN,CANCEL_RETURN+4,0x2b74514f8,0x2b7373c10],(False,True),(0,0x130c000,0x20d8000)):cases.append({'selector':selector(caller,valid,slide)})
 for mode,incoming,slide in itertools.product((0,1),(0,1,0x71020000),(0,0x20d8000)):cases.append({'task':task(mode,incoming,slide)})
 for mode,window,error,slide in itertools.product((0,1),(0,1),(0,13),(0,0x130c000,0x20d8000)):cases.append({'gate':gate(mode,window,error,slide)})
 identities={hex(f):{'sha256':e['sha256'],'end':e['end']} for f,(_,e) in originals.items()}
 result={'passed':True,'candidate_change':'Only original cancel caller can request a refresh inside the existing coalescing window; normal callers retain original behavior','image':IMAGE,'image_uuid':UUID,'helper_bytes':len(code),'helper_sha256':digest(code),'helper_source_sha256':digest(source.read_bytes()),'symbols':{k:hex(v) for k,v in symbols.items()},'text_fixups':entries,'case_count':len(cases),'cases':cases,'original_code_identities':identities,'frame_bytes':FRAME_BYTES,'unused_frame_tail':[MODE_OFFSET,FRAME_BYTES],'frame_direct_accesses':accesses,'dead_void_input_explicitly_normalized':True,'original_query_body_unchanged':True,'original_window_flag_not_modified_by_forced_path':True,'original_timer_and_interval_bytes_unchanged':True,'no_extra_timer_on_forced_busy_path':True,'caller_return_copy_authenticated':True,'old_behavior_fixes_and_name_probes_not_in_this_table':True,'trace_code':'0x2b28e300','trace_fields':['WiFiInterface object','forced mode','original window flag','reserved0'],'phone_operations':False,'hardware_verified':False,'limitations':['PAC authentication and the SVC boundary are explicit models, not hardware cryptographic execution.','Swift task allocation is modeled; the original forwarding instructions, marker store and gate execute.','The original CWF query/publish implementation is retained by byte identity and control flow; its Foundation/XPC behavior is not emulated.','The physical coalescing branch was not directly observed in022; its signature and the fast/slow restoration evidence support this candidate, which still requires rapid-cancel and genuine-join phone tests.']}
 (a.out/'report.json').write_text(json.dumps(result,indent=2)+'\n');(a.out/'text-fixups.txt').write_text('\n'.join(x['entry'] for x in entries)+'\n');cache.save(a.out/'cache-ranges.json');print(json.dumps({'passed':True,'cases':len(cases),'helper_bytes':len(code),'fixups':len(entries),'hardware_verified':False}))
if __name__=='__main__':main()
