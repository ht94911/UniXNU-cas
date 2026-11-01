"""Read only fixed system kernel collection paths; seal bounded API evidence."""
import bisect,hashlib,importlib.util,json,os,pathlib,struct,subprocess,sys
from kc_image import Image
P=pathlib.Path;out=P(os.environ['RUNNER_TEMP'])/'ios27-sdk-contracts';out.mkdir(exist_ok=False);files=[]
report={'scope':'Darwin27 system kernel exported contract names and bounded code samples; no phone or user data','inputs':[],'phone_operations':False}
report['kernel_version']=subprocess.run(['uname','-r'],capture_output=True,text=True).stdout.strip()
for pathname in ['/System/Library/KernelCollections/BootKernelExtensions.kc','/System/Library/KernelCollections/SystemKernelExtensions.kc','/System/Library/Kernels/kernel']:
    path=P(pathname);record={'path':pathname};report['inputs'].append(record)
    if not path.is_file():record['exists']=False;continue
    record['bytes']=path.stat().st_size
    if record['bytes']>1024**3:record['skipped']='1GiB input bound';continue
    try:
        raw=path.read_bytes();record['sha256']=hashlib.sha256(raw).hexdigest();record['magic']=raw[:16].hex()
        record['file_type']=subprocess.run(['file','-b',str(path)],capture_output=True,text=True).stdout.strip()
        if raw[:4] in (bytes.fromhex('cafebabe'),bytes.fromhex('cafebabf'),bytes.fromhex('bebafeca'),bytes.fromhex('bfbafeca')):
            archs=subprocess.run(['xcrun','lipo','-archs',str(path)],capture_output=True,text=True)
            record['fat_archs']=archs.stdout.strip()
            options=archs.stdout.split();chosen=next((a for a in ['arm64e','arm64','x86_64'] if a in options),None)
            if chosen is None:raise ValueError('no supported64-bit Mach-O slice')
            thin=out.parent/('thin-'+path.name)
            subprocess.run(['xcrun','lipo',str(path),'-thin',chosen,'-output',str(thin)],check=True,capture_output=True)
            raw=thin.read_bytes();record['slice']=chosen;record['slice_sha256']=hashlib.sha256(raw).hexdigest()
        record['cpu_type']=hex(struct.unpack_from('<I',raw,4)[0])
        image=Image(raw);record['entries']=list(image.entries)
        core=Image(raw,image.entries['com.apple.kernel'][1]) if 'com.apple.kernel' in image.entries else image
        symbols=core.symbols();starts=core.function_starts();selected=[];total=0
        for address,names in symbols.items():
            labels=[n for n in names if any(s in n.lower() for s in ['image4','callplatformfunction','ml_get_topology_info','code_signing_configuration'])]
            if not labels:continue
            row={'address':hex(address),'names':labels};selected.append(row)
            i=bisect.bisect_left(starts,address)
            if i>=len(starts) or starts[i]!=address or i+1>=len(starts):continue
            length=starts[i+1]-address
            if not 0<length<=4096 or total+length>1024**2:continue
            data=core.read(address,length);name='code/'+path.name+'/'+hex(address)+'.bin';target=out/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);files.append(name);total+=length;row.update(bytes=length,code=name,sha256=hashlib.sha256(data).hexdigest())
        record.update(symbol_count=sum(map(len,symbols.values())),function_start_count=len(starts),selected=selected,code_bytes=total)
    except Exception as error:record['error']=type(error).__name__+': '+str(error)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');files.append('report.json')
spec=importlib.util.spec_from_file_location('existing_artifacts',P(__file__).with_name('artifacts.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
m.seal(out,files,P(__file__).with_name('artifact-recipient.pem'),out.parent/'ios27-sdk-sealed')
print('fixed-path kernel API inventory sealed; files:',len(files))
