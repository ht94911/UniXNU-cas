"""Exercise the real BDS boot-option decoder, including malformed RAM variables."""
import ctypes
import os
import struct
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(Path(__file__).resolve().parents[2] / "uefi-cas")))
GUID = uuid.UUID("9b665204-a133-47a2-9396-9e2b370a4e25").bytes_le
MARKER = "unixnu:firmware\0".encode("utf-16le")
DESC = "UniXNU\0".encode("utf-16le")
FV = bytes([1, 3, 24, 0]) + struct.pack("<IQQ", 11, 0x9f000000, 0x9f7fffff)
FILE = bytes([4, 6, 20, 0]) + GUID
END = bytes([127, 255, 4, 0])

def option(path=FV + FILE + END, marker=MARKER):
    return struct.pack("<IH", 1, len(path)) + DESC + path + marker

class BootOption(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        output = Path(cls.tmp.name) / "decoder.so"
        inc = UEFI / "Mu_Basecore/MdePkg/Include"
        source = UEFI / "Platforms/Xiaomi/casPkg/Drivers/CasBdsDxe/CasBootOption.c"
        subprocess.run(["cc", "-shared", "-fPIC", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
                        "-I", str(inc), "-I", str(inc / "X64"), str(source), "-o", str(output)], check=True)
        cls.lib = ctypes.CDLL(str(output))
        cls.parse = cls.lib.CasParseUniXnuOption
        cls.parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                             ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        cls.parse.restype = ctypes.c_uint8

    def accepted(self, value):
        offset, size = ctypes.c_size_t(), ctypes.c_size_t()
        result = self.parse(value, len(value), GUID, ctypes.byref(offset), ctypes.byref(size))
        if result:
            self.assertEqual(offset.value, 6 + len(DESC))
            self.assertEqual(size.value, struct.unpack_from("<H", value, 4)[0])
        return bool(result)

    def test_registered_fv_option(self):
        self.assertTrue(self.accepted(option()))
        self.assertTrue(self.accepted(option(FV + bytes([4, 7, 20, 0]) + bytes(16) + FILE + END)))

    def test_every_truncation(self):
        good = option()
        for size in range(len(good)):
            self.assertFalse(self.accepted(good[:size]), size)

    def test_bad_header_description_marker_and_guid(self):
        for offset in [0, 1, 2, 3, 6, 18, 6 + len(DESC) + len(FV) + 4, len(option()) - 2]:
            bad = bytearray(option());bad[offset] ^= 1
            self.assertFalse(self.accepted(bytes(bad)), offset)

    def test_invalid_lengths(self):
        for length in [0, 1, 23, 49, 0xffff]:
            bad = bytearray(option());struct.pack_into("<H", bad, 4, length)
            self.assertFalse(self.accepted(bytes(bad)))
        for length in [0, 1, 3, 25, 0xffff]:
            bad = bytearray(option());struct.pack_into("<H", bad, 6 + len(DESC) + 2, length)
            self.assertFalse(self.accepted(bytes(bad)))

    def test_no_alternative_instance_or_media_path(self):
        for path in [FV + FILE + bytes([127, 1, 4, 0]), FV + END,
                     FV + FILE + FILE + END, FV + bytes([4, 4, 4, 0]) + FILE + END,
                     bytes([3, 18, 4, 0]) + FILE + END, FV + FILE + END + END]:
            self.assertFalse(self.accepted(option(path)))

    def test_no_trailing_bytes(self):
        self.assertFalse(self.accepted(option() + b"x"))
        self.assertFalse(self.accepted(option(marker=MARKER + bytes(8192))))

if __name__ == "__main__":
    unittest.main()
