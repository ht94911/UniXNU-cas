"""Compile and execute the actual reservation/paging C used by CasDiag EFI."""
import ctypes as c
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UEFI = Path(os.environ.get('CAS_UEFI_ROOT', ROOT.parent / 'uefi-cas'))

class Range(c.Structure):
    _fields_ = [('base', c.c_ulonglong), ('size', c.c_ulonglong)]

def cells(*ranges):
    return b''.join(struct.pack('>QQ', *r) for r in ranges)

class InputsView(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cas-inputs-view-')
        compiler = shutil.which('clang-18') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('actual C compiler required')
        source = UEFI / 'Platforms/Xiaomi/casPkg/Applications/CasDiag/DiagCore.c'
        library = Path(cls.temp.name) / 'core.so'
        subprocess.run([compiler, '-std=c11', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(library)], check=True, capture_output=True)
        cls.lib = c.CDLL(str(library))
        cls.lib.CasDiagReservedRanges.argtypes = [c.c_void_p, c.c_int] * 4 + [c.POINTER(Range), c.c_uint, c.POINTER(c.c_uint)]
        cls.lib.CasDiagReservedRanges.restype = c.c_int
        cls.lib.CasDiagTextLine.argtypes = [c.c_void_p, c.c_uint, c.c_uint, c.c_uint, c.c_void_p, c.c_uint]
        cls.lib.CasDiagTextLine.restype = c.c_uint
        for name in ('CasDiagPageEnd', 'CasDiagPagePrevious'):
            fn = getattr(cls.lib, name)
            fn.argtypes = [c.c_void_p] + [c.c_uint] * 4
            fn.restype = c.c_uint
        cls.lib.CasDiagEligible.argtypes = [Range] + [t for _ in range(4) for t in (c.POINTER(Range), c.c_uint)] + [c.c_uint]
        cls.lib.CasDiagEligible.restype = c.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def reserve(self, reg=None, size=None, align=None, allowed=None, capacity=128, lengths=None):
        output = (Range * 128)(*(Range(0xdead, 0xbeef) for _ in range(128)))
        count = c.c_uint(0xffffffff)
        args = []
        for i, value in enumerate((reg, size, align, allowed)):
            length = len(value) if value is not None else -1
            if lengths and i in lengths:
                length = lengths[i]
            args.extend((value, length))
        status = self.lib.CasDiagReservedRanges(*args, output, capacity, c.byref(count))
        return status, [(r.base, r.size) for r in output[:count.value]], (output[0].base, output[0].size)

    def test_fixed_reg_excludes_all_tuples_exactly(self):
        values = [(0x80900000, 0x200000), (0x100000000, 0x1000)]
        status, out, _ = self.reserve(reg=cells(*values))
        self.assertEqual((status, out), (0, values))

    def test_dynamic_excludes_whole_allowed_range_not_requested_size(self):
        status, out, _ = self.reserve(size=struct.pack('>Q', 0xc00000), align=struct.pack('>Q', 0x400000), allowed=cells((0, 0xffffffff)))
        self.assertEqual((status, out), (1, [(0, 0xffffffff)]))

    def test_dynamic_multiple_choices_all_excluded_even_if_one_too_small(self):
        choices = [(0x100000000, 0x1000), (0x120000000, 0x1000000)]
        status, out, _ = self.reserve(size=struct.pack('>Q', 0x400000), allowed=cells(*choices))
        self.assertEqual((status, out), (1, choices))

    def test_unknown_dynamic_bounds_do_not_become_complete(self):
        for kwargs in ({}, {'size': struct.pack('>Q', 0x1000)}, {'allowed': cells((0, 0xffffffff))}):
            status, out, untouched = self.reserve(**kwargs)
            self.assertEqual((status, out, untouched), (2, [], (0xdead, 0xbeef)))

    def test_malformed_reg_does_not_fall_back_to_size(self):
        for reg in (b'', b'x' * 15, b'x' * 17):
            status, out, untouched = self.reserve(reg=reg, size=struct.pack('>Q', 0x1000), allowed=cells((0, 0xffffffff)))
            self.assertEqual((status, out, untouched), (3, [], (0xdead, 0xbeef)))

    def test_property_error_is_not_treated_as_missing_reg(self):
        status, out, _ = self.reserve(size=struct.pack('>Q', 0x1000), allowed=cells((0, 0xffffffff)), lengths={0: -4})
        self.assertEqual((status, out), (3, []))

    def test_zero_wrapped_and_malformed_ranges_refused_without_partial_output(self):
        for blob in (cells((1, 0)), cells((0xfffffffffffff000, 0x2000)), cells((0, 0x1000), (0xfffffffffffff000, 0x2000)), b'x' * (129 * 16)):
            status, out, untouched = self.reserve(reg=blob)
            self.assertEqual((status, out, untouched), (3, [], (0xdead, 0xbeef)))

    def test_bad_size_alignment_and_unplaceable_dynamic_refused(self):
        for size, align, allowed in ((b'1234', None, cells((0, 0xffffffff))), (struct.pack('>Q', 0), None, cells((0, 0xffffffff))), (struct.pack('>Q', 0x1000), struct.pack('>Q', 3), cells((0, 0xffffffff))), (struct.pack('>Q', 0x1000), struct.pack('>Q', 0), cells((0, 0xffffffff))), (struct.pack('>Q', 0x1000), b'1234', cells((0, 0xffffffff))), (struct.pack('>Q', 0x2000), struct.pack('>Q', 0x2000), cells((0x1001, 0x2000))), (struct.pack('>Q', 0x2000), struct.pack('>Q', 0x4000), cells((0xffffffffffffd001, 0x2000)))):
            status, out, untouched = self.reserve(size=size, align=align, allowed=allowed)
            self.assertEqual((status, out, untouched), (3, [], (0xdead, 0xbeef)))

    def test_capacity_is_checked_before_output(self):
        status, out, untouched = self.reserve(reg=cells((0, 1), (1, 1)), capacity=1)
        self.assertEqual((status, out, untouched), (4, [], (0xdead, 0xbeef)))

    def test_fdt_resolution_does_not_grant_uefi_ownership(self):
        region = (Range * 1)(Range(0xc0000000, 0x140000000))
        protected = (Range * 1)(Range(0, 0xffffffff))
        self.assertEqual(self.lib.CasDiagEligible(Range(0x100000000, 0x10000000), region, 1, region, 1, None, 0, protected, 1, 15), 5)

    def line(self, payload, offset=0, width=79):
        text = c.create_string_buffer(256)
        end = self.lib.CasDiagTextLine(payload, len(payload), offset, width, text, len(text))
        return end, text.value

    def test_exact_width_newline_does_not_create_extra_empty_row(self):
        self.assertEqual(self.line(b'12345\nX', width=5), (6, b'12345'))
        self.assertEqual(self.line(b'12345\r\nX', width=5), (7, b'12345'))

    def test_blank_crlf_and_lone_cr_consume_one_physical_row(self):
        self.assertEqual(self.line(b'\nX'), (1, b''))
        self.assertEqual(self.line(b'\r\nX'), (2, b''))
        self.assertEqual(self.line(b'\rX'), (1, b''))

    def test_terminal_controls_never_inject_new_rows_or_tab_wrap(self):
        self.assertEqual(self.line(b'A\tB\x1bC\x7f'), (6, b'A?B?C?'))

    def test_invalid_width_and_capacity_cannot_overwrite_buffer(self):
        output = c.create_string_buffer(b'abcdef')
        self.assertEqual(self.lib.CasDiagTextLine(b'123456', 6, 0, 6, output, 6), 0)
        self.assertEqual(output.raw[1:], b'bcdef\0')
        self.assertEqual(self.line(b'ABC', width=0), (0, b''))
        self.assertEqual(self.line(b'ABC', offset=3), (3, b''))

    def test_wrapped_uefi_output_fits_25_rows_without_losing_payload(self):
        # Same descriptor shape as the runtime failure; this is synthetic host data.
        payload = b''.join(('UEFI type=7 phys=%016x virt=%016x bytes=%016x attr=%016x\n' % (0x94000000 + i * 4096, 0, 4096, 0x100f)).encode() for i in range(60))
        offset = 0
        starts = []
        recovered = []
        while offset < len(payload):
            starts.append(offset)
            end = self.lib.CasDiagPageEnd(payload, len(payload), offset, 79, 16)
            self.assertGreater(end, offset)
            rows = 0
            while offset < end:
                next_offset, text = self.line(payload, offset)
                self.assertLessEqual(len(text), 79)
                self.assertGreater(next_offset, offset)
                recovered.append(text)
                offset = next_offset
                rows += 1
            self.assertLessEqual(rows, 16)
            self.assertLess(6 + rows + 2, 25)
        self.assertEqual(b''.join(recovered), payload.replace(b'\n', b''))
        for i, start in enumerate(starts):
            self.assertEqual(self.lib.CasDiagPagePrevious(payload, len(payload), start, 79, 16), starts[max(0, i - 1)])

    def test_page_navigation_empty_invalid_and_first_page(self):
        self.assertEqual(self.lib.CasDiagPageEnd(b'', 0, 0, 79, 16), 0)
        self.assertEqual(self.lib.CasDiagPageEnd(b'ABC', 3, 0, 0, 16), 0)
        self.assertEqual(self.lib.CasDiagPageEnd(b'ABC', 3, 0, 79, 0), 0)
        self.assertEqual(self.lib.CasDiagPagePrevious(b'ABC', 3, 0, 79, 16), 0)
        self.assertEqual(self.lib.CasDiagPagePrevious(b'ABC', 3, 4, 79, 16), 0)

if __name__ == '__main__':
    unittest.main()
