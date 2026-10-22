"""Execute the real shim C validator on controlled DTBs with logical addresses.

The compiled host library never accesses those physical addresses. This checks
the exact C validation logic, not the ARM entry stub, MMU or actual ABL placement.
"""
import ctypes
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(ROOT.parent / "uefi-cas")))


class ShimValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        library = cls.directory / "validate.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                        "-I", str(UEFI / "Platforms/Xiaomi/casPkg/Include"), str(UEFI / "BootShim/CasBootValidate.c"),
                        "-o", str(library)], check=True, capture_output=True)
        cls.library = ctypes.CDLL(str(library))
        cls.validate = cls.library.CasValidateBoot
        cls.validate.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_void_p]
        cls.validate.restype = ctypes.c_uint32
        cls.dtb = cls.directory / "original.dtb"
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-p", "8192", "-o", str(cls.dtb),
                        str(UEFI / "Resources/DTBs/cas.dts")], check=True, capture_output=True)
        cls.original = cls.dtb.read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.work = self.directory / "case.dtb"
        self.work.write_bytes(self.original)

    def put(self, node, prop, values):
        subprocess.run(["fdtput", "-t", "x", str(self.work), node, prop, *[hex(v) for v in values]],
                       check=True, capture_output=True)

    def call(self, data=None, kernel=0xa0080000, size=0x806000, fdt=0xae000000):
        raw = self.work.read_bytes() if data is None else bytes(data)
        source = ctypes.create_string_buffer(raw)
        info = ctypes.create_string_buffer(128)
        code = self.validate(kernel, size, fdt, source, info)
        return code, info.raw

    def test_actual_minimal_dtb_with_logical_addresses(self):
        code, info = self.call()
        self.assertEqual(code, 0)
        self.assertEqual(struct.unpack_from("<IIII", info), (0x48534143, 1, 128, 1))
        self.assertEqual(struct.unpack_from("<Q", info, 64)[0], 0)

    def test_outside_kernel_window(self):
        self.assertEqual(self.call(kernel=0x9f000000)[0], 1)
        self.assertEqual(self.call(size=0x5000000)[0], 1)
        self.assertEqual(self.call(kernel=0xa0080001)[0], 1)

    def test_fdt_outside_window_before_read(self):
        self.assertEqual(self.call(fdt=0x9f800000)[0], 2)
        self.assertEqual(self.call(fdt=0xfffffffffffffff8)[0], 2)
        self.assertEqual(self.call(fdt=0xb0000000 - 16)[0], 2)

    def test_bad_header_and_declared_size(self):
        raw = bytearray(self.original)
        raw[0] ^= 1
        self.assertEqual(self.call(raw)[0], 3)
        raw = bytearray(self.original)
        struct.pack_into(">I", raw, 4, 0xffffffff)
        self.assertEqual(self.call(raw)[0], 3)

    def test_fdt_overlaps_kernel(self):
        self.assertEqual(self.call(fdt=0xa0100000)[0], 7)

    def test_initrd_pair_and_order(self):
        self.put("/chosen", "linux,initrd-start", [0, 0xafd00000])
        self.assertEqual(self.call()[0], 6)
        self.put("/chosen", "linux,initrd-end", [0, 0xafc00000])
        self.assertEqual(self.call()[0], 6)

    def test_initrd_size_and_kernel_overlap(self):
        self.put("/chosen", "linux,initrd-start", [0, 0xa8000000])
        self.put("/chosen", "linux,initrd-end", [0, 0xad000000])
        self.assertEqual(self.call()[0], 6)
        self.put("/chosen", "linux,initrd-start", [0, 0xa0100000])
        self.put("/chosen", "linux,initrd-end", [0, 0xa0200000])
        self.assertEqual(self.call()[0], 7)

    def test_initrd_fdt_overlap(self):
        self.put("/chosen", "linux,initrd-start", [0, 0xae000000])
        self.put("/chosen", "linux,initrd-end", [0, 0xae200000])
        self.assertEqual(self.call()[0], 7)

    def test_32_and_64_bit_initrd(self):
        for cells in (1, 2):
            prefix = [] if cells == 1 else [0]
            self.put("/chosen", "linux,initrd-start", [*prefix, 0xafd00000])
            self.put("/chosen", "linux,initrd-end", [*prefix, 0xafe00000])
            code, info = self.call(fdt=0xad000000)
            self.assertEqual(code, 0)
            self.assertEqual(struct.unpack_from("<QQQ", info, 56), (0xafd00000, 0x100000, 0xa4000000))

    def test_allows_overlap_safe_copy_destination(self):
        self.put("/chosen", "linux,initrd-start", [0, 0xa4100000])
        self.put("/chosen", "linux,initrd-end", [0, 0xa4300000])
        self.assertEqual(self.call()[0], 0)

    def test_runtime_reservation_cannot_cover_destination(self):
        self.put("/reserved-memory/splash_region", "reg", [0, 0x94000000, 0, 0x100000])
        self.assertEqual(self.call()[0], 5)

    def test_runtime_ram_must_cover_destinations(self):
        self.put("/memory", "reg", [0, 0x80000000, 0, 0x10000000])
        self.assertEqual(self.call()[0], 8)

    def test_wrong_root_cells(self):
        self.put("/", "#address-cells", [1])
        self.assertNotEqual(self.call()[0], 0)

    def test_unbalanced_structure(self):
        raw = bytearray(self.original)
        start = struct.unpack_from(">I", raw, 8)[0]
        struct.pack_into(">I", raw, start, 2)
        self.assertEqual(self.call(raw)[0], 4)

    def test_structure_strings_overlap(self):
        raw = bytearray(self.original)
        start = struct.unpack_from(">I", raw, 8)[0]
        struct.pack_into(">I", raw, 12, start)
        self.assertEqual(self.call(raw)[0], 3)


if __name__ == "__main__":
    unittest.main()
