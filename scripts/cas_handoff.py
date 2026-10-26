#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a pinned, compile-time dry-run DarwinBoot overlay. No phone operations."""
import argparse,difflib,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PIN='d099354d0eab34df5088ca1ba89c9c88bc3260bb'
SOURCE_SHA='db189a520fb7af3decb80f6ad65e2b7ab2aa9e3e6c5ab6e2286a23df6108d8f2'
REVIEWED_PINS={PIN:SOURCE_SHA,'3325e352d7df68a9f9ad74832ed94a6acbbd7390':'d13facd71aa802e02a33b8c463d83a7a9a2f9d5b0a5e5a5fcd96cfdce7985e1a'}
def digest(data):return hashlib.sha256(data).hexdigest()
def replace_once(text,old,new):
 if text.count(old)!=1:raise ValueError('fixed source context mismatch: '+old[:100])
 return text.replace(old,new,1)
def transform(source):
 if digest(source) not in REVIEWED_PINS.values():raise ValueError('DarwinBoot source hash mismatch')
 text=source.decode()
 text=replace_once(text,'#include "WindowSize.h"','#include "WindowSize.h"\n#include <Protocol/MemoryAttribute.h>\n#define CAS_HANDOFF_DRY_RUN 1')
 text=replace_once(text,'STATIC UINT32 U32 (','#include "Trace.h"\n\nSTATIC UINT32 U32 (')
 text=replace_once(text,'  Root = NULL;\n  Image = NULL;', '  if (!CasTraceStart ()) { return EFI_UNSUPPORTED; }\n  Root = NULL;\n  Image = NULL;')
 text=replace_once(text,'  Address = 0;\n  Capacity = 0;', '  Address = 0;\n  Capacity = 0;\n  WindowSize = 0;')
 text=replace_once(text,'  Length = StrSize (Request->KernelPath);','  CasTrace (2, EFI_SUCCESS, 0, 0, Kernel.Entry, 0);\n  Length = StrSize (Request->KernelPath);')
 # Do not broaden the upstream search to arbitrary RAM. The accepted X05
 # allocation began at 4GiB; this image has a distinct +16KiB address contract.
 text=replace_once(text,'    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);',
 '    if (Candidate != 0x100004000ULL || WindowSize != SIZE_256MB || Candidate + WindowSize > 0x120000000ULL) { continue; }\n    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);')
 text=replace_once(text,'  ArgsAddress = ALIGN_VALUE (Address + Kernel.End - Kernel.Base, SIZE_16KB);',
 '  Status = CasWindowPermissions (Address, WindowSize);\n  if (EFI_ERROR (Status)) { goto Fail; }\n  CasTrace (3, EFI_SUCCESS, Address, 0, Address + Kernel.Entry - Kernel.Base, WindowSize);\n  ArgsAddress = ALIGN_VALUE (Address + Kernel.End - Kernel.Base, SIZE_16KB);')
 text=replace_once(text,'  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);\n  if (EFI_ERROR (Status)) { goto Fail; }\n  Print (L"DARWINBOOT: ARM64',
 '  CasTrace (4, EFI_SUCCESS, Address, ArgsAddress, Address + Kernel.Entry - Kernel.Base, WindowSize);\n#if CAS_HANDOFF_DRY_RUN\n  Print (L"CAS X06: kernel/ADT/boot_args loaded; dry-run frees owned memory; no ExitBootServices or kernel execution\\n");\n  Status = EFI_SUCCESS;\n  goto Fail;\n#else\n  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);\n  if (EFI_ERROR (Status)) { goto Fail; }\n  Print (L"DARWINBOOT: ARM64')
 if 'means the kernel entry code ran.' in text:text=replace_once(text,'means the kernel entry code ran.','marks the loader before the kernel branch.')
 elif 'DEBUG ((DEBUG_ERROR, "\\nExiting boot services. A green bar across the top\\nmarks the loader handoff before the kernel branch.\\n"));' not in text:raise ValueError('reviewed green-bar wording mismatch')
 text=replace_once(text,'  for (Attempt = 0; Attempt < 3; Attempt++) {\n    MapSize = Capacity;','  CasTrace (0x10, EFI_SUCCESS, Address, ArgsAddress, Address + Kernel.Entry - Kernel.Base, WindowSize);\n  for (Attempt = 0; Attempt < 3; Attempt++) {\n    MapSize = Capacity;')
 # Commit C trace before the first GetMemoryMap/EBS attempt. The retry loop
 # may call only GetMemoryMap/EBS; raw assembly markers use no boot services.
 text=replace_once(text,'  return EFI_ABORTED;\nFail:','  return EFI_ABORTED;\n#endif\nFail:')
 text=replace_once(text,'  if (Address != 0) { gBS->FreePages (Address, EFI_SIZE_TO_PAGES (WindowSize)); }\n  Print (L"DARWINBOOT: boot refused: %r; firmware retained\\n", Status);',
 '  if (Address != 0) {\n    EFI_STATUS Released = gBS->FreePages (Address, EFI_SIZE_TO_PAGES (WindowSize));\n    if (EFI_ERROR (Released)) { Status = Released; }\n  }\n  CasTrace (EFI_ERROR (Status) ? 0x7f : 5, Status, Address, 0, 0, WindowSize);\n  Print (L"CAS X06: load/release returned %r; firmware retained\\n", Status);')
 # Variables used only by the EBS branch are harmless in the dry-run profile.
 text=replace_once(text,'  UINTN                     Attempt;', '  UINTN                     Attempt;') if '  UINTN                     Attempt;' in text else text
 return text

