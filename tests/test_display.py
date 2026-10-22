"""Execute the firmware's actual display decoder/blits against controlled RAM.

Snapshots below are synthetic register fixtures, not observed hardware state.
The host library excludes DisplayHardware.c and never accesses device MMIO.
"""
import ctypes as c
import itertools
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(Path(__file__).resolve().parents[2] / "uefi-cas")))
U = c.c_uint32
Q = c.c_uint64


class Pipe(c.Structure):
    _fields_ = [(name, U) for name in "size xy out_size out_xy address stride format unpack opmode multirect secure".split()]


class Ctl(c.Structure):
    _fields_ = [(name, U) for name in "top intf start flush".split()] + [("layers", (U * 4) * 6)]


class Snapshot(c.Structure):
    _fields_ = [("revision", U), ("pipes", Pipe * 8), ("ctls", Ctl * 6),
                ("intf_auto", U * 2), ("intf_tear", U * 2), ("pp_auto", U * 6)]


class Frame(c.Structure):
    _fields_ = [("base", Q), ("bytes", Q)] + [(name, U) for name in
        "width height stride red green blue reserved rshift gshift bshift pipes ctls manual intfs".split()]


class Display(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / "display.so"
        source = UEFI / "Platforms/Xiaomi/casPkg/Library/CasDisplayLib"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                        "-I", str(UEFI / "Platforms/Xiaomi/casPkg/Include"), str(source / "DisplayDecode.c"),
                        str(source / "DisplayBlt.c"), "-o", str(lib)], check=True, capture_output=True)
        cls.lib = c.CDLL(str(lib))
        cls.decode = cls.lib.CasDecodeDisplay
        cls.decode.argtypes = [c.POINTER(Snapshot), c.POINTER(Frame)]
        cls.decode.restype = U
        cls.blt = cls.lib.CasDisplayBlt
        cls.blt.argtypes = [c.POINTER(Frame), c.c_void_p, U, Q, Q, Q, Q, Q, Q, Q]
        cls.blt.restype = U

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.s = Snapshot()
        self.s.revision = 0x60000000
        p = self.s.pipes[0]
        p.size = p.out_size = (2340 << 16) | 1080
        p.address = 0x9c123000  # Deliberately not the splash base.
        p.stride = 4352  # Deliberately padded.
        p.format, p.unpack = 0x236ff, 0x03020001
        self.s.ctls[0].top = 1 << 17
        self.s.ctls[0].intf = 2
        self.s.ctls[0].layers[0][0] = 1
        self.f = Frame()

    def check(self, expected=0):
        self.assertEqual(self.decode(c.byref(self.s), c.byref(self.f)), expected)

    def test_actual_base_stride_and_format_are_decoded(self):
        self.check()
        self.assertEqual((self.f.base, self.f.stride, self.f.bytes), (0x9c123000, 4352, 4352 * 2340))
        self.assertEqual((self.f.red, self.f.green, self.f.blue), (0xff0000, 0xff00, 0xff))
        self.assertEqual((self.f.pipes, self.f.ctls, self.f.manual), (1, 1, 1))

    def test_all_rgba_byte_permutations(self):
        for parts in itertools.permutations(range(4)):
            self.s.pipes[0].unpack = sum(part << (index * 8) for index, part in enumerate(parts))
            self.check()
            self.assertEqual(self.f.red, 255 << (8 * parts.index(2)))
            self.assertEqual(self.f.green, 255 << (8 * parts.index(0)))
            self.assertEqual(self.f.blue, 255 << (8 * parts.index(1)))

    def test_alpha_enable_is_supported(self):
        self.s.pipes[0].format |= 1 << 8
        self.check()

    def test_bad_hardware_and_route(self):
        self.s.revision = 0x50000000; self.check(1)
        self.s.revision = 0x60000000
        self.s.ctls[0].intf = 1; self.check(2)
        self.s.ctls[0].intf = 2; self.s.ctls[0].top = 0; self.check(2)

    def test_virtual_and_secure_are_rejected(self):
        self.s.ctls[0].layers[0][3] = 1; self.check(3)
        self.s.ctls[0].layers[0][3] = 0
        self.s.pipes[0].secure = 1; self.check(6)
        self.s.pipes[0].secure = 0; self.s.pipes[0].multirect = 1; self.check(3)

    def test_source_scaling_and_stride(self):
        self.s.pipes[0].size -= 1; self.check(4)
        self.s.pipes[0].size += 1
        self.s.pipes[0].stride = 4316; self.check(4)
        self.s.pipes[0].stride = 4353; self.check(4)

    def test_non_linear_yuv_rotation_and_bad_unpack(self):
        for bit in (30, 31, 15, 14, 11, 22, 19):
            self.s.pipes[0].format = 0x236ff | (1 << bit); self.check(5)
        self.s.pipes[0].format = 0x236ff
        for op in (1, 1 << 13, 1 << 14, 1 << 22):
            self.s.pipes[0].opmode = op; self.check(5)
        self.s.pipes[0].opmode = 0
        self.s.pipes[0].unpack = 0x03020000; self.check(5)

    def test_full_extent_must_fit_splash(self):
        for address in (0, 0x94000000, 0x9e200000, 0xfffff000, 0x9c000001):
            self.s.pipes[0].address = address; self.check(7)

    def test_manual_and_automatic_refresh_are_distinct(self):
        self.s.intf_auto[0] = 0x80000001
        self.check(); self.assertEqual(self.f.manual, 0)
        self.s.intf_auto[0] = 0x80000000; self.check(10)
        self.s.intf_auto[0] = 0x80000001; self.s.ctls[0].intf = 6; self.check(10)

    def test_split_contiguous_framebuffer_and_gap(self):
        p = self.s.pipes[0]
        p.size = p.out_size = (2340 << 16) | 540
        self.check(9)
        self.s.pipes[1] = p
        self.s.pipes[1].address += 540 * 4
        self.s.pipes[1].out_xy = 540
        self.s.ctls[0].layers[0][0] |= 1 << 3
        self.check(); self.assertEqual(self.f.pipes, 3)
        self.s.pipes[1].out_xy = 544
        self.check(4)

    def test_inconsistent_planes_do_not_pick_arbitrary_one(self):
        self.s.pipes[1] = self.s.pipes[0]
        self.s.ctls[0].layers[0][0] |= 1 << 3
        self.check()
        self.s.pipes[1].address += 0x1000; self.check(8)

    def test_dma_pipe_stage_decoding(self):
        self.s.pipes[6] = self.s.pipes[0]
        self.s.ctls[0].layers[0][0] = 0
        self.s.ctls[0].layers[0][2] = 1
        self.check(); self.assertEqual(self.f.pipes, 1 << 6)

    def memory_frame(self):
        self.memory = (U * 22)(*([0x13579bdf] * 22))
        self.f = Frame()
        self.f.base = c.addressof(self.memory) + 4
        self.f.width, self.f.height, self.f.stride, self.f.bytes = 4, 4, 20, 80
        self.f.rshift, self.f.gshift, self.f.bshift = 16, 8, 0
        self.f.reserved = 0xff000000

    def test_blt_fill_preserves_padding_and_guards(self):
        self.memory_frame(); color = U(0x00123456)
        self.assertEqual(self.blt(c.byref(self.f), c.byref(color), 0, 0, 0, 0, 0, 4, 4, 0), 0)
        for index, value in enumerate(self.memory):
            self.assertEqual(value, 0x13579bdf if index in (0, 5, 10, 15, 20, 21) else 0xff123456)

    def test_blt_roundtrip_rgb_permutation_and_delta(self):
        self.memory_frame(); self.f.rshift, self.f.bshift = 0, 16
        source = (U * 8)(0, 0, 0x123456, 0xabcdef, 0, 0, 0x010203, 0x246810)
        self.assertEqual(self.blt(c.byref(self.f), source, 2, 2, 0, 1, 1, 2, 2, 16), 0)
        self.assertEqual(self.memory[7], 0xff563412)
        output = (U * 4)()
        self.assertEqual(self.blt(c.byref(self.f), output, 1, 1, 1, 0, 0, 2, 2, 0), 0)
        self.assertEqual(list(output), [0x123456, 0xabcdef, 0x010203, 0x246810])

    def test_video_overlap_matches_snapshot_copy(self):
        self.memory_frame()
        for i in range(1, 21): self.memory[i] = i
        before = list(self.memory)
        self.assertEqual(self.blt(c.byref(self.f), None, 3, 0, 0, 1, 1, 3, 3, 0), 0)
        for y in range(3):
            for x in range(3): self.assertEqual(self.memory[1 + (y + 1) * 5 + x + 1], before[1 + y * 5 + x])

    def test_blt_bad_sizes_coordinates_delta_and_overflow(self):
        self.memory_frame(); buffer = (U * 8)(); before = list(self.memory)
        cases = [(2, 0, 0, 4, 0, 1, 1, 0), (2, 0, 0, 0, 0, 0, 1, 0),
                 (2, 2, 0, 0, 0, 2, 1, 8), (2, 0, 0, 0, 0, 2, 1, 7),
                 (2, 0, 2**64 - 1, 0, 0, 1, 2, 8), (3, 2**64 - 1, 0, 0, 0, 1, 1, 0),
                 (99, 0, 0, 0, 0, 1, 1, 0)]
        for case in cases: self.assertEqual(self.blt(c.byref(self.f), buffer, *case), 1)
        self.assertEqual(list(self.memory), before)
