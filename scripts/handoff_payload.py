#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Deterministic independent 32MiB FAT16 for an X06 host-only dry run."""
import argparse,hashlib,json,struct,tomllib
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
TOTAL=65536;FAT_SECTORS=128;ROOT_SECTOR=257;DATA_SECTOR=289;CLUSTER=1024
DATE=((2026-1980)<<9)|(10<<5)|25
KERNEL_SHA='6c4993dea6d94544bc6f0c89bd72c6d292401ce2e19caaf65725749434c0c648'
DRIVER_SHA='ff6712315585ee940ba08ac049c396af945cf554927203f6f02d3c27b17db8d2'
FIXTURE_SHA='b0d09551ce2e8098953fa6679f52c63777f71b23812ce5aea21ca9186c65b9a4'
def sha(data):return hashlib.sha256(data).hexdigest()
def pe(raw,subsystem):
 if len(raw)<64 or raw[:2]!=b'MZ':raise ValueError('PE header required')
 offset=struct.unpack_from('<I',raw,60)[0]
 if offset+94>len(raw) or raw[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',raw,offset+4)[0]!=0xaa64 or struct.unpack_from('<H',raw,offset+92)[0]!=subsystem:raise ValueError('AARCH64 PE subsystem mismatch')
def entry(short,attr,cluster,size=0):
 if len(short)!=11:raise ValueError('short name')
 item=bytearray(32);item[:11]=short;item[11]=attr
 struct.pack_into('<HHHH',item,14,0,DATE,DATE,0);struct.pack_into('<HHHI',item,22,0,DATE,cluster,size)
 return bytes(item)
def lfn(name,short):
 units=list(struct.unpack('<'+'H'*(len(name.encode('utf-16le'))//2),name.encode('utf-16le')))+[0]
 if len(units)>65:raise ValueError('bounded payload name')
 while len(units)%13:units.append(0xffff)
 checksum=0
 for n in short:checksum=(((checksum&1)<<7)+(checksum>>1)+n)&255
 result=b''
 for index in reversed(range(len(units)//13)):
  item=bytearray(32);item[0]=(index+1)|(0x40 if index==len(units)//13-1 else 0);item[11]=0x0f;item[13]=checksum
  for offset,value in zip([1,3,5,7,9,14,16,18,20,22,24,28,30],units[index*13:index*13+13]):struct.pack_into('<H',item,offset,value)
  result+=item
 return result
def build(config,diag,driver,kernel,adt,viewer):
 parsed=tomllib.loads(config.decode())
 expected=tomllib.loads((ROOT/'config/handoff-dryrun.toml').read_text())
 if sha(config)!='9fff86baae6752c8905af6b6f59aa3e264db92ee4342ce193baa45b5f90ee607':raise ValueError('dry-run menu identity mismatch')
 if parsed!=expected:raise ValueError('fixed dry-run menu required')
 pe(diag,10);pe(driver,11);pe(viewer,10)
 if sha(driver)!=DRIVER_SHA:raise ValueError('fixed dry-run driver identity mismatch')
 if sha(kernel)!=KERNEL_SHA or sha(adt)!=FIXTURE_SHA:raise ValueError('X06 host kernel/fixture identity mismatch')
 image=bytearray(TOTAL*512);image[:3]=b'\xeb\x3c\x90';image[3:11]=b'CASX0632'
 struct.pack_into('<HBHBHHBHHHII',image,11,512,2,1,2,512,0,0xf8,FAT_SECTORS,32,64,0,TOTAL)
 struct.pack_into('<BBBI',image,36,0x80,0,0x29,0x43415806);image[43:54]=b'CAS_X06    ';image[54:62]=b'FAT16   ';image[510:512]=b'\x55\xaa'
 fat=bytearray(FAT_SECTORS*512);struct.pack_into('<HH',fat,0,0xfff8,0xffff);next_cluster=2
 def allocate(raw):
  nonlocal next_cluster
  count=max(1,(len(raw)+CLUSTER-1)//CLUSTER);start=next_cluster
  if start+count>(TOTAL-DATA_SECTOR)//2+2:raise ValueError('payload exceeds32MiB capacity')
  for n in range(count):
   c=start+n;struct.pack_into('<H',fat,c*2,0xffff if n==count-1 else c+1)
   chunk=raw[n*CLUSTER:(n+1)*CLUSTER];pos=DATA_SECTOR*512+(c-2)*CLUSTER;image[pos:pos+len(chunk)]=chunk
  next_cluster+=count;return start
 dirs={name:allocate(b'') for name in ['EFI','UNIXNU','TOOLS','DRIVERS','XNU']}
 content=[('EFI/UniXNU/unixnu.toml','UNIXNU','unixnu.toml',b'UNIXNU~1TOM',config),
          ('EFI/Tools/CasDiag.efi','TOOLS','CasDiag.efi',b'CASDIAG EFI',diag),
          ('EFI/Drivers/DarwinBoot.efi','DRIVERS','DarwinBoot.efi',b'DARWIN~1EFI',driver),
          ('EFI/Tools/CasTraceView.efi','TOOLS','CasTraceView.efi',b'CASTRA~1EFI',viewer),
          ('XNU/kernel.macho','XNU','kernel.macho',b'KERNEL~1MAC',kernel),
          ('XNU/kernel.macho.adt','XNU','kernel.macho.adt',b'KERNEL~1ADT',adt)]
 children={name:[] for name in dirs};refs={}
 for path,parent,name,short,raw in content:
  cluster=allocate(raw);children[parent].append(lfn(name,short)+entry(short,0x20,cluster,len(raw)));refs[path]={'bytes':len(raw),'sha256':sha(raw)}
 for name,parent in [('UNIXNU','EFI'),('TOOLS','EFI'),('DRIVERS','EFI')]:children['EFI'].append(entry(name.encode().ljust(11,b' '),0x10,dirs[name]))
 for name,cluster in dirs.items():
  parent=dirs['EFI'] if name in ('UNIXNU','TOOLS','DRIVERS') else 0
  data=entry(b'.          ',0x10,cluster)+entry(b'..         ',0x10,parent)+b''.join(children[name])
  if len(data)>=CLUSTER:raise ValueError('directory exceeds bounded cluster')
  pos=DATA_SECTOR*512+(cluster-2)*CLUSTER;image[pos:pos+len(data)]=data
 root=entry(b'CAS_X06    ',8,0)+entry(b'EFI        ',0x10,dirs['EFI'])+entry(b'XNU        ',0x10,dirs['XNU'])
 image[ROOT_SECTOR*512:ROOT_SECTOR*512+len(root)]=root;image[512:129*512]=fat;image[129*512:257*512]=fat
 return bytes(image),refs
def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in ('diag','driver','kernel','adt','viewer','out'):ap.add_argument('--'+name,type=Path,required=True)
 a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 data,files=build((ROOT/'config/handoff-dryrun.toml').read_bytes(),a.diag.read_bytes(),a.driver.read_bytes(),a.kernel.read_bytes(),a.adt.read_bytes(),a.viewer.read_bytes())
 (a.out/'payload-fat.img').write_bytes(data)
 (a.out/'manifest.json').write_text(json.dumps({'profile':'cas-x06-host-fixture','fat_bytes':len(data),'fat_sha256':sha(data),'files':files,
  'boot_ready':False,'kernel_executed':False,'host_fixture_contains_entropy':False,'phone_boot_forbidden':True,
  'warning':'X03 fixture has no fresh entropy. This payload is host-only and cannot be promoted to a phone candidate.'},indent=2)+'\n')
 print(json.dumps({'result':'built','bytes':len(data),'sha256':sha(data),'phone_boot_forbidden':True}))
if __name__=='__main__':main()
