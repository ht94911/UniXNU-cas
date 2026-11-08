#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build QcaBluetooth/WCN7860 with the selected kernel's real C/C++ records."""
from pathlib import Path
import hashlib,importlib.util,json,os,shlex,subprocess,tomllib
from kernel_target import ARCH,TRIPLE,check_object

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/nezha27-bluetooth'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    if ARCH!='arm64e' or not TRIPLE.startswith('arm64e-apple-ios27.'):raise SystemExit('Bluetooth requires the selected iPhone27 arm64e target')
    OUT.mkdir(parents=True,exist_ok=True)
    helper=load('bt_compile',ROOT/'scripts/build-sheng-source-drivers.py')
    headers=load('bt_headers',ROOT/'scripts/prepare-storage-headers.py').prepare(ROOT/'refer/IOStorageFamily','7edb88fbae296fb7c8ce2f64e115e116e566d51c',OUT/'headers')
    device=Path(os.environ['UNIXNU_DEVICE']).resolve()
    overrides=tomllib.loads((device/'local.toml').read_text())['source']
    soc=next(v for v in overrides if v['kind']=='soc' and v['name']=='sm8850');soc=(device/soc['local']).resolve()
    qca=ROOT/'drivers/QcaBluetooth'
    subprocess.run(['python3',qca/'tools/prepare-linux-wcn7860.py'],check=True)
    base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    records={'.c':json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(',')),
             '.cpp':json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))}
    includes=['-I',str(OUT/'headers/include'),'-idirafter',str(Path(headers['sdk'])/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
    extra=['-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(qca/'include'),'-I',str(qca/'src'),'-I',str(qca/'shim'),'-I',str(qca/'out/linux-wcn7860'),
           '-I',str(soc/'include'),'-I',str(ROOT/'drivers/QcomXnuUsbDebug/include'),'-DQBT_WCN7860_SOURCE=1',
           '-Wno-sign-compare','-Wno-unknown-warning-option']
    board=ROOT/'kits/nezha/bluetooth27'
    selected=[qca/'src'/n for n in ('qbt_core.c','qbt_linux.c','qbt_geni.c','qbt_transport.c','qbt_firmware.c')]
    selected += [qca/'out/linux-wcn7860/btqca.c',*sorted(board.glob('*.c')),*sorted(board.glob('*.cpp'))]
    selected += [soc/n for n in ('Sm8850BtClock.c','Sm8850BtPmic.c','Sm8850BtDma.c')]
    # Sm8850Rpmh.c is already compiled once by Wi-Fi; both radios call it.
    objects=[];details=[]
    for source in selected:
        record=records[source.suffix];obj=OUT/(source.stem+'.o')
        command=helper.compile_command(record,source,obj,includes,extra)
        (OUT/(source.stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
        result=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
        (OUT/(source.stem+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            print(result.stderr);details.append({'source':str(source),'error':result.returncode});continue
        check_object(obj.read_bytes(),source.name)
        deps={}
        for name in shlex.split(obj.with_suffix('.d').read_text().replace('\\\n','').split(':',1)[1]):
            path=Path(name);path=path if path.is_absolute() else Path(record['directory'])/path
            deps[str(path.resolve())]=sha(path)
        objects.append(obj);details.append({'source':str(source),'source_sha256':sha(source),'object':str(obj),'object_sha256':sha(obj),'headers':deps})
    (OUT/'compile-report.json').write_text(json.dumps(details,indent=2)+'\n')
    if len(objects)!=len(selected):raise SystemExit('Bluetooth target compile failed; no archive written')
    archive=OUT/'libNezha27Bluetooth.a';archive.unlink(missing_ok=True)
    subprocess.run(['xcrun','ar','rcs',archive,*map(str,objects)],check=True)
    (OUT/'build-report.json').write_text(json.dumps({'target':TRIPLE,'objects':len(objects),'sources':details,'archive_sha256':sha(archive),
        'firmware_included':False,'old_objects_used':False,'hardware_verified':False,'single_rpmh_definition_in_WiFi_archive':True},indent=2)+'\n')
    print('Built '+str(len(objects))+' actual iPhone27 Bluetooth objects; hardware validation separate')
if __name__=='__main__':main()
