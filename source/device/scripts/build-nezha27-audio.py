#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the selected GPU015 audio increment with its native XNU records.

Uses existing compile_command, kernel_target and sheng HAL SDK/signing paths.
No phone/firmware/ACDB input, no hardware start, no private-slot fallback ABI.
"""
from pathlib import Path
import hashlib,importlib.util,json,os,shlex,shutil,subprocess,sys,tempfile
from kernel_target import ARCH,IOS_VERSION,TRIPLE,check_object
ROOT=Path(__file__).resolve().parents[1];SRC=ROOT/'kits/nezha/audio27';OUT=ROOT/'build/nezha27-audio'
OUT.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(command,**kw):return subprocess.check_output([str(x) for x in command],timeout=300,**kw)
def load(path):
 s=importlib.util.spec_from_file_location('audio_compile_helper',path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
if ARCH!='arm64e' or IOS_VERSION.split('.')[0]!='27':raise SystemExit('audio27 requires the selected arm64e/iOS27 native build')
helper=load(ROOT/'scripts/build-sheng-source-drivers.py')
smmu=ROOT/'kits/ArmSmmu4K'
shared=json.loads((SRC/'integration/shared-component.json').read_text())
for name,digest in shared['files'].items():
 if sha(smmu/name)!=digest:raise SystemExit('selected GPU015 ArmSmmu4K identity differs: '+name)
base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
records={False:json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(',')),True:json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))}
# The task accessor requires the actual osfmk compilation environment, including
# its generated features, private declarations and libsa header precedence.
# Adding MACH_KERNEL_PRIVATE to a BSD record does not reproduce that contract.
task_record=json.loads((base/'osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
if 'MACH_KERNEL_PRIVATE' not in task_record['arguments']:
 raise SystemExit('selected osfmk record does not expose the real task layout')
sources=[p for folder in ['core','platform','upstream','os'] for p in sorted((SRC/folder).glob('*')) if p.suffix in ['.c','.cpp']]+[SRC/'hal/AudioClock.c']
objects=[];rows=[];errors=[]
for source in sources:
 name=str(source.relative_to(SRC)).replace('/','_');obj=OUT/(name+'.o');rc=records[source.suffix=='.c']
 extra=['-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(SRC),'-I',str(smmu),'-I',str(ROOT/'drivers/QcomXnuUsbDebug/include'),'-Wno-unknown-warning-option']
 if source.name=='NezhaAudioTask.c':rc=task_record
 command=helper.compile_command(rc,source,obj,[],extra)
 (OUT/(name+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
 cp=subprocess.run(command,cwd=rc['directory'],capture_output=True,text=True);(OUT/(name+'.log')).write_text(cp.stdout+cp.stderr)
 row={'source':str(source.relative_to(SRC)),'source_sha256':sha(source),'returncode':cp.returncode,'kernel_recipe_source':rc['file']}
 if cp.returncode:errors.append(name)
 else:
  check_object(obj.read_bytes(),name);row['object_sha256']=sha(obj);deps={}
  for name_dep in shlex.split(obj.with_suffix('.d').read_text().replace(chr(92)+chr(10),'').split(':',1)[1]):
   dep=Path(name_dep);dep=dep if dep.is_absolute() else Path(rc['directory'])/dep
   deps[str(dep.resolve())]=sha(dep)
  (OUT/(name+'-headers.json')).write_text(json.dumps(deps,indent=2)+'\n');objects.append(obj)
 rows.append(row)
# The HAL can be qualified even when a kernel API error needs a second pass.
hal={'status':'pending'}
try:
 ios=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip();mac=run(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
 bundle=OUT/'NezhaAudio27.driver';bundle.mkdir(exist_ok=True);exe=bundle/'NezhaAudio27'
 with tempfile.TemporaryDirectory() as tmp:
  headers=Path(tmp)/'CoreAudio';headers.mkdir()
  for name in ['AudioServerPlugIn.h','AudioHardwareBase.h','AudioHardware.h']:(headers/name).symlink_to(Path(mac)/'System/Library/Frameworks/CoreAudio.framework/Headers'/name)
  cmd=['xcrun','clang','-target','arm64e-apple-ios27.0','-isysroot',ios,'-I',tmp,'-O2','-g0','-Wall','-Wextra','-Werror','-bundle','-fvisibility=hidden','-framework','CoreFoundation','-framework','IOKit',str(SRC/'hal/NezhaAudioPlugIn.c'),str(SRC/'hal/AudioClock.c'),str(SRC/'core/AudioRing.c'),'-o',str(exe)]
  (OUT/'HAL-command.json').write_text(json.dumps(cmd,indent=2)+'\n');cp=subprocess.run(cmd,capture_output=True,text=True);(OUT/'HAL-build.log').write_text(cp.stdout+cp.stderr)
  hal={'returncode':cp.returncode,'sdk':ios,'header_sdk':mac}
  if cp.returncode:errors.append('HAL')
  else:
   shutil.copyfile(SRC/'hal/Info.plist',bundle/'Info.plist')
   run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.audio27',bundle]);run(['codesign','--verify','--strict',bundle])
   cp=subprocess.run(['codesign','--display','--verbose=4',bundle],capture_output=True,text=True,check=True);(OUT/'HAL-signature.txt').write_text(cp.stderr)
   hal.update(status='compiled-signed',sha256=sha(exe))
except Exception as e:hal={'error':str(e)};errors.append('HAL-exception')
report={'objects':rows,'HAL':hal,'errors':errors,'native_sources':len(sources),'native_framework_abi_only':True,'hardware_started':False,'private_firmware_uploaded':False,'shared_component':shared}
(OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
if errors:raise SystemExit('audio source/API qualification failed: '+','.join(errors))
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin'
service=OUT/'os_NezhaAudioService.cpp.o';late=OUT/'NezhaAudioService.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NezhaAudioService.cpp',service,late])
objects=[late if p==service else p for p in objects]
archive=OUT/'libNezha27Audio.a';archive.unlink(missing_ok=True);run(['xcrun','ar','rcs',archive,*objects],env=dict(os.environ,ZERO_AR_DATE='1'))
report.update(archive_sha256=sha(archive),objects_list=[p.name for p in objects],duplicate_ArmSmmu_objects=False)
(OUT/'defined-symbols.txt').write_bytes(run(['xcrun','nm','-gU',archive]));(OUT/'undefined-symbols.txt').write_bytes(run(['xcrun','nm','-u',archive]))
(OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('Audio27 native archive and HAL compiled; hardware and private input integration remain unverified')
