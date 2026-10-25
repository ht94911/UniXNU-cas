#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Package a NEW private CPU0 image locally. No device/network/Git mutations."""
import argparse,hashlib,json,os,shutil,stat,subprocess,sys,tomllib
from pathlib import Path
from cpu0_payload import build as build_payload
from check_boot import inspect
from provenance import source_state,require_clean,check_ref
from cas_adt import private_directory,private_write
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def ref(p):return {'path':str(p.resolve()),'bytes':p.stat().st_size,'sha256':sha(p)}
def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in ['uefi','base-package','firmware-build','loader-build','adt-dir','kernel','symbols','out']:ap.add_argument('--'+name,type=Path,required=True)
 a=ap.parse_args();a.out=a.out.resolve();private_directory(a.out);os.umask(0o077)
 uefi=a.uefi.resolve();sources=source_state(uefi,ROOT);require_clean(sources)
 commands=[]
 def run(argv,name):
  argv=list(map(str,argv))
  with (a.out/name).open('x') as log:code=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT).returncode
  commands.append({'argv':argv,'log':name,'exit_code':code})
  if code:raise RuntimeError(name+' failed; preserve output')
 base=a.base_package.resolve();base_run=json.loads((base/'package-run.json').read_text())
 firmware=json.loads((a.firmware_build/'run.json').read_text());loader=json.loads((a.loader_build/'result.json').read_text());overlay=json.loads((a.loader_build/'overlay/overlay.json').read_text())
 if base_run.get('result')!='pass' or firmware.get('result')!='pass' or not firmware.get('diagnostics'):raise ValueError('successful matching diagnostic base required')
 for path in [base/'source-manifest.json',base/'boot.img']:
  expected=base_run['source_manifest'] if path.name=='source-manifest.json' else base_run['image'];check_ref(expected)
 if json.loads(Path(firmware['source_state']['path']).read_text())!=sources:raise ValueError('current sources differ from base firmware build')
 loader_sources=json.loads((a.loader_build/'intent.json').read_text())['sources']
 for repo in [ROOT,uefi,uefi.parent/'UniXNU']:
  state=loader_sources[str(repo)];head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
  if state['head']!=head or state['dirty']:raise ValueError('loader source commit/cleanliness mismatch')
 adt=a.adt_dir/'cas.adt';audit=json.loads((a.adt_dir/'audit.json').read_text())
 if adt.stat().st_mode&0o077 or a.adt_dir.stat().st_mode&0o077:raise ValueError('private ADT POSIX permissions missing')
 contract=json.loads((ROOT/'config/cpu0-contract.json').read_text())
 if sha(a.symbols)!=contract['kernel']['symbols_sha256']:raise ValueError('matching symbols identity mismatch')
 driver=a.loader_build/'Build/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi';viewer=a.loader_build/'Build/DEBUG_CLANGPDB/AARCH64/CasTraceView.efi';diag=base/'modules/CasDiag.efi'
 expected_diag=a.firmware_build/'Build/DEBUG_CLANGPDB/AARCH64/CasDiag.efi'
 if diag.read_bytes()!=expected_diag.read_bytes():raise ValueError('diagnostic EFI differs from base firmware build')
 config=(ROOT/'config/cpu0.toml').read_bytes()
 raw,files,payload_audit=build_payload(config,diag.read_bytes(),driver.read_bytes(),a.kernel.read_bytes(),adt.read_bytes(),viewer.read_bytes(),adt_report=audit,loader_report=loader,overlay=overlay)
 fat=a.out/'payload-fat.img';private_write(fat,raw)
 run(['fsck.fat','-n',fat],'fat-fsck.log');run(['mdir','-i',fat,'-s','::/'],'fat-tree.log')
 (a.out/'readback').mkdir(mode=0o700)
 for name,expected in files.items():
  dest=a.out/'readback'/name;dest.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
  run(['mcopy','-i',fat,'::/'+name,dest],'readback-'+Path(name).name+'.log')
  if sha(dest)!=expected['sha256'] or dest.stat().st_size!=expected['bytes']:raise ValueError('FAT file readback mismatch')
 cfg=tomllib.loads((uefi/'Resources/Configs/cas.toml').read_text());memory=json.loads((ROOT/'config/memory-map.json').read_text())
 for name in ['kernel.gz','SILICIUM_UEFI.fd','cas.dtb']:shutil.copyfile(base/name,a.out/name)
 shim=base/'shim/CasBootShim.bin'
 if not shim.exists():
  parts=list((base/'shim').rglob('*.bin'))
  if len(parts)!=1:raise ValueError('ambiguous shim');
  shim=parts[0]
 b=cfg['boot_image'];image=a.out/'boot.img'
 argv=[sys.executable,uefi/'Resources/Scripts/mkbootimg.py','--kernel',a.out/'kernel.gz','--ramdisk',fat,'--dtb',a.out/'cas.dtb','--board','cas','--cmdline',b['cmdline']]
 for key in ['header_version','pagesize','base','kernel_offset','ramdisk_offset','dtb_offset','tags_offset']:argv+=['--'+key,str(b[key])]
 run([*argv,'--output',image,'--id'],'mkbootimg.log')
 check,parts=inspect(image.read_bytes(),(a.out/'SILICIUM_UEFI.fd').read_bytes(),shim.read_bytes(),(a.out/'cas.dtb').read_bytes(),raw,cfg,memory)
 private_write(a.out/'image-check.json',(json.dumps(check,indent=2)+'\n').encode())
 # Keep all trial inputs together, private. No entropy value is put in metadata.
 for name,path in [('kernel.macho',a.kernel),('kernel.symbols.txt',a.symbols),('cas.adt',adt),('DarwinBoot.efi',driver),('CasTraceView.efi',viewer),('CasDiag.efi',diag)]:shutil.copyfile(path,a.out/name)
 archive=a.out/'source-cpu0.tar'
 with archive.open('xb') as stream:subprocess.run(['git','archive','--format=tar','HEAD'],cwd=ROOT,stdout=stream,check=True)
 manifest={'schema_version':1,'board':'cas','build_id':contract['profile'],'profile':'CPU0 RAM boot; no UFS/Data/touch/GPU','result':'pass','repositories':sources,'source_date_epoch':'1792643741',
  'base_package_manifest':ref(base/'source-manifest.json'),'firmware_build':firmware,'loader_build':loader,'overlay':overlay,'contract':contract,
  'ADT_private_audit':audit,'payload':{'files':files,**payload_audit},'image_checks':check,'commands':commands,'image':ref(image),
  'private_candidate':True,'runtime_success':False,'requires_validation_return_then_distinct_boot':True,'candidate_tag':overlay['candidate_tag'],
  'configuration_inputs':[ref(ROOT/'config/cpu0.toml'),ref(ROOT/'config/cpu0-contract.json'),ref(uefi/'Resources/Configs/cas.toml'),ref(ROOT/'config/memory-map.json')],
  'outputs':[ref(p) for p in sorted(a.out.rglob('*')) if p.is_file()]}
 if source_state(uefi,ROOT)!=sources:raise ValueError('sources changed during packaging')
 private_write(a.out/'source-manifest.json',(json.dumps(manifest,indent=2)+'\n').encode())
 lines=[sha(p)+'  '+str(p.relative_to(a.out)) for p in sorted(a.out.rglob('*')) if p.is_file()]
 private_write(a.out/'SHA256SUMS',('\n'.join(lines)+'\n').encode())
 print(json.dumps({'result':'pass','image':ref(image),'manifest':ref(a.out/'source-manifest.json'),'candidate_tag':overlay['candidate_tag'],'kernel_executed':False,'device_actions':0,'github_mutations':0}))
if __name__=='__main__':main()
