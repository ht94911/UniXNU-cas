#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression tests for the A840 PM4 guard and Phase 6 release CLI."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from check_contracts import CACHE_PARENT, GateError
import cp_protect_audit as protect
from pm4_check import type4_header, type7_header
from pm4_xml_index import DEFAULT_MESA
import phase6_release_gate as release

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools/verify"


def stream(*words: int) -> bytes:
    return struct.pack(f"<{len(words)}I", *words)


def type4(register: int, count: int = 1) -> bytes:
    return stream(type4_header(register, count), *([0] * count))


def type7(opcode: int, *payload: int) -> bytes:
    return stream(type7_header(opcode, len(payload)), *payload)


class ProtectionTableTests(unittest.TestCase):
    def test_exact_hardware_slot_order_and_extents(self):
        # Hash of the 54 PROT_* calls in NezhaGpu.c:3069-3088, one call per line.
        source = "".join(f"PROT_{access}(0x{base:05x}, 0x{length:04x})\n"
                         for access, base, length in protect.A840_PROTECT_RANGES)
        self.assertEqual(len(protect.A840_PROTECT_RANGES), 54)
        self.assertEqual(hashlib.sha256(source.encode("ascii")).hexdigest(),
                         "fbab35dc37dc1fcf773311f525acdd53c87a6fd531a727a49e04c66215ba0d77")


def boundary_case(slot: int):
    def test_boundaries(self):
        _, base, length = protect.A840_PROTECT_RANGES[slot]
        end = base + length
        for register in sorted({base - 1, base, end, end + 1}):
            # Some slots are adjacent or overlap. Leaving one slot may still
            # be forbidden by another, including RO/NA overlaps.
            expected = [(index, access) for index, (access, start, span)
                        in enumerate(protect.A840_PROTECT_RANGES) if start <= register <= start + span]
            with self.subTest(register=hex(register)):
                report = protect.audit_bytes(type4(register))
                self.assertEqual(report["status"], "FAIL" if expected else "PASS")
                self.assertEqual(report["errors"], [])
                self.assertEqual(report["violation_count"], len(expected))
                self.assertEqual([(hit["segment_index"], hit["access"]) for hit in report["violations"]],
                                 expected)
    return test_boundaries


for _slot in range(54):
    setattr(ProtectionTableTests, f"test_hardware_slot_{_slot:02d}_boundaries", boundary_case(_slot))


