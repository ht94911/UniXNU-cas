"""Safety boundary tests for the real cas memory design, without a phone."""
import copy
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_memory import interval, number, validate_map, validate_runtime, validate_sizes


class CasMemoryBoundaries(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / "config/memory-map.json").read_text())
        # Hypothetical values within the permitted corridors; not a device log.
        self.runtime = {"kernel": {"base": "0xa0080000", "size": "0x801000"},
                        "dtb": {"base": "0xafe00000", "size": "0x10000"},
                        "ramdisk": {"base": "0xac000000", "size": "0x2000000"}}

    def test_design_and_boundary_sizes(self):
        validate_sizes(self.config, {"fd": 0x800000, "dtb": 0x100000, "ramdisk": 0x4000000, "shim": 0x10000, "boot_image": 0x8000000})

    def test_uint64_overflow(self):
        with self.assertRaises(ValueError):
            interval({"base": "0xfffffffffffff000", "size": "0x2000"})

    def test_no_float_or_boolean_truncation(self):
        for value in (True, 123.5, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                number(value)

    def test_framebuffer_is_never_heap(self):
        self.config["regions"][0]["base"] = "0x9c000000"
        with self.assertRaisesRegex(ValueError, "protected"):
            validate_map(self.config)

    def test_secure_region_is_never_heap(self):
        self.config["regions"][0]["base"] = "0x80000000"
        with self.assertRaisesRegex(ValueError, "protected"):
            validate_map(self.config)

    def test_fd_cannot_overlap_stack(self):
        self.config["regions"][1]["size"] = "0xa00000"
        with self.assertRaisesRegex(ValueError, "overlaps"):
            validate_map(self.config)

    def test_heap_not_across_bank_gap(self):
        self.config["regions"][0]["base"] = "0xb8000000"
        with self.assertRaisesRegex(ValueError, "outside"):
            validate_map(self.config)

    def test_low32_truncation_cannot_hide_wrong_region(self):
        self.config["regions"][0]["base"] = "0x194000000"
        with self.assertRaisesRegex(ValueError, "low-4GB"):
            validate_map(self.config)

    def test_alignment(self):
        self.config["regions"][1]["base"] = "0x9f000001"
        with self.assertRaisesRegex(ValueError, "unaligned"):
            validate_map(self.config)

    def test_actual_image_growth(self):
        for name, size in (("fd", 0x800001), ("fd", 0x7fffff), ("ramdisk", 0x4000001), ("dtb", 0x100001), ("boot_image", 0x8000001)):
            with self.subTest(name=name, size=size), self.assertRaises(ValueError):
                validate_sizes(self.config, {name: size})

    def test_accepted_synthetic_runtime(self):
        validate_runtime(self.config, self.runtime, {"ramdisk": 0x2000000})

    def test_zero_ramdisk_for_minimal_firmware(self):
        self.runtime["ramdisk"] = {"base": 0, "size": 0}
        validate_runtime(self.config, self.runtime, {"ramdisk": 0})

    def test_runtime_overwrite(self):
        self.runtime["dtb"]["base"] = "0x9f000000"
        with self.assertRaises(ValueError):
            validate_runtime(self.config, self.runtime)

    def test_runtime_components_must_not_overlap(self):
        self.runtime["ramdisk"]["base"] = "0xa0080000"
        with self.assertRaisesRegex(ValueError, "overlaps kernel"):
            validate_runtime(self.config, self.runtime)

    def test_runtime_ramdisk_growth_and_mismatch(self):
        for size in (0x5000000, 0x1000000):
            self.runtime["ramdisk"]["size"] = size
            with self.subTest(size=size), self.assertRaises(ValueError):
                validate_runtime(self.config, self.runtime, {"ramdisk": 0x2000000})


if __name__ == "__main__":
    unittest.main()
