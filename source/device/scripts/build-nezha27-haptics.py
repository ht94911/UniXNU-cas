#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the isolated GPU051 haptic increment using actual XNU compile records."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from kernel_target import ARCH, TRIPLE, check_object

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/nezha27-haptics'

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def main():
    if ARCH!='arm64e' or not TRIPLE.startswith('arm64e-apple-ios27.'):
        raise SystemExit('Haptics requires the selected iPhone27 arm64e target')
    OUT.mkdir(parents=True,exist_ok=True)
    device=Path(os.environ['UNIXNU_DEVICE']).resolve()
    component=device/'components/PaganiHaptics'
    soc=device/'components/sm8850/kits/Sm8850SpmiV8'
    source=ROOT/'kits/nezha/haptics27'
    helper=module('haptics_compile',ROOT/'scripts/build-sheng-source-drivers.py')
    headers=module('haptics_headers',ROOT/'scripts/prepare-storage-headers.py').prepare(
        ROOT/'refer/IOStorageFamily','7edb88fbae296fb7c8ce2f64e115e116e566d51c',OUT/'headers')
    base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    records={'.c':json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(',')),
             '.cpp':json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))}
    sources=[soc/'Sm8850SpmiV8.c',component/'HapticsSpmi.c',component/'HapticsCore.c']
    sources += [source/n for n in ['NezhaHapticsBoard.c','NezhaHapticsFactory.c','NezhaHapticsQueue.c',
                                 'NezhaHapticsRuntime.cpp','NezhaHapticsIOS.cpp','NezhaHapticsReady.cpp']]
    additions=['-I',str(OUT/'headers/include'),'-idirafter',str(Path(headers['sdk'])/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers'),
               '-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(device/'components'),'-I',str(device/'components/sm8850/kits'),
               '-I',str(source),'-I',str(ROOT/'drivers/QcomXnuUsbDebug/include'),
               '-mgeneral-regs-only','-Wno-unknown-warning-option']
    rows=[];objects=[];errors=[]
    for path in sources:
        obj=OUT/(path.stem+'.o');record=records[path.suffix]
        command=helper.compile_command(record,path,obj,[],additions)
        (OUT/(path.stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
        cp=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
        (OUT/(path.stem+'.log')).write_text(cp.stdout+cp.stderr)
        row={'source':str(path),'sha256':sha(path),'returncode':cp.returncode};rows.append(row)
        if cp.returncode:errors.append(path.name)
        else:check_object(obj.read_bytes(),path.name);objects.append(obj)
    report={'target':TRIPLE,'sources':rows,'hardware_verified':False,'source_base':'GPU051/76c595268304',
            'private_inputs_included':False,'factory_caps_mv':2000}
    (OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    if errors:raise SystemExit('Haptics compile failed: '+','.join(errors))
    # Register the readiness class only when the working endpoint and iOS
    # assets are ready, through the existing native ALS late-constructor path.
    llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
    ready=OUT/'NezhaHapticsReady.o';late=OUT/'NezhaHapticsReady.late.o'
    subprocess.run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
                    '--globalize-symbol=__GLOBAL__sub_I_NezhaHapticsReady.cpp',ready,late],check=True)
    objects=[late if obj==ready else obj for obj in objects]
    report['readiness_late_constructor']=True
    report['readiness_late_object_sha256']=sha(late)
    archive=OUT/'libNezha27Haptics.a';archive.unlink(missing_ok=True)
    subprocess.run(['xcrun','ar','rcs',str(archive),*map(str,objects)],check=True)
    report['archive_sha256']=sha(archive)
    (OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Built9 haptic objects with the selected iPhone27 XNU ABI')

if __name__=='__main__':main()
