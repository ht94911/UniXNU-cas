#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Shared deterministic 32MiB FAT16 writer; policy stays with each profile."""
import hashlib,struct
TOTAL=65536;FAT_SECTORS=128;ROOT_SECTOR=257;DATA_SECTOR=289;CLUSTER=1024
DATE=((2026-1980)<<9)|(10<<5)|25
def sha(raw):return hashlib.sha256(raw).hexdigest()
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
def build(content,*,label=b'CAS_X06    ',serial=0x43415806):
 if len(label)!=11:raise ValueError('FAT volume label length')
 image=bytearray(TOTAL*512);image[:3]=b'\xeb\x3c\x90';image[3:11]=b'CASX0632'
 struct.pack_into('<HBHBHHBHHHII',image,11,512,2,1,2,512,0,0xf8,FAT_SECTORS,32,64,0,TOTAL)
 struct.pack_into('<BBBI',image,36,0x80,0,0x29,serial);image[43:54]=label;image[54:62]=b'FAT16   ';image[510:512]=b'\x55\xaa'
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

 children={name:[] for name in dirs};refs={}
 for path,parent,name,short,raw in content:
  cluster=allocate(raw);children[parent].append(lfn(name,short)+entry(short,0x20,cluster,len(raw)));refs[path]={'bytes':len(raw),'sha256':sha(raw)}
 for name,parent in [('UNIXNU','EFI'),('TOOLS','EFI'),('DRIVERS','EFI')]:children['EFI'].append(entry(name.encode().ljust(11,b' '),0x10,dirs[name]))
 for name,cluster in dirs.items():
  parent=dirs['EFI'] if name in ('UNIXNU','TOOLS','DRIVERS') else 0
  data=entry(b'.          ',0x10,cluster)+entry(b'..         ',0x10,parent)+b''.join(children[name])
  if len(data)>=CLUSTER:raise ValueError('directory exceeds bounded cluster')
  pos=DATA_SECTOR*512+(cluster-2)*CLUSTER;image[pos:pos+len(data)]=data
 root=entry(label,8,0)+entry(b'EFI        ',0x10,dirs['EFI'])+entry(b'XNU        ',0x10,dirs['XNU'])
 image[ROOT_SECTOR*512:ROOT_SECTOR*512+len(root)]=root;image[512:129*512]=fat;image[129*512:257*512]=fat
 return bytes(image),refs