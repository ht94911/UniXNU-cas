"""Actual C rejection telemetry and differential checks against frozen diag006."""
import ctypes as c
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_cas_smem import UEFI, Work, Info, READ, fixture, put


class RejectTelemetry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cas-smem-reject-')
        cls.addClassCleanup(cls.temp.cleanup)
        app = UEFI / 'Platforms/Xiaomi/casPkg/Applications/CasDiag'
        compiler = shutil.which('clang-18') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('actual C compiler required')
        old = Path(cls.temp.name) / 'diag006.c'
        old.write_bytes(subprocess.check_output(['git', '-C', str(UEFI), 'show',
            '51b5d0d440e3669beef29c4d6a211d0e2ef9eb62:Platforms/Xiaomi/casPkg/Applications/CasDiag/SmemCore.c']))
        cls.libraries = []
        for name, source in [('current', app / 'SmemCore.c'), ('diag006', old)]:
            target = Path(cls.temp.name) / (name + '.so')
            subprocess.run([compiler, '-std=c11', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra',
                '-Werror', '-I' + str(app), str(source), str(app / 'DiagCore.c'), '-o', str(target)],
                check=True, capture_output=True)
            lib = c.CDLL(str(target))
            lib.CasDiagSmemRead.argtypes = [READ, c.c_void_p, c.POINTER(Work), c.POINTER(Info)]
            lib.CasDiagSmemRead.restype = c.c_int
            cls.libraries.append(lib)
        cls.libraries[0].CasDiagSmemCheckName.argtypes = [c.c_uint]
        cls.libraries[0].CasDiagSmemCheckName.restype = c.c_char_p

    def observe(self, mem, lib=None, fault=None, change=None, work=None):
        calls = []
        seen = {}
        original = bytes(mem)
        @READ
        def read(context, offset, size, out):
            self.assertLessEqual(offset + size, len(mem))
            self.assertLessEqual(size, 4096)
            calls.append((offset, size))
            seen[offset] = seen.get(offset, 0) + 1
            if offset == fault:
                return 1
            data = bytearray(mem[offset:offset + size])
            if change is not None and offset == change and seen[offset] == 2:
                data[0] ^= 1
            c.memmove(out, bytes(data), size)
            return 0
        work = work if work is not None else Work()
        info = Info()
        code = (lib or self.libraries[0]).CasDiagSmemRead(read, None, c.byref(work), c.byref(info))
        self.assertEqual(bytes(mem), original)
        return code, work, info, calls

    def test_primary_header_failure_preserves_only_requested_metadata(self):
        mem, _ = fixture()
        put(mem, 204, 'I', 0x12345678)
        code, work, info, calls = self.observe(mem)
        self.assertEqual((code, work.check, work.read_offset, work.read_length, work.sample_bytes), (2, 11, 192, 16, 16))
        self.assertEqual(bytes(work.sample[:16]), bytes(mem[192:208]))
        self.assertEqual(bytes(work.sample[16:]), bytes(32))
        self.assertEqual(calls, [(92, 4), (192, 16)])
        self.assertEqual((info.smem_version, info.offset, info.size), (12, 0, 0))
        self.assertEqual(self.libraries[0].CasDiagSmemCheckName(work.check), b'primary-header')

    def test_each_pre_item_format_group_has_its_own_check_and_offset(self):
        toc = 0x1ff000
        for offset, value, check, read_offset, size in [
            (toc, 0, 20, toc, 32), (toc + 12, 1, 20, toc, 32),
            (toc + 40, 1, 22, toc + 32, 48), (toc + 52, 1, 22, toc + 32, 48),
            (0x3000, 0, 25, 0x3000, 32), (0x3014, 1, 25, 0x3000, 32),
            (0x3020, 0, 28, 0x3020, 16), (0x302c, 1, 28, 0x3020, 16),
        ]:
            with self.subTest(offset=offset):
                mem, _ = fixture()
                put(mem, offset, 'I', value)
                code, work, _, _ = self.observe(mem)
                self.assertEqual((code, work.check, work.read_offset, work.sample_bytes), (2, check, read_offset, size))
                self.assertEqual(bytes(work.sample[:size]), bytes(mem[read_offset:read_offset + size]))

    def test_callback_failure_cannot_print_stale_or_uninitialized_bytes(self):
        mem, _ = fixture()
        code, work, _, calls = self.observe(mem, fault=0x1ff000)
        self.assertEqual((code, work.check, work.read_offset, work.read_length), (1, 4, 0x1ff000, 32))
        self.assertEqual((work.sample_bytes, bytes(work.sample)), (0, bytes(48)))
        self.assertEqual(calls[-1], (0x1ff000, 32))

    def test_ram_item_failure_never_samples_names_or_payload_even_when_short(self):
        for heap in (11, 12):
            mem, offset = fixture(heap=heap)
            # A 24-byte malformed item must remain excluded from the 48-byte metadata sample.
            if heap == 11:
                put(mem, 208 + 402 * 16 + 8, 'I', 24)
            else:
                put(mem, offset - 16 + 4, 'I', 24)
                put(mem, 0x300c, 'I', offset - 0x3000 + 24)
            mem[offset:offset + 24] = b'PRIVATE-ITEM-NAME-SECRET'
            code, work, _, _ = self.observe(mem)
            self.assertEqual((code, work.check), (2, 40))
            self.assertEqual((work.sample_bytes, bytes(work.sample)), (0, bytes(48)))

    def test_second_observation_change_records_the_new_bounded_metadata(self):
        mem, _ = fixture()
        code, work, _, _ = self.observe(mem, change=192)
        self.assertEqual((code, work.check, work.compare, work.read_offset), (6, 5, 1, 192))
        self.assertEqual(work.sample[0], mem[192] ^ 1)

    def test_reused_work_resets_prior_failure_and_sample(self):
        mem, _ = fixture()
        put(mem, 204, 'I', 1)
        _, work, _, _ = self.observe(mem)
        valid, _ = fixture()
        code, work, _, _ = self.observe(valid, work=work)
        self.assertEqual((code, work.check, work.sample_bytes, bytes(work.sample)), (0, 0, 0, bytes(48)))

    def test_telemetry_does_not_change_006_acceptance_output_or_read_sequence(self):
        toc = 0x1ff000
        cases = []
        for heap in (11, 12):
            for table in (1, 2, 3, 4):
                mem, _ = fixture(heap=heap, table=table)
                cases.append(mem)
        mem, item = fixture()
        offsets = (92, 192, 204, toc, toc+4, toc+8, toc+12, toc+32, toc+36,
                   toc+40, toc+44, toc+48, toc+52, 0x3000, 0x3004, 0x3008,
                   0x300c, 0x3010, 0x3014, 0x3020, 0x3024, 0x3028, 0x302c,
                   item, item+4, item+8, item+12, item+16, item+20)
        for offset in offsets:
            for value in (0, 1, 0xffffffff):
                changed = bytearray(mem)
                put(changed, offset, 'I', value)
                cases.append(changed)
        for index, case in enumerate(cases):
            with self.subTest(case=index):
                current = self.observe(case, self.libraries[0])
                baseline = self.observe(case, self.libraries[1])
                self.assertEqual((current[0], bytes(current[2]), current[3]),
                                 (baseline[0], bytes(baseline[2]), baseline[3]))

    def test_known_and_unknown_check_names_are_bounded_static_strings(self):
        for check in (0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 20, 21, 22,
                      23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 40, 41, 42, 43, 44, 50, 0xffffffff):
            name = self.libraries[0].CasDiagSmemCheckName(check)
            self.assertLessEqual(len(name), 20)
            self.assertTrue(all(32 <= value < 127 for value in name))


if __name__ == '__main__':
    unittest.main()
