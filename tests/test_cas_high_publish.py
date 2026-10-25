"""Actual C admission and EFI publication with synthetic fault-injected services.

No phone, physical RAM, page tables, or real GCD is accessed by these tests.
"""
import ctypes as c
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
UEFI=Path(os.environ.get('CAS_UEFI_ROOT',ROOT.parent/'uefi-cas'))
APP=UEFI/'Platforms/Xiaomi/casPkg/Applications/CasDiag'
class Range(c.Structure):_fields_=[('base',c.c_uint64),('size',c.c_uint64)]

class Admission(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-publish-admission-');cls.addClassCleanup(cls.temp.cleanup)
  lib=Path(cls.temp.name)/'core.so';cc=shutil.which('clang-18') or shutil.which('cc')
  subprocess.run([cc,'-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(APP/'DiagCore.c'),'-o',str(lib)],check=True,capture_output=True)
  cls.lib=c.CDLL(str(lib));cls.lib.CasDiagPublishEligible.argtypes=[Range]+[t for _ in range(3) for t in (c.POINTER(Range),c.c_uint)]+[c.c_uint]
  cls.lib.CasDiagPublishEligible.restype=c.c_int
 def gate(self,banks=None,smem=None,reserved=None,mask=15,candidate=(0x100000000,0x20000000)):
  values=[banks if banks is not None else [(0xc0000000,0x140000000)],smem if smem is not None else [(0xc0000000,0x140000000)],reserved if reserved is not None else [(0,0x100000000)]]
  args=[Range(*candidate)]
  for rows in values:args.extend(((Range*len(rows))(*(Range(*r) for r in rows)),len(rows)))
  return self.lib.CasDiagPublishEligible(*args,mask)
 def test_full_fixed_window_admitted_without_claiming_ownership(self):self.assertEqual(self.gate(),0)
 def test_every_incomplete_or_extra_mask_rejects(self):
  for mask in list(range(15))+[16,31,0xffffffff]:self.assertEqual(self.gate(mask=mask),2)
 def test_missing_or_fragmented_provenance_rejects(self):
  self.assertEqual(self.gate(banks=[]),3);self.assertEqual(self.gate(smem=[]),4)
  self.assertEqual(self.gate(banks=[(0x100000000,0x10000000),(0x110000000,0x10000000)]),3)
  self.assertEqual(self.gate(smem=[(0x100000000,0x10000000),(0x110000000,0x10000000)]),4)
 def test_any_protected_byte_inside_the_full_512mib_rejects(self):
  for base in [0x100000000,0x10fffffff,0x110000000,0x11fffffff]:
   with self.subTest(base=base):self.assertEqual(self.gate(reserved=[(base,1)]),6)
  self.assertEqual(self.gate(reserved=[(0x120000000,4096)]),0)
 def test_wrap_zero_and_overlapping_metadata_reject(self):
  self.assertEqual(self.gate(banks=[(0xfffffffffffff000,0x2000)]),1)
  self.assertEqual(self.gate(smem=[(0x100000000,0x30000000),(0x110000000,0x10000000)]),1)
  self.assertEqual(self.gate(reserved=[(0,0)]),1)
 def test_only_exact_platform_window_is_admitted(self):
  for candidate in [(0xc0000000,0x20000000),(0x100000000,0x10000000),(0x100000000,0x40000000),(0x100200000,0x20000000),(0xffffffffff000000,0x20000000)]:self.assertEqual(self.gate(candidate=candidate),1)

HARNESS=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "DiagCore.h"
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;typedef uint8_t UINT8;typedef uint32_t UINT32;
typedef uint64_t UINT64;typedef size_t UINTN;typedef long INTN;
typedef uint64_t EFI_STATUS;
#define EFI_ERROR(x) ((x)>>63)
#define ERR (1ULL<<63)
#define EFI_MEMORY_WB 8ULL
#define EFI_MEMORY_XP 0x4000ULL
#define EFI_MEMORY_DESCRIPTOR_VERSION 1
#define CAS_DIAG_HIGH_BASE CD_PUBLISH_BASE
#define CAS_DIAG_HIGH_SIZE CD_PUBLISH_SIZE
enum {EfiGcdMemoryTypeNonExistent=0,EfiGcdMemoryTypeSystemMemory=2};
typedef struct {UINT32 Type;UINT64 PhysicalStart,VirtualStart,NumberOfPages,Attribute;} EFI_MEMORY_DESCRIPTOR;
typedef struct {UINT64 BaseAddress,Length,Capabilities,Attributes;UINT32 GcdMemoryType;void *ImageHandle,*DeviceHandle;} EFI_GCD_MEMORY_SPACE_DESCRIPTOR;
typedef struct Protocol Protocol;
struct Protocol {EFI_STATUS (*GetMemoryAttributes)(Protocol *,UINT64,UINT64,UINT64 *);};
typedef Protocol EFI_MEMORY_ATTRIBUTE_PROTOCOL;
static CD_RANGE mBanks[128],mSmem[128],mFree[128],mReserved[128];
static unsigned mBankCount,mSmemCount,mFreeCount,mReservedCount,mComplete,mFlags;
static BOOLEAN mDropped,mPublishAttempted,mPublishFailed;
static UINT64 mHighMap[0x10000/sizeof(UINT64)];
static unsigned mode,added,publishes,enumerates,map_queries,permissions,gcd_queries,accesses;
static int gEfiMemoryAttributeProtocolGuid;
static void Line(const char *f,...) {(void)f;}
static void Persist(void) {}
static void ZeroMem(void *p,UINTN n) {memset(p,0,n);}
static void Enumerate(void) {
 enumerates++;mComplete=(mode&1)?13:15;mDropped=(mode&8192)!=0;
 mBankCount=mSmemCount=mReservedCount=1;mFreeCount=0;
 mBanks[0]=(CD_RANGE){0xc0000000,0x140000000};mSmem[0]=mBanks[0];
 mReserved[0]=(CD_RANGE){0,0x100000000ULL};
}
static BOOLEAN UefiFacts(BOOLEAN verbose) {
 (void)verbose;mFreeCount=(mode&256)?0:1;
 mFree[0]=(CD_RANGE){CD_PUBLISH_BASE,CD_PUBLISH_SIZE};return 1;
}
static EFI_STATUS Attr(Protocol *p,UINT64 base,UINT64 size,UINT64 *out) {
 (void)p;permissions++;
 if(base!=CD_PUBLISH_BASE || (size!=CD_PUBLISH_SIZE && size!=CD_REQUEST))accesses++;
 *out=((!added && (mode&16)) || (added && (mode&128)))?0:EFI_MEMORY_XP;
 return mode&16384?ERR:0;
}
static Protocol protocol={Attr};
static EFI_STATUS Locate(void *g,void *r,void **out) {(void)g;(void)r;*out=&protocol;return 0;}
static EFI_STATUS Map(UINTN *size,void *buffer,UINTN *key,UINTN *stride,UINT32 *version) {
 EFI_MEMORY_DESCRIPTOR d={7,0x94000000,0,0x8000,EFI_MEMORY_WB};map_queries++;
 if(mode&8)d.PhysicalStart=CD_PUBLISH_BASE;
 if(mode&1024)d.NumberOfPages=~0ULL;
 memcpy(buffer,&d,sizeof(d));*size=sizeof(d);*stride=sizeof(d);*key=1;*version=1;
 if(mode&32768)*size=0x10001;
 if(mode&65536)*stride=0;
 return 0;
}
static struct {EFI_STATUS (*LocateProtocol)(void *,void *,void **);EFI_STATUS (*GetMemoryMap)(UINTN *,void *,UINTN *,UINTN *,UINT32 *);} boot={Locate,Map},*gBS=&boot;
static EFI_STATUS Gcd(UINT64 address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *out) {
 gcd_queries++;out->BaseAddress=CD_PUBLISH_BASE;out->Length=CD_PUBLISH_SIZE;
 if(address!=CD_PUBLISH_BASE)accesses++;
 if(mode&2)return ERR;
 if(added || (mode&4)) {
  out->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;out->ImageHandle=(void *)1;
  out->Capabilities=EFI_MEMORY_WB|EFI_MEMORY_XP;
 }
 if(added && (mode&64))out->Length=CD_REQUEST;
 if(mode&4096)out->DeviceHandle=(void *)1;
 return 0;
}
static EFI_STATUS Add(UINT32 type,UINT64 base,UINT64 size,UINT64 caps) {
 publishes++;added=1;
 if(type!=EfiGcdMemoryTypeSystemMemory || base!=CD_PUBLISH_BASE || size!=CD_PUBLISH_SIZE || caps!=(EFI_MEMORY_WB|EFI_MEMORY_XP))accesses++;
 return mode&32?ERR:0;
}
static struct {EFI_STATUS (*GetMemorySpaceDescriptor)(UINT64,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *);EFI_STATUS (*AddMemorySpace)(UINT32,UINT64,UINT64,UINT64);} dxe={Gcd,Add},*gDS=&dxe;
static int ProbeAllocate(void *ctx,CD_RANGE r,CD_U64 *a) {(void)ctx;(void)r;(void)a;accesses++;return 1;}
static int ProbeWrite(void *ctx,CD_RANGE r,CD_U32 p) {(void)ctx;(void)r;(void)p;accesses++;return 1;}
static int ProbeRead(void *ctx,CD_RANGE r,CD_U32 p) {(void)ctx;(void)r;(void)p;accesses++;return 1;}
static int ProbeCache(void *ctx,CD_RANGE r) {(void)ctx;(void)r;accesses++;return 1;}
static int ProbeClear(void *ctx,CD_RANGE r) {(void)ctx;(void)r;accesses++;return 1;}
static int ProbeFree(void *ctx,CD_RANGE r) {(void)ctx;(void)r;accesses++;return 1;}
'''
WRAPPER=r'''
int run(unsigned flags,unsigned repeats,int probe_after) {
 mode=flags;added=publishes=enumerates=map_queries=permissions=gcd_queries=accesses=0;
 mPublishAttempted=mPublishFailed=mDropped=0;mFlags=0;gDS=(mode&2048)?NULL:&dxe;
 while(repeats--)PublishHigh();
 if(probe_after)Probe();
 return mPublishFailed;
}
unsigned count(unsigned which) {
 const unsigned all[]={publishes,enumerates,map_queries,permissions,gcd_queries,accesses,mPublishAttempted};return all[which];
}
'''
class EfiPublication(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-efi-publication-');cls.addClassCleanup(cls.temp.cleanup)
  source=(APP/'CasDiag.c').read_text()
  helpers=source.split('// These helpers query descriptors and permissions, never target RAM data.\n',1)[1].split('STATIC int ProbeAllocate(',1)[0]
  probe='STATIC VOID Probe(VOID) {'+source.split('STATIC VOID Probe(VOID) {',1)[1].split('STATIC EFI_INPUT_KEY Key(',1)[0]
  code=Path(cls.temp.name)/'publication.c';code.write_text(HARNESS+helpers+probe+WRAPPER)
  cc=shutil.which('clang-18') or shutil.which('cc');lib=code.with_suffix('.so')
  subprocess.run([cc,'-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(APP),str(code),str(APP/'DiagCore.c'),'-o',str(lib)],check=True,capture_output=True)
  cls.lib=c.CDLL(str(lib));cls.lib.run.argtypes=[c.c_uint,c.c_uint,c.c_int];cls.lib.run.restype=c.c_int;cls.lib.count.argtypes=[c.c_uint];cls.lib.count.restype=c.c_uint
 def invoke(self,flags=0,repeats=1,probe=False):
  failed=self.lib.run(flags,repeats,probe);return failed,tuple(self.lib.count(i) for i in range(7))
 def test_success_is_single_publication_not_probe_access(self):
  failed,counts=self.invoke();self.assertEqual(failed,0);self.assertEqual(counts[0],1);self.assertEqual(counts[1],1);self.assertEqual(counts[5],0);self.assertEqual(counts[6],1)
 def test_incomplete_facts_gcd_conflict_map_overlap_permissions_refuse_before_add(self):
  for flags in [1,2,4,8,16,1024,2048,4096,8192,16384,32768,65536]:
   with self.subTest(flags=flags):
    failed,counts=self.invoke(flags);self.assertEqual(failed,1);self.assertEqual(counts[0],0);self.assertEqual(counts[5],0)
 def test_failed_or_partially_effective_add_never_permits_probe(self):
  for flags in [32,64,128,256]:
   with self.subTest(flags=flags):
    failed,counts=self.invoke(flags,probe=True);self.assertEqual(failed,1);self.assertEqual(counts[0],1);self.assertEqual(counts[1],1);self.assertEqual(counts[5],0)
 def test_repeating_menu_action_never_repeats_publication(self):
  for flags in [0,1,32,64,256]:
   with self.subTest(flags=flags):
    one=self.invoke(flags);two=self.invoke(flags,repeats=2);self.assertEqual(two,one)
 def test_no_publication_call_before_gcd_uefi_and_permissions_checks(self):
  failed,counts=self.invoke(2);self.assertEqual(counts[2:4],(0,0));self.assertEqual(counts[0],0)

if __name__=='__main__':unittest.main()
