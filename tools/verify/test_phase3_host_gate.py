#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 3 host gate and anonymous corpus audit tests."""

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
from unittest import mock

from check_contracts import CACHE_PARENT, GateError, cache_directory, cache_environment
import corpus_audit as audit
import hwtest_parser
import phase3_host_gate as gate
from pm4_xml_index import DEFAULT_MESA

REPO = Path(__file__).resolve().parents[2]
MESA = Path(os.environ.get("A840_VERIFY_MESA", DEFAULT_MESA))
TEST_CACHE = CACHE_PARENT / "w06-l4/unittest"
A, B, C = ("11" * 32, "22" * 32, "33" * 32)
DIRECTORY = 128


def record(key: str = A, tier=0, time=0, kind="app") -> dict:
    return {"pipeline_key_sha256": key, "hit_tier": tier, "compile_us": time, "process_kind": kind}


def authenticate(blob: bytearray) -> None:
    blob[32:64] = bytes(32)
    blob[32:64] = hashlib.sha256(blob).digest()


def wire_pack(keys: tuple[str, ...], tier: int) -> bytearray:
    """A directory fixture assembled separately from the audit parser."""
    buckets = 8
    payload = DIRECTORY + 128 * len(keys)
    entry_payload_bytes = 24448  # Metadata, binding plan, padding and one ISA block.
    blob = bytearray(payload + entry_payload_bytes * len(keys))
    struct.pack_into("<4sBBHQQII", blob, 0, b"8PK1", 0x81 | (tier << 2), 3,
                     len(keys), 0x100, 0x200, 0x44050A31, len(blob) - payload)
    slots = [0xFFFFFFFF] * buckets
    for index, text in enumerate(keys):
        key = bytes.fromhex(text)
        start = payload + index * entry_payload_bytes
        struct.pack_into("<32s9I", blob, DIRECTORY + 128 * index, key, 0, 1, start,
                         0, 0, start + 640, 23632, start, entry_payload_bytes)
        fnv = 14695981039346656037
        for byte in key:
            fnv = ((fnv ^ byte) * 1099511628211) % (1 << 64)
        slot = fnv % buckets
        while slots[slot] != 0xFFFFFFFF:
            slot = (slot + 1) % buckets
        slots[slot] = index
    struct.pack_into("<8I", blob, 64, *slots)
    authenticate(blob)
    return blob


class CachedCase(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="phase3-", dir=cache_directory(TEST_CACHE))
        self.addCleanup(temp.cleanup)
        self.cache = Path(temp.name)
        self.env = cache_environment(self.cache)

    def cli(self, script: str, *args, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(REPO / "tools/verify" / script), *map(str, args)],
                              cwd=REPO, env=self.env, capture_output=True, text=True, check=False,
                              timeout=timeout)


