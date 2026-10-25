#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import json,shutil,subprocess
import argparse
ap=argparse.ArgumentParser(description='Test actual trace-viewer C with host service doubles; no phone operations.')
ap.add_argument('--driver-build',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--evidence-root',type=Path,required=True);ap.add_argument('--kernel',type=Path,required=True)
a=ap.parse_args();D=Path(__file__).resolve().parents[1];WORK=D.parent;BASE=D/'tests'
p=D/'package/CasHandoff/TraceView.c';driver=a.driver_build.resolve()/'overlay/DarwinBootPkg/Driver'
if p.read_bytes()!=(driver.parent/'TraceView/TraceView.c').read_bytes():raise ValueError('viewer source differs from the actual target build')
OUT=a.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
for f in driver.iterdir():
 if f.is_file():shutil.copyfile(f,OUT/f.name)
shutil.copyfile(p,OUT/'TraceView.c')
s=(OUT/'DarwinBoot.c').read_text();old='#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })';assert s.count(old)==1;(OUT/'DarwinBoot.c').write_text(s.replace(old,'#define READ_SYSREG(Name) (4ULL)'))
s=(OUT/'Trace.h').read_text();old='__asm__ volatile ("mov %0, sp" : "=r"(mTrace.Stack));';assert s.count(old)==1;(OUT/'Trace.h').write_text(s.replace(old,'mTrace.Stack=0xfeed0000;'))
harness=(BASE/'handoff_host.c').read_text();harness=harness.replace('#include "DarwinBoot.c"','#include "DarwinBoot.c"\n#include "TraceView.c"\nstatic int ViewRecord=-1,ViewRaw=-1;')
old='UINTN EFIAPI Print(CONST CHAR16 *F,...){(void)F;return 0;}'
new='''UINTN EFIAPI Print(CONST CHAR16 *F,...){char Text[128];size_t I;va_list Args;CONST CHAR16 *P;
 for(I=0;I<127 && F[I];I++)Text[I]=(char)F[I];Text[I]=0;
 if(strstr(Text,"Record status:") || strstr(Text,"Raw pair")){va_start(Args,F);P=va_arg(Args,CONST CHAR16 *);va_end(Args);
  if(strstr(Text,"Record status:"))ViewRecord=P[0]==L'C';else ViewRaw=P[0]==L'v';
 }return 0;}'''
assert harness.count(old)==1;harness=harness.replace(old,new)
extra='''
static EFI_STATUS EFIAPI Clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T){(void)T;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI KeyRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *T,EFI_INPUT_KEY *K){(void)T;K->ScanCode=0;K->UnicodeChar=L'\\r';return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Wait(UINTN N,EFI_EVENT *E,UINTN *I){(void)N;(void)E;*I=0;return EFI_SUCCESS;}
static void ViewerReadonlyTests(void){
 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out={0};EFI_SIMPLE_TEXT_INPUT_PROTOCOL In={0};CAS_HANDOFF_TRACE Original,Expected;unsigned Case;
 memcpy(&Original,(void *)(UINTN)CAS_TRACE_BASE,256);Out.ClearScreen=Clear;In.ReadKeyStroke=KeyRead;In.WaitForKey=(void *)1;gST->ConOut=&Out;gST->ConIn=&In;gBS->WaitForEvent=Wait;
 for(Case=0;Case<5;Case++){
  Expected=Original;if(Case==1)Expected.Address^=1;if(Case==2)Expected.RawStageInverse^=1;
  if(Case==3){Expected.RawStage=0;Expected.RawStageInverse=~0ULL;}
  if(Case==4){Expected.RawStage=0x99;Expected.RawStageInverse=~0x99ULL;}
  memcpy((void *)(UINTN)CAS_TRACE_BASE,&Expected,256);ViewRecord=-1;ViewRaw=-1;
  if(CasTraceViewEntry(NULL,gST)!=EFI_SUCCESS)abort();
  if(ViewRecord!=(Case==1?0:1) || (Case!=1 && ViewRaw!=(Case==0?1:0)))abort();
  if(memcmp((void *)(UINTN)CAS_TRACE_BASE,&Expected,256) || PoolCount)abort();
 }
 memcpy((void *)(UINTN)CAS_TRACE_BASE,&Original,256);
}
'''
harness=harness.replace('int main(int Argc,char **Argv){',extra+'\nint main(int Argc,char **Argv){')
harness=harness.replace(' free(KernelInput);free(TreeInput);return 0;',' if(Mode==0)ViewerReadonlyTests();\n free(KernelInput);free(TreeInput);return 0;')
(OUT/'harness.c').write_text(harness)
inc=WORK/'uefi-cas/Mu_Basecore/MdePkg/Include'
argv=['clang-18','-std=c11','-D_GNU_SOURCE','-fshort-wchar','-fno-builtin','-O1','-g','-Werror','-Wno-unused-variable','-Wno-unused-but-set-variable','-I'+str(OUT),'-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(WORK/'UniXNU/package/UniXNUPkg/Include'),str(OUT/'harness.c'),'-o',str(OUT/'probe')]
with (OUT/'compile.log').open('x') as log:subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,check=True)
root=a.evidence_root.resolve()
with (OUT/'run.log').open('x') as log:subprocess.run([str(OUT/'probe'),str(a.kernel.resolve()),str(root/'records/xnu-adt/xa01-001/fixture-cpu0-001/cas.adt'),'0',str(OUT/'trace.bin')],stdout=log,stderr=subprocess.STDOUT,check=True)
result={'result':'pass','viewer_cases':['valid CRC and stage pair','bad immutable body CRC','torn stage inverse','zero raw stage','unknown raw stage'],'fixed_slot_unchanged_by_every_view':True,'UEFI_pool_leaks':0,'hardware_tested':False}
(OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
