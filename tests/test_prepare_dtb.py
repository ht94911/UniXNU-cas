"""Offline data-provenance tests; fixtures are not bootable device images."""
import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from prepare_boot_dtb import prepare


class StockDtbPreparation(unittest.TestCase):
    def setUp(self):
        self.source = struct.pack(">10I", 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16) + bytes(16) + struct.pack(">4I", 1, 0, 2, 9)
        result = bytearray(self.source + bytes(8192))
        struct.pack_into(">I", result, 4, len(result))
        self.expected = bytes(result)
        self.policy = {"source": "stock-soc-dtb", "source_bytes": len(self.source),
                       "source_sha256": hashlib.sha256(self.source).hexdigest(), "padding": 8192,
                       "prepared_sha256": hashlib.sha256(result).hexdigest()}

    def pin_source(self, source):
        self.policy.update(source_bytes=len(source), source_sha256=hashlib.sha256(source).hexdigest())
        return source

    def test_only_totalsize_and_trailing_padding_change(self):
        got = prepare(self.source, self.policy)
        self.assertEqual(got, self.expected)
        self.assertEqual(got[8:len(self.source)], self.source[8:])

    def test_wrong_source_hash(self):
        data = bytearray(self.source)
        data[28] = 1
        with self.assertRaisesRegex(ValueError, "source hash"):
            prepare(data, self.policy)

    def test_missing_bytes(self):
        with self.assertRaisesRegex(ValueError, "source size"):
            prepare(self.source[:-1], self.policy)

    def test_second_tree_rejected(self):
        with self.assertRaisesRegex(ValueError, "one complete DTB"):
            prepare(self.pin_source(self.source * 2), self.policy)

    def test_nonzero_tail_not_silently_discarded(self):
        data = bytearray(self.source + b"X")
        struct.pack_into(">I", data, 4, len(data))
        with self.assertRaisesRegex(ValueError, "trailing data"):
            prepare(self.pin_source(data), self.policy)

    def test_memory_bound_enforced(self):
        with self.assertRaisesRegex(ValueError, "exceeds policy"):
            prepare(self.source, self.policy, max_bytes=len(self.expected) - 1)

    def test_expected_output_pin_enforced(self):
        self.policy["prepared_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "prepared DTB hash"):
            prepare(self.source, self.policy)


if __name__ == "__main__":
    unittest.main()
