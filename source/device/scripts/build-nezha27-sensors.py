#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the sensor increment in a human-selected Format2 baseline.

Prepared source recipe only. No board/archive selection is changed by this file.
UNIXNU_SENSOR_COMPONENT must name the fixed local QcomSscSensors source export.
"""
from pathlib import Path
import ast
import hashlib
import importlib.util
import json
import os
import shlex
import struct
import subprocess
from kernel_target import ARCH, TRIPLE, check_object
ROOT=Path(__file__).resolve().parents[1]
D=Path(os.environ['UNIXNU_DEVICE']).resolve()
COMP=Path(os.environ.get('UNIXNU_SENSOR_COMPONENT',D/'components/QcomSscSensors')).resolve()
OUT=ROOT/'build/nezha27-sensors'
OUT.mkdir(parents=True,exist_ok=True)

def require(ok,why):
    if not ok:raise SystemExit(why)

def run(command,**kw):
    return subprocess.check_output([str(x) for x in command],timeout=300,**kw)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

require(ARCH=='arm64e','selected iOS27 arm64e target required')
abi=json.loads((D/'scripts/als-event-service-iphone-24A446.json').read_text())
fixture=json.loads((D/'scripts/sensors-event-constructors-iphone-24A446.json').read_text())
require(abi['instance_bytes']==200 and abi['virtual_entries']==219,'actual original EventService layout')
require(abi['kernelcache_sha256']==fixture['kernelcache_sha256'],'same exact donor for input/ALS/sensors')
input_out=ROOT/'build/nezha27-input'
input_report=json.loads((input_out/'build-report.json').read_text())
require(input_report['public_HID_commit']==abi['IOHIDFamily_revision'],'same2238 original interface header')
als_report=json.loads((ROOT/'build/nezha27-als/build-report.json').read_text())
require(als_report['kernelcache_sha256']==fixture['kernelcache_sha256'],'same existing ALS original ABI')
provided_bindings={n:int(v,16) for n,v in als_report['bindings'].items()}
include=input_out/'headers/include'
header=include/'IOKit/hid/IOHIDEventService.h'
require(sha(header)=='9f9e51072c66a315f68438872303fa8cf851fedb96988db0216324008390f620','same original opaque EventService header')
opaque=OUT/'headers/include';target=opaque/'IOKit/hid/IOHIDEventService.h';target.parent.mkdir(parents=True,exist_ok=True)
event_include='#if KERNEL_PRIVATE\n#include <IOKit/hid/IOHIDEvent.h>\n#endif\n'
text=header.read_text();require(text.count(event_include)==1,'one private event header include');target.write_text(text.replace(event_include,'',1))
# Reuse the completed original219-slot PAC binding helpers without importing
# the executable input/ALS recipes or rebuilding their already owned objects.
names={'discriminator','macho','object_symbols','object_vtable'}
parsed=ast.parse((ROOT/'scripts/build-nezha27-input.py').read_text())
defs=[n for n in parsed.body if isinstance(n,ast.FunctionDef) and n.name in names]
require({n.name for n in defs}==names,'fixed input binding helpers')
ns={'struct':struct,'require':require};exec(compile(ast.Module(body=defs,type_ignores=[]),'<input binding helpers>','exec'),ns)
compiler=load('sensor_compile',ROOT/'scripts/build-sheng-source-drivers.py')
sdk=Path(run(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip())
base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
records={False:json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(',')),
         True:json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))}
qrtr=ROOT/'kits/nezha/als-owner/upstream'
require(sha(qrtr/'QcomQrtr.h')=='665853eacbd6bf293ed16f02bce71f90a656da4feb02c5fe1da1d6eb032b2f5f','single existing owner QRTR header')
source_dir=D/'package/sensors'
sources=[source_dir/'NezhaSensorHid.cpp',source_dir/'NezhaSensors.c',source_dir/'NezhaSensorNativeCaller.cpp',
         *[COMP/'src'/name for name in ['QcomSscWire.c','QcomSscMath.c','QcomSscClient.c','QcomSscMotion.c']]]
objects=[];details=[];errors=[]
for source in sources:
    obj=OUT/(source.stem+'.o');record=records[source.suffix=='.c']
    inc=['-I',str(COMP/'include'),'-I',str(source_dir),'-I',str(qrtr),'-I',str(opaque),'-I',str(include),
         '-iquote',str(include/'IOKit/hid'),'-idirafter',str(sdk/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
    command=compiler.compile_command(record,source,obj,inc,['-I',str(ROOT/'.cache/xnu-work/osfmk'),'-Wno-unknown-warning-option','-mgeneral-regs-only'])
    (OUT/(source.stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
    p=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
    (OUT/(source.stem+'.log')).write_text(p.stdout+p.stderr)
    row=dict(source=str(source),source_sha256=sha(source),returncode=p.returncode)
    if p.returncode:errors.append(source.name)
    else:
        check_object(obj.read_bytes(),source.name);row['object_sha256']=sha(obj);objects.append(obj)
        deps={}
        for n in shlex.split(obj.with_suffix('.d').read_text().replace(chr(92)+chr(10),'').split(':',1)[1]):
            path=Path(n);path=path if path.is_absolute() else Path(record['directory'])/path;deps[str(path.resolve())]=sha(path)
        row['headers']=deps
        q=[p for p in deps if Path(p).name=='QcomQrtr.h']
        if q:require(q==[str((qrtr/'QcomQrtr.h').resolve())],'no second QRTR header')
    details.append(row)
require(not errors,'compile errors: '+', '.join(errors))
frontend=OUT/'NezhaSensorHid.o';raw=frontend.read_bytes();symbols=ns['object_symbols'](raw)
vtable=[n for n,_,kind in symbols if n.startswith('__ZTV') and kind!=1 and '9MetaClassE' not in n]
require(len(vtable)==1,'one NativeNezhaSensor vtable')
entries=ns['object_vtable'](raw,symbols,vtable[0],219)
require(all(entries),'all219 native authenticated slots')
for i,(_,word) in enumerate(entries):
    require(word>>63 and word>>48&1 and word>>49&3==0 and word>>32&0xffff==int(abi['slots'][i]['diversity'],16),f'sensor PAC slot{i}')
all_bindings={n:int(v,16) for n,v in abi['bindings'].items()}
for name,row in fixture['constructors'].items():all_bindings[name]=int(row['address'],16)
for name,_,kind in symbols:
    if kind!=1 or '17IOHIDEventService' not in name or name in all_bindings:continue
    candidates=[i for i,s in enumerate(abi['slots']) if int(s['diversity'],16)==ns['discriminator'](name[1:])]
    require(len(candidates)==1,'unclassified original import '+name);all_bindings[name]=int(abi['slots'][candidates[0]]['target'],16)
needed={n for n,_,kind in symbols if kind==1}
bindings={n:v for n,v in all_bindings.items() if n in needed and n not in provided_bindings}
for name in needed&provided_bindings.keys():
    require(name not in all_bindings or provided_bindings[name]==all_bindings[name],
            'existing original binding differs: '+name)
# ALS already owns common EventService thunks and absolute class exports.
# Emit only new sensor constructors; duplicate strong symbols would fail the
# formal kernel link when both archives are selected.
assembly='.text\n.p2align 2\n';addresses=''
for name,address in sorted(bindings.items()):
    if name in {'__ZN17IOHIDEventService10gMetaClassE','__ZTV17IOHIDEventService'}:addresses+=f'.globl {name}\n.set {name}, {address:#x}\n'
    else:
        addresses+=f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
        assembly+=f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
iphone_sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
for name,content in [('sensor-bindings',assembly),('sensor-addresses',addresses)]:
    p=OUT/(name+'.S');p.write_text(content);run(['xcrun','clang','-target',TRIPLE,'-isysroot',iphone_sdk,'-c',p,'-o',OUT/(name+'.o')]);objects.append(OUT/(name+'.o'))
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin'
late=OUT/'NezhaSensorHid.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NezhaSensorHid.cpp',frontend,late])
objects[objects.index(frontend)]=late
archive=OUT/'libNezha27Sensors.a';archive.unlink(missing_ok=True);run(['xcrun','ar','rcs',archive,*objects])
(OUT/'build-report.json').write_text(json.dumps(dict(sources=details,archive_sha256=sha(archive),compiled_original_PAC_slots=219,
    original_bindings={n:hex(v) for n,v in bindings.items()},single_QRTR_owner=True,hardware_qualified=False),indent=2)+'\n')
print('Sensor source archive compiled; strict prelink and hardware validation remain separate')