class PackTests(CachedCase):
    def parse_pack(self, blob: bytearray) -> audit.PackDirectory:
        path = self.cache / "pack.bin"
        path.write_bytes(blob)
        return audit.read_pack(path)

    def reject(self, blob: bytearray, code: str, *, resign: bool = False) -> None:
        if resign:
            authenticate(blob)
        with self.assertRaisesRegex(GateError, code):
            self.parse_pack(blob)

    def test_tier_zero_authenticated_directory(self):
        blob = wire_pack((A, B, C), 0)
        parsed = self.parse_pack(blob)
        self.assertEqual((parsed.tier, parsed.keys, parsed.sha256), (0, {A, B, C}, blob[32:64].hex()))

    def test_tier_one_control_bit(self):
        self.assertEqual(self.parse_pack(wire_pack((A,), 1)).tier, 1)

    def test_empty_authenticated_directory(self):
        self.assertEqual(self.parse_pack(wire_pack((), 0)).keys, set())

    def test_bad_magic(self):
        blob = wire_pack((A,), 0)
        blob[0] = ord("7")
        self.reject(blob, "PACK_MAGIC", resign=True)

    def test_bad_version(self):
        blob = wire_pack((A,), 0)
        blob[4] = 0x82
        self.reject(blob, "PACK_VERSION", resign=True)

    def test_wrong_hbb_even_with_valid_sha(self):
        blob = wire_pack((A,), 0)
        blob[4] = (15 << 3) | 1
        self.reject(blob, "PACK_HBB", resign=True)

    def test_wrong_chip(self):
        blob = wire_pack((A,), 0)
        blob[24] ^= 1
        self.reject(blob, "PACK_IDENTITY", resign=True)

    def test_excessive_bucket_log(self):
        blob = wire_pack((A,), 0)
        blob[5] = 17
        self.reject(blob, "PACK_INDEX_SIZE", resign=True)

    def test_directory_exceeding_index_load(self):
        blob = wire_pack((A,), 0)
        struct.pack_into("<H", blob, 6, 5)
        self.reject(blob, "PACK_INDEX_SIZE", resign=True)

    def test_hash_field_tampered(self):
        blob = wire_pack((A,), 0)
        blob[32] ^= 1
        self.reject(blob, "PACK_SHA256_MISMATCH")

    def test_isa_payload_tampered(self):
        blob = wire_pack((A,), 0)
        blob[-128] ^= 1
        self.reject(blob, "PACK_SHA256_MISMATCH")

    def test_truncated_header(self):
        self.reject(wire_pack((A,), 0)[:63], "PACK_LENGTH")

    def test_truncated_or_appended_payload(self):
        blob = wire_pack((A,), 0)
        self.reject(blob[:-1], "PACK_PAYLOAD_LENGTH")
        self.reject(blob + b"\x00", "PACK_PAYLOAD_LENGTH")

    def test_duplicate_directory_key(self):
        self.reject(wire_pack((A, A), 0), "PACK_DIRECTORY_KEY_OR_RESERVED")

    def test_unsorted_directory_key(self):
        self.reject(wire_pack((B, A), 0), "PACK_DIRECTORY_KEY_OR_RESERVED")

    def test_zero_directory_key(self):
        self.reject(wire_pack(("0" * 64,), 0), "PACK_DIRECTORY_KEY_OR_RESERVED")

    def test_nonzero_index_padding(self):
        blob = wire_pack((A,), 0)
        blob[97] = 1
        self.reject(blob, "PACK_INDEX_PADDING", resign=True)

    def test_unknown_bucket_id(self):
        blob = wire_pack((A,), 0)
        slots = struct.unpack_from("<8I", blob, 64)
        struct.pack_into("<I", blob, 64 + slots.index(0) * 4, 3)
        self.reject(blob, "PACK_BUCKET_DIRECTORY_IDS", resign=True)

    def test_unreachable_key_behind_empty_bucket(self):
        blob = wire_pack((A,), 0)
        slots = struct.unpack_from("<8I", blob, 64)
        slot = slots.index(0)
        struct.pack_into("<I", blob, 64 + slot * 4, 0xFFFFFFFF)
        struct.pack_into("<I", blob, 64 + ((slot + 1) % 8) * 4, 0)
        self.reject(blob, "PACK_UNREACHABLE_DIRECTORY_KEY", resign=True)

    def test_rehashed_entry_cannot_point_outside_payload(self):
        blob = wire_pack((A,), 0)
        struct.pack_into("<I", blob, DIRECTORY + 40, 0xFFFF0000)
        self.reject(blob, "PACK_DIRECTORY_PAYLOAD", resign=True)

    def test_rehashed_directory_reserved_bytes_reject(self):
        blob = wire_pack((A,), 0)
        blob[DIRECTORY + 68] = 1
        self.reject(blob, "PACK_DIRECTORY_KEY_OR_RESERVED", resign=True)