class PacketTests(unittest.TestCase):
    def test_probe005_write_is_reported_at_exact_offsets(self):
        data = type7(0x10, 0, 0) + bytes.fromhex("01009e40 00000000")
        report = protect.audit_bytes(data)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["violations"], [{
            "packet_index": 1, "header_dword_offset": 3, "dword_offset": 4,
            "register": 0x9e00, "register_hex": "0x09e00", "segment_index": 30,
            "access": "NA", "segment_start": 0x9e00, "segment_end": 0x9fff,
        }])

    def test_type4_crosses_into_protected_range(self):
        report = protect.audit_bytes(type4(0x9dff, 3))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["register_writes"], 3)
        self.assertEqual([(hit["register"], hit["dword_offset"]) for hit in report["violations"]],
                         [(0x9e00, 2), (0x9e01, 3)])

    def test_type4_crosses_out_of_protected_range(self):
        report = protect.audit_bytes(type4(0x9fff, 3))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual([hit["register"] for hit in report["violations"]], [0x9fff])

    def test_overlap_reports_both_slots(self):
        report = protect.audit_bytes(type4(0x8ec))
        self.assertEqual([(hit["segment_index"], hit["access"]) for hit in report["violations"]],
                         [(12, "RO"), (13, "NA")])

    def test_ro_write_is_forbidden(self):
        report = protect.audit_bytes(type4(0x789))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["violations"][0]["access"], "RO")

    def test_unprotected_type4_writes(self):
        report = protect.audit_bytes(type4(0, 8) + type4(0x3fffe, 2))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["packet_count"], 2)
        self.assertEqual(report["register_writes"], 10)

    def test_type7_steps_over_type4_lookalike_payload(self):
        report = protect.audit_bytes(type7(0x10, type4_header(0x9e00, 1), 0) + type4(0x9dff))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["packet_count"], 2)
        self.assertEqual(report["register_writes"], 1)

    def test_type7_zero_payload(self):
        self.assertEqual(protect.audit_bytes(type7(0x26))["status"], "PASS")

    def test_type7_largest_payload(self):
        data = stream(type7_header(0x10, 0x3fff)) + bytes(4 * 0x3fff) + type4(0x9e00)
        report = protect.audit_bytes(data)
        self.assertEqual(report["violations"][0]["header_dword_offset"], 0x4000)

    def test_type4_count_parity(self):
        self.assert_packet_error(stream(type4_header(0, 1) ^ (1 << 7), 0), "odd-parity")

    def test_type4_register_parity(self):
        self.assert_packet_error(stream(type4_header(0, 1) ^ (1 << 27), 0), "odd-parity")

    def test_type7_count_parity(self):
        self.assert_packet_error(stream(type7_header(0x10, 1) ^ (1 << 15), 0), "odd-parity")

    def test_type7_opcode_parity(self):
        self.assert_packet_error(stream(type7_header(0x10, 0) ^ (1 << 23)), "odd-parity")

    def test_type4_reserved_bit(self):
        self.assert_packet_error(stream(type4_header(0, 1) | (1 << 26), 0), "reserved")

    def test_type7_reserved_bits(self):
        for bit in (14, 24, 25, 26, 27):
            with self.subTest(bit=bit):
                self.assert_packet_error(stream(type7_header(0x10, 0) | (1 << bit)), "reserved")

    def test_type4_zero_length_fails(self):
        self.assert_packet_error(stream(0x48000080), "invalid Type4")

    def test_type4_register_overflow_fails(self):
        self.assert_packet_error(stream(0x4bffff02, 0, 0), "invalid Type4")

    def test_type4_truncation_fails(self):
        self.assert_packet_error(stream(type4_header(0, 2), 0), "truncated Type-4")

    def test_type7_truncation_fails(self):
        self.assert_packet_error(stream(type7_header(0x10, 2), 0), "truncated Type-7")

    def test_unknown_packet_types_fail(self):
        for kind in (0, 2, 3, 5, 6, 8, 15):
            with self.subTest(kind=kind):
                self.assert_packet_error(stream(kind << 28), "unsupported packet type")

    def test_error_after_valid_packet_carries_cursor(self):
        report = protect.audit_bytes(type7(0x10, 0) + stream(0xf0000000))
        self.assertEqual(report["errors"][0]["packet_index"], 1)
        self.assertEqual(report["errors"][0]["dword_offset"], 2)

    def test_empty_and_partial_dword_streams_fail(self):
        for data in (b"", b"x", type4(0x9dff) + b"xx"):
            with self.subTest(data=data):
                self.assert_packet_error(data, "nonempty aligned")

    def assert_packet_error(self, data: bytes, message: str) -> None:
        report = protect.audit_bytes(data)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn(message, report["errors"][0]["error"])


class CacheFixture(unittest.TestCase):
    def setUp(self):
        root = Path(os.environ.get("TMPDIR") or release.DEFAULT_CACHE)
        try:
            root = release.checked_output_path(root, directory=True)
        except GateError:
            root = release.DEFAULT_CACHE
        root.mkdir(parents=True, exist_ok=True)
        scratch = tempfile.TemporaryDirectory(prefix="phase6-test-", dir=root)
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)


