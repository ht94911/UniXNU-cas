# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Actual C checks for shifted-window boundaries and a retained allocation."""
import ctypes as c
from pathlib import Path
import shutil,subprocess,tempfile,unittest
class Range(c.Structure):_fields_=[('base',c.c_uint64),('size',c.c_uint64)]
class Result(c.Structure):_fields_=[('result',c.c_int),('cleanup',c.c_int),('release',c.c_int)]
Write=c.CFUNCTYPE(c.c_int,c.c_void_p,Range,c.c_uint32)
Simple=c.CFUNCTYPE(c.c_int,c.c_void_p,Range)
class Io(c.Structure):_fields_=[('write',Write),('cache',Simple),('read',Write),('clear',Simple)]
class Cpu0GuardTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-cpu0-core-');p=Path(cls.temp.name)
  app=Path(__file__).resolve().parents[2]/'uefi-cas/Platforms/Xiaomi/casPkg/Applications/CasDiag'
  cc=shutil.which('clang-18') or shutil.which('cc')
  if not cc:raise RuntimeError('native C compiler required')
  subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-shared','-fPIC',str(app/'DiagCore.c'),'-o',str(p/'core.so')],check=True,capture_output=True)
  cls.lib=c.CDLL(str(p/'core.so'));cls.lib.CasDiagCpu0Eligible.argtypes=[Range,c.POINTER(Range),c.c_uint32,c.POINTER(Range),c.c_uint32,c.POINTER(Range),c.c_uint32,c.POINTER(Range),c.c_uint32,c.c_uint32]
  cls.lib.CasDiagEligible.argtypes=cls.lib.CasDiagCpu0Eligible.argtypes
  cls.lib.CasDiagHeldCoverage.argtypes=[Range,c.POINTER(Range),c.c_uint32]
  cls.lib.CasDiagRunHeldProbe.argtypes=[Range,c.c_int,c.POINTER(Io),c.c_void_p];cls.lib.CasDiagRunHeldProbe.restype=Result
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def eligible(self,*,candidate=None,free=None,reserved=(),complete=15):
  bank=(Range*1)(Range(0x100000000,0x20000000));f=(Range*len(free))(*free) if free is not None else bank;r=(Range*len(reserved))(*reserved)
  return self.lib.CasDiagCpu0Eligible(candidate or Range(0x100004000,0x10000000),bank,1,bank,1,f,len(free) if free is not None else 1,r,len(reserved),complete)
 def test_exact_shifted_window_is_accepted(self):self.assertEqual(self.eligible(),0)
 def test_old_success_does_not_cover_new_tail(self):self.assertEqual(self.eligible(free=[Range(0x100000000,0x10000000)]),5)
 def test_tail_reservation_is_protected(self):self.assertEqual(self.eligible(reserved=[Range(0x110000000,0x4000)]),6)
 def test_candidate_is_a_strict_whitelist(self):
  for base,size in [(0x100000000,0x10000000),(0x100005000,0x10000000),(0x100004000,0xffff000),(0x100004000,0x10001000),(0xfffffffffffff000,0x10000000)]:
   with self.subTest(base=base,size=size):self.assertEqual(self.eligible(candidate=Range(base,size)),1)
 def test_incomplete_and_overlapping_observations_are_rejected(self):
  self.assertEqual(self.eligible(complete=14),2)
  self.assertEqual(self.eligible(free=[Range(0x100000000,0x20000000),Range(0x100004000,0x10000000)]),1)
 def test_legacy_gate_still_rejects_shifted_but_accepts_its_original_range(self):
  bank=(Range*1)(Range(0x100000000,0x20000000))
  for base,expected in [(0x100000000,0),(0x100004000,1)]:self.assertEqual(self.lib.CasDiagEligible(Range(base,0x10000000),bank,1,bank,1,bank,1,None,0,15),expected)
 def test_held_map_covers_contiguous_unsorted_splits(self):
  parts=(Range*2)(Range(0x108004000,0x8000000),Range(0x100004000,0x8000000))
  self.assertEqual(self.lib.CasDiagHeldCoverage(Range(0x100004000,0x10000000),parts,2),1)
 def test_held_map_gap_duplicate_and_missing_tail_are_rejected(self):
  cases=[[Range(0x100000000,0x10000000)],[Range(0x100004000,0x8000000),Range(0x108005000,0x7fff000)],[Range(0x100004000,0x10000000)]*2]
  for parts in cases:self.assertEqual(self.lib.CasDiagHeldCoverage(Range(0x100004000,0x10000000),(Range*len(parts))(*parts),len(parts)),0)
 def probe(self,*,gate=0,write_fail=-1,cache_fail=-1,read_fail=-1,clear_fail=False):
  calls=[]
  def write(_,r,p):calls.append(('write',p));return p==write_fail
  def read(_,r,p):calls.append(('read',p));return p==read_fail
  def cache(_,r):n=sum(x[0]=='cache' for x in calls);calls.append(('cache',n));return n==cache_fail
  def clear(_,r):calls.append(('clear',0));return clear_fail
  funcs=[Write(write),Simple(cache),Write(read),Simple(clear)];io=Io(*funcs)
  result=self.lib.CasDiagRunHeldProbe(Range(0x100004000,0x10000000),gate,c.byref(io),None);return result,calls
 def test_refused_probe_never_touches_memory(self):r,calls=self.probe(gate=5);self.assertEqual(r.result,1);self.assertEqual(calls,[])
 def test_held_probe_has_no_allocation_or_release(self):
  r,calls=self.probe();self.assertEqual((r.result,r.cleanup,r.release),(0,0,0));self.assertEqual(calls,[('write',0),('cache',0),('read',0),('write',1),('cache',1),('read',1),('clear',0),('cache',2)])
 def test_read_failure_still_clears_and_caches(self):r,calls=self.probe(read_fail=1);self.assertEqual(r.result,6);self.assertEqual(calls[-2:],[('clear',0),('cache',2)])
 def test_write_and_cache_failures_still_clean(self):
  for options,expected in [({'write_fail':0},4),({'cache_fail':0},5)]:
   r,calls=self.probe(**options);self.assertEqual(r.result,expected);self.assertEqual(calls[-2][0],'clear');self.assertEqual(calls[-1][0],'cache')
 def test_clear_or_final_cache_failure_cannot_pass(self):
  for options in [{'clear_fail':True},{'cache_fail':2}]:r,calls=self.probe(**options);self.assertEqual((r.result,r.cleanup),(7,1))
if __name__=='__main__':unittest.main()