class CorpusTests(CachedCase):
    def setUp(self):
        super().setUp()
        self.pack = self.cache / "pack.bin"
        self.pack.write_bytes(wire_pack((A, B), 0))
        self.corpus = self.cache / "corpus.log"
        self.output = self.cache / "report.json"

    def write_records(self, *records) -> None:
        self.corpus.write_text("\n".join(json.dumps(row) if isinstance(row, dict) else row
                                         for row in records) + "\n", encoding="utf-8")

    def run_audit(self, top_n: int = 10) -> dict:
        return audit.run_audit(self.pack, self.corpus, self.output, top_n)

    def command(self, output: Path | None = None) -> subprocess.CompletedProcess:
        return self.cli("corpus_audit.py", "--pack", self.pack, "--corpus", self.corpus,
                        "--json-out", self.output if output is None else output)

    def test_all_tier_zero_hits_have_full_coverage(self):
        self.write_records(record(A), record(B), record(A.upper(), kind="render_server"))
        report = self.run_audit()
        self.assertEqual((report["tier0_hit_rate"], report["tier1_hit_rate"]), (1.0, 0.0))
        self.assertEqual(report["pack_coverage"]["records_in_directory"], 3)
        self.assertEqual(report["cold_compile_misses"], 0)
        self.assertEqual(report["cold_compile_time_share"], 0.0)

    def test_all_tier_one_hits(self):
        self.pack.write_bytes(wire_pack((A, B), 1))
        self.write_records(record(A, "tier-1"), record(B, 1))
        report = self.run_audit()
        self.assertEqual(report["tier1_hit_rate"], 1.0)
        self.assertEqual(report["pack_coverage"]["reported_hits_absent_from_supplied_tier"], 0)

    def test_mixed_jsonl_and_key_value_stats(self):
        self.write_records(record(A, 0, 3), f"process_kind=app compile_us=1.25 hit_tier=1 pipeline_key_sha256={B}",
                           record(C, "MISS", 4.25))
        report = self.run_audit()
        self.assertAlmostEqual(report["tier0_hit_rate"], 1 / 3)
        self.assertAlmostEqual(report["tier1_hit_rate"], 1 / 3)
        self.assertAlmostEqual(report["cold_compile_time_share"], 0.5)
        self.assertEqual((report["compile_us"], report["cold_compile_us"]), (8.5, 4.25))

    def test_frequency_and_elapsed_time_rank_differently(self):
        self.write_records(record(A), record(B, "miss", 4), record(B, -1, 6), record(C, None, 15))
        report = self.run_audit()
        self.assertEqual(report["top_misses_by_count"][0],
                         {"pipeline_key_sha256": B, "miss_count": 2, "compile_us": 10})
        self.assertEqual(report["top_misses_by_compile_us"][0],
                         {"pipeline_key_sha256": C, "miss_count": 1, "compile_us": 15})

    def test_top_n_and_final_key_tiebreak(self):
        self.write_records(record(C, "miss", 4), record(B, "miss", 4), record(A, "miss", 4))
        report = self.run_audit(2)
        for field in ("top_misses_by_count", "top_misses_by_compile_us"):
            self.assertEqual([item["pipeline_key_sha256"] for item in report[field]], [A, B])

    def test_candidate_pack_coverage_keeps_logged_tier_counts(self):
        self.write_records(record(A, "miss", 4), record(C, 0), record(B, 1))
        report = self.run_audit()
        self.assertEqual(report["pack_coverage"], {"records_in_directory": 2, "miss_records_in_directory": 1,
                                                   "reported_hits_absent_from_supplied_tier": 1})
        self.assertEqual(report["cold_compile_misses"], 1)

    def test_invalid_pipeline_hash_rejected(self):
        self.write_records(record("g" * 64))
        with self.assertRaisesRegex(GateError, r"CORPUS_INVALID_RECORD line=1"):
            self.run_audit()

    def test_sha_case_normalizes_for_coverage_and_grouping(self):
        key = "ab" * 32
        self.pack.write_bytes(wire_pack((key,), 0))
        self.write_records(record(key.upper()), record(key, "miss", 3), record(key.upper(), "miss", 5))
        report = self.run_audit()
        self.assertEqual(report["pack_coverage"]["records_in_directory"], 3)
        self.assertEqual(report["top_misses_by_count"], [
            {"pipeline_key_sha256": key, "miss_count": 2, "compile_us": 8}])

    def test_invalid_tier_rejected(self):
        for tier in (True, "warm", 2):
            with self.subTest(tier=tier):
                self.write_records(record(A, tier))
                with self.assertRaises(GateError):
                    self.run_audit()

    def test_invalid_time_rejected(self):
        for invalid in (-1, float("inf"), float("nan"), 1 << 64):
            with self.subTest(invalid=invalid):
                self.write_records(record(A, 0, invalid))
                with self.assertRaises(GateError):
                    self.run_audit()

    def test_empty_corpus_rejected(self):
        self.write_records("")
        with self.assertRaisesRegex(GateError, "CORPUS_EMPTY"):
            self.run_audit()

    def test_zero_top_n_rejected(self):
        self.write_records(record())
        with self.assertRaisesRegex(GateError, "TOP_N_MUST_BE_POSITIVE"):
            self.run_audit(0)

    def test_duplicate_json_fields_rejected(self):
        line = json.dumps(record())
        self.write_records(line[:-1] + ', "hit_tier": 1}')
        with self.assertRaisesRegex(GateError, "CORPUS_INVALID_RECORD"):
            self.run_audit()

    def test_duplicate_key_value_fields_rejected(self):
        self.write_records(f"pipeline_key_sha256={A} hit_tier=0 compile_us=0 process_kind=app compile_us=3")
        with self.assertRaisesRegex(GateError, "CORPUS_INVALID_RECORD"):
            self.run_audit()

    def test_unknown_log_field_rejected_without_echo(self):
        self.write_records(dict(record(), extra="DO_NOT_COPY_PRIVATE_TEXT"))
        result = self.command()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("DO_NOT_COPY_PRIVATE_TEXT", result.stdout + result.stderr)
        self.assertFalse(self.output.exists())

    def test_private_path_in_kind_rejected_without_echo(self):
        private = "/private-example/anonymous-source"
        self.write_records(record(kind=private))
        result = self.command()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(private, result.stdout + result.stderr)

    def test_cli_writes_only_anonymous_report(self):
        self.write_records(record(C, "miss", 5, "local_anonymous_bucket"))
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), json.loads(self.output.read_text(encoding="utf-8")))
        self.assertNotIn("local_anonymous_bucket", result.stdout)
        self.assertNotIn(str(self.pack), result.stdout)
        self.assertNotIn(str(self.corpus), result.stdout)

    def test_reject_windows_and_worktree_output(self):
        self.write_records(record())
        for output in (Path("C:/blocked.json"), REPO / "blocked-w06.json", CACHE_PARENT / "blocked-w06.json"):
            with self.subTest(output=output):
                result = self.command(output)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(json.loads(result.stdout)["error"], "OUTPUT_OUTSIDE_PROJECT_CACHE")

    def test_reject_symlink_escaping_cache(self):
        self.write_records(record())
        self.output.symlink_to(REPO / "blocked-w06.json")
        result = self.command()
        self.assertEqual(result.returncode, 1)
        self.assertTrue(self.output.is_symlink())

    def test_report_cannot_overwrite_input(self):
        self.write_records(record())
        original = self.pack.read_bytes()
        result = self.command(self.pack)
        self.assertEqual(json.loads(result.stdout)["error"], "OUTPUT_ALIASES_INPUT")
        self.assertEqual(self.pack.read_bytes(), original)

    def test_failed_input_keeps_previous_report(self):
        self.write_records(record())
        self.run_audit()
        original = self.output.read_bytes()
        self.pack.write_bytes(b"truncated")
        result = self.command()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.output.read_bytes(), original)


