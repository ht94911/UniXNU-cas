#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import json,shutil,subprocess
import argparse
ap=argparse.ArgumentParser(description='Run actual CPU0 loader control flow with host platform/EBS doubles; never execute XNU.')
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--kernel',type=Path,required=True)
ap.add_argument('--adt',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--cases',help='comma-separated existing case indices; default all; use affected cases for unchanged live guards')
a=ap.parse_args();D=Path(__file__).resolve().parents[1];W=D.parent
BUILD=a.build.resolve();OUT=a.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
driver=BUILD/'overlay/DarwinBootPkg/Driver'
for p in driver.iterdir():
 if p.is_file():shutil.copyfile(p,OUT/p.name)
s=(OUT/'DarwinBoot.c').read_text();old='#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })';assert s.count(old)==1;(OUT/'DarwinBoot.c').write_text(s.replace(old,'STATIC UINT64 CasHostReadReg(CONST CHAR8 *Name);\n#define READ_SYSREG(Name) CasHostReadReg(#Name)'))
s=(OUT/'Trace.h').read_text();old='__asm__ volatile ("mov %0, sp" : "=r"(mTrace.Stack));';assert s.count(old)==1;(OUT/'Trace.h').write_text(s.replace(old,'mTrace.Stack=0xfeed0000;'))
shutil.copyfile(D/'tests/cpu0_host.c',OUT/'harness.c');inc=W/'uefi-cas/Mu_Basecore/MdePkg/Include'
argv=['clang-18','-std=c11','-D_GNU_SOURCE','-fshort-wchar','-fno-builtin','-O1','-g','-Werror','-Wno-unused-variable','-Wno-unused-but-set-variable','-I'+str(OUT),'-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(W/'UniXNU/package/UniXNUPkg/Include'),str(OUT/'harness.c'),str(OUT/'DiagCore.c'),'-o',str(OUT/'probe')]
with (OUT/'compile.log').open('x') as f:subprocess.run(argv,stdout=f,stderr=subprocess.STDOUT,check=True)
kernel=a.kernel.resolve();adt=a.adt.resolve()
names=['positive handoff stub','wrong CPU subtype','RO allocation','missing conventional window','wrong log mapping','missing ADT','legitimate executable allocation','fresh facts denied','held readback tail failure','EBS stale key once','EBS device error','EBS retries exhausted','map fails after EBS attempt','map fails before EBS','validation returns/releases before EBS','non-whitelisted boot args rejected','prior record CRC corrupt','foreign candidate','same boot validation','old XNU entry is not validation','missing prior validation','wrong prior ABI','torn prior raw pair','failed prior status','old unshifted prior range','missing held-pass flag','zero prior boot stamp','zero prior sequence','current BSP affinity is not CPU0','prior validation from wrong CPU affinity']
results=[]
selected=list(range(len(names))) if a.cases is None else [int(x) for x in a.cases.split(',')]
if not selected or len(set(selected))!=len(selected) or any(x<0 or x>=len(names) for x in selected):raise ValueError('invalid case selection')
for mode in selected:
 name=names[mode]
 p=subprocess.run([str(OUT/'probe'),str(kernel),str(adt),str(mode),str(OUT/f'trace-{mode}.bin')],capture_output=True,text=True)
 (OUT/f'run-{mode}.log').write_text(p.stdout+p.stderr)
 assert p.returncode==0,(mode,name,p.returncode,p.stdout,p.stderr)
 result=json.loads(p.stdout);result['case']=name;results.append(result)
require_recovered=bool(json.loads((D/'config/cpu0-contract.json').read_text()).get('requires_recovered_validation',True))
data={'result':'pass','cases':results,'actual_held_probe_core':True,'full_256mib_patterns':True,'board_FDT_SMEM_UEFI_facts':'platform callback double; actual producer separately target-compiled and source-bound','hardware_tested':False,'XNU_executed':False,'recovered_validation_required':require_recovered,'historical_records_never_replace_fresh_held_probe':True,'validation_can_run_without_prior_record':True,'firmware_services_after_EBS_attempt':'only GetMemoryMap/EBS; prohibited CRC/pool calls abort the test'}
(OUT/'result.json').write_text(json.dumps(data,indent=2)+'\n');print(json.dumps(data,indent=2))
