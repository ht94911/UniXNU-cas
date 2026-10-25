#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import hashlib,json,re,shutil,subprocess,zlib
import argparse
ap=argparse.ArgumentParser(description='Run the full dry-run DarwinBoot with host UEFI service doubles; no phone.')
ap.add_argument('--driver-build',type=Path,required=True)
ap.add_argument('--evidence-root',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--kernel',type=Path)
a=ap.parse_args();DEVICE=Path(__file__).resolve().parents[1]
BASE=DEVICE/'tests';ROOT=a.evidence_root.resolve();WORK=DEVICE.parent
BUILD=a.driver_build.resolve();DRIVER=BUILD/'overlay/DarwinBootPkg/Driver'
OUT=a.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
for p in DRIVER.iterdir():
 if p.is_file():shutil.copyfile(p,OUT/p.name)
source=(OUT/'DarwinBoot.c').read_text()
old='#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })'
assert source.count(old)==1
(OUT/'DarwinBoot.c').write_text(source.replace(old,'#define READ_SYSREG(Name) (4ULL)'))
trace=(OUT/'Trace.h').read_text();old='__asm__ volatile ("mov %0, sp" : "=r"(mTrace.Stack));';assert trace.count(old)==1
(OUT/'Trace.h').write_text(trace.replace(old,'mTrace.Stack=0xfeed0000;'))
shutil.copyfile(BASE/'handoff_host.c',OUT/'harness.c')
inc=WORK/'uefi-cas/Mu_Basecore/MdePkg/Include'
command=['clang-18','-std=c11','-D_GNU_SOURCE','-fshort-wchar','-fno-builtin','-O1','-g','-Werror','-Wno-unused-variable','-Wno-unused-but-set-variable',
 '-I'+str(OUT),'-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(WORK/'UniXNU/package/UniXNUPkg/Include'),str(OUT/'harness.c'),'-o',str(OUT/'probe')]
with (OUT/'compile.log').open('x') as log:code=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT).returncode
assert code==0,(OUT/'compile.log').read_text()
results=[]
for mode in range(6):
 tracefile=OUT/f'trace-{mode}.bin'
 p=subprocess.run([str(OUT/'probe'),str(a.kernel.resolve() if a.kernel else ROOT/'records/xnu-build/x02-001/run-005/decrypted/kernel.unstripped.macho'),str(ROOT/'records/xnu-adt/xa01-001/fixture-cpu0-001/cas.adt'),str(mode),str(tracefile)],capture_output=True,text=True)
 (OUT/f'run-{mode}.log').write_text(p.stdout+p.stderr)
 assert p.returncode==0,(mode,p.returncode,p.stdout,p.stderr)
 item=json.loads(p.stdout);item['case']=['load-copy-zero-args-adt-release','wrong-CPU','memory-permissions','no-conventional-window','unreserved-log-region','missing-ADT'][mode]
 raw=tracefile.read_bytes()
 if mode!=4:
  assert int.from_bytes(raw[:8],'little')==0x363048534143
  assert zlib.crc32(raw[:232])==int.from_bytes(raw[232:236],'little')
  stage=int.from_bytes(raw[240:248],'little');inverse=int.from_bytes(raw[248:],'little');assert inverse==stage^((1<<64)-1)
  assert stage==(5 if mode==0 else 0x7f);item['trace_crc32_verified']=True;item['trace_stage']=stage
 else:assert not any(raw)
 results.append(item)
data={'result':'pass','source_sha256':hashlib.sha256(source.encode()).hexdigest(),'target_driver_sha256':hashlib.sha256((BUILD/'Build/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi').read_bytes()).hexdigest(),
 'host_adaptations':['READ_SYSREG fixed synthetic value','trace SP fixed synthetic value','UEFI services implemented by host test double; exact source Boot control flow retained'],
 'cases':results,'physical_hardware_tested':False,'kernel_executed':False}
(OUT/'result.json').write_text(json.dumps(data,indent=2)+'\n');print(json.dumps(data,indent=2))