class AuditPathTests(CacheFixture):
    def test_batch_recurses_and_reports_each_bad_file(self):
        nested = self.scratch / "nested" / "deep"
        nested.mkdir(parents=True)
        (nested / "readable.pm4").write_bytes(type7(0x10, 0))
        (nested / "bad.PM4").write_bytes(type4(0x9e00))
        (self.scratch / "ignore.txt").write_text("not PM4", encoding="ascii")
        report = protect.audit_paths([self.scratch])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["file_count"], 2)
        self.assertEqual(report["violation_count"], 1)
        self.assertEqual({Path(row["path"]).name: row["status"] for row in report["files"]},
                         {"readable.pm4": "PASS", "bad.PM4": "FAIL"})

    def test_duplicate_inputs_are_counted_once(self):
        path = self.scratch / "clean.pm4"
        path.write_bytes(type4(0))
        self.assertEqual(protect.audit_paths([path, self.scratch])["file_count"], 1)

    def test_empty_directory_fails_closed(self):
        self.assertEqual(protect.audit_paths([self.scratch])["status"], "FAIL")

    def test_missing_input_is_structured_failure(self):
        report = protect.audit_paths([self.scratch / "missing.pm4"])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["file_count"], 0)
        self.assertTrue(report["errors"])

    def test_pm4_symlink_fails_closed(self):
        link = self.scratch / "outside.pm4"
        link.symlink_to("/tmp/phase6-unused.pm4")
        report = protect.audit_paths([self.scratch])
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(report["errors"])

    def test_raw_file_cli_reports_pass_and_fail(self):
        path = self.scratch / "sample.pm4"
        for register, expected in ((0x9dff, 0), (0x9e00, 1)):
            with self.subTest(register=register):
                path.write_bytes(type4(register))
                result = subprocess.run([sys.executable, "-B", str(TOOLS / "cp_protect_audit.py"),
                                         "--input", str(path)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, expected, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["violation_count"], expected)
                self.assertEqual(report["protection_range_count"], 54)


class CachePathTests(CacheFixture):
    def test_rejects_windows_absolute_and_drive_relative_paths(self):
        for path in ("C:/not-created/phase6", "c:phase6", r"C:\not-created\phase6", r"\\host\share"):
            with self.subTest(path=path), self.assertRaises(GateError):
                release.checked_output_path(path, directory=True)

    def test_rejects_mnt_c_and_tmp(self):
        for path in ("/mnt/c/phase6", "/tmp/phase6", "/var/tmp/phase6"):
            with self.subTest(path=path), self.assertRaises(GateError):
                release.checked_output_path(path, directory=True)

    def test_rejects_cache_root_and_traversal(self):
        for path in (CACHE_PARENT, self.scratch / "../out"):
            with self.subTest(path=path), self.assertRaises(GateError):
                release.checked_output_path(path, directory=True)

    def test_rejects_directory_symlink_escape(self):
        link = self.scratch / "outside"
        link.symlink_to("/tmp", target_is_directory=True)
        with self.assertRaises(GateError):
            release.checked_output_path(link / "phase6", directory=True)

    def test_rejects_output_path_symlink_escape(self):
        link = self.scratch / "summary.json"
        link.symlink_to("/mnt/c/phase6-summary.json")
        with self.assertRaises(GateError):
            release.checked_output_path(link)

    def test_accepts_non_lane_specific_cache_paths(self):
        path = CACHE_PARENT / "coordinator phase 6" / "publication" / "nested"
        self.assertEqual(release.checked_output_path(path, directory=True), path)
        self.assertEqual(release.checked_output_path(path / "report.json"), path / "report.json")

    def test_accepts_safe_in_cache_symlink(self):
        target = self.scratch / "target"
        target.mkdir()
        link = self.scratch / "alias"
        link.symlink_to(target, target_is_directory=True)
        self.assertEqual(release.checked_output_path(link / "nested", directory=True), target / "nested")

    def test_invalid_output_is_refused_before_creating_cache(self):
        path = self.scratch / "would-create"
        with self.assertRaises(GateError):
            release.run_gate(REPO, REPO, path, out_json="/tmp/no-phase6-summary.json")
        self.assertFalse(path.exists())


class ReleaseContractTests(unittest.TestCase):
    def report(self, mode: str, **statuses: str) -> dict:
        report = release.result_base(mode, False)
        report["results"] = [{"name": name, "status": statuses.get(name, "PASS")} for name in release.GATES]
        release.summarize(report)
        return report

    def test_eleven_registered_gates(self):
        self.assertEqual(len(release.GATES), 11)
        self.assertEqual(len(set(release.GATES)), 11)

    def test_full_readiness_requires_eleven_passes(self):
        report = self.report("full")
        self.assertEqual(report["status"], "PASS")
        self.assertIs(report["phase6_host_release_ready"], True)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)

    def test_full_missing_siblings_cannot_signal_release_ready(self):
        report = self.report("full", **dict.fromkeys(release.SIBLINGS, release.DEFERRED))
        self.assertEqual(report["status"], "FAIL")
        self.assertIs(report["phase6_host_release_ready"], False)
        self.assertEqual(report["deferred_siblings"], ["W11-L1", "W11-L2", "W11-L3"])

    def test_quick_can_pass_with_deferred_siblings(self):
        report = self.report("quick", verify_wave=release.SKIPPED,
                             **dict.fromkeys(release.SIBLINGS, release.DEFERRED))
        self.assertEqual(report["status"], "PASS")
        self.assertIs(report["phase6_host_release_ready"], False)
        self.assertEqual(report["deferred_siblings"], ["W11-L1", "W11-L2", "W11-L3"])

    def test_quick_never_signals_release_ready_without_deferrals_either(self):
        report = self.report("quick", verify_wave=release.SKIPPED)
        self.assertEqual(report["status"], "PASS")
        self.assertIs(report["phase6_host_release_ready"], False)

    def test_only_siblings_may_be_deferred(self):
        report = self.report("quick", cp_protect_pm4_audit=release.DEFERRED)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["deferred_siblings"], [])

    def test_only_verify_wave_may_be_skipped_in_quick(self):
        report = self.report("quick", phase5_non_regression=release.SKIPPED)
        self.assertEqual(report["status"], "FAIL")

    def test_failure_is_fatal_even_with_other_siblings_deferred(self):
        for gate in release.GATES:
            with self.subTest(gate=gate):
                report = self.report("quick", **{"kmd_phase6_release_freeze": release.DEFERRED, gate: "FAIL"})
                self.assertEqual(report["status"], "FAIL")
                self.assertIs(report["phase6_host_release_ready"], False)

    def test_duplicate_or_missing_gate_never_makes_full_ready(self):
        report = release.result_base("full", True)
        report["results"] = [{"name": name, "status": "PASS"}
                             for name in (*release.GATES[:-1], release.GATES[0])]
        release.summarize(report)
        self.assertEqual(report["status"], "FAIL")
        self.assertIs(report["phase6_host_release_ready"], False)

    def test_device_signoff_marks_the_unified_system_pending(self):
        checklist = release.real_device_checklist()
        probe = checklist["probe006"]
        self.assertEqual((probe["passed"], probe["total"], len(probe["cases"])), (8, 8, 8))
        self.assertTrue(all(case["status"] == "PASS" for case in probe["cases"]))
        unified = checklist["unified_system"]
        self.assertEqual(unified["status"], "pending_real_device")
        self.assertEqual(unified["target_refresh_hz"], 120)
        self.assertEqual(len(unified["cases"]), 9)
        self.assertTrue(all(case["status"] == "pending_real_device" for case in unified["cases"]))
        self.assertIn("Paging or moving", unified["cases"][3]["scenario"])

    def test_child_hardware_claim_is_rejected(self):
        for flag in ("device_verified", "hardware_acceptance"):
            report = {"status": "PASS", "mode": "quick", "with_clang": False,
                      **release.evidence_fields(), flag: True}
            with self.subTest(flag=flag), self.assertRaisesRegex(GateError, "invalid Phase"):
                release.require_host_regression(report, 3, False)

    def test_phase4_cannot_defer_pre_wave_11_siblings(self):
        report = {"status": "PASS", "mode": "quick", "with_clang": False, "phase4_host_ready": True,
                  **release.evidence_fields(), "deferred_siblings": ["W08-L3"]}
        with self.assertRaisesRegex(GateError, "unverified pre-Wave-11"):
            release.require_host_regression(report, 4, False)


