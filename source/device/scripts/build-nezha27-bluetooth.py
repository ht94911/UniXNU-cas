#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the reused Bluetooth stack with the selected iPhone27 XNU records.

The compiler/header recipe is the same one used by Wi-Fi023. Apple images,
radio firmware and device identities are not build inputs. Sm8850Rpmh is
already supplied by the Wi-Fi archive; do not compile a second copy.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tomllib
from kernel_target import ARCH, TRIPLE, check_object

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/nezha27-bluetooth'

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    if ARCH!='arm64e' or not TRIPLE.startswith('arm64e-apple-ios27.'):
        raise SystemExit('Bluetooth requires the selected iPhone27 arm64e target')
    OUT.mkdir(parents=True,exist_ok=True)
    device=Path(os.environ['UNIXNU_DEVICE']).resolve()
    component=device/'components/QcaBluetooth'
    subprocess.run([sys.executable,component/'tools/prepare-linux-wcn7860.py'],check=True)
    helper=load('bluetooth_recorded_compile',ROOT/'scripts/build-sheng-source-drivers.py')
    headers=load('bluetooth_kernel_headers',ROOT/'scripts/prepare-storage-headers.py').prepare(
        ROOT/'refer/IOStorageFamily','7edb88fbae296fb7c8ce2f64e115e116e566d51c',OUT/'headers')
    overrides=tomllib.loads((device/'local.toml').read_text())['source']
    soc=next(x for x in overrides if x['name']=='sm8850' and x['kind']=='soc')
    usb=next(x for x in overrides if x['name']=='QcomXnuUsbDebug')
    soc=(device/soc['local']).resolve();usb=(device/usb['local']).resolve()
    board=ROOT/'kits/nezha/bluetooth27'
    base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    records={'.c':json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(',')),
             '.cpp':json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))}
    includes=['-I',str(OUT/'headers/include'),'-idirafter',
        str(Path(headers['sdk'])/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
    extra=['-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(board),'-I',str(soc/'include'),
        '-I',str(usb/'include'),'-DQBT_WCN7860_SOURCE=1','-Wno-sign-compare','-Wno-unknown-warning-option']
    for d in ['include','src','shim','out/linux-wcn7860']:extra+=['-I',str(component/d)]
    selected=[component/'src'/n for n in ['qbt_core.c','qbt_linux.c','qbt_geni.c','qbt_transport.c','qbt_firmware.c']]
    selected+=[component/'out/linux-wcn7860/btqca.c',*sorted(board.glob('*.c')),*sorted(board.glob('*.cpp'))]
    objects=[];details=[]
    for source in selected:
        obj=OUT/(source.stem+'.o');record=records[source.suffix]
        command=helper.compile_command(record,source,obj,includes,extra)
        (OUT/(source.stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
        result=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True,timeout=300)
        (OUT/(source.stem+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            details.append({'source':str(source),'error':result.returncode});print(result.stderr);continue
        check_object(obj.read_bytes(),source.name)
        deps={}
        for name in shlex.split(obj.with_suffix('.d').read_text().replace('\\\n','').split(':',1)[1]):
            p=Path(name);p=p if p.is_absolute() else Path(record['directory'])/p
            deps[str(p.resolve())]=sha(p)
        objects.append(obj);details.append({'source':str(source),'sha256':sha(source),
            'object':str(obj),'object_sha256':sha(obj),'headers':deps})
    (OUT/'compile-report.json').write_text(json.dumps(details,indent=2)+'\n')
    if len(objects)!=len(selected):raise SystemExit('Bluetooth compile failed; visible errors saved, archive not written')
    archive=OUT/'libNezha27Bluetooth.a';archive.unlink(missing_ok=True)
    subprocess.run(['xcrun','ar','rcs',str(archive),*map(str,objects)],check=True)
    (OUT/'build-report.json').write_text(json.dumps({'target':TRIPLE,'objects':len(objects),
        'sources':details,'archive_sha256':sha(archive),'firmware_included':False,
        'hardware_verified':False,'RPMh_supplied_by':'libNezha27Wifi.a',
        'native_transport':'com.apple.uart.sk.bluetooth','board':'SM8850/QUP3-SE5-UART18'},indent=2)+'\n')
    print('Built '+str(len(objects))+' iPhone27 Bluetooth objects with the recorded XNU ABI')

if __name__=='__main__':main()
