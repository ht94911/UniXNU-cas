"""Host regression gates for cas ADT bytes, ranges and privacy boundaries.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from adt_codec import MAX_BLOB, MAX_DEPTH, Node, decode, string, u32, u64
from cas_adt import audit_sources, audit_tree, build_tree, empty_nvram_proxy, load_json, load_schema, validate_facts, validate_nvram

FACTS = load_json(ROOT / "config/cas-adt-facts.json")
SCHEMA = load_schema()


class CodecTests(unittest.TestCase):
    def test_wire_format_against_hand_encoded_header(self):
        golden = struct.pack("<II32sI", 1, 0, b"name", 12) + b"device-tree\0"
        self.assertEqual(Node("device-tree").encode(), golden)
        self.assertEqual(decode(golden).encode(), golden)

    def test_every_truncation_of_minimal_tree_refused(self):
        raw = Node("device-tree").encode()
        for end in range(len(raw)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                decode(raw[:end])

    def test_counts_overflow_refused_without_walking(self):
        for raw in (bytes(8), b"\xff" * 8, u32(1) + u32(0xffffffff) + bytes(64)):
            with self.subTest(raw=raw[:8]), self.assertRaises(ValueError):
                decode(raw)

    def test_property_size_overflow_refused(self):
        raw = bytearray(Node("device-tree").encode())
        struct.pack_into("<I", raw, 40, 0xffffffff)
        with self.assertRaises(ValueError):
            decode(bytes(raw))

    def test_property_key_padding_must_be_zero(self):
        raw = bytearray(Node("device-tree").encode()); raw[13] = 1
        with self.assertRaises(ValueError):
            decode(bytes(raw))

    def test_value_alignment_padding_must_be_zero(self):
        raw = bytearray(Node("device-tree", {"x": b"a"}).encode()); raw[-1] = 1
        with self.assertRaises(ValueError):
            decode(bytes(raw))

    def test_duplicate_property_in_wire_refused(self):
        raw = bytearray(Node("device-tree", {"x": u32(1)}).encode())
        raw[56:88] = b"name" + bytes(28)
        with self.assertRaises(ValueError):
            decode(bytes(raw))

    def test_duplicate_child_in_wire_refused(self):
        raw = Node("device-tree", children=[Node("test")]).encode()
        raw = raw[:4] + u32(2) + raw[8:] + raw[56:]
        with self.assertRaises(ValueError):
            decode(raw)

    def test_bad_root_and_trailing_bytes_refused(self):
        raw = Node("device-tree").encode()
        for value in (raw + b"\0", raw[:44] + b"DEVICE-tree\0"):
            with self.subTest(), self.assertRaises(ValueError):
                decode(value)

    def test_node_name_requires_one_nul_terminated_ascii_string(self):
        raw = bytearray(Node("device-tree").encode()); raw[47] = 0
        with self.assertRaises(ValueError):
            decode(bytes(raw))
        raw[47] = 255
        with self.assertRaises(ValueError):
            decode(bytes(raw))

    def test_integer_limits_reject_bool_negative_and_truncation(self):
        for method, values in ((u32, [True, -1, 1 << 32, 1.0]), (u64, [False, -1, 1 << 64, "1"])):
            for value in values:
                with self.subTest(value=value), self.assertRaises(ValueError):
                    method(value)
        self.assertEqual(u64(0x100000000), b"\0\0\0\0\x01\0\0\0")

    def test_encoder_refuses_bad_values_and_names(self):
        for tree in [Node("wrong-root"), Node("device-tree", {"name": b"bad\0"}),
                     Node("device-tree", {"x" * 32: b""}), Node("device-tree", {"x": bytearray(4)}),
                     Node("device-tree", children=[Node("x"), Node("x")])]:
            with self.subTest(), self.assertRaises(ValueError):
                tree.encode()

    def test_depth_and_cycles_are_bounded(self):
        root = Node("device-tree"); node = root
        for _ in range(MAX_DEPTH + 1):
            child = Node("child"); node.children.append(child); node = child
        with self.assertRaises(ValueError):
            root.encode()
        root.children = [root]
        with self.assertRaises(ValueError):
            root.encode()

    def test_blob_and_child_limits(self):
        for tree in [Node("device-tree", {"huge": bytes(MAX_BLOB)}),
                     Node("device-tree", children=[Node("n" + str(i)) for i in range(257)])]:
            with self.subTest(), self.assertRaises(ValueError):
                tree.encode()
        with self.assertRaises(ValueError):
            decode(bytes(MAX_BLOB + 1))


class FactsTests(unittest.TestCase):
    def setUp(self):
        self.facts = copy.deepcopy(FACTS)

    def refused(self):
        with self.assertRaises(ValueError):
            validate_facts(self.facts)

    def test_recorded_ranges_and_conservative_framebuffer_exclusion(self):
        report = validate_facts(self.facts)
        self.assertEqual(report["bank_total"], 0x2f9900000)
        self.assertEqual(report["reservation_count"], 31)
        self.assertFalse(report["current_memory_qualified"])

    def test_unknown_identity_field_refused(self):
        self.facts["serial_number"] = "not-a-device-serial"
        self.refused()

    def test_bool_is_not_a_schema_version(self):
        self.facts["schema_version"] = True
        self.refused()

    def test_duplicate_or_overwide_cpu_affinity(self):
        for value in (0, 1 << 32):
            self.facts = copy.deepcopy(FACTS)
            self.facts["cpus"][1]["affinity"] = value
            self.refused()

    def test_unobserved_psci_conduit_refused(self):
        self.facts["psci"]["method"] = "hvc"
        self.refused()

    def test_timer_ppi_must_match_fixed_armcpu(self):
        self.facts["timer"]["interrupt_cells"][7] = 12
        self.refused()

    def test_redistributor_capacity_must_cover_hardware_topology(self):
        self.facts["gic"]["redistributor"]["size"] = 0x20000
        self.refused()

    def test_overlapping_banks_refused(self):
        self.facts["memory_banks"][1]["base"] = 0x90000000
        self.refused()

    def test_zero_negative_unaligned_and_overflowing_range_refused(self):
        for base, size in [(0x80000000, 0), (0x80000000, -4096), (0x80000001, 4096), ((1 << 64) - 4096, 4096)]:
            self.facts = copy.deepcopy(FACTS)
            self.facts["memory_banks"][0] = {"base": base, "size": size}
            self.refused()

    def test_firmware_and_hardware_reservations_must_not_overlap(self):
        self.facts["reservations"][-1]["base"] = self.facts["reservations"][0]["base"]
        self.refused()

    def test_mmio_cannot_be_claimed_as_ram(self):
        self.facts["mmio"][0]["base"] = 0x100000000
        self.refused()

    def test_candidate_cannot_cross_memory_gap(self):
        self.facts["owned_memory"]["candidate_base"] = 0xb0000000
        self.refused()

    def test_candidate_cannot_overlap_known_firmware(self):
        self.facts["owned_memory"]["candidate_base"] = 0x90000000
        self.refused()

    def test_half_open_adjacent_reservation_is_allowed(self):
        self.facts["reservations"].append({"name": "adjacent-test-reservation", "base": 0x120000000,
                                             "size": 4096, "role": "protected"})
        self.assertTrue(validate_facts(self.facts)["ranges_static_pass"])

    def test_small_or_unaligned_loader_request_refused(self):
        for size in (0x8000000, 0x10001000, 0x40000000):
            self.facts = copy.deepcopy(FACTS)
            self.facts["owned_memory"]["request_size"] = size
            self.refused()

    def test_profile_cannot_promote_historical_memory_to_verified(self):
        self.facts["owned_memory"]["state"] = "verified"
        self.refused()

    def test_hardcoded_framebuffer_address_refused(self):
        self.facts["framebuffer"]["base"] = 0x9c000000
        self.refused()

    def test_missing_framebuffer_enclosure_refused(self):
        self.facts["framebuffer"]["enclosure_name"] = "unproven"
        self.refused()

    def test_source_path_cannot_escape_workspace(self):
        self.facts["sources"]["board"]["path"] = "../outside.json"
        self.refused()

    def test_source_verification_requires_originals(self):
        with self.assertRaisesRegex(ValueError, "evidence root required"):
            audit_sources(self.facts)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises((ValueError, OSError)):
                audit_sources(self.facts, evidence_root=Path(tmp))


class TreeContractTests(unittest.TestCase):
    def setUp(self):
        self.tree, self.trace = build_tree(FACTS)

    def audit(self, **kwargs):
        return audit_tree(self.tree, FACTS, SCHEMA, **kwargs)

    def test_cpu0_default_and_all_affinities(self):
        self.assertEqual(self.audit()["cpu_affinities"], [0])
        tree, _ = build_tree(FACTS, cpu_profile="all")
        report = audit_tree(tree, FACTS, SCHEMA, cpu_profile="all")
        self.assertEqual(report["cpu_affinities"], [i << 8 for i in range(8)])
        self.assertFalse(report["boot_ready"])

    def test_wire_roundtrip_and_public_golden_fixtures(self):
        for profile in ("cpu0", "all"):
            tree, _ = build_tree(FACTS, cpu_profile=profile)
            blob = tree.encode()
            self.assertEqual(decode(blob).encode(), blob)
            self.assertEqual(blob, (ROOT / "tests/fixtures/cas-adt" / (profile + ".adt")).read_bytes())

    def test_every_property_including_names_has_provenance(self):
        actual = {path + "::" + key for path, node in self.tree.walk() for key in ("name", *node.properties)}
        self.assertEqual(actual, self.trace.keys())
        self.assertEqual(len(actual), 110)

    def test_fixture_contains_no_entropy_or_identity_or_root(self):
        chosen = self.tree.at("/chosen").properties
        self.assertNotIn("random-seed", chosen)
        self.assertEqual(self.tree.at("/chosen/memory-map").properties, {})
        self.assertEqual(self.audit()["identity_fields"], [])
        self.assertEqual(self.audit()["entropy_bytes"], 0)

    def test_private_entropy_is_fresh_and_not_public_trace(self):
        first, trace = build_tree(FACTS, mode="private")
        second, _ = build_tree(FACTS, mode="private")
        seed = first.at("/chosen").properties["random-seed"]
        self.assertEqual(len(seed), 64)
        self.assertNotEqual(seed, second.at("/chosen").properties["random-seed"])
        self.assertNotIn(seed.hex(), json.dumps(trace))
        self.assertEqual(audit_tree(first, FACTS, SCHEMA, mode="private")["entropy_bytes"], 64)

    def test_private_seed_cannot_be_sneaked_into_fixture(self):
        self.tree.at("/chosen").properties["random-seed"] = bytes(range(64))
        with self.assertRaises(ValueError):
            self.audit()

    def test_private_mode_rejects_missing_or_zero_seed(self):
        with self.assertRaises(ValueError):
            self.audit(mode="private")
        self.tree.at("/chosen").properties["random-seed"] = bytes(64)
        with self.assertRaises(ValueError):
            self.audit(mode="private")

    def test_unknown_or_sensitive_node_and_property_refused(self):
        for key in ("unique-chip-id", "serial-number", "root-matching", "log-address", "unknown"):
            tree, _ = build_tree(FACTS)
            tree.at("/chosen").properties[key] = u64(1)
            with self.subTest(key=key), self.assertRaises(ValueError):
                audit_tree(tree, FACTS, SCHEMA)
        self.tree.children.append(Node("storage"))
        with self.assertRaises(ValueError):
            self.audit()

    def test_required_node_removal_refused(self):
        self.tree.children = [child for child in self.tree.children if child.name != "options"]
        with self.assertRaises(ValueError):
            self.audit()

    def test_cpu_reg_and_gic_integer_widths_are_exact(self):
        self.tree.at("/cpus/cpu0").properties["reg"] = u64(0)
        with self.assertRaises(ValueError):
            self.audit()
        self.tree, _ = build_tree(FACTS)
        self.tree.at("/arm-io/intc").properties["redistributor-stride"] = u32(0x20000)
        with self.assertRaises(ValueError):
            self.audit()

    def test_gic_bus_translation_cannot_drift(self):
        self.tree.at("/arm-io").properties["ranges"] = struct.pack("<QQQ", 0, 0, 0x20000000)
        with self.assertRaises(ValueError):
            self.audit()

    def test_memory_high_bits_are_preserved(self):
        reg = self.tree.at("/memory").properties["reg"]
        self.assertEqual(struct.unpack_from("<QQ", reg, 16), (0xc0000000, 0x140000000))
        self.assertEqual(struct.unpack_from("<QQ", reg, 32), (0x200000000, 0x180000000))

    def test_reserved_range_truncation_refused(self):
        self.tree.at("/reserved-memory/region0").properties["reg"] = bytes(8)
        with self.assertRaises(ValueError):
            self.audit()

    def test_empty_nvram_chrp_and_corruption(self):
        image = empty_nvram_proxy()
        self.assertEqual(validate_nvram(image)["variables"], 0)
        for offset in (1, 16, 34, 100, 2100, 8191):
            corrupt = bytearray(image); corrupt[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                validate_nvram(bytes(corrupt))

    def test_nvram_bank_size_and_proxy_must_agree(self):
        self.tree.at("/chosen").properties["nvram-bank-size"] = u32(4096)
        with self.assertRaises(ValueError):
            self.audit()


class CliTests(unittest.TestCase):
    def invoke(self, out, *extra):
        return subprocess.run([sys.executable, str(ROOT / "scripts/make-cas-adt.py"), "--out", str(out), *extra],
                              cwd=out.parent, capture_output=True, text=True)

    def test_fixture_cli_from_unrelated_directory_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "fixture"
            result = self.invoke(out)
            self.assertEqual(result.returncode, 0, result.stderr)
            before = (out / "cas.adt").read_bytes()
            result = self.invoke(out)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((out / "cas.adt").read_bytes(), before)
            report = load_json(out / "audit.json")
            self.assertEqual(report["source_audit"]["result"], "not_run")
            self.assertFalse(report["boot_ready"])

    def test_private_files_and_reports_do_not_disclose_entropy(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "private"
            result = self.invoke(out, "--mode", "private")
            self.assertEqual(result.returncode, 0, result.stderr)
            seed = decode((out / "cas.adt").read_bytes()).at("/chosen").properties["random-seed"]
            for name in ("audit.json", "provenance.json"):
                self.assertNotIn(seed.hex(), (out / name).read_text())
                if os.name == "posix":
                    self.assertEqual((out / name).stat().st_mode & 0o777, 0o600)
            if os.name == "posix":
                self.assertEqual(out.stat().st_mode & 0o777, 0o700)
                self.assertEqual((out / "cas.adt").stat().st_mode & 0o777, 0o600)

    def test_invalid_facts_leave_no_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); path = root / "bad.json"
            facts = copy.deepcopy(FACTS); facts["serial-number"] = "forbidden"
            path.write_text(json.dumps(facts))
            result = self.invoke(root / "output", "--facts", str(path))
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "output").exists())

    def test_duplicate_json_key_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "duplicate.json"
            path.write_text('{"board":"cas","board":"other"}')
            with self.assertRaises(ValueError):
                load_json(path)


if __name__ == "__main__":
    unittest.main()