def state(path):
 return {'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=path,text=True).strip(),
         'dirty':subprocess.check_output(['git','status','--porcelain'],cwd=path,text=True),
         'diff_sha256':digest(subprocess.check_output(['git','diff','--binary','HEAD'],cwd=path)),
         'untracked_sha256':{p:digest((path/p).read_bytes()) for p in subprocess.check_output(['git','ls-files','--others','--exclude-standard'],cwd=path,text=True).splitlines()}}
def prepare(framework,out):
 source=framework/'drivers/DarwinBoot';before=state(source)
 if before['head'] not in REVIEWED_PINS or before['dirty']:raise ValueError('pinned DarwinBoot must be clean')
 if digest((source/'DarwinBootPkg/Driver/DarwinBoot.c').read_bytes())!=REVIEWED_PINS[before['head']]:raise ValueError('reviewed loader commit/blob mismatch')
 out.mkdir(parents=True,exist_ok=False);pkg=out/'DarwinBootPkg';pkg.mkdir();driver=pkg/'Driver';driver.mkdir()
 inputs={}
 for path in (source/'DarwinBootPkg/Driver').iterdir():
  if path.is_file():
   raw=path.read_bytes();inputs[str(path.relative_to(source))]=digest(raw);(driver/path.name).write_bytes(raw)
 original=(driver/'DarwinBoot.c').read_bytes();patched=transform(original);(driver/'DarwinBoot.c').write_text(patched)
 asm=(driver/'Entry.S').read_text()
 raw_macro="""
.macro CAS_RAW_STAGE phase
  mov x9, #0x9f960000
  mov x10, \\phase
  mvn x11, x10
  stp x10, x11, [x9, #240]
  dsb sy
.endmacro
"""
 asm=replace_once(asm,'ASM_FUNC(DarwinCurrentEl)',raw_macro+'\nASM_FUNC(DarwinCurrentEl)')
 asm=replace_once(asm,'ASM_FUNC(DarwinEnter)','ASM_FUNC(DarwinEnter)\n  CAS_RAW_STAGE 0x20')
 asm=replace_once(asm,'  mrs x4, sctlr_el1','  CAS_RAW_STAGE 0x21\n  mrs x4, sctlr_el1')
 asm=replace_once(asm,'  br x1','  CAS_RAW_STAGE 0x22\n  br x1')
 asm=asm.replace('buffer shows that this code ran, whatever the kernel does afterwards.','buffer marks only loader code before the kernel branch.')
 (driver/'Entry.S').write_text(asm)

 trace=ROOT/'package/CasHandoff/Trace.h';shutil.copyfile(trace,driver/'Trace.h')
 shutil.copyfile(ROOT/'package/CasHandoff/TraceAbi.h',driver/'TraceAbi.h')
 viewer=pkg/'TraceView';viewer.mkdir()
 for name in ['TraceView.c','TraceView.inf','TraceAbi.h']:shutil.copyfile(ROOT/'package/CasHandoff'/name,viewer/name)
 inf=(driver/'DarwinBoot.inf').read_text().replace('  DarwinBoot.c\n','  DarwinBoot.c\n  Trace.h\n').replace('  gEfiGraphicsOutputProtocolGuid ## CONSUMES','  gEfiGraphicsOutputProtocolGuid ## CONSUMES\n  gEfiMemoryAttributeProtocolGuid ## CONSUMES')
 (driver/'DarwinBoot.inf').write_text(inf)
 dsc=(source/'DarwinBootPkg/DarwinBoot.dsc').read_text().replace('  Test/Validation.inf','')
 dsc=dsc.replace('OUTPUT_DIRECTORY = build/DarwinBoot','OUTPUT_DIRECTORY = $(CAS_HANDOFF_BUILD_DIR)')
 dsc=dsc.replace('  DarwinBootPkg/Driver/DarwinBoot.inf','  DarwinBootPkg/Driver/DarwinBoot.inf\n  DarwinBootPkg/TraceView/TraceView.inf')
 dsc=dsc.replace('  UefiDriverEntryPoint|','  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf\n  UefiDriverEntryPoint|')
 dsc += '\n[BuildOptions]\n  *_CLANGPDB_*_DLINK_FLAGS = /DEBUG:NONE /ALIGN:0x1000\n  *_CLANGPDB_*_CC_FLAGS = -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function\n'
 (pkg/'DarwinBoot.dsc').write_text(dsc)
 shutil.copyfile(source/'LICENSE',out/'LICENSE.DarwinBoot')
 (out/'DarwinBoot.c.diff').write_text(''.join(difflib.unified_diff(original.decode().splitlines(True),patched.splitlines(True),fromfile=PIN+'/DarwinBoot.c',tofile='cas-x06-dryrun/DarwinBoot.c')))
 if before!=state(source):raise ValueError('upstream changed during staging')
 result={'profile':'cas-x06-dryrun','upstream':before,'input_sha256':inputs,'trace_sha256':digest(trace.read_bytes()),
         'kernel_execution_call_compiled':False,'exit_boot_services_call_compiled':False,
         'stage':'source overlay only','files':{str(p.relative_to(out)):digest(p.read_bytes()) for p in out.rglob('*') if p.is_file()}}
 (out/'overlay.json').write_text(json.dumps(result,indent=2)+'\n');return result

