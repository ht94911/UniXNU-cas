"""Actual portable C guard/log/rollback tests; no phone or physical memory use."""
import ctypes as c
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cas_diag_log import decode
from make_ram_fat import validate_config
UEFI=Path(os.environ.get('CAS_UEFI_ROOT',ROOT.parent/'uefi-cas'))

class Range(c.Structure):_fields_=[('base',c.c_ulonglong),('size',c.c_ulonglong)]
class Log(c.Structure):_fields_=[('length',c.c_uint),('flags',c.c_uint),('sequence',c.c_ulonglong),('boot',c.c_ulonglong),('build',c.c_ubyte*32)]
class Result(c.Structure):_fields_=[('result',c.c_int),('cleanup',c.c_int),('release',c.c_int)]
Alloc=c.CFUNCTYPE(c.c_int,c.c_void_p,Range,c.POINTER(c.c_ulonglong))
Phase=c.CFUNCTYPE(c.c_int,c.c_void_p,Range,c.c_uint)
Step=c.CFUNCTYPE(c.c_int,c.c_void_p,Range)
class Io(c.Structure):_fields_=[('allocate',Alloc),('write',Phase),('cache',Step),('read',Phase),('clear',Step),('free',Step)]

class Core(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cas-diag-core-')
  source=UEFI/'Platforms/Xiaomi/casPkg/Applications/CasDiag/DiagCore.c'
  compiler=shutil.which('clang-18') or shutil.which('cc')
  if not compiler:raise RuntimeError('actual C compiler required')
  lib=Path(cls.temp.name)/'core.so'
  subprocess.run([compiler,'-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(lib)],check=True,capture_output=True)
  cls.lib=c.CDLL(str(lib))
  cls.lib.CasDiagRangeValid.argtypes=[Range,c.c_ulonglong];cls.lib.CasDiagRangeValid.restype=c.c_int
  cls.lib.CasDiagEligible.argtypes=[Range]+[t for _ in range(4) for t in (c.POINTER(Range),c.c_uint)]+[c.c_uint];cls.lib.CasDiagEligible.restype=c.c_int
  cls.lib.CasDiagPattern.argtypes=[c.c_ulonglong,c.c_uint];cls.lib.CasDiagPattern.restype=c.c_ulonglong
  cls.lib.CasDiagRunProbe.argtypes=[Range,c.c_int,c.POINTER(Io),c.c_void_p];cls.lib.CasDiagRunProbe.restype=Result
  cls.lib.CasDiagLogEncode.argtypes=[c.c_void_p,c.c_uint,c.c_void_p,c.c_uint,c.c_ulonglong,c.c_ulonglong,c.c_uint,c.c_char_p];cls.lib.CasDiagLogEncode.restype=c.c_uint
  cls.lib.CasDiagLogValidate.argtypes=[c.c_void_p,c.c_uint,c.c_char_p,c.POINTER(Log)];cls.lib.CasDiagLogValidate.restype=c.c_int
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def gate(self,banks=None,smem=None,free=None,reserved=None,complete=15,candidate=(0x100000000,0x10000000)):
  entries=[banks if banks is not None else [(0xc0000000,0x140000000)],smem if smem is not None else [(0x100000000,0x20000000)],free if free is not None else [(0x100000000,0x20000000)],reserved if reserved is not None else [(0,0x100000000)]]
  args=[Range(*candidate)]
  for values in entries:args.extend(((Range*len(values))(*(Range(*v) for v in values)),len(values)))
  return self.lib.CasDiagEligible(*args,complete)
 def test_live_agreement_allows_only_one_owned_interval(self):self.assertEqual(self.gate(),0)
 def test_every_missing_live_source_refuses(self):
  for mask in range(15):
   with self.subTest(mask=mask):self.assertEqual(self.gate(complete=mask),2)
 def test_history_alone_and_gaps_do_not_qualify(self):
  self.assertEqual(self.gate(smem=[]),4);self.assertEqual(self.gate(free=[]),5)
  self.assertEqual(self.gate(banks=[]),3)
  self.assertEqual(self.gate(banks=[(0x100000000,0x8000000),(0x110000000,0x10000000)]),3)
 def test_overlapping_and_wrapped_input_maps_are_rejected(self):
  for kwargs in [{'banks':[(0x100000000,0x20000000),(0x110000000,0x10000000)]},{'smem':[(0xfffffffffffff000,0x2000)]},{'free':[(0x100000000,0)]}]:self.assertEqual(self.gate(**kwargs),1)
 def test_candidate_width_alignment_and_size(self):
  for value in [(0x94000000,0x10000000),(0x100000001,0x10000000),(0x100000000,0x8000000),(0xffffffffff000000,0x10000000)]:self.assertEqual(self.gate(candidate=value),1)
 def test_protected_interval_edges_are_half_open(self):
  self.assertEqual(self.gate(reserved=[(0x110000000,4096)]),0)
  self.assertEqual(self.gate(reserved=[(0xffffffff,2)]),6)
  self.assertEqual(self.gate(reserved=[(0x10fffffff,1)]),6)
 def test_invalid_range_alignment_zero_and_overflow(self):
  for value,align in [((0,0),4096),((1,4096),4096),((0,4096),3),((0xfffffffffffff000,4096),4096),((0,1),0)]:self.assertFalse(self.lib.CasDiagRangeValid(Range(*value),align))
 def transaction(self,fault=None,gate=0,actual=0x100000000):
  events=[];counts={}
  def event(name):events.append(name);return int(name==fault)
  @Alloc
  def alloc(ctx,r,out):out[0]=actual;return event('allocate')
  @Phase
  def write(ctx,r,phase):return event('write'+str(phase))
  @Step
  def cache(ctx,r):counts['cache']=counts.get('cache',0)+1;return event('cache'+str(counts['cache']))
  @Phase
  def read(ctx,r,phase):return event('read'+str(phase))
  @Step
  def clear(ctx,r):return event('clear')
  @Step
  def free(ctx,r):return event('free')
  io=Io(alloc,write,cache,read,clear,free)
  return self.lib.CasDiagRunProbe(Range(0x100000000,0x10000000),gate,c.byref(io),None),events
 def test_denied_probe_has_no_allocation_or_access(self):
  result,events=self.transaction(gate=2);self.assertEqual(result.result,1);self.assertEqual(events,[])
 def test_allocation_failure_has_no_access_or_free(self):
  result,events=self.transaction('allocate');self.assertEqual(result.result,2);self.assertEqual(events,['allocate'])
 def test_unexpected_allocation_address_frees_without_access(self):
  result,events=self.transaction(actual=0x120000000);self.assertEqual(result.result,3);self.assertEqual(events,['allocate','free'])
 def test_success_orders_cache_before_read_and_clears_before_release(self):
  result,events=self.transaction();self.assertEqual((result.result,result.cleanup,result.release),(0,0,0));self.assertEqual(events,['allocate','write0','cache1','read0','write1','cache2','read1','clear','cache3','free'])
 def test_write_cache_and_read_failures_take_cleanup(self):
  for fault,stage in [('write0',4),('cache1',5),('read0',6),('write1',4),('cache2',5),('read1',6)]:
   with self.subTest(fault=fault):
    result,events=self.transaction(fault);self.assertEqual(result.result,stage);self.assertEqual(events[-1],'free');self.assertIn('clear',events);self.assertLess(events.index('clear'),len(events)-1)
 def test_cleanup_and_release_failure_cannot_pass(self):
  for fault in ['clear','cache3','free']:
   result,events=self.transaction(fault);self.assertNotEqual(result.result,0);self.assertEqual(events[-1],'free')
 def test_wrong_address_release_failure_is_reported(self):
  result,events=self.transaction('free',actual=0x120000000);self.assertEqual((result.result,result.release),(3,1));self.assertEqual(events,['allocate','free'])
 def test_pattern_uses_full_physical_address_and_complement(self):
  a=self.lib.CasDiagPattern(0x100000000,0);b=self.lib.CasDiagPattern(0,0)
  self.assertNotEqual(a,b);self.assertEqual(a^self.lib.CasDiagPattern(0x100000000,1),2**64-1)
 def encoded(self,payload=b'ENUM facts\nRETURN\n',flags=0):
  out=c.create_string_buffer(65536);n=self.lib.CasDiagLogEncode(out,len(out),payload,len(payload),4,123,flags,b'cas-diag-001');self.assertGreater(n,0);return out.raw[:n]
 def ccheck(self,raw,build=b'cas-diag-001'):
  info=Log();status=self.lib.CasDiagLogValidate(raw,len(raw),build,c.byref(info));return status,info
 def test_real_c_record_decodes_with_independent_python_reader(self):
  raw=self.encoded(flags=1);report=decode(raw,current_boot=123)
  self.assertEqual(report['payload'],'ENUM facts\nRETURN\n');self.assertEqual(report['sequence'],4);self.assertEqual(report['boot_relation'],'same_boot');self.assertFalse(report['persistence_verified']);self.assertTrue(report['warm_reset_requested'])
 def test_hand_encoded_python_fixture_decodes_in_c(self):
  payload=b'HOST ONLY\n';raw=bytearray(96+len(payload));raw[:8]=b'CASLOG01';struct.pack_into('<IIIIQQII',raw,8,1,96,len(raw),len(payload),7,456,0,0);raw[48:60]=b'cas-diag-001\0';raw[96:]=payload;struct.pack_into('<I',raw,44,zlib.crc32(raw))
  status,info=self.ccheck(bytes(raw));self.assertEqual((status,info.length,info.sequence,info.boot),(0,len(payload),7,456))
 def test_every_truncation_is_rejected_by_both_readers(self):
  raw=self.encoded()
  for length in range(len(raw)):
   with self.subTest(length=length):
    self.assertNotEqual(self.ccheck(raw[:length])[0],0)
    with self.assertRaises(ValueError):decode(raw[:length])
 def test_bit_flips_never_validate(self):
  original=self.encoded()
  for offset in range(len(original)):
   raw=bytearray(original);raw[offset]^=1
   with self.subTest(offset=offset):
    self.assertNotEqual(self.ccheck(bytes(raw))[0],0)
    with self.assertRaises(ValueError):decode(bytes(raw))
 def test_foreign_build_and_prior_boot_are_explicit(self):
  raw=self.encoded();status,_=self.ccheck(raw,b'cas-diag-002');self.assertEqual(status,5)
  report=decode(raw,'cas-diag-002',999);self.assertEqual(report['result'],'foreign_build');self.assertEqual(report['boot_relation'],'prior_boot_candidate');self.assertFalse(report['persistence_verified'])
 def test_header_lengths_versions_and_reserved_bytes_rejected_even_with_new_crc(self):
  original=self.encoded()
  for offset,value in [(8,2),(12,80),(16,65537),(20,0xffffffff),(40,8),(80,1)]:
   raw=bytearray(original);struct.pack_into('<I',raw,offset,value);struct.pack_into('<I',raw,44,0);struct.pack_into('<I',raw,44,zlib.crc32(raw))
   self.assertNotEqual(self.ccheck(bytes(raw))[0],0)
   with self.assertRaises(ValueError):decode(bytes(raw))
 def test_zero_stamps_non_ascii_and_overflow_not_encoded(self):
  out=c.create_string_buffer(65536)
  for payload,seq,boot,flags,build in [(b'a',0,1,0,b'cas-diag-001'),(b'a',1,0,0,b'cas-diag-001'),(b'\xff',1,1,0,b'cas-diag-001'),(b'a',1,1,8,b'cas-diag-001'),(b'a',1,1,0,b'x'*32),(b'a'*65536,1,1,0,b'cas-diag-001')]:self.assertEqual(self.lib.CasDiagLogEncode(out,65536,payload,len(payload),seq,boot,flags,build),0)
 def test_cli_reads_saved_record_without_changing_it(self):
  p=Path(self.temp.name)/'saved-record.bin';raw=self.encoded();p.write_bytes(raw)
  r=subprocess.run([sys.executable,str(ROOT/'scripts/read-cas-diag-log.py'),str(p),'--current-boot','123'],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads(r.stdout)['boot_relation'],'same_boot');self.assertEqual(p.read_bytes(),raw)
 def test_diagnostic_fat_only_admits_its_single_efi_entry(self):
  config=(ROOT/'config/diagnostics.toml').read_bytes();self.assertEqual(validate_config(config,diagnostics=True)['boot']['default'],'diag')
  with self.assertRaises(ValueError):validate_config(config)
  with self.assertRaises(ValueError):validate_config(config.replace(b'CasDiag.efi',b'kernel.efi'),diagnostics=True)
if __name__=='__main__':unittest.main()
