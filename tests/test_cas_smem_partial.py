"""Actual C partial-row telemetry; diagnostic only, with frozen009 differential."""
import ctypes as c
from pathlib import Path
import shutil
import subprocess
import test_cas_smem_reject as reject
from test_cas_smem import UEFI, Work, Info, READ, fixture, put


class PartialRows(reject.unittest.TestCase):
    observe = reject.RejectTelemetry.observe

    @classmethod
    def setUpClass(cls):
        reject.RejectTelemetry.setUpClass.__func__(cls)
        app = UEFI / 'Platforms/Xiaomi/casPkg/Applications/CasDiag'
        old = Path(cls.temp.name) / 'diag009.c'
        old.write_bytes(subprocess.check_output(['git', '-C', str(UEFI), 'show',
            '532a5888d4426fafd2ba34b58ef55c381717a6a9:Platforms/Xiaomi/casPkg/Applications/CasDiag/SmemCore.c']))
        target = Path(cls.temp.name) / 'diag009.so'
        compiler = shutil.which('clang-18') or shutil.which('cc')
        subprocess.run([compiler, '-std=c11', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra',
            '-Werror', '-I' + str(app), str(old), str(app / 'DiagCore.c'), '-o', str(target)],
            check=True, capture_output=True)
        cls.previous = c.CDLL(str(target))
        cls.previous.CasDiagSmemRead.argtypes = [READ, c.c_void_p, c.POINTER(Work), c.POINTER(Info)]
        cls.previous.CasDiagSmemRead.restype = c.c_int

    def test_rejected_overlap_keeps_both_numeric_rows_and_stops_at_prefix(self):
        rows = [(0x100000000, 0x20000000, 0x10000000, 1, 14, 3, 1),
                (0x110000000, 0x10000000, 0, 2, 99, 7, 4),
                (0x200000000, 0x1000, 0x1000, 1, 14, 1, 1)]
        for version in (1, 2, 3):
            mem, offset = fixture(table=version, rows=rows)
            # Names remain in the private table; the public telemetry is numeric.
            private_name = b'PRIVATE-NAME-01!'
            self.assertEqual(len(private_name), 16)
            mem[offset + 24:offset + 40] = private_name
            status, work, info, calls = self.observe(mem)
            self.assertEqual((status, work.check, info.count, work.parsed_rows), (2, 44, 3, 2))
            self.assertEqual((work.overlap_a, work.overlap_b), (1, 0))
            for index in range(2):
                part = info.parts[index]
                expected = list(rows[index])
                if version == 1:
                    expected[2] = expected[1]
                self.assertEqual((part.base, part.size, part.available, part.attr,
                                  part.category, part.domain, part.type), tuple(expected))
            self.assertEqual(bytes(info.parts[2]), bytes(c.sizeof(info.parts[2])))
            self.assertEqual((work.sample_bytes, bytes(work.sample)), (0, bytes(48)))
            self.assertEqual(calls[-1], (offset, 24 + 3 * (64 if version == 1 else 72)))

    def test_partial_prefix_bounds_at_last_row_and_invalid_count(self):
        rows = [(0x100000000 + index * 0x2000, 0x1000, 0x1000, 1, 14, 1, 1)
                for index in range(32)]
        rows[-1] = rows[0]
        mem, offset = fixture(rows=rows)
        status, work, info, _ = self.observe(mem)
        self.assertEqual((status, work.check, info.count, work.parsed_rows,
                          work.overlap_a, work.overlap_b), (2, 44, 32, 32, 31, 0))
        put(mem, offset + 16, 'I', 33)
        status, work, info, _ = self.observe(mem)
        self.assertEqual((status, work.check, info.count, work.parsed_rows,
                          work.overlap_a, work.overlap_b), (5, 42, 0, 0, 32, 32))
        mem, offset = fixture()
        put(mem, offset + 24 + 24, 'Q', 0)
        status, work, _, _ = self.observe(mem)
        self.assertEqual((status, work.check, work.parsed_rows, work.overlap_a, work.overlap_b),
                         (5, 43, 1, 32, 32))

    def test_new_pass_and_reused_work_never_expose_a_stale_prefix(self):
        mem, _ = fixture(rows=[(0x100000000, 0x2000, 0x2000, 1, 14, 1, 1)] * 2)
        _, work, _, _ = self.observe(mem)
        self.assertEqual((work.parsed_rows, work.overlap_a, work.overlap_b), (2, 1, 0))
        valid, _ = fixture()
        status, work, _, _ = self.observe(valid, work=work, fault=192)
        self.assertEqual((status, work.parsed_rows, work.overlap_a, work.overlap_b), (1, 0, 32, 32))
        status, work, _, _ = self.observe(valid, work=work, change=192)
        self.assertEqual((status, work.compare, work.parsed_rows, work.overlap_a, work.overlap_b),
                         (6, 1, 0, 32, 32))

    def test_valid_two_observations_have_a_complete_non_ownership_prefix(self):
        for heap in (11, 12):
            for version in (1, 2, 3):
                mem, _ = fixture(heap, version)
                status, work, info, calls = self.observe(mem)
                self.assertEqual((status, work.check, work.parsed_rows, info.count,
                                  work.overlap_a, work.overlap_b), (0, 0, 2, 2, 32, 32))
                self.assertEqual(calls[:len(calls)//2], calls[len(calls)//2:])

    def test_010_has_identical_status_info_and_physical_reads_to_frozen009(self):
        cases = []
        for heap in (11, 12):
            for version in (1, 2, 3, 4):
                mem, _ = fixture(heap, version)
                cases.append(mem)
        mem, offset = fixture()
        for at in (92, 192, 204, 0x1ff000, 0x1ff028, 0x3000, 0x3020,
                   offset, offset + 8, offset + 16, offset + 24 + 16,
                   offset + 24 + 24, offset + 24 + 64, offset + 24 + 72 + 16):
            for value in (0, 1, 0xffffffff):
                changed = bytearray(mem)
                put(changed, at, 'I', value)
                cases.append(changed)
        for index, mem in enumerate(cases):
            with self.subTest(index=index):
                current = self.observe(mem)
                previous = self.observe(mem, lib=self.previous)
                self.assertEqual((current[0], bytes(current[2]), current[3]),
                                 (previous[0], bytes(previous[2]), previous[3]))