def build(framework,uefi,out):
 out.mkdir(parents=True,exist_ok=False);prepare(framework,out/'overlay')
 env=dict(os.environ,WORKSPACE=str(uefi),EDK_TOOLS_PATH=str(uefi/'Mu_Basecore/BaseTools'),PYTHON_COMMAND=sys.executable,
          CONF_PATH=str(out/'Conf'),SOURCE_DATE_EPOCH='1792643741',LC_ALL='C.UTF-8',TZ='UTC',PYTHONHASHSEED='0')
 env['PACKAGES_PATH']=os.pathsep.join([str(out/'overlay'),str(uefi/'Mu_Basecore'),str(framework/'package')])
 env['PATH']=str(uefi/'Mu_Basecore/BaseTools/BinWrappers/PosixLike')+os.pathsep+env['PATH']
 env['PYTHONPATH']=str(uefi/'Mu_Basecore/BaseTools/Source/Python')
 version=subprocess.check_output(['clang','--version'],env=env,text=True)
 if 'version 18.1.3' not in version:raise ValueError('LLVM18.1.3 required')
 (out/'Conf').mkdir()
 for src,name in [('target.template','target.txt'),('tools_def.template','tools_def.txt'),('build_rule.template','build_rule.txt')]:
  shutil.copyfile(uefi/'Mu_Basecore/BaseTools/Conf'/src,out/'Conf'/name)
 argv=['build','-a','AARCH64','-t','CLANGPDB','-b','DEBUG','-p','DarwinBootPkg/DarwinBoot.dsc','-n','8',
       '--conf='+str(out/'Conf'),'-D','CAS_HANDOFF_BUILD_DIR='+str(out/'Build'),'-y',str(out/'BuildReport.txt'),'-Y','BUILD_FLAGS','-Y','LIBRARY']
 sources={str(p):state(p) for p in [ROOT,uefi,framework]}
 (out/'intent.json').write_text(json.dumps({'argv':argv,'cwd':str(uefi),'sources':sources,'device_actions':0},indent=2)+'\n')
 with (out/'build.log').open('x') as log:code=subprocess.run(argv,cwd=uefi,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
 result={'exit_code':code,'result':'pass' if code==0 else 'fail','sources_unchanged':sources=={str(p):state(p) for p in [ROOT,uefi,framework]},'hardware_tested':False,'kernel_executed':False}
 efi=out/'Build/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi'
 if code==0:result['efi']={'path':str(efi),'bytes':efi.stat().st_size,'sha256':digest(efi.read_bytes())}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 if code or not result['sources_unchanged']:raise RuntimeError('build failed or source changed; preserve this output')
 return result
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--framework',type=Path,required=True);ap.add_argument('--uefi',type=Path);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--prepare-only',action='store_true');a=ap.parse_args()
 result=prepare(a.framework.resolve(),a.out.resolve()) if a.prepare_only else build(a.framework.resolve(),a.uefi.resolve(),a.out.resolve())
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
