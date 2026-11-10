#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the isolated power increment using the selected XNU compile records."""
from pathlib import Path
import hashlib, importlib.util, json, subprocess
from kernel_target import ARCH, check_object
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/nezha27-power'; OUT.mkdir(parents=True,exist_ok=True)
if ARCH!='arm64e': raise SystemExit('power27 requires the selected arm64e target')
spec=importlib.util.spec_from_file_location('power_source_compile',ROOT/'scripts/build-sheng-source-drivers.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
records={'.c':json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(',')),'.cpp':json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))}
source=ROOT/'kits/nezha/power27'
includes=['-I',str(source/'include'),'-I',str(source/'vendor/qcom'),'-I',str(source/'vendor/usb'),
 '-I',str(source/'gen4-adc'),
 '-I',str(ROOT/'drivers/QcomXnuUsbDebug/include'),'-I',str(ROOT/'.cache/xnu-work/osfmk')]
files=sorted([*source.glob('core/*.c'),*source.glob('platform/*.c'),*source.glob('platform/*.cpp'),*source.glob('vendor/qcom/*.c'),*source.glob('gen4-adc/*.c'),*source.glob('gen4-adc/*.cpp')])
objects=[];details={};failures=[]
for path in files:
 obj=OUT/(path.stem+'.o');record=records[path.suffix]
 command=helper.compile_command(record,path,obj,includes,[])
 if any('host_stubs' in str(arg) for arg in command):raise SystemExit('Host fixture leaked into native compilation')
 (OUT/(path.stem+'-command.json')).write_text(json.dumps(command,indent=2))
 p=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
 (OUT/(path.stem+'.log')).write_text(p.stdout+p.stderr)
 if p.returncode:
  failures.append(path.name+':\n'+p.stdout+p.stderr)
  continue
 check_object(obj.read_bytes(),path.name)
 details[str(path.relative_to(source))]={'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest()}
 objects.append(obj)
if failures:raise SystemExit('\n'.join(failures))
llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
original=OUT/'NezhaBatteryService.o';late=OUT/'NezhaBatteryService.late.o'
subprocess.run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NezhaBatteryService.cpp',original,late],check=True)
check_object(late.read_bytes(),'late power service')
objects=[late if x==original else x for x in objects]
archive=OUT/'libNezha27Power.a';archive.unlink(missing_ok=True)
subprocess.run(['xcrun','ar','rcs',archive,*objects],check=True)
(OUT/'build-report.json').write_text(json.dumps({'objects':details,'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'class':'native XNU IOPMPowerSource','manual_late_registration':True,'board':'GPU015 guarded normal-buck control and SC8585 ADC; inherited ICC; fixed OS4 profile','charger_writes':True,'charger_write_scope':'mode2 readback, mode3 FCC500/1000 feedback, mode4 fixed5/9V and SC8585 USB-OVP field readback with FCC<=1500, mode5 normal5V CN JEITA/full/recharge with FCC<=1500, ICL/FV/ITERM readback and1B charge-enable; pump stays off','hardware_qualified':False},indent=2)+'\n')
print('power27 archive compiled; target/objects checked; hardware remains unqualified')
