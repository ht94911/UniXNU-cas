#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the current format2 build; export only enumerated encrypted owner results."""
import argparse,datetime,hashlib,json,os,platform,shutil,subprocess,sys,tarfile
from pathlib import Path
HERE=Path(__file__).resolve().parent;DEVICE=HERE.parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('-j',type=int,default=4);a=p.parse_args()
 if platform.system()!='Darwin':raise SystemExit('This execution profile requires the actual macOS/Xcode host')
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);logs=out/'logs';exports=out/'exports';logs.mkdir();exports.mkdir()
 manifest=json.loads((HERE/'format2-sources.json').read_text());report={'schema_version':1,'profile':'cas-format2-continuous-001','started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'result':'running','device_actions':0,'source_manifest':manifest,'source_manifest_sha256':sha(HERE/'format2-sources.json'),'host':{'system':platform.platform(),'machine':platform.machine()},'commands':[]}
 def save(): (exports/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
 def run(label,argv,cwd=None):
  with (logs/(label+'.log')).open('x') as log:r=subprocess.run(list(map(str,argv)),cwd=cwd,stdout=log,stderr=subprocess.STDOUT)
  report['commands'].append({'label':label,'argv':list(map(str,argv)),'exit_code':r.returncode});save()
  if r.returncode:
   print((logs/(label+'.log')).read_text(errors='replace')[-10000:]);raise RuntimeError(label+' failed')
 def capture(argv):return subprocess.check_output(argv,text=True).strip()
 save()
 try:
  env={'xcode':capture(['xcodebuild','-version']),'iphoneos_sdk':capture(['xcrun','--sdk','iphoneos','--show-sdk-version']),'clang':capture(['xcrun','--sdk','iphoneos','clang','--version']),'python':sys.version,'developer_dir':os.environ.get('DEVELOPER_DIR')}
  if not env['xcode'].startswith('Xcode26.6') and not env['xcode'].startswith('Xcode 26.6'):raise ValueError('Selected actual Xcode is not the reviewed26.6 profile')
  report['toolchain']=env;(exports/'toolchain.json').write_text(json.dumps(env,indent=2)+'\n')
  framework=out/'UniXNU';run('framework-clone',['git','clone','--no-checkout',manifest['framework']['git'],framework]);run('framework-pin',['git','checkout','--detach',manifest['framework']['commit']],framework)
  if capture(['git','-C',str(framework),'rev-parse','HEAD'])!=manifest['framework']['commit']:raise ValueError('Framework pin mismatch')
  driver=framework/'unixnu';common=['-D',DEVICE,'--out',out/'out','-j',str(a.j)]
  for label,cmd in [('defconfig',[driver,'defconfig','cas_continuous_defconfig',*common]),('fetch',[driver,'fetch',*common]),('status',[driver,'status','--json',*common]),('porting',[sys.executable,framework/'scripts/porting-record.py','check','--device',DEVICE]),('tree',[driver,'tree',*common]),('kernel',[driver,'build','kernel',*common])]:run(label,cmd,framework)
  work=out/'out/cas/ipados-17.7.11-21H461/cas_continuous';tree=work/'tree';obj=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
  # Reuse independent Mach-O and actual compiler page-size checks, without the legacy build path.
  sys.path.insert(0,str(HERE));from build_source import kernel_identity,audit_page_size
  kernel=obj/'mach.development.bcm2837.unstripped.noctf';report['kernel']=kernel_identity(kernel);report['page_size']=audit_page_size(tree,logs)
  for src,name in [(kernel,'kernel.unstripped.macho'),(work/'kernel/kernel.macho','kernel.prelinked.macho'),(work/'kernel/kernel.json','prelink.json'),(DEVICE/'unixnu.lock','unixnu.lock'),(work/'.config','configuration.txt'),(work/'xnu-patches.list','patch-order.txt'),(work/'xnu-inputs.json','xnu-inputs.json')]:
   if not src.is_file():raise FileNotFoundError(src)
   shutil.copyfile(src,exports/name)
  with (exports/'kernel.symbols.txt').open('x') as symbols:subprocess.run(['xcrun','nm','-nm',kernel],stdout=symbols,check=True)
  run('own-user-fixture',[sys.executable,tree/'scripts/build-native-user-fixture.py','--output',tree/'build/cas-user-fixture'],tree)
  fixture=tree/'build/cas-user-fixture'
  for src,name in [('launchd.macho','user-fixture.macho'),('trustcache.bundle','user-fixture.trustcache'),('trustcache.json','user-fixture.json'),('addresses.json','user-fixture-addresses.json')]:shutil.copyfile(fixture/src,exports/name)
  symbols=(exports/'kernel.symbols.txt').read_text()
  required=['_cas_vm_trace','_cas_runtime_step','_cas_irq_publish','_cas_runtime_panic','_cas_armcpu_timer_checkpoint']
  missing=[s for s in required if not any(line.split()[-1:]==[s] for line in symbols.splitlines())]
  if missing:raise ValueError('Continuous symbols missing: '+str(missing))
  report['prelinked']={'sha256':sha(exports/'kernel.prelinked.macho'),'bytes':(exports/'kernel.prelinked.macho').stat().st_size};report['symbols']={'sha256':sha(exports/'kernel.symbols.txt'),'required_present':required};report['result']='pass';report['runtime']='not_run';report['user_fixture']='own signed static process; not Apple launchd';save()
 except BaseException as e:report['result']='fail';report['error']=str(e);save();raise
 finally:
  report['ended_at']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
  with tarfile.open(exports/'logs.tar.gz','w:gz') as tar:
   tar.add(logs,arcname='executor-logs')
   for name in ['out/cas/ipados-17.7.11-21H461/cas_continuous/logs','out/cas/ipados-17.7.11-21H461/cas_continuous/tree/build/cas-armcpu-overlay/cas-overlay.json']:
    if (out/name).exists():tar.add(out/name,arcname=name)
  allowed={'build-report.json','logs.tar.gz','toolchain.json','kernel.unstripped.macho','kernel.prelinked.macho','prelink.json','unixnu.lock','configuration.txt','patch-order.txt','xnu-inputs.json','kernel.symbols.txt','user-fixture.macho','user-fixture.trustcache','user-fixture.json','user-fixture-addresses.json'}
  names={p.name for p in exports.iterdir()}
  if not names<=allowed:raise ValueError('Unexpected export names')
  from artifacts import seal
  seal(exports,sorted(names),HERE/'artifact-recipient.pem',out/'sealed')
if __name__=='__main__':main()
