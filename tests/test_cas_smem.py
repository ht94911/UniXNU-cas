"""Actual bounded SMEM C reader: synthetic memory only, no device access."""
import ctypes as c
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
UEFI=Path(os.environ.get('CAS_UEFI_ROOT',ROOT.parent/'uefi-cas'))
READ=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint,c.c_uint,c.POINTER(c.c_ubyte))
class Part(c.Structure):_fields_=[('base',c.c_ulonglong),('size',c.c_ulonglong),('available',c.c_ulonglong),('attr',c.c_uint),('category',c.c_uint),('domain',c.c_uint),('type',c.c_uint)]
class Info(c.Structure):_fields_=[(n,c.c_uint) for n in ('smem_version','table_version','offset','size','count','reads','trace_bytes')]+[('parts',Part*32)]
class Work(c.Structure):_fields_=[('trace',c.c_ubyte*65536),('table',c.c_ubyte*4096)]+[(n,c.c_uint) for n in ('used','cursor','reads','compare')]+[('read',READ),('context',c.c_void_p)]+[(n,c.c_uint) for n in ('check','read_offset','read_length','sample_bytes')]+[('sample',c.c_ubyte*48)]+[(n,c.c_uint) for n in ('partition_offset','partition_size','partition_flags','partition_cacheline','parsed_rows','overlap_a','overlap_b')]
def put(mem,off,fmt,*values):struct.pack_into('<'+fmt,mem,off,*values)
def fixture(heap=12,table=2,rows=None,prefix=1):
 mem=bytearray(0x200000);put(mem,92,'I',heap<<16);put(mem,192,'IIII',1,0,0,0)
 rows=rows or [(0xc0000000,0x140000000,0x140000000,1,14,1,1),(0x200000000,0x180000000,0x180000000,1,14,3,1)]
 stride=64 if table==1 else 72;data=bytearray(24+len(rows)*stride);put(data,0,'IIIIII',0x9da5e0a8,0xaf9ec4e2,table,0,len(rows),0)
 for i,(base,size,avail,attr,category,domain,kind) in enumerate(rows):
  p=24+i*stride;put(data,p+16,'QQIIII',base,size,attr,category,domain,kind)
  if stride==72:put(data,p+64,'Q',avail)
 if heap==11:
  off=0x4000;put(mem,196,'II',off+len(data),0x1000);put(mem,208+402*16,'IIII',1,off,len(data),0)
 else:
  toc=0x200000-4096;mem[toc:toc+4]=b'$TOC';put(mem,toc+4,'II',1,1)
  part=0x3000;size=0x180000;put(mem,toc+32,'IIIHHI',part,size,0,0xfffe,0xfffe,64)
  mem[part:part+4]=b'$PRT';pos=32
  for i in range(prefix):
   put(mem,part+pos,'HHIHHI',0xa5a5,99,8,0,0,0);mem[part+pos+16:part+pos+24]=b'PRIVATE!';pos+=24
  put(mem,part+pos,'HHIHHI',0xa5a5,402,len(data),0,0,0);off=part+pos+16
  put(mem,part+4,'HHIII',0xfffe,0xfffe,size,pos+16+len(data),size)
 mem[off:off+len(data)]=data
 return mem,off

