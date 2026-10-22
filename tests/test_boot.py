"""Reject malformed images using synthetic FV/TE data and the pinned AOSP writer.

These fixtures are never device images. The production checker is also run on
the actual FD and exact boot.img as part of package_boot.py.
"""
import gzip
import json
import struct
import subprocess
import sys
import tempfile
import tomllib
import unittest
import uuid
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
UEFI = DEVICE.parent / "uefi-cas"
sys.path.insert(0, str(DEVICE / "scripts"))
from check_boot import inspect, inspect_fd
from provenance import check_ref, file_ref


def fd_fixture():
    data = bytearray(b"\xff" * 0x800000)
    header = bytearray(72)
    struct.pack_into("<I", header, 0, 0x14000400)
    header[16:32] = uuid.UUID("8c8ce578-8a3d-4f1c-9935-896185c32dd3").bytes_le
    struct.pack_into("<Q4sIHHHBB", header, 32, len(data), b"_FVH", 0x000cfeff, 72, 0, 0, 0, 2)
    struct.pack_into("<4I", header, 56, 0x800, 0x1000, 0, 0)
    struct.pack_into("<H", header, 50, -sum(struct.unpack("<36H", header)) & 0xffff)
    data[:72] = header
    te = bytearray(0x1100)
    # TE is at FD offset100; stripped size140 makes ImageBase exactly FD base.
    struct.pack_into("<2sHBBHIIQ", te, 0, b"VZ", 0xaa64, 1, 11, 140, 0x1000, 0x1000, 0x9f000000)
    section = (len(te) + 4).to_bytes(3, "little") + bytes([0x12]) + te
    ffs = bytearray(24)
    ffs[:16] = uuid.UUID("dc337caf-861b-45b7-aab6-8b7f019043ec").bytes_le
    ffs[17:20] = bytes([0xaa, 3, 0])
    ffs[20:23] = (24 + len(section)).to_bytes(3, "little")
    ffs[23] = 0xf8
    ffs[16] = -(sum(ffs) - ffs[17] - ffs[23]) & 255
    data[72:72 + 24 + len(section)] = ffs + section
    return bytes(data)


class BootImageChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = tomllib.loads((UEFI / "Resources/Configs/cas.toml").read_text())
        cls.memory = json.loads((DEVICE / "config/memory-map.json").read_text())
        cls.fd = fd_fixture()
        shim = bytearray(4096)
        struct.pack_into("<II6Q4sI", shim, 0, 0x14000010, 0xd503201f, 0x80000,
                         len(shim) + len(cls.fd), 0, 0, 0, 0, b"ARM\x64", 0)
        cls.shim = bytes(shim)
        cls.dtb = struct.pack(">10I", 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16) + bytes(16) + struct.pack(">4I", 1, 0, 2, 9)
        cls.raw = cls.shim + cls.fd
        cls.kernel = gzip.compress(cls.raw, compresslevel=9, mtime=0)

    def make_image(self, kernel=None, ramdisk=b"", dtb=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "kernel").write_bytes(self.kernel if kernel is None else kernel)
            (root / "ramdisk").write_bytes(ramdisk)
            (root / "dtb").write_bytes(self.dtb if dtb is None else dtb)
            subprocess.run([sys.executable, str(UEFI / "Resources/Scripts/mkbootimg.py"),
                            "--kernel", str(root / "kernel"), "--ramdisk", str(root / "ramdisk"),
                            "--dtb", str(root / "dtb"), "--header_version", "2", "--pagesize", "4096",
                            "--base", "0", "--kernel_offset", "0x8000", "--ramdisk_offset", "0x1000000",
                            "--dtb_offset", "0x1f00000", "--tags_offset", "0x1e00000", "--board", "cas",
                            "--output", str(root / "boot.img")], check=True, capture_output=True)
            return (root / "boot.img").read_bytes()

    def check(self, image, **kwargs):
        return inspect(image, kwargs.get("fd", self.fd), kwargs.get("shim", self.shim),
                       kwargs.get("dtb", self.dtb), kwargs.get("ramdisk", b""), self.config, self.memory)

    def test_pinned_writer_roundtrip_and_sec_entry(self):
        report, parts = self.check(self.make_image())
        self.assertEqual(report["fd"]["sec_entry_physical"], "0x9f001000")
        self.assertEqual(parts["fd"], self.fd)
        self.assertEqual(report["header"]["ramdisk_address"], 0)
        self.assertFalse(report["hardware_proven"])

    def test_nonempty_ramdisk_alignment(self):
        ramdisk = bytes(range(256)) * 17
        report, parts = self.check(self.make_image(ramdisk=ramdisk), ramdisk=ramdisk)
        self.assertEqual(report["header"]["ramdisk_address"], 0x1000000)
        self.assertEqual(parts["ramdisk"], ramdisk)
        self.assertEqual(report["parts"]["dtb"]["file_offset"] % 4096, 0)

    def test_empty_ramdisk_must_have_zero_load_address(self):
        image = bytearray(self.make_image())
        struct.pack_into("<I", image, 20, 0x1000000)
        with self.assertRaisesRegex(ValueError, "ramdisk_address"):
            self.check(image)

    def test_header_mismatch(self):
        for offset, value in ((36, 2048), (40, 0), (12, 0x9f000000), (32, 0x100)):
            with self.subTest(offset=offset):
                image = bytearray(self.make_image())
                struct.pack_into("<I", image, offset, value)
                with self.assertRaises(ValueError):
                    self.check(image)

    def test_truncated_section(self):
        with self.assertRaisesRegex(ValueError, "truncated boot section"):
            self.check(self.make_image()[:-1])

    def test_nonzero_padding(self):
        image = bytearray(self.make_image())
        image[4096 + len(self.kernel)] = 1
        with self.assertRaisesRegex(ValueError, "padding"):
            self.check(image)

    def test_image_id_tampering(self):
        image = bytearray(self.make_image())
        image[576] ^= 1
        with self.assertRaisesRegex(ValueError, "image ID"):
            self.check(image)

    def test_unexpected_footer(self):
        with self.assertRaisesRegex(ValueError, "trailing boot"):
            self.check(self.make_image() + bytes(4096))

    def test_concatenated_gzip(self):
        extra = gzip.compress(b"", mtime=0)
        with self.assertRaisesRegex(ValueError, "concatenated"):
            self.check(self.make_image(self.kernel + extra))

    def test_truncated_gzip_with_valid_android_id(self):
        with self.assertRaisesRegex(ValueError, "gzip"):
            self.check(self.make_image(self.kernel[:-1]))

    def test_gzip_crc_corruption_with_valid_android_id(self):
        kernel = bytearray(self.kernel)
        kernel[-8] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            self.check(self.make_image(kernel))

    def test_appended_dtb_and_decompression_bound(self):
        kernel = gzip.compress(self.raw + self.dtb, mtime=0)
        with self.assertRaisesRegex(ValueError, "kernel bound"):
            self.check(self.make_image(kernel))

    def test_wrong_shim_image_size(self):
        shim = bytearray(self.shim)
        struct.pack_into("<Q", shim, 16, len(self.raw) - 16)
        with self.assertRaisesRegex(ValueError, "ARM64 image size"):
            self.check(self.make_image(), shim=shim)

    def test_second_dtb_rejected(self):
        dtb = self.dtb + self.dtb
        with self.assertRaisesRegex(ValueError, "one complete DTB"):
            self.check(self.make_image(dtb=dtb), dtb=dtb)

    def test_fd_checksum_and_sec_target(self):
        fd = bytearray(self.fd)
        fd[50] ^= 1
        with self.assertRaisesRegex(ValueError, "FV header checksum"):
            inspect_fd(fd, 0x9f000000)
        fd = bytearray(self.fd)
        struct.pack_into("<I", fd, 108, 0x1004)  # TE entry RVA, not the reset vector.
        with self.assertRaisesRegex(ValueError, "SEC entry"):
            inspect_fd(fd, 0x9f000000)

    def test_stale_build_product_ref(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "product"
            path.write_bytes(b"one")
            ref = file_ref(path)
            check_ref(ref)
            path.write_bytes(b"two")
            with self.assertRaisesRegex(ValueError, "changed"):
                check_ref(ref)


if __name__ == "__main__":
    unittest.main()
