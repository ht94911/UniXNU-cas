"""Actual C: Boot1 carveout/default-domain evidence, never allocation ownership."""
import ctypes as c
from pathlib import Path
import shutil
import subprocess
import test_cas_smem_reject as reject
from test_cas_smem import UEFI, Work, Info, Part, READ, fixture, put

class Range(c.Structure):
    _fields_=[('base',c.c_ulonglong),('size',c.c_ulonglong)]

def rows():
    # Critical observed labels/ranges only. Available values, SRAM rows and the
    # sixth bank are SYNTHETIC; the photos did not provide a full decoded table.
    return [(0x80000000,0x40000000,0x39000000,1,14,0,1),
            (0x200000000,0x180000000,0x180000000,1,14,0,1),
            (0x1480000,0x18000,0x18000,1,4,0,1),
            (0x14680000,0x4000,0x4000,1,5,0,1),
            (0x80060000,0x1f000,0x1f000,1,14,0,2),
            (0xc0000000,0x140000000,0x140000000,1,14,0,1)]

class RowRoles(reject.unittest.TestCase):
    observe=reject.RejectTelemetry.observe
    @classmethod
    def setUpClass(cls):
        reject.RejectTelemetry.setUpClass.__func__(cls)
        app=UEFI/'Platforms/Xiaomi/casPkg/Applications/CasDiag'
        source=Path(cls.temp.name)/'diag011.c'
        source.write_bytes(subprocess.check_output(['git','-C',str(UEFI),'show',
          '6d66c536ec532855aa452e83ff1fb8fdea720587:Platforms/Xiaomi/casPkg/Applications/CasDiag/SmemCore.c']))
        out=Path(cls.temp.name)/'diag011.so'
        subprocess.run([shutil.which('clang-18') or shutil.which('cc'),'-std=c11','-shared','-fPIC','-O2',
          '-Wall','-Wextra','-Werror','-I'+str(app),str(source),str(app/'DiagCore.c'),'-o',str(out)],
          check=True,capture_output=True)
        cls.previous=c.CDLL(str(out))
        cls.previous.CasDiagSmemRead.argtypes=[READ,c.c_void_p,c.POINTER(Work),c.POINTER(Info)]
        cls.previous.CasDiagSmemRead.restype=c.c_int
        cls.libraries[0].CasDiagSmemSystemPart.argtypes=[c.POINTER(Part)]
        cls.libraries[0].CasDiagSmemSystemPart.restype=c.c_int
        cls.libraries[0].CasDiagEligible.argtypes=[Range,c.POINTER(Range),c.c_uint,
          c.POINTER(Range),c.c_uint,c.POINTER(Range),c.c_uint,c.POINTER(Range),c.c_uint,c.c_uint]
        cls.libraries[0].CasDiagEligible.restype=c.c_int

    def system(self,part):
        return self.libraries[0].CasDiagSmemSystemPart(c.byref(part))

    def test_observed_boot1_relationship_accepts_synthetic_table_in_two_equal_passes(self):
        for heap in (11,12):
            for version in (1,2,3):
                code,work,info,calls=self.observe(fixture(heap,version,rows=rows())[0])
                self.assertEqual((code,work.check,info.count,work.parsed_rows),(0,0,6,6))
                self.assertEqual((work.overlap_a,work.overlap_b),(32,32))
                self.assertEqual([self.system(info.parts[i]) for i in range(6)],[1,1,0,0,0,1])
                self.assertEqual(calls[:len(calls)//2],calls[len(calls)//2:])
                self.assertEqual((info.parts[4].base,info.parts[4].size,info.parts[4].type),(0x80060000,0x1f000,2))
                self.assertEqual((work.sample_bytes,bytes(work.sample)),(0,bytes(48)))

    def test_boot_before_parent_and_read_only_boot_are_never_system_rows(self):
        data=rows()
        for initial in ([data[4],data[0]],[data[0],data[4]]):
            for attr in (0,1):
                order=list(initial);boot=next(i for i,row in enumerate(order) if row[6]==2)
                row=list(order[boot]);row[3]=attr;row[2]=0;order[boot]=tuple(row)
                code,_,info,_=self.observe(fixture(rows=order)[0])
                self.assertEqual(code,0)
                self.assertEqual(self.system(info.parts[boot]),0)
                self.assertEqual((info.parts[boot].base,info.parts[boot].size),(0x80060000,0x1f000))

    def test_system_duplicates_and_cross_domain_bank_overlap_remain_check44(self):
        for domain in (0,1,2,3,0xffffffff):
            data=[rows()[0],(0x80001000,0x1000,0x1000,1,14,domain,1)]
            code,work,_,_=self.observe(fixture(rows=data)[0])
            self.assertEqual((code,work.check,work.overlap_a,work.overlap_b),(2,44,1,0))

    def test_unknown_or_crossing_boot_relationships_remain_rejected(self):
        bank,boot=rows()[0],rows()[4]
        for field,value in [(0,0x7ffff000),(0,0xbffff000),(3,2),(4,99),(5,4),(6,3),(6,99)]:
            changed=list(boot);changed[field]=value
            code,work,_,_=self.observe(fixture(rows=[bank,tuple(changed)])[0])
            self.assertEqual((code,work.check),(2,44),(field,value))
        for domain in (2,4):
            changed=list(bank);changed[5]=domain
            code,work,_,_=self.observe(fixture(rows=[tuple(changed),boot])[0])
            self.assertEqual((code,work.check),(2,44))

    def test_boot_boundaries_are_proper_bounded_and_order_independent(self):
        bank=(0x100000000,0x20000000,0x20000000,1,14,0,1)
        for base in (bank[0],bank[0]+bank[1]-4096):
            boot=(base,4096,4096,1,14,0,2)
            for order in ([bank,boot],[boot,bank]):
                self.assertEqual(self.observe(fixture(rows=order)[0])[0],0)
        for boot in [(bank[0],bank[1],bank[1],1,14,0,2),
                     (bank[0]+bank[1]-4096,8192,8192,1,14,0,2)]:
            self.assertEqual(self.observe(fixture(rows=[bank,boot])[0])[1].check,44)
        for base,size,available in [(0xfffffffffffff000,4096,4096),(bank[0],0,0),(bank[0],4096,8192)]:
            code,work,_,_=self.observe(fixture(rows=[bank,(base,size,available,1,14,0,2)])[0])
            self.assertEqual((code,work.check),(5,43))

    def test_default_domain_is_evidence_and_bad_fields_never_qualify(self):
        self.assertEqual(self.libraries[0].CasDiagSmemSystemPart(None),0)
        for domain in (0,1,3):
            self.assertEqual(self.system(Part(0x100000000,0x20000000,0x20000000,1,14,domain,1)),1)
        base=[0x100000000,0x20000000,0x20000000,1,14,0,1]
        for field,value in [(0,1),(1,0),(2,0),(2,0x20001000),(2,1),(3,0),(3,2),(4,4),(5,2),(5,4),(6,2),(6,99)]:
            changed=list(base);changed[field]=value
            self.assertEqual(self.system(Part(*changed)),0,(field,value))

    def gate(self,candidate,smem,free,reserved,complete=15):
        bank=(Range*1)(Range(0x100000000,0x40000000))
        return self.libraries[0].CasDiagEligible(candidate,bank,1,smem,len(smem),free,len(free),
                                                reserved,len(reserved),complete)

    def test_full_boot_exclusion_denies_probe_even_when_bank_and_uefi_cover_it(self):
        candidate=Range(0x100000000,0x10000000)
        smem=(Range*1)(Range(0x100000000,0x40000000))
        free=(Range*1)(Range(0x100000000,0x40000000))
        reserved=(Range*1)(Range(0x100060000,0x1f000))
        self.assertEqual(self.gate(candidate,smem,free,reserved),6)
        self.assertEqual(self.gate(candidate,smem,free,(Range*0)()),0) # exclusion matters
        self.assertEqual(self.gate(candidate,smem,(Range*0)(),(Range*0)()),5) # no ownership
        self.assertEqual(self.gate(candidate,smem,free,(Range*0)(),13),2) # incomplete evidence

    def test_unavailable_tail_and_other_rows_remain_without_ownership(self):
        data=[(0x100000000,0x40000000,0x30000000,1,14,0,1),(0x200000000,0x1000,0,0xffffffff,99,0,99)]
        code,_,info,_=self.observe(fixture(rows=data)[0])
        self.assertEqual(code,0)
        self.assertEqual([self.system(info.parts[i]) for i in range(2)],[1,0])
        smem=(Range*1)(Range(info.parts[0].base,info.parts[0].available))
        free=(Range*1)(Range(0x100000000,0x40000000))
        reserved=(Range*1)(Range(0x130000000,0x10000000))
        self.assertEqual(self.gate(Range(0x130000000,0x10000000),smem,free,reserved),4)

    def test_frozen011_differential_changes_only_supported_boot_relationship(self):
        cases=[fixture(heap,version)[0] for heap in (11,12) for version in (1,2,3,4)]
        original,offset=fixture()
        for at in (92,192,204,0x1ff000,0x1ff028,0x3000,0x3020,offset,offset+8,offset+16,
                   offset+24+16,offset+24+24,offset+24+64):
            for value in (0,1,0xffffffff):
                mem=bytearray(original);put(mem,at,'I',value);cases.append(mem)
        for mem in cases:
            current=self.observe(mem);old=self.observe(mem,lib=self.previous)
            self.assertEqual((current[0],bytes(current[2]),current[3]),(old[0],bytes(old[2]),old[3]))
        mem=fixture(rows=rows())[0]
        current=self.observe(mem);old=self.observe(mem,lib=self.previous)
        self.assertEqual((old[0],old[1].check,old[1].overlap_a,old[1].overlap_b),(2,44,4,0))
        self.assertEqual(current[0],0)
        self.assertEqual(current[3],old[3]*2)
        for i in range(5):self.assertEqual(bytes(current[2].parts[i]),bytes(old[2].parts[i]))

    def test_changed_boot_metadata_and_failed_callback_still_refuse(self):
        mem,offset=fixture(rows=rows())
        code,work,_,_=self.observe(mem,change=offset)
        self.assertEqual((code,work.check,work.compare,work.parsed_rows),(6,5,1,0))
        code,work,_,_=self.observe(mem,fault=offset)
        self.assertEqual((code,work.check,work.parsed_rows),(1,4,0))
