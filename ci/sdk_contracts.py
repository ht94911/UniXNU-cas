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
# Generate the public SDK method declaration with Apple's own IIG. The
# complete generated client is sealed, not logged or claimed boot-qualified.
iigdir=out/'iig';iigdir.mkdir()
source=iigdir/'IOService.iig'
# Apple IIG requires the real OSObject/OSMetaClassBase declarations; the
# deliberately minimal layout experiment was not sufficient for its frontend.
import urllib.request
source_digests={'OSObject.iig': 'bcd00a8dfda6bbb511156ca23cb9a6b1685a828c6b91bb589144bba91215aca1', 'IOService.iig': '874c48d99a6663f339a21479d05fd39d2fdca0f6fdf0c1515d8d71391826363a'}
include=iigdir/'include/DriverKit';include.mkdir(parents=True)
for filename,digest in source_digests.items():
    url='https://raw.githubusercontent.com/apple-oss-distributions/xnu/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/iokit/DriverKit/'+filename
    raw=urllib.request.urlopen(url,timeout=40).read()
    if hashlib.sha256(raw).hexdigest()!=digest:raise ValueError('pinned public IIG source changed')
    text=raw.decode()
    if filename=='IOService.iig':
        anchor='class KERNEL IOService : public OSObject\n{\npublic:'
        if text.count(anchor)!=1:raise ValueError('IOService source declaration changed')
        method='\n    virtual kern_return_t CallPlatformFunction(OSString *functionName, bool waitForFunction, OSDictionary *inParam, OSDictionary **outParam) LOCAL IIG_SERIALIZABLE;'
        source.write_text(text.replace(anchor,anchor+method,1))
    else:(include/filename).write_text(text)
found=command(['xcrun','--sdk','macosx','--find','iig']);report['iig_tool']=found
sdk=P(command(['xcrun','--sdk','macosx','--show-sdk-path'])['stdout'].strip())
kernel_headers=sdk/'System/Library/Frameworks/Kernel.framework/Headers'
if found['returncode']==0:
    report['iig_generate']=command([found['stdout'].strip(),'--def',str(source),'--header',str(iigdir/'IOService.h'),'--impl',str(iigdir/'IOService.iig.cpp'),'--framework-name','DriverKit','--','-x','c++','-std=gnu++2b','-D__IIG=1','-DDRIVERKIT_PRIVATE=1','-I'+str(iigdir/'include'),'-I'+str(kernel_headers),'-isysroot',str(sdk)])
report['pinned_iig_sources']=source_digests
for sourcefile in sorted(iigdir.iterdir()):
    if sourcefile.is_file() and sourcefile.suffix in ('.h','.cpp','.iig'):
        names.append(str(sourcefile.relative_to(out)))
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');names.append('report.json')
spec=importlib.util.spec_from_file_location('existing_artifacts',P(__file__).with_name('artifacts.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
m.seal(out,names,P(__file__).with_name('artifact-recipient.pem'),out.parent/'ios27-sdk-sealed')
print('SDK contract inventory sealed:',len(report['headers']),'headers;',total,'bytes; no device operations')
