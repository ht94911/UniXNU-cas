#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Reuse the selected XNU compile records and standard archive tooling."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

SHARED=['QcomGlink.c','QcomSmem.c','QcomSmp2p.c','QcomMdt.c','QcomQmp.c','QcomRpmh.c']
PLATFORM=['NbBaseband.c','NbMss.c','NbPas.c','NbBridgeGlue.c','NbArchive.c','NbFirmware.c','NbPower.c','NbScm.c','NbNative.cpp','NbBoard.cpp']

def sources(tree,kind):
    tree=Path(tree);pkg=tree/'kits/nezha/baseband27';soc=tree/'build/nezha27-baseband-stage/soc'
    if kind=='shared':return [pkg/'upstream'/name for name in SHARED]+[soc/'src/Sm8850Resources.c',soc/'src/Sm8850ResourcesNative.cpp']
    return [pkg/'platform'/name for name in PLATFORM]+[pkg/'transport/NbQrtr.c']+[
        pkg/'services'/name for name in ['NbMemshare.c','NbRmtfs.c','NbServloc.c','NbTftp.c']]+[
        pkg/'ipa'/name for name in ['nb_ipa_board.c','nb_ipa55_config.c','nb_gsi55.c']]+[
        pkg/'integration/NbBasebandStart.c',pkg/'integration/NbDiagnostic.c',pkg/'bridge/coachz_baseband.cpp']

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def build(tree,kind):
    tree=Path(tree).resolve();sys.path.insert(0,str(tree/'scripts'))
    from kernel_target import ARCH,IOS_VERSION,check_object
    if ARCH!='arm64e' or IOS_VERSION.split('.')[0]!='27':raise ValueError('requires the selected arm64e/iOS27 build')
    stage=json.loads((tree/'build/nezha27-baseband-stage/stage.json').read_text())
    helper=load(tree/'scripts/build-sheng-source-drivers.py','bb_compile_helper')
    pkg=tree/'kits/nezha/baseband27';soc=Path(stage['soc']);out=tree/('build/nezha27-shared' if kind=='shared' else 'build/nezha27-baseband');out.mkdir(parents=True,exist_ok=True)
    objroot=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    records={'.c':objroot/'bsd/DEVELOPMENT/vfs_conf.o.json','.cpp':objroot/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'}
    objects=[];report=[]
    for index,source in enumerate(sources(tree,kind)):
        record=json.loads(records[source.suffix].read_text().strip().rstrip(','));obj=out/(source.name+'.o')
        includes=[pkg/'bridge',pkg/'bridge/upstream',pkg,pkg/'upstream',soc/'include',tree/'kits/ArmSmmu4K',tree/'drivers/QcomXnuUsbDebug/include',tree/'.cache/xnu-work/osfmk']
        # compile_command already ends with -Wall -Wextra -Werror, its reviewed -Wno-* set and
        # -mgeneral-regs-only. Repeating -Wall/-Wextra here would switch those exceptions back on.
        flags=[item for include in includes for item in ['-I',str(include)]]+['-Wno-unknown-warning-option']
        command=helper.compile_command(record,source,obj,[],flags)
        (out/(source.name+'.command.json')).write_text(json.dumps(command,indent=2)+'\n')
        cp=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
        (out/(source.name+'.log')).write_text(cp.stdout+cp.stderr)
        row={'source':str(source),'sha256':sha(source),'returncode':cp.returncode};report.append(row)
        (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
        if cp.returncode:raise ValueError('native compile failed: '+source.name)
        check_object(obj.read_bytes(),source.name)
        if source.name=='coachz_baseband.cpp':
            late=out/'coachz_baseband.late.o'
            load(tree/'scripts/nezha_macho_late.py','bb_late').stage_late(obj,late,'__GLOBAL__sub_I_coachz_baseband.cpp')
            obj=late
        objects.append(obj)
    archive=out/('libNezha27Shared.a' if kind=='shared' else 'libNezha27Baseband.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun','ar','rcs',str(archive),*map(str,objects)],check=True,env=dict(os.environ,ZERO_AR_DATE='1'))
    for name,flags in [('defined',['-gU']),('undefined',['-u'])]:
        (out/(name+'-symbols.txt')).write_bytes(subprocess.check_output(['xcrun','nm',*flags,str(archive)]))
    (out/'archive.json').write_text(json.dumps({'sha256':sha(archive),'objects':[p.name for p in objects],'phone_operations':False},indent=2)+'\n')
    print(archive)

def main(kind):
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--tree',type=Path,default=Path(__file__).resolve().parents[1]);parser.add_argument('--plan',action='store_true')
    args=parser.parse_args()
    if args.plan:print(json.dumps([str(p) for p in sources(args.tree,kind)],indent=2));return
    build(args.tree,kind)
