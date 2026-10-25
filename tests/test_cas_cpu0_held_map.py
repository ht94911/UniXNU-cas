# SPDX-License-Identifier: BSD-2-Clause-Patent
# Run the actual held-descriptor check with malformed/current-map fixtures.
import ctypes as c
from pathlib import Path
import subprocess,tempfile,unittest
class HeldMapTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-held-descriptors-');tmp=Path(cls.temp.name);root=Path(__file__).resolve().parents[1];work=root.parent
  text=(root/'package/CasCpu0/Guard.c').read_text();body='STATIC BOOLEAN HeldDescriptor('+text.split('STATIC BOOLEAN HeldDescriptor(',1)[1].split('EFI_STATUS CasCpu0ProbeHeld(',1)[0]
  code='''#include <Uefi.h>
#include "DiagCore.h"
static UINT64 mHighMap[8192];static unsigned Mode;
static EFI_STATUS EFIAPI GetMap(UINTN *N,EFI_MEMORY_DESCRIPTOR *P,UINTN *K,UINTN *S,UINT32 *V){
 UINTN Cap=*N;*K=1;*S=sizeof(*P);*V=EFI_MEMORY_DESCRIPTOR_VERSION;*N=sizeof(*P);
 if(Mode==10){*S=1;return EFI_SUCCESS;}if(Mode==11){*N=Cap+1;return EFI_SUCCESS;}
 if(Mode==12){*V=99;return EFI_SUCCESS;}
 P[0]=(EFI_MEMORY_DESCRIPTOR){EfiLoaderCode,0x100004000ULL,0,0x10000,EFI_MEMORY_WB|EFI_MEMORY_XP};
 if(Mode==1)P[0].Attribute=EFI_MEMORY_WB;
 if(Mode==2)P[0].Type=EfiLoaderData;
 if(Mode==3)P[0].Attribute|=EFI_MEMORY_RUNTIME;
 if(Mode==4)P[0].Attribute|=EFI_MEMORY_RO;
 if(Mode==5)P[0].Attribute|=EFI_MEMORY_RP;
 if(Mode==6)P[0].Attribute=EFI_MEMORY_XP;
 if(Mode==7){*N=2*sizeof(*P);P[1]=P[0];}
 if(Mode==8){*N=2*sizeof(*P);P[0].NumberOfPages=0x8000;P[1]=P[0];P[1].PhysicalStart+=0x8001000ULL;P[1].NumberOfPages=0x7fff;}
 if(Mode==9)P[0].NumberOfPages=~0ULL;
 if(Mode==13){P[0].PhysicalStart=0x100000000ULL;}
 if(Mode==14){*N=2*sizeof(*P);P[0].NumberOfPages=0x8000;P[1]=P[0];P[1].PhysicalStart+=0x8000000ULL;}
 return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Services;EFI_BOOT_SERVICES *gBS=&Services;
'''+body+'''int test(unsigned M){Mode=M;Services.GetMemoryMap=GetMap;return HeldDescriptor((CD_RANGE){CD_CPU0_BASE,CD_REQUEST});}
'''
  (tmp/'test.c').write_text(code);app=work/'uefi-cas/Platforms/Xiaomi/casPkg/Applications/CasDiag';inc=work/'uefi-cas/Mu_Basecore/MdePkg/Include'
  built=subprocess.run(['clang-18','-std=c11','-fshort-wchar','-shared','-fPIC','-O2','-Werror','-I'+str(app),'-I'+str(inc),'-I'+str(inc/'X64'),str(tmp/'test.c'),str(app/'DiagCore.c'),'-o',str(tmp/'map.so')],capture_output=True,text=True)
  if built.returncode:raise RuntimeError(built.stderr)
  cls.lib=c.CDLL(str(tmp/'map.so'));cls.lib.test.argtypes=[c.c_uint];cls.lib.test.restype=c.c_int
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def test_loadercode_WB_XP_or_executable_and_contiguous_splits(self):
  for mode in [0,1,14]:self.assertEqual(self.lib.test(mode),1,mode)
 def test_wrong_type_runtime_RO_RP_or_missing_WB_never_pass(self):
  for mode in [2,3,4,5,6]:self.assertEqual(self.lib.test(mode),0,mode)
 def test_duplicates_gaps_overflow_stride_size_version_tail_refused(self):
  for mode in [7,8,9,10,11,12,13]:self.assertEqual(self.lib.test(mode),0,mode)
if __name__=='__main__':unittest.main()