def graph_report(**overrides) -> dict:
    return {"status": "PASS", "targets": {name: {"sources": ["native.c"]}
                                          for name in release.GRAPH_REQUIRED},
            "forbidden_targets": sorted(release.GRAPH_FORBIDDEN), "violations": [],
            "audit_only": True, "zero_turnip_verified": True, "zero_fallback_verified": True,
            "scanned_source_count": 1, **release.evidence_fields(), **overrides}


class GraphTests(unittest.TestCase):
    def test_graph_declares_forbidden_targets_without_selecting_them(self):
        release.check_release_graph(graph_report())

    def test_forbidden_release_targets_fail_even_if_child_claims_pass(self):
        for target in (*sorted(release.GRAPH_FORBIDDEN), "metal2vulkan_runtime", r"src\freedreno\vulkan"):
            with self.subTest(target=target), self.assertRaisesRegex(GateError, "forbidden release graph"):
                release.check_release_graph(graph_report(dependencies=[f"prefix/{target}"]))

    def test_forbidden_dependency_name_cannot_hide_in_key(self):
        report = graph_report()
        report["targets"]["Adreno840Metal.bundle"]["libVulkan_freedreno"] = []
        with self.assertRaisesRegex(GateError, "forbidden release graph"):
            release.check_release_graph(report)

    def test_forbidden_targets_metadata_cannot_hide_a_dependency_mapping(self):
        with self.assertRaisesRegex(GateError, "invalid release graph"):
            release.check_release_graph(graph_report(forbidden_targets={"dependency": "metal2vulkan-runtime"}))

    def test_graph_empty_pass_is_refused(self):
        with self.assertRaisesRegex(GateError, "incomplete"):
            release.check_release_graph({"status": "PASS"})

    def test_graph_missing_required_target_is_refused(self):
        report = graph_report()
        del report["targets"]["NativeMetalTranslate"]
        with self.assertRaisesRegex(GateError, "incomplete"):
            release.check_release_graph(report)

    def test_graph_failure_is_fatal(self):
        with self.assertRaisesRegex(GateError, "did not report PASS"):
            release.check_release_graph({"status": "FAIL", "targets": []})


