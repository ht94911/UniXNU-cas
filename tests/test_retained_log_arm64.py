# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the production ARM64 clear helper and verify every16-byte log word."""
import contextlib,io,os,shutil,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];UEFI=Path(os.environ.get('CAS_UEFI_ROOT',str(ROOT.parent/'uefi-cas')))
sys.path.insert(0,str(ROOT/'scripts'));from build_shim import build
class RetainedLogTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not shutil.which('qemu-system-aarch64'):raise RuntimeError('QEMU is required')
  cls.tmp=tempfile.TemporaryDirectory(prefix='cas-real-retained-log-');cls.addClassCleanup(cls.tmp.cleanup);cls.root=Path(cls.tmp.name);cls.images={};fixture=ROOT/'tests/fixtures/arm64-retained-log'
  for diag in [False,True]:
   out=cls.root/('diag' if diag else 'normal')
   with contextlib.redirect_stdout(io.StringIO()):build(UEFI,out,diagnostics=diag)
   obj=out/'entry.o';elf=out/'probe.elf';flags=['-DCAS_XNU_DIAGNOSTICS=1'] if diag else []
   subprocess.run(['clang','--target=aarch64-none-elf',*flags,'-I',str(UEFI/'Platforms/Xiaomi/casPkg/Include'),'-c',str(fixture/'entry.S'),'-o',str(obj)],check=True,capture_output=True)
   subprocess.run(['ld.lld','-T',str(fixture/'probe.lds'),str(obj),str(out/'CasBootLog.o'),'-o',str(elf)],check=True,capture_output=True);cls.images[diag]=elf
 def execute(self,diag,el):
  p=subprocess.run(['qemu-system-aarch64','-nodefaults','-nic','none','-machine','virt,secure=off,virtualization='+('on' if el==2 else 'off'),'-cpu','cortex-a76','-m','2048M','-display','none','-serial','none','-monitor','none','-semihosting-config','enable=on,target=native','-device','loader,file='+str(self.images[diag])+',cpu-num=0','-no-reboot'],capture_output=True,text=True,timeout=15)
  self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('ACTUAL-LOG-CLEAR: PASS',p.stdout+p.stderr);self.assertNotIn('FAIL',p.stdout+p.stderr)
 def test_diagnostic_prefix_cleared_both_retained_slots_preserved(self):
  for el in [1,2]:
   with self.subTest(el=el):self.execute(True,el)
 def test_normal_profile_still_clears_the_entire_log(self):
  for el in [1,2]:
   with self.subTest(el=el):self.execute(False,el)
if __name__=='__main__':unittest.main()