def samples(error: str = "0", note: str = "cpu_reference_only", path: str = "raw") -> str:
    return "".join(f"HWTEST case={case} status=PASS path={path} max_err={error} mismatched_px=0 "
                   f"time_us=23 note={note}\n" for case in gate.CASES)


def wave_report(with_clang: bool = True) -> dict:
    names = ["py_compile", "contracts", "driver_constants", "unittest", "pm4_xml_index",
             "pm4_decode", *gate.SUITES, "no_fallback"]
    if with_clang:
        names += [suite + ":clang18" for suite in gate.CLANG_SUITES]
    return {"status": "PASS", "with_clang": with_clang,
            "results": [{"name": name, "status": "PASS"} for name in names]}


class HostGateTests(CachedCase):
    def setUp(self):
        super().setUp()
        self.predicates = hwtest_parser.load_predicates(REPO / "contracts/DualPathPredicates.tsv")

    def stub_suites(self, **changes):
        methods = {method: mock.DEFAULT for method in
                   ("tool", "kmd", "pack", "gmem", "pixel_hwtest", "corpus", "no_fallback", "wave")}
        methods.update(changes)
        return mock.patch.multiple(gate.HostRun, **methods)

    def test_ca_glass_one_lsb_passes_and_parser_globals_stay_fixed(self):
        initial = hwtest_parser.CASES
        result = gate.check_ca_glass_samples(samples("1"), self.predicates)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["measurement_kind"], "CPU_REFERENCE_FIXTURE")
        self.assertIs(result["hardware_acceptance"], False)
        self.assertEqual(hwtest_parser.CASES, initial)

    def test_ca_glass_more_than_one_lsb_rejected(self):
        with self.assertRaisesRegex(GateError, "ERROR_TOLERANCE_EXCEEDED"):
            gate.check_ca_glass_samples(samples("1.01"), self.predicates)

    def test_ca_glass_missing_duplicate_and_truncated_records_rejected(self):
        both = samples()
        for invalid in (both.splitlines(keepends=True)[0], both + both, both[:-1]):
            with self.subTest(invalid=invalid):
                with self.assertRaises(GateError):
                    gate.check_ca_glass_samples(invalid, self.predicates)

    def test_ca_glass_rejects_non_host_provenance(self):
        with self.assertRaisesRegex(GateError, "HWTEST_CPU_POLICY_FAILED"):
            gate.check_ca_glass_samples(samples(note="device_like_sample"), self.predicates)

    def test_ca_glass_rejects_registered_fallback(self):
        route = "fallback:" + min(self.predicates)
        with self.assertRaisesRegex(GateError, "FALLBACK_FORBIDDEN"):
            gate.check_ca_glass_samples(samples(path=route), self.predicates)

    def test_wave_all_fourteen_required_with_clang(self):
        self.assertEqual(len(gate.check_wave_report(wave_report(), True)), 14)

    def test_wave_rejects_skipped_or_failed_required_gate(self):
        report = wave_report()
        for status in ("SKIP_NOT_YET_MERGED", "FAIL"):
            with self.subTest(status=status):
                report["results"][0]["status"] = status
                with self.assertRaisesRegex(GateError, "VERIFY_WAVE_INCOMPLETE"):
                    gate.check_wave_report(report, True)

    def test_wave_rejects_duplicate_gate_and_clang_mismatch(self):
        report = wave_report()
        report["results"][-1] = report["results"][0]
        with self.assertRaisesRegex(GateError, "VERIFY_WAVE_INCOMPLETE"):
            gate.check_wave_report(report, True)
        with self.assertRaisesRegex(GateError, "VERIFY_WAVE_INCOMPLETE"):
            gate.check_wave_report(wave_report(False), True)

    def test_malformed_wave_json_is_rejected_as_a_gate_failure(self):
        with self.assertRaisesRegex(GateError, "VERIFY_WAVE_INCOMPLETE"):
            gate.check_wave_report([], True)

    def test_child_failure_is_recorded_and_host_ready_is_false(self):
        failure = mock.Mock(side_effect=GateError("bad pack"))
        with self.stub_suites(pack=failure) as stubs:
            report = gate.run_gate(REPO, MESA, self.cache / "quick")
            stubs["wave"].assert_not_called()
        self.assertEqual(report["status"], "FAIL")
        self.assertIs(report["phase3_host_ready"], False)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        self.assertEqual(next(row["error"] for row in report["results"] if row["name"] == "pack_v1"), "bad pack")
        self.assertEqual(json.loads(Path(report["summary"]).read_text()), report)

    def test_full_uses_wave_with_clang_and_propagates_failure(self):
        fail_wave = mock.Mock(side_effect=GateError("missing wave report"))
        with self.stub_suites(wave=fail_wave):
            report = gate.run_gate(REPO, MESA, self.cache / "full", "full", True)
        fail_wave.assert_called_once()
        self.assertIs(fail_wave.call_args.args[1], True)
        self.assertIs(report["verify_wave_skipped"], False)
        self.assertFalse(report["phase3_host_ready"])
        self.assertIs(report["hardware_acceptance"], False)

    def test_optional_suite_presence_distinguishes_missing_and_broken_link(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        row = {}
        self.assertFalse(run.present(row, "missing.cpp"))
        self.assertEqual(row["optional_suite"]["status"], "NOT_PRESENT")
        (self.cache / "broken.cpp").symlink_to(self.cache / "missing.cpp")
        self.assertTrue(run.present(row, "broken.cpp"))
        self.assertEqual(row["optional_suite"]["status"], "PRESENT")

    def test_cli_failure_preserves_both_evidence_flags(self):
        result = self.cli("phase3_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--cache-dir", REPO / "blocked-w06", "--mode", "quick")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIs(report["phase3_host_ready"], False)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)

    def test_real_quick_gate_builds_runs_and_keeps_host_evidence_boundary(self):
        output = self.cache / "quick"
        result = self.cli("phase3_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--cache-dir", output, "--mode", "quick", timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        summary = json.loads(result.stdout)
        self.assertIs(summary["phase3_host_ready"], True)
        self.assertIs(summary["device_verified"], False)
        self.assertIs(summary["hardware_acceptance"], False)
        self.assertIs(summary["verify_wave_skipped"], True)
        suites = {row["name"]: row for row in summary["results"]}
        self.assertEqual(set(suites), {"contracts", "driver_constants", "kmd_phase3", "pack_v1",
                                       "gmem", "pixel_hwtest", "corpus_audit", "no_fallback"})
        self.assertTrue(all(suite["status"] == "PASS" for suite in suites.values()))
        self.assertEqual(suites["pixel_hwtest"]["sample_counts"]["gcc"],
                         {"phase3": 6, "ca_glass": 2, "joint_phase2_phase3": 14})
        self.assertTrue(suites["gmem"]["streams"])
        self.assertTrue(all(stream["packets"] > 0 for stream in suites["gmem"]["streams"]))
        self.assertGreater(suites["no_fallback"]["symbols_scanned"], 0)
        self.assertEqual(suites["corpus_audit"]["records"], 5)
        saved = json.loads((output / "phase3_host_summary.json").read_text(encoding="utf-8"))
        kmd = next(row for row in saved["results"] if row["name"] == "kmd_phase3")
        self.assertTrue(any(run["command"][:3] == ["setarch", "x86_64", "-R"] and
                            run["command"][-2:] == ["--tsan-stress", "20"] and run["returncode"] == 0
                            for run in kmd["commands"]))


if __name__ == "__main__":
    unittest.main()
