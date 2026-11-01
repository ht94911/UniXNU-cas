"""Bounded SDK contract inventory; no phone or kernel execution."""
import hashlib,importlib.util,json,os,pathlib,shutil,subprocess,sys
P=pathlib.Path;out=P(os.environ['RUNNER_TEMP'])/'ios27-sdk-contracts';out.mkdir(exist_ok=False);names=[];report={'purpose':'installed SDK API/type inventory only','phone_operations':False,'headers':[],'exports':[]}
def command(argv):
 p=subprocess.run(argv,capture_output=True,text=True);return dict(command=argv,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
report['xcode']=command(['xcodebuild','-version']);report['compiler']=command(['xcrun','clang','--version'])
report['sdks']=[];total=0
markers=['/TrustedExecutionMonitor/','/image4/','/img4/','/arm/cpu_topology.h','/arm/machine_routines.h','/kern/kern_types.h','/IOKit/IOService.h','/DriverKit/IOService.h','/DriverKit/IORPC.h','/sys/code_signing.h','/sys/code_signing_internal.h']
for sdk in ['macosx','iphoneos']:
 data=command(['xcrun','--sdk',sdk,'--show-sdk-path']);report['sdks'].append(data)
 if data['returncode']:continue
 root=P(data['stdout'].strip()).resolve()
 # Search only selected SDK header/framework scopes; cap output and sizes.
 for relative in ['System/Library/Frameworks/Kernel.framework','usr/include','System/Library/Frameworks/DriverKit.framework']:
  scope=root/relative
  if not scope.exists():continue
  for directory,dirs,files in os.walk(scope,followlinks=False):
   for name in sorted(files):
    source=P(directory)/name;rel=source.relative_to(root).as_posix();match=any(m in '/'+rel for m in markers)
    if name.endswith('.h') and match:
     data=source.read_bytes();total+=len(data)
     if len(data)>2*1024**2 or total>10*1024**2 or len(report['headers'])>=200:raise ValueError('SDK inventory exceeded its fixed bound')
     dest='sdk/'+sdk+'/'+rel;target=out/dest;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);names.append(dest)
     report['headers'].append(dict(sdk=sdk,path=rel,bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
    if name.endswith('.tbd') and 'Kernel.framework' in rel:
     matches=[line.strip() for line in source.read_text().splitlines() if any(k in line for k in ['image4','Image4','CallPlatformFunction','ml_get_topology_info'])]
     if matches:report['exports'].append(dict(sdk=sdk,path=rel,matches=matches))
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');names.append('report.json')
spec=importlib.util.spec_from_file_location('existing_artifacts',P(__file__).with_name('artifacts.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
m.seal(out,names,P(__file__).with_name('artifact-recipient.pem'),out.parent/'ios27-sdk-sealed')
print('SDK contract inventory sealed:',len(report['headers']),'headers;',total,'bytes; no device operations')