class Smem(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-smem-core-');cls.app=UEFI/'Platforms/Xiaomi/casPkg/Applications/CasDiag'
  out=Path(cls.temp.name)/'smem.so';cc=shutil.which('clang-18') or shutil.which('cc')
  if not cc:raise RuntimeError('actual C compiler required')
  subprocess.run([cc,'-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(cls.app/'SmemCore.c'),str(cls.app/'DiagCore.c'),'-o',str(out)],check=True,capture_output=True)
  cls.lib=c.CDLL(str(out));cls.lib.CasDiagSmemRead.argtypes=[READ,c.c_void_p,c.POINTER(Work),c.POINTER(Info)];cls.lib.CasDiagSmemRead.restype=c.c_int
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def run_reader(self,mem,mutator=None,fault=None):
  calls=[];bad=[];counts={};original=bytes(mem)
  @READ
  def read(ctx,off,length,out):
   calls.append((off,length));counts[off]=counts.get(off,0)+1
   if off>len(mem) or length>len(mem)-off:bad.append((off,length));return 1
   if off==fault:return 1
   data=bytearray(mem[off:off+length])
   if mutator:mutator(off,counts[off],data)
   c.memmove(out,bytes(data),length);return 0
  work=Work();info=Info();status=self.lib.CasDiagSmemRead(read,None,c.byref(work),c.byref(info))
  self.assertEqual(bad,[]);self.assertEqual(bytes(mem),original,'read-only implementation must not modify input')
  return status,info,calls
 def test_heap_11_12_and_table_1_2_3(self):
  for heap in (11,12):
   for version in (1,2,3):
    with self.subTest(heap=heap,version=version):
     mem,off=fixture(heap,version);s,i,calls=self.run_reader(mem);self.assertEqual(s,0);self.assertEqual((i.smem_version,i.table_version,i.count),(heap,version,2));self.assertEqual(i.offset,off);self.assertEqual(i.parts[0].base,0xc0000000);self.assertEqual(i.parts[1].available,0x180000000)
     self.assertEqual(calls[:len(calls)//2],calls[len(calls)//2:]);self.assertLess(i.trace_bytes,65536)
 def test_other_payload_and_cached_heap_are_never_read(self):
  mem,off=fixture();s,i,calls=self.run_reader(mem);self.assertEqual(s,0)
  self.assertNotIn((0x3000+48,8),calls)
  for at,n in calls:self.assertFalse(at<0x3000+56 and 0x3000+48<at+n)
 def test_unknown_heap_or_table_never_qualifies(self):
  for heap,table in ((10,2),(13,2),(12,0),(12,4)):
   mem,_=fixture(heap,table);self.assertEqual(self.run_reader(mem)[0],3)
 def test_invalid_initialization_and_reserved(self):
  for off,value in ((192,0),(192,2),(204,1)):
   mem,_=fixture();put(mem,off,'I',value);self.assertEqual(self.run_reader(mem)[0],2)
 def test_legacy_auxiliary_and_missing_allocation_refuse(self):
  for off,value,status in ((208+402*16,0,4),(208+402*16,2,2),(208+402*16+12,0x90000000,2)):
   mem,_=fixture(11);put(mem,off,'I',value);self.assertEqual(self.run_reader(mem)[0],status)
 def test_legacy_wrapped_outside_or_unaligned_allocation(self):
  for off,value in ((196,0xffffffff),(208+402*16+4,0xfffffff8),(208+402*16+4,0x4001),(208+402*16+8,0xfffffff8),(208+402*16+8,0)):
   mem,_=fixture(11);put(mem,off,'I',value);self.assertEqual(self.run_reader(mem)[0],5)
 def test_directory_bounds_magic_version_and_hosts(self):
  toc=0x200000-4096
  for off,fmt,value,status in ((toc,'I',0,2),(toc+4,'I',2,2),(toc+8,'I',0xffffffff,5),(toc+8,'I',0,5),(toc+32,'I',0xfffffff8,5),(toc+36,'I',0xffffffff,5),(toc+44,'H',1,4),(toc+48,'I',3,5)):
   mem,_=fixture();put(mem,off,fmt,value);self.assertEqual(self.run_reader(mem)[0],status)
 def test_duplicate_global_partition_refuses(self):
  mem,_=fixture();toc=0x200000-4096;put(mem,toc+8,'I',2);mem[toc+80:toc+128]=mem[toc+32:toc+80];self.assertEqual(self.run_reader(mem)[0],2)
 def test_partition_and_allocation_bounds(self):
  for off,fmt,value,status in ((0x3000,'I',0,2),(0x3004,'H',0,2),(0x3008,'I',64,2),(0x300c,'I',0xfffffff8,5),(0x3010,'I',0,5),(0x3020,'H',0,2),(0x3024,'I',0xfffffff8,5),(0x3028,'H',8,5),(0x302a,'H',0xfff8,5)):
   mem,_=fixture();put(mem,off,fmt,value);self.assertEqual(self.run_reader(mem)[0],status)
 def test_cached_only_missing_item_refuses(self):
  mem,_=fixture();put(mem,0x300c,'I',32);self.assertEqual(self.run_reader(mem)[0],4)
 def test_table_magics_count_lengths_and_versions(self):
  for delta,fmt,value,status in ((0,'I',0,2),(4,'I',0,2),(12,'I',1,2),(20,'I',1,2),(16,'I',0,5),(16,'I',33,5),(16,'I',3,5)):
   mem,off=fixture();put(mem,off+delta,fmt,value);self.assertEqual(self.run_reader(mem)[0],status)
 def test_all_truncated_table_allocations_refuse(self):
  for length in range(24,168,8):
   mem,off=fixture(11);put(mem,208+402*16+8,'I',length);self.assertEqual(self.run_reader(mem)[0],5)
 def test_partitions_overflow_overlap_available_and_alignment(self):
  for delta,value,status in ((24+16,0xfffffffffffff000,5),(24+24,0,5),(24+24,0xfffffffffffff000,5),(24+64,0x140001000,5),(24+64,1,5),(24+72+16,0xc0000000,2)):
   mem,off=fixture();put(mem,off+delta,'Q',value);self.assertEqual(self.run_reader(mem)[0],status)
 def test_metadata_and_item_changes_are_refused(self):
  for kind in ('version','item','header'):
   mem,off=fixture();target={'version':92,'item':off,'header':0x3000}[kind]
   def mutate(at,n,data):
    if at==target and n==2:data[0]^=1
   self.assertEqual(self.run_reader(mem,mutate)[0],6)
 def test_io_failure_never_returns_partial_success(self):
  mem,off=fixture()
  for fault in (92,192,0x200000-4096,0x3000,off):self.assertEqual(self.run_reader(mem,fault=fault)[0],1)
 def test_allocation_scan_has_a_hard_budget(self):
  mem,_=fixture(prefix=1300);s,i,calls=self.run_reader(mem);self.assertEqual(s,7);self.assertLessEqual(len(calls),1200)
 def test_unknown_or_zero_available_rows_are_observable_without_ownership(self):
  mem,_=fixture(rows=[(0x100000000,0x20000000,0,0xffffffff,99,0,99)])
  s,i,calls=self.run_reader(mem);self.assertEqual(s,0);self.assertEqual((i.parts[0].available,i.parts[0].category),(0,99))

class Mapping(unittest.TestCase):
 def test_actual_mapping_changes_only_reserved_smem_and_is_repeatable(self):
  with tempfile.TemporaryDirectory(prefix='cas-diag-map-') as tmp:
   tmp=Path(tmp);(tmp/'Library').mkdir()
   (tmp/'Library/MemoryMapLib.h').write_text("""
#include <stdint.h>
#define STATIC static
#define VOID void
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(A) (sizeof(A)/sizeof((A)[0]))
typedef uint8_t UINT8;typedef unsigned long UINTN;typedef int BOOLEAN;
enum {NoHob,AddMem,AddDev,HobOnlyNoCacheSetting};
enum {SYS_MEM,MEM_RES,MMAP_IO};
enum {SYS_MEM_CAP=31,UNCACHEABLE=1};
enum {Conv,BsData,Reserv,MmIO};
enum {ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP,ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED,ARM_MEMORY_REGION_ATTRIBUTE_DEVICE};
typedef struct {char Name[32];uint64_t Address,Length;int HobOption,ResourceType;uint64_t ResourceAttribute;int MemoryType,ArmAttributes;} EFI_MEMORY_REGION_DESCRIPTOR;
""")
   wrapper=UEFI/'Platforms/Xiaomi/casPkg/Library/MemoryMapDiagLib/MemoryMapDiagLib.c'
   harness=tmp/'mapping.c';harness.write_text('#include <string.h>\n#include <stdio.h>\n#include "'+str(wrapper)+'"\n'+r"""
int main(void) {
 EFI_MEMORY_REGION_DESCRIPTOR *Map,Before[128],Expected;UINT8 Count,Original;UINTN I,Changes=0;uint64_t Heap=0;
 CasDefaultGetMemoryMap(&Map,&Original);memcpy(Before,Map,Original*sizeof(*Map));
 GetMemoryMap(&Map,&Count);if(Count!=Original)return 1;
 for(I=0;I<Count;++I) {
  Expected=Before[I];
  if(Expected.Address==CAS_SMEM_BASE && Expected.Length==CAS_SMEM_SIZE) {Expected.HobOption=AddMem;++Changes;}
  if(memcmp(&Expected,&Map[I],sizeof(Expected)))return 2;
  if(Map[I].MemoryType==Conv)Heap+=Map[I].Length;
 }
 if(Changes!=1 || Heap!=0x8000000)return 3;
 GetMemoryMap(&Map,&Count);if(Count!=Original)return 4;
 mApplied=FALSE;
 for(I=0;I<Count;++I)if(Map[I].Address==CAS_SMEM_BASE) {Map[I].HobOption=HobOnlyNoCacheSetting;Map[I].MemoryType=Conv;}
 GetMemoryMap(&Map,&Count);if(Count!=0)return 5;
 puts("only reserved SMEM HOB option changed; repeat call stable; bad type refused; heap=128MiB");return 0;
}
""")
   cc=shutil.which('clang-18') or shutil.which('cc');out=tmp/'mapping'
   r=subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-DCAS_XNU_DIAGNOSTICS=1','-I'+str(tmp),'-I'+str(UEFI/'Platforms/Xiaomi/casPkg/Include'),str(harness),'-o',str(out)],text=True,capture_output=True)
   self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(out)],text=True,capture_output=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
if __name__=='__main__':unittest.main()