class StageIntegrationTests(CacheFixture):
    def make_run(self, repo: Path = REPO) -> release.ReleaseRun:
        return release.ReleaseRun(repo, REPO, self.scratch / "run", "quick", False)

    def test_missing_wave_11_siblings_are_all_deferred(self):
        run = self.make_run(self.scratch)
        for action, lane in ((run.freeze, "W11-L1"), (run.release_graph, "W11-L2"), (run.cutover, "W11-L3")):
            with self.subTest(lane=lane):
                row = {"status": "PASS"}
                action(row)
                self.assertEqual(row["status"], release.DEFERRED)
                self.assertEqual(row["sibling"]["name"], lane)

    def test_partial_sibling_fails_closed(self):
        run = self.make_run(self.scratch)
        builder = self.scratch / release.GRAPH_BUILDER
        builder.parent.mkdir(parents=True)
        builder.touch()
        row = {"status": "PASS"}
        with self.assertRaisesRegex(GateError, "MISSING_SOURCE"):
            run.release_graph(row)
        self.assertNotEqual(row["status"], release.DEFERRED)

    def test_present_release_graph_executes_the_sibling_cli(self):
        run = self.make_run(self.scratch)
        builder = self.scratch / release.GRAPH_BUILDER
        builder.parent.mkdir(parents=True)
        builder.write_text(
            "import argparse, json\nfrom pathlib import Path\n"
            "cli = argparse.ArgumentParser()\n"
            "cli.add_argument('--audit-graph-only', action='store_true', required=True)\n"
            "cli.add_argument('--repo', required=True)\n"
            "cli.add_argument('--out-json', type=Path, required=True)\n"
            "args = cli.parse_args()\n"
            f"args.out_json.write_text(json.dumps({graph_report()!r}))\n",
            encoding="ascii",
        )
        (self.scratch / release.GRAPH_TEST).write_text(
            "import unittest\nclass GraphFixture(unittest.TestCase):\n"
            + "".join(f"    def test_{index}(self): self.assertTrue(True)\n" for index in range(30)),
            encoding="ascii",
        )
        row = {"status": "PASS", "commands": []}
        run.release_graph(row)
        self.assertEqual(row["status"], "PASS")
        self.assertEqual(row["tests_run"], 30)
        self.assertEqual(len(row["commands"]), 2)

    def test_protected_writes_from_repository_subdirectories_fail_gate(self):
        repo = self.scratch / "repo"
        nested = repo / "fixtures" / "stream"
        nested.mkdir(parents=True)
        (nested / "injected.pm4").write_bytes(type4(0x9e00))
        run = self.make_run(repo)
        row = {}
        with self.assertRaisesRegex(GateError, "CP_PROTECT"):
            run.audit_pm4(row)
        self.assertEqual(row["violations"], 1)
        self.assertEqual(row["failed_files"][0]["violations"][0]["register"], 0x9e00)

    def test_gate_includes_legacy_phase4_output_in_audit(self):
        run = self.make_run(self.scratch)
        legacy = self.scratch / "phase4"
        legacy.mkdir()
        (legacy / "msaa.pm4").write_bytes(type7(0x10, 0))
        run.scan_roots = [run.output, legacy]
        row = {}
        run.audit_pm4(row)
        self.assertEqual(row["files_scanned"], 1)
        self.assertEqual(row["violations"], 0)

    def compile_object(self, run: release.ReleaseRun, text: str) -> None:
        subprocess.run(["gcc-13", "-c", "-x", "c", "-", "-o", str(run.output / "sample.o")],
                       input=text, text=True, capture_output=True, check=True, timeout=30, env=run.env)

    def test_final_symbol_gate_accepts_native_object(self):
        run = self.make_run()
        run.scan_roots = [run.output]
        self.compile_object(run, "int a840_native_draw(void) { return 0; }\n")
        registry = run.wave_output / "suites/translate/cargo-home/registry/src/package/test-data"
        registry.mkdir(parents=True)
        (registry / "prebuilt.a").write_bytes(b"non-host archive fixture")
        row = {"commands": []}
        run.no_fallback(row)
        self.assertEqual(row["profile"], "final")
        self.assertEqual(row["artifacts_scanned"], 1)
        self.assertEqual(row["violations"], [])

    def test_final_profile_rejects_native_bridge_symbol(self):
        run = self.make_run()
        self.compile_object(run, "int NativeBridgeMetalFrontend(void) { return 0; }\n")
        row = {"commands": []}
        with self.assertRaises(GateError):
            run.no_fallback(row)
        self.assertTrue(any(item["rule"] == "NATIVE_BRIDGE_FRONTEND" for item in row["violations"]))

    def test_release_symbol_guard_rejects_unlisted_vulkan_and_turnip_symbols(self):
        run = self.make_run()
        self.compile_object(run,
                            "extern int vkQueueSubmit(void);\n"
                            "int tu_submit(void) { return vkQueueSubmit(); }\n"
                            "int vk_device_init(void) { return 0; }\n"
                            "int NB9(void) { return 0; }\n")
        row = {"commands": []}
        with self.assertRaisesRegex(GateError, "forbidden release symbols"):
            run.no_fallback(row)
        self.assertEqual({item["match"] for item in row["violations"]},
                         {"vkQueueSubmit", "vk_device_init", "tu_submit", "NB9"})

    def test_compiler_namespace_does_not_imply_a_vulkan_loader(self):
        for symbol in ("metal2vulkan::metal::format::vk_format",
                       "<metal2vulkan::spirv::ir::module::MemAccess>::vulkan_memory_model"):
            with self.subTest(symbol=symbol):
                self.assertIsNone(release.FORBIDDEN_SYMBOL.search(symbol))

    def test_empty_symbol_inventory_fails_closed(self):
        run = self.make_run()
        run.scan_roots = [run.output]
        with self.assertRaisesRegex(GateError, "empty"):
            run.no_fallback({"commands": []})


