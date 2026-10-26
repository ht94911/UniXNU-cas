#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Local CPU0/held-range guard overlay; no device or GitHub operations."""
import argparse,hashlib,json,shutil
from pathlib import Path
import cas_handoff as parent
from cpu0_identity import candidate_tag
ORIGINAL_PREPARE=parent.prepare
ROOT=Path(__file__).resolve().parents[1]
MODE='execute'
ADT_AUDIT=None
def prepare(framework,out,uefi=None,contract_file=None):
 original=ORIGINAL_PREPARE(framework,out)
 (out/'overlay.json').rename(out/'parent-x06-overlay.json')
 pkg=out/'DarwinBootPkg';driver=pkg/'Driver';uefi=uefi or framework.parent/'uefi-cas'
 if ADT_AUDIT is None:raise ValueError('private ADT audit required for candidate identity')
 adt_meta=json.loads(ADT_AUDIT.read_text())
 if adt_meta.get('mode')!='private' or adt_meta.get('cpu_profile')!='cpu0' or adt_meta.get('entropy_bytes')!=64:raise ValueError('private CPU0 entropy audit required')
 contract=json.loads((contract_file or ROOT/'config/cpu0-contract.json').read_text())
 requested_base=int(contract['owned']['base'],0)
 if contract['owned']['bytes']!=0x10000000 or requested_base%4096 or requested_base<0x100000000 or requested_base+0x10000000>0x120000000:raise ValueError('fixed CPU0 range must fit the existing512MiB container')
 if contract.get('identity_version',1)==2:
  expected_base=0x100000000+(int(contract['kernel']['minimum_va'],0)&0x1fffff)
  if requested_base!=expected_base:raise ValueError('fixed PA must match the actual Mach-O minimum VA modulo2MiB')

 base_args='-v keepsyms=1 debug=0x14e cpus=1'
 execute_args=contract['command_lines']['execute']
 if execute_args not in (base_args,base_args+' rd=md0') or contract['command_lines']['validate']!=execute_args+' cas-validate=1':raise ValueError('only explicit single-core RAM-root arguments are supported')
 source_heads={}
 for label,repo in [('device',ROOT),('uefi',uefi),('framework',framework)]:
  repo_state=parent.state(repo)
  if repo_state['dirty']:raise ValueError('CPU0 candidate requires frozen clean sources')
  source_heads[label]=repo_state['head']
 tag=candidate_tag(contract,MODE,adt_meta['adt_sha256'],source_heads)
 require_recovered=contract.get('requires_recovered_validation',True)
 if type(require_recovered) is not bool:raise ValueError('explicit boolean recovery policy required')
 tag_header='// SPDX-License-Identifier: BSD-2-Clause-Patent\n#define CAS_CPU0_CANDIDATE_TAG 0x%016xULL\n#define CAS_CPU0_REQUIRE_RECOVERED %d\n'%(tag,int(require_recovered))
 (driver/'CandidateTag.h').write_text(tag_header);(pkg/'TraceView/CandidateTag.h').write_text(tag_header)

 app=uefi/'Platforms/Xiaomi/casPkg/Applications/CasDiag'
 staged={}
 for name in ['CasDiag.c','DiagCore.c','DiagCore.h','SmemCore.c','SmemCore.h']:
  source=app/name;shutil.copyfile(source,driver/name);staged[str(source)]=parent.digest(source.read_bytes())
 # Keep only the exact reviewed facts and held-probe helpers. The loader
 # translation unit contains no publication action, menu, warm reset or legacy
 # allocation/release probe. The original app retains those explicit actions.
 source=driver/'CasDiag.c';facts=source.read_text()
 for marker in ['// These helpers query descriptors','STATIC BOOLEAN HighUefiAbsent(', 'STATIC int ProbeWrite(', 'STATIC int ProbeFree(']:
  if facts.count(marker)!=1:raise ValueError('facts source boundary mismatch: '+marker)
 selected=facts.split('// These helpers query descriptors',1)[0]
 selected+='// These helpers query descriptors'+facts.split('// These helpers query descriptors',1)[1].split('STATIC BOOLEAN HighUefiAbsent(',1)[0]
 selected+= 'STATIC int ProbeWrite('+facts.split('STATIC int ProbeWrite(',1)[1].split('STATIC int ProbeFree(',1)[0]
 selected=selected.replace(' || !gDS->AddMemorySpace','')
 if 'AddMemorySpace(' in selected or '->AddMemorySpace(' in selected or 'AllocatePages(' in selected or 'ResetSystem(' in selected:raise ValueError('mutating action leaked into facts stage')
 source.write_text(selected)
 for name in ['Guard.c','Guard.h']:
  source=ROOT/'package/CasCpu0'/name;shutil.copyfile(source,driver/name);staged[str(source)]=parent.digest(source.read_bytes())
 source=uefi/'Platforms/Xiaomi/casPkg/Include/CasPlatform.h';shutil.copyfile(source,driver/'CasPlatform.h');staged[str(source)]=parent.digest(source.read_bytes())
 shutil.copyfile(source,pkg/'TraceView/CasPlatform.h')
 abi=driver/'TraceAbi.h';abi_text=abi.read_text()
 abi_text=parent.replace_once(abi_text,'#define CAS_TRACE_BASE 0x9f960000ULL','#include "CasPlatform.h"\n#define CAS_TRACE_BASE 0x9f960000ULL')
 abi_text=parent.replace_once(abi_text,'#endif','STATIC_ASSERT(CAS_TRACE_BASE == (UINT64)CAS_LOG_BASE + CAS_HANDOFF_LOG_OFFSET, "trace/shim address ABI");\nSTATIC_ASSERT(sizeof(CAS_HANDOFF_TRACE) <= CAS_HANDOFF_LOG_SIZE, "trace fits retained slot");\nSTATIC_ASSERT(CAS_VM_EXCEPTION_OFFSET + sizeof(CAS_VM_EXCEPTION) <= CAS_HANDOFF_LOG_SIZE, "exception fits retained slot");\nSTATIC_ASSERT(CAS_TRACE_TOTAL_BYTES <= CAS_HANDOFF_LOG_SIZE, "all records fit retained slot");\n#endif')
 abi.write_text(abi_text);(pkg/'TraceView/TraceAbi.h').write_text(abi_text)

 p=driver/'DarwinBoot.c';s=p.read_text()
 if MODE=='execute':s=parent.replace_once(s,'#define CAS_HANDOFF_DRY_RUN 1','#define CAS_HANDOFF_DRY_RUN 0')
 s=parent.replace_once(s,'#include "Trace.h"','#include "Trace.h"\n#include "Guard.h"')
 s=parent.replace_once(s,'  Root = NULL;\n  Image = NULL;','  mTrace.Reserved[5] = READ_SYSREG (mpidr_el1);\n  mTrace.Reserved[6] = mTrace.Reserved[5] & 0xFF00FFFFFFULL;\n  CasTrace (1, EFI_SUCCESS, 0, 0, 0, 0);\n  if (mTrace.Reserved[6] != 0) {\n    CasTrace (0x7f, EFI_UNSUPPORTED, 0, 0, 0, 0);\n    Print (L"CAS CPU0: unexpected BSP affinity; no allocation or kernel branch\\n");\n    return EFI_UNSUPPORTED;\n  }\n  Root = NULL;\n  Image = NULL;')
 s=parent.replace_once(s,'  Status = AllocateWindow (&Kernel, WindowSize, &Address);',
  '  Status = CasCpu0Preflight (0x100004000ULL, WindowSize);\n  if (EFI_ERROR (Status)) { goto Fail; }\n  Status = AllocateWindow (&Kernel, WindowSize, &Address);')
 s=parent.replace_once(s,'  CasTrace (3, EFI_SUCCESS, Address, 0, Address + Kernel.Entry - Kernel.Base, WindowSize);',
  '  Status = CasCpu0ProbeHeld (Address, WindowSize);\n  if (EFI_ERROR (Status)) { goto Fail; }\n  mTrace.Reserved[0] = mTrace.BootStamp;\n  mTrace.Reserved[1] = Address;\n  mTrace.Reserved[2] = WindowSize;\n  mTrace.Reserved[3] = 1;\n  mTrace.Reserved[4] = CAS_CPU0_CANDIDATE_TAG;\n  CasTrace (3, EFI_SUCCESS, Address, 0, Address + Kernel.Entry - Kernel.Base, WindowSize);')
 s=parent.replace_once(s,'  if (!CasTraceStart ()) { return EFI_UNSUPPORTED; }',
  '  if (AsciiStrCmp (CommandLine, "-v keepsyms=1 debug=0x14e cpus=1") != 0 &&\n      AsciiStrCmp (CommandLine, "-v keepsyms=1 debug=0x14e cpus=1 cas-validate=1") != 0) { return EFI_INVALID_PARAMETER; }\n  if (!CasTraceStart ()) { return EFI_UNSUPPORTED; }')
 s=parent.replace_once(s,'#if CAS_HANDOFF_DRY_RUN',
  '  if (AsciiStrCmp (CommandLine, "-v keepsyms=1 debug=0x14e cpus=1 cas-validate=1") == 0) {\n    Print (L"CAS CPU0: full fresh guard/copy passed; validation-only request returns before EBS\\n");\n    Status = EFI_SUCCESS;\n    goto Fail;\n  }\n#if CAS_HANDOFF_DRY_RUN')
 s=parent.replace_once(s,'  if (!CasTraceStart ()) { return EFI_UNSUPPORTED; }','  mCpuRequireRecovered = CAS_CPU0_REQUIRE_RECOVERED && AsciiStrCmp (CommandLine, "-v keepsyms=1 debug=0x14e cpus=1") == 0;\n  if (!CasTraceStart ()) {\n    Print (L"CAS CPU0: matching prior-boot validation/control region required; previous record retained\\n");\n    return EFI_NOT_READY;\n  }')
 s=s.replace('CAS X06:','CAS CPU0:')
 if execute_args!=base_args:s=s.replace(base_args,execute_args)
 p.write_text(s)
 # Mu can legitimately change a code allocation from XP to executable. Neither
 # RO/RP nor mixed/changed permissions are accepted; the held map must be WB.
 p=driver/'Trace.h';s=p.read_text();s=parent.replace_once(s,'A!=EFI_MEMORY_XP','(A!=0 && A!=EFI_MEMORY_XP)')
 recovery="""
#include "CandidateTag.h"
STATIC BOOLEAN mCpuRecoveredValidation,mCpuRequireRecovered;
STATIC BOOLEAN CasCpu0PreviousValidation(UINT64 CurrentBoot) {
 CAS_HANDOFF_TRACE A,B;UINTN I;UINT32 Crc;volatile CONST UINT8 *P=(VOID *)(UINTN)CAS_TRACE_BASE;
 for(I=0;I<sizeof(A);I++)((UINT8 *)&A)[I]=P[I];MemoryFence();
 for(I=0;I<sizeof(B);I++)((UINT8 *)&B)[I]=P[I];MemoryFence();
 if(CompareMem(&A,&B,sizeof(A)) || A.Magic!=CAS_TRACE_MAGIC || A.Version!=1 || A.Bytes!=sizeof(A) || !A.Sequence || !A.BootStamp || A.BootStamp==CurrentBoot || A.Stage!=5 || A.Status!=EFI_SUCCESS || A.RawStage!=5 || A.RawStageInverse!=~5ULL || A.Reserved[0]!=A.BootStamp || A.Reserved[1]!=0x100004000ULL || A.Reserved[2]!=SIZE_256MB || A.Reserved[3]!=1 || A.Reserved[4]!=CAS_CPU0_CANDIDATE_TAG || (A.Reserved[5]&0xFF00FFFFFFULL)!=0 || A.Reserved[6]!=0)return FALSE;
 return !EFI_ERROR(gBS->CalculateCrc32(&A,232,&Crc)) && A.Crc32==Crc;
}
"""
 s=parent.replace_once(s,'STATIC BOOLEAN mTraceReady;',recovery+'\nSTATIC BOOLEAN mTraceReady;')
 s=parent.replace_once(s,'  // Only a new loader request clears the extension;', '  mCpuRecoveredValidation=CasCpu0PreviousValidation(Boot[13]);\n  if(mCpuRequireRecovered && !mCpuRecoveredValidation)return FALSE;\n  // Only a new loader request clears the extension;')
 p.write_text(s)
 p=driver/'DarwinBoot.inf';s=p.read_text().replace('  DarwinBoot.c\n','  DarwinBoot.c\n  Guard.c\n  Guard.h\n  DiagCore.c\n  SmemCore.c\n').replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  EmbeddedPkg/EmbeddedPkg.dec')
 s=s.replace('[LibraryClasses]','[LibraryClasses]\n  CacheMaintenanceLib\n  FdtLib\n  DxeServicesTableLib\n  UefiRuntimeServicesTableLib')
 s+='\n[Guids]\n  gFdtTableGuid\n';p.write_text(s)
 p=pkg/'DarwinBoot.dsc';s=p.read_text().replace('[LibraryClasses]','[LibraryClasses]\n  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf\n  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf\n  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf\n  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf')
 s += '\n[BuildOptions]\n  *_CLANGPDB_*_CC_FLAGS = -D CAS_XNU_DIAGNOSTICS=1\n'
 p.write_text(s)
 if requested_base!=0x100004000:
  for target in [driver/'DarwinBoot.c',driver/'Trace.h',driver/'DiagCore.h']:
   raw=target.read_text()
   if '0x100004000ULL' not in raw:raise ValueError('fixed range source context absent: '+str(target))
   target.write_text(raw.replace('0x100004000ULL','0x%xULL'%requested_base))
 p=pkg/'TraceView/TraceView.c';s=p.read_text();s=parent.replace_once(s,'#include "TraceAbi.h"','#include "TraceAbi.h"\n#include "CandidateTag.h"');anchor='  Print(L"Status %lx  window %lx\\r\\nPA %lx  args %lx\\r\\nentry %lx\\r\\n",A.Status,A.Window,A.Address,A.Args,A.Entry);'
 s=parent.replace_once(s,anchor,anchor+'\n  if(A.Reserved[3]==1)Print(L"Held probe: boot %lx  PA %lx  bytes %lx\\r\\n",A.Reserved[0],A.Reserved[1],A.Reserved[2]);');s=parent.replace_once(s,'  Print(L"Status %lx  window %lx', '  Print(L"Candidate %lx expected %lx: %s\\r\\n",A.Reserved[4],(UINT64)CAS_CPU0_CANDIDATE_TAG,A.Reserved[4]==CAS_CPU0_CANDIDATE_TAG ? L"MATCH" : L"different candidate");\n  Print(L"Status %lx  window %lx');s=parent.replace_once(s,'  Print(L"Pre-EBS EL', '  Print(L"MPIDR %lx  affinity %lx (expected 0)\\r\\n",A.Reserved[5],A.Reserved[6]);\n  Print(L"Pre-EBS EL');p.write_text(s)
 manifest={'profile':'cas-cpu0-'+MODE,'parent_x06':original,'mode':MODE,'candidate_tag':hex(tag),'source_heads':source_heads,'MPIDR_affinity_0_required':True,'private_adt_sha256':adt_meta['adt_sha256'],'recovered_validation_required':require_recovered,'boot_gate':'fresh current facts and exact held readback every Boot; prior trace never substitutes','kernel_execution_call_compiled':MODE=='execute','fresh_exact_range_probe_required':True,'fact_sources':staged,'legacy_diagnostic_probe_changed':False,
  'files':{str(p.relative_to(out)):parent.digest(p.read_bytes()) for p in out.rglob('*') if p.is_file()}}
 (out/'overlay.json').write_text(json.dumps(manifest,indent=2)+'\n');return manifest
def main():
 global MODE,ADT_AUDIT
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--framework',type=Path,required=True);ap.add_argument('--uefi',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--adt-audit',type=Path,required=True);ap.add_argument('--mode',choices=['execute','validate'],default='execute');a=ap.parse_args();MODE=a.mode;ADT_AUDIT=a.adt_audit.resolve()
 parent.prepare=prepare
 print(json.dumps(parent.build(a.framework.resolve(),a.uefi.resolve(),a.out.resolve()),indent=2))
if __name__=='__main__':main()
