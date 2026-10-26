#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Audit actual canonical cas ADT bytes against independent board facts; no device operations."""
import argparse,hashlib,json,os,stat,struct
from pathlib import Path
from adt_codec import decode
from cas_adt import validate_facts,validate_nvram
ROOT=Path(__file__).resolve().parents[1]
def audit(raw,facts):
 validate_facts(facts);tree=decode(raw)
 def get(path,key):return tree.at(path).properties[key]
 def expect(path,key,value):
  if get(path,key)!=value:raise ValueError(path+'::'+key+' differs from actual cas facts/policy')
 def pairs(items):return b''.join(struct.pack('<QQ',i['base'],i['size']) for i in items)
 expect('/','model',b'iPad7,1\0');expect('/','hardware-model',facts['model'].encode()+b'\0')
 expect('/memory','reg',pairs(facts['memory_banks']));expect('/chosen','dram-ranges',pairs(facts['memory_banks']))
 reservations=sorted(struct.unpack('<QQ',n.properties['reg']) for n in tree.at('/reserved-memory').children)
 if reservations!=sorted((r['base'],r['size']) for r in facts['reservations']):raise ValueError('Reservation set differs from frozen facts')
 if len(tree.at('/cpus').children)!=1:raise ValueError('Only CPU0 is enabled for this candidate')
 expect('/cpus/cpu0','reg',struct.pack('<I',0));expect('/cpus/cpu0','state',b'running\0');expect('/cpus/cpu0','timebase-frequency',struct.pack('<I',19200000))
 child,base,size=struct.unpack('<QQQ',get('/arm-io','ranges'));reg=struct.unpack('<4Q',get('/arm-io/intc','reg'))
 gic=facts['gic'];d=gic['distributor'];r=gic['redistributor']
 if child!=0 or (base+reg[0],reg[1],base+reg[2],reg[3])!=(d['base'],d['size'],r['base'],r['size']):raise ValueError('Physical GIC geometry differs')
 expect('/arm-io/intc','compatible',b'arm,gic-v3\0');expect('/arm-io/intc','redistributor-stride',struct.pack('<Q',gic['redistributor_stride']))
 expect('/timer','clock-frequency',struct.pack('<I',19200000))
 expect('/chosen','darwinboot-memory-size',struct.pack('<Q',0x10000000))
 entropy=get('/chosen','random-seed')
 if len(entropy)!=64 or len(set(entropy))<16:raise ValueError('Private64-byte entropy required; deterministic fixtures refuse')
 if struct.unpack('<Q',get('/chosen','unique-chip-id'))[0]==0:raise ValueError('Nonzero explicit local identity required')
 expect('/chosen','chip-id',struct.pack('<I',0x8011));validate_nvram(get('/chosen','nvram-proxy-data'))
 return {'schema_version':1,'mode':'private','producer':'standard format2 DTS→ADT; checked against independent cas facts','cpu_profile':'cpu0','entropy_bytes':64,'adt_sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),'bank_count':3,'reservation_count':len(reservations),'GIC_physical_geometry_verified':True,'memory_ownership':'not granted by ADT; runtime UEFI allocation and held probe required','device_actions':0,'runtime_success':False}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('adt',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--facts',type=Path,default=ROOT/'config/cas-adt-facts.json');a=p.parse_args()
 if a.out.exists():raise FileExistsError(a.out)
 if os.name=='posix' and stat.S_IMODE(a.adt.stat().st_mode)&0o077:raise ValueError('Private ADT must have mode0600 inside a private build directory')
 result=audit(a.adt.read_bytes(),json.loads(a.facts.read_text()));a.out.write_text(json.dumps(result,indent=2)+'\n');a.out.chmod(0o600)
 print(json.dumps(result))
if __name__=='__main__':main()