class RealQuickTests(CacheFixture):
    def test_real_quick_checks_the_current_source_and_preserves_hardware_flags(self):
        mesa = os.environ.get("A840_VERIFY_MESA") or str(DEFAULT_MESA)
        cache = self.scratch / "quick"
        summary = self.scratch / "quick-summary.json"
        result = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "phase6_release_gate.py"),
             "--repo", str(REPO), "--mesa", mesa, "--mode", "quick",
             "--cache-dir", str(cache), "--out-json", str(summary)],
            text=True, capture_output=True, timeout=600, cwd=REPO,
        )
        self.assertTrue(summary.is_file(), result.stderr or result.stdout)
        report = json.loads(summary.read_text(encoding="utf-8"))
        self.assertEqual(report, json.loads((cache / "phase6_release_summary.json").read_text(encoding="utf-8")))
        cli_report = json.loads(result.stdout)
        for candidate in (report, cli_report):
            self.assertIs(candidate["phase6_host_release_ready"], False)
            self.assertIs(candidate["device_verified"], False)
            self.assertIs(candidate["hardware_acceptance"], False)
            self.assertEqual(tuple(row["name"] for row in candidate["results"]), release.GATES)
        rows = {row["name"]: row for row in report["results"]}

        legacy_write = "a840_emit_reg(&batch, A840_REG_PC_DRAWCALL_CNTL, initiator)"
        old_3d = legacy_write in (REPO / release.EMIT / "a840_emit_3d.c").read_text(encoding="utf-8")
        old_msaa = legacy_write in (REPO / release.EMIT / "a840_emit_msaa_tess_vis.c").read_text(encoding="utf-8")
        expected_bad = ({"icb.pm4"} if old_3d else set()) | ({"msaa-vis.pm4", "tess-amp.pm4"} if old_msaa else set())
        audit = rows["cp_protect_pm4_audit"]
        self.assertGreaterEqual(audit["files_scanned"], 44)
        self.assertEqual(audit["errors"], [])
        self.assertEqual({Path(item["path"]).name for item in audit["failed_files"]}, expected_bad)
        self.assertEqual(audit["violations"], len(expected_bad))
        for item in audit["failed_files"]:
            self.assertEqual(item["errors"], [])
            self.assertEqual([(hit["register"], hit["segment_index"], hit["access"])
                              for hit in item["violations"]], [(0x9e00, 30, "NA")])
        self.assertEqual(result.returncode, int(bool(expected_bad)))
        self.assertEqual(report["status"], "FAIL" if expected_bad else "PASS")

        expected_deferred = []
        for name, row in rows.items():
            with self.subTest(gate=name):
                if name == "verify_wave":
                    self.assertEqual(row["status"], release.SKIPPED)
                elif name in release.SIBLINGS and not row["sibling"]["present_paths"]:
                    self.assertEqual(row["status"], release.DEFERRED)
                    expected_deferred.append(release.SIBLINGS[name])
                elif name == "cp_protect_pm4_audit" and expected_bad:
                    self.assertEqual(row["status"], "FAIL")
                else:
                    self.assertEqual(row["status"], "PASS", row.get("error"))
        self.assertEqual(report["deferred_siblings"], expected_deferred)
        final_symbols = rows["no_fallback_final_strict"]
        self.assertEqual(final_symbols["profile"], "final")
        self.assertEqual(final_symbols["violations"], [])
        self.assertGreater(final_symbols["artifacts_scanned"], 0)


if __name__ == "__main__":
    unittest.main()
