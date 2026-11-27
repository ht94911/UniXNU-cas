#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 4 route, safety, CLI and real C++ oracle tests; standard library only.

Temporary fixtures and compiler outputs stay in .cache/w07-l4. A source/hash
bound sanitizer binary can be reused from the mandatory C++ gate; otherwise
the bridge compiles it once. Every bridge run actually executes the binary
and audits its five CPU-only HWTEST samples.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import contextlib
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import fallback_audit as audit

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/verify/fallback_audit.py"
PREDICATES = REPO / "contracts/DualPathPredicates.tsv"
TEST_CACHE = audit.CACHE_ROOT / "unittest"
EXPECTED_PREDICATES = {
    "raw:unimplemented.global", "raw:blit.unsupported_format", "raw:blit.3d_or_compressed",
    "raw:compute.uncompiled_kernel", "raw:compute.indirect_dispatch", "raw:render.gmem_required",
    "raw:render.ubwc_attachment", "raw:render.ycbcr_or_xr10", "raw:render.fbfetch_blend",
    "raw:render.tess_or_amplify", "raw:render.indirect_or_icb", "raw:render.msaa_resolve",
    "raw:resource.heap_aliasing", "raw:resource.sparse_texture", "raw:compiler.daemon_unavailable",
    "raw:sync.shared_event_cross_proc",
}
HEAP = "raw:resource.heap_aliasing"
ICB = "raw:render.indirect_or_icb"
COMPUTE = "raw:compute.indirect_dispatch"


def route(proc="SpringBoard", path="raw", count=None, scene=None) -> str:
    return (f"ROUTE proc={proc} path={path}" + (f" count={count}" if count is not None else "") +
            (f" scene={scene}" if scene is not None else "") + "\n")


def zero_log() -> str:
    return route("backboardd", count=10, scene="home") + route(count=20, scene="home") + route("Camera", count=5, scene="camera")


def hwtest(case="gpu.heap.aliasing", path="raw", status="PASS", maximum="0", mismatches="0",
           elapsed="1", note="cpu_reference_only") -> str:
    return (f"HWTEST case={case} status={status} path={path} max_err={maximum} "
            f"mismatched_px={mismatches} time_us={elapsed} note={note}\n")


def safe_env(directory: Path) -> dict:
    env = os.environ.copy()
    env.update(TMPDIR=str(directory), TEMP=str(directory), TMP=str(directory),
               PYTHONPYCACHEPREFIX=str(directory / "pycache"), PYTHONDONTWRITEBYTECODE="1",
               ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return env


class AuditTestBase(unittest.TestCase):
    def setUp(self) -> None:
        self.assertEqual(audit.CACHE_ROOT.resolve(), audit.CACHE_ROOT)
        TEST_CACHE.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix=self.__class__.__name__ + "-", dir=TEST_CACHE)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = safe_env(self.directory)
        self.catalog = audit.load_predicates(PREDICATES)

    def put(self, name: str, text: str) -> Path:
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def parse(self, text: str, **kwargs) -> dict:
        return audit.audit_lines(text, self.catalog, **kwargs)

    def codes(self, text: str, **kwargs) -> set[str]:
        return {row["code"] for row in self.parse(text, **kwargs)["errors"]}

    def cli(self, *args) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(SCRIPT), *map(str, args)],
                              cwd=REPO, env=self.env, text=True, capture_output=True,
                              timeout=60, check=False)

    def assert_host_only(self, report: dict) -> None:
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        self.assertEqual(report["source_evidence_kind"], "host_log_audit")


class ContractTests(AuditTestBase):
    def test_all_sixteen_exact_predicate_names_loaded(self) -> None:
        self.assertEqual(set(self.catalog), EXPECTED_PREDICATES)
        self.assertEqual(len(self.catalog), 16)

    def test_metadata_is_loaded_from_the_frozen_tsv(self) -> None:
        with PREDICATES.open(encoding="utf-8", newline="") as source:
            expected = {row["predicate"]: row for row in csv.DictReader(source, delimiter="\t")}
        for predicate, info in self.catalog.items():
            self.assertEqual(info.owner_lane, expected[predicate]["owner_lane"])
            self.assertEqual(info.target_phase, expected[predicate]["target_phase"])
        self.assertEqual((self.catalog[HEAP].owner_lane, self.catalog[HEAP].target_phase), ("L3", "Phase4"))
        self.assertEqual(self.catalog["raw:resource.sparse_texture"].owner_lane, "L1")
        self.assertEqual(self.catalog["raw:compiler.daemon_unavailable"].owner_lane, "L2")

    def test_all_sixteen_registered_routes_are_reported(self) -> None:
        text = "".join(route("NezhaGpuTest", "fallback:" + p) for p in sorted(EXPECTED_PREDICATES))
        report = self.parse(text, targets=("NezhaGpuTest",), allow_fallbacks=True, top_n=16)
        self.assertEqual(report["status"], "PASS")
        self.assertFalse(report["zero_fallback_passed"])
        self.assertEqual(report["fallback_count"], 16)
        self.assertEqual({row["predicate"] for row in report["top_predicates"]}, EXPECTED_PREDICATES)

    def test_missing_contract_row_is_rejected(self) -> None:
        text = PREDICATES.read_text().splitlines(keepends=True)
        with self.assertRaisesRegex(audit.AuditError, "REQUIRES_16"):
            audit.load_predicates(self.put("missing.tsv", "".join(text[:-1])))

    def test_duplicate_contract_row_is_rejected(self) -> None:
        text = PREDICATES.read_text().splitlines(keepends=True)
        text[-1] = text[1]
        with self.assertRaisesRegex(audit.AuditError, "CONTRACT_DUPLICATE"):
            audit.load_predicates(self.put("duplicate.tsv", "".join(text)))

    def test_malformed_contract_header_is_rejected(self) -> None:
        text = PREDICATES.read_text().replace("owner_lane", "owner", 1)
        with self.assertRaisesRegex(audit.AuditError, "CONTRACT_HEADER"):
            audit.load_predicates(self.put("header.tsv", text))

    def test_changed_valid_metadata_cannot_replace_frozen_contract(self) -> None:
        text = PREDICATES.read_text().replace("\tL3\tPhase4", "\tL4\tPhase4", 1)
        with self.assertRaisesRegex(audit.AuditError, "FROZEN_SHA256"):
            audit.load_predicates(self.put("replaced.tsv", text))

    def test_seventeenth_valid_predicate_is_rejected(self) -> None:
        text = PREDICATES.read_text() + "raw:render.new_feature\trender\tL3\tPhase4\topen\tUndeclared feature\n"
        with self.assertRaisesRegex(audit.AuditError, "REQUIRES_16"):
            audit.load_predicates(self.put("extra.tsv", text))

    def test_contract_read_error_does_not_echo_path(self) -> None:
        with self.assertRaises(audit.AuditError) as caught:
            audit.load_predicates(self.directory / "private-contract-input.tsv")
        self.assertEqual(str(caught.exception), "CONTRACT_READ_FAILED")


class RecordTests(AuditTestBase):
    def test_route_optional_count_and_scene_defaults(self) -> None:
        report = self.parse(route(), targets=("SpringBoard",))
        self.assertEqual(report["total_submissions"], 1)
        self.assertEqual(report["by_scene"]["unlabelled"]["raw_count"], 1)
        self.assertTrue(report["zero_fallback_passed"])

    def test_timestamp_prefix_and_unrelated_noise_are_supported(self) -> None:
        report = self.parse("boot ok\n[12:00:01] " + zero_log())
        self.assertEqual(report["total_submissions"], 35)
        self.assertEqual(report["sources"][0]["ignored_lines"], 1)
        self.assertTrue(report["zero_fallback_passed"])

    def test_unknown_predicate_in_any_process_is_fatal(self) -> None:
        report = self.parse(zero_log() + route("MPS", "fallback:raw:compute.undeclared"))
        self.assertEqual(report["unknown_predicate_count"], 1)
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["zero_fallback_passed"])

    def test_unknown_predicate_in_noise_cannot_hide(self) -> None:
        report = self.parse(zero_log() + "diagnostic path=fallback:raw:render.undeclared\n")
        self.assertIn("UNKNOWN_PREDICATE", {row["code"] for row in report["errors"]})
        self.assertFalse(report["zero_fallback_passed"])

    def test_unknown_predicate_is_fatal_even_in_report_mode(self) -> None:
        report = self.parse(zero_log() + route("MPS", "fallback:raw:compute.undeclared"), allow_fallbacks=True)
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["counts_complete"])

    def test_shortened_or_noncontract_route_is_rejected(self) -> None:
        self.assertIn("INVALID_ROUTE", self.codes(zero_log() + route("MPS", "fallback:heap_aliasing")))

    def test_duplicate_field_is_rejected(self) -> None:
        self.assertIn("UNKNOWN_OR_DUPLICATE_FIELD", self.codes(zero_log() + "ROUTE proc=MPS path=raw count=1 count=2\n"))

    def test_missing_path_field_is_rejected(self) -> None:
        self.assertIn("MISSING_FIELDS", self.codes(zero_log() + "ROUTE proc=MPS count=1\n"))

    def test_unknown_field_cannot_forge_hardware_acceptance(self) -> None:
        report = self.parse(zero_log() + "ROUTE proc=MPS path=raw device_verified=true hardware_acceptance=true\n")
        self.assertEqual(report["status"], "FAIL")
        self.assert_host_only(report)

    def test_zero_route_count_cannot_fabricate_process_coverage(self) -> None:
        self.assertIn("INTEGER_RANGE", self.codes(zero_log() + route("MPS", count=0)))

    def test_negative_signed_fractional_and_nonfinite_counts_rejected(self) -> None:
        for count in ("-1", "+1", "1.5", "nan", "inf"):
            with self.subTest(count=count):
                self.assertIn("INVALID_INTEGER", self.codes(zero_log() + route("MPS", count=count)))

    def test_count_uint64_overflow_rejected(self) -> None:
        self.assertIn("INTEGER_RANGE", self.codes(zero_log() + route("MPS", count=1 << 64)))

    def test_uint64_maximum_count_is_exact_python_integer(self) -> None:
        report = self.parse(route(count=(1 << 64) - 1), targets=("SpringBoard",))
        self.assertEqual(report["raw_count"], (1 << 64) - 1)
        self.assertEqual(report["raw_ratio"], 1)

    def test_invalid_process_or_scene_tokens_rejected(self) -> None:
        self.assertIn("INVALID_PROCESS_OR_SCENE", self.codes(zero_log() + route("MPS|extra")))
        self.assertIn("INVALID_PROCESS_OR_SCENE", self.codes(zero_log() + route("MPS", scene="<scene>")))

    def test_truncated_route_and_hwtest_are_rejected(self) -> None:
        self.assertIn("TRUNCATED_RECORD", self.codes(zero_log() + route("MPS").rstrip("\n")))
        self.assertIn("TRUNCATED_RECORD", self.codes(zero_log() + hwtest().rstrip("\n")))

    def test_two_record_markers_cannot_share_a_line(self) -> None:
        text = "ROUTE proc=MPS path=raw HWTEST case=gpu.heap.aliasing\n"
        self.assertIn("AMBIGUOUS_RECORD", self.codes(zero_log() + text))

    def test_crlf_route_records_supported(self) -> None:
        self.assertTrue(self.parse(zero_log().replace("\n", "\r\n"))["zero_fallback_passed"])

    def test_hwtest_samples_are_separated_from_runtime_counts(self) -> None:
        text = route("NezhaGpuTest", count=7) + hwtest()
        report = self.parse(text, targets=("NezhaGpuTest",))
        self.assertEqual((report["total_submissions"], report["runtime_submissions"], report["hwtest_samples"]), (8, 7, 1))
        self.assertEqual(report["cpu_reference_samples"], 1)
        self.assertEqual(report["hwtest_cases"], {"gpu.heap.aliasing": 1})
        self.assert_host_only(report)

    def test_hwtest_registered_fallback_is_counted(self) -> None:
        report = self.parse(hwtest(path="fallback:" + HEAP), targets=("NezhaGpuTest",), allow_fallbacks=True)
        self.assertEqual(report["fallback_count"], 1)
        self.assertFalse(report["zero_fallback_passed"])

    def test_hwtest_failure_status_is_fatal_in_report_mode(self) -> None:
        self.assertIn("HWTEST_FAILED", self.codes(zero_log() + hwtest(status="FAIL"), allow_fallbacks=True))

    def test_hwtest_pixel_mismatch_is_fatal(self) -> None:
        self.assertIn("HWTEST_PIXEL_MISMATCH", self.codes(zero_log() + hwtest(mismatches="1")))

    def test_phase4_exact_oracles_reject_one_lsb(self) -> None:
        for case in set(audit.PHASE4_TOLERANCES) - {"gpu.argbuf.bindless"}:
            with self.subTest(case=case):
                self.assertIn("HWTEST_ERROR_TOLERANCE", self.codes(zero_log() + hwtest(case=case, maximum="1")))

    def test_exact_zero_rejects_float_underflow_decimal(self) -> None:
        self.assertIn("HWTEST_ERROR_TOLERANCE", self.codes(zero_log() + hwtest(maximum="1e-9999")))

    def test_argbuf_one_lsb_limit_and_two_lsb_failure(self) -> None:
        self.assertEqual(self.codes(zero_log() + hwtest(case="gpu.argbuf.bindless", maximum="1")), set())
        self.assertIn("HWTEST_ERROR_TOLERANCE", self.codes(zero_log() + hwtest(case="gpu.argbuf.bindless", maximum="2")))

    def test_hwtest_nonfinite_metrics_rejected(self) -> None:
        for maximum in ("nan", "inf", "1e9999", "-1"):
            with self.subTest(maximum=maximum):
                self.assertIn("INVALID_HWTEST_METRICS", self.codes(zero_log() + hwtest(maximum=maximum)))


class AggregateTests(AuditTestBase):
    def multi_log(self) -> str:
        return (zero_log() + route("mediaserverd", count=3, scene="camera") +
                route("mediaserverd", "fallback:" + ICB, 7, "camera") +
                route("CoreImage", count=4, scene="home") + route("CoreImage", "fallback:" + HEAP, 2, "home") +
                route("MPS", count=6, scene="training") + route("MPS", "fallback:" + COMPUTE, 1, "training") +
                route("NezhaGpuTest", count=3, scene="tests"))

    def test_seven_processes_and_scenes_exact_totals(self) -> None:
        report = self.parse(self.multi_log())
        self.assertEqual((report["total_submissions"], report["raw_count"], report["fallback_count"]), (61, 51, 10))
        self.assertAlmostEqual(report["raw_ratio"], 51 / 61)
        self.assertEqual(len(report["by_process"]), 7)
        self.assertEqual(report["by_scene"]["home"]["total_submissions"], 36)
        self.assertEqual(report["by_scene"]["camera"]["total_submissions"], 15)
        self.assertEqual(report["by_process_scene"]["CoreImage"]["home"]["fallback_count"], 2)

    def test_top_predicates_frequency_owner_phase_and_shares(self) -> None:
        report = self.parse(self.multi_log())
        top = report["top_predicates"]
        self.assertEqual([row["predicate"] for row in top], [ICB, HEAP, COMPUTE])
        self.assertEqual([row["count"] for row in top], [7, 2, 1])
        self.assertEqual((top[0]["owner_lane"], top[0]["target_phase"]), ("L3", "Phase4"))
        self.assertEqual(top[0]["fallback_share"], .7)
        self.assertAlmostEqual(top[0]["submission_share"], 7 / 61)

    def test_ties_are_lexical_and_independent_of_input_order(self) -> None:
        lines = [route("MPS", "fallback:" + predicate, 2, "compute") for predicate in (ICB, HEAP, COMPUTE)]
        a = self.parse(zero_log() + "".join(lines))
        b = self.parse(zero_log() + "".join(reversed(lines)))
        self.assertEqual(a["top_predicates"], b["top_predicates"])
        self.assertEqual([row["predicate"] for row in a["top_predicates"]], sorted((ICB, HEAP, COMPUTE)))

    def test_top_n_does_not_hide_total_fallbacks(self) -> None:
        report = self.parse(self.multi_log(), top_n=1)
        self.assertEqual(len(report["top_predicates"]), 1)
        self.assertEqual(report["fallback_count"], 10)

    def test_default_three_zero_targets_pass_with_observed_raw_routes(self) -> None:
        report = self.parse(zero_log())
        self.assertTrue(report["zero_fallback_passed"])
        self.assertEqual(report["zero_fallback_procs"], ["backboardd", "SpringBoard", "Camera"])
        self.assert_host_only(report)

    def test_one_target_fallback_blocks_zero_gate(self) -> None:
        report = self.parse(zero_log() + route("Camera", "fallback:" + HEAP))
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["zero_fallback_passed"])
        self.assertEqual(report["by_process"]["Camera"]["fallback_count"], 1)

    def test_report_mode_retains_failed_zero_flag(self) -> None:
        report = self.parse(zero_log() + route("Camera", "fallback:" + HEAP), allow_fallbacks=True)
        self.assertEqual(report["status"], "PASS")
        self.assertFalse(report["zero_fallback_passed"])
        self.assert_host_only(report)

    def test_registered_nontarget_fallback_is_reported_without_blocking_targets(self) -> None:
        report = self.parse(self.multi_log())
        self.assertTrue(report["zero_fallback_passed"])
        self.assertEqual(report["fallback_count"], 10)

    def test_missing_target_coverage_cannot_pass_zero(self) -> None:
        report = self.parse(route())
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["zero_fallback_passed"])
        self.assertEqual(report["missing_target_procs"], ["Camera", "backboardd"])

    def test_empty_and_noise_only_logs_fail_without_nan_ratios(self) -> None:
        for text in ("", "boot ready\n"):
            with self.subTest(text=text):
                report = self.parse(text)
                self.assertEqual(report["status"], "FAIL")
                self.assertFalse(report["zero_fallback_passed"])
                self.assertEqual(report["raw_ratio"], 0)

    def test_custom_zero_targets_are_checked_exactly(self) -> None:
        report = self.parse(self.multi_log(), targets=("MPS", "CoreImage"))
        self.assertFalse(report["zero_fallback_passed"])
        self.assertEqual({row["proc"] for row in report["zero_fallback_blockers"]}, {"MPS", "CoreImage"})

    def test_empty_or_duplicate_zero_targets_rejected(self) -> None:
        for targets in ("", "SpringBoard,", "SpringBoard,SpringBoard"):
            with self.subTest(targets=targets), self.assertRaises(audit.AuditError):
                self.parse(zero_log(), targets=targets)

    def test_nonpositive_and_boolean_top_n_rejected(self) -> None:
        for top_n in (0, -1, True):
            with self.subTest(top_n=top_n), self.assertRaises(audit.AuditError):
                self.parse(zero_log(), top_n=top_n)

    def test_multiple_log_files_have_separate_evidence_hashes(self) -> None:
        a = self.put("one.log", zero_log())
        b = self.put("two.log", route("MPS", "fallback:" + COMPUTE, 4, "compute"))
        report = audit.audit_logs([a, b], self.catalog)
        self.assertEqual(report["total_submissions"], 39)
        self.assertEqual(len(report["sources"]), 2)
        self.assertEqual(report["sources"][0]["sha256"], hashlib.sha256(a.read_bytes()).hexdigest())
        self.assertEqual(report["sources"][1]["sha256"], hashlib.sha256(b.read_bytes()).hexdigest())


class SafetyTests(AuditTestBase):
    def test_all_four_required_private_field_names_rejected(self) -> None:
        for name in ("udid", "ecid", "serial_number", "ap_nonce"):
            with self.subTest(name=name), self.assertRaisesRegex(audit.AuditError, "REDACTION_REQUIRED"):
                self.parse(zero_log() + f"ROUTE proc=Camera path=raw {name}=private-value\n")

    def test_private_field_in_ignored_noise_is_still_rejected(self) -> None:
        with self.assertRaisesRegex(audit.AuditError, "REDACTION_REQUIRED"):
            self.parse(zero_log() + '{"udid": "never-publish-this"}\n')

    def test_private_field_case_and_camel_variants_rejected(self) -> None:
        for name in ("UDID", "ECID", "serialNumber", "apNonce", "AP_NONCE", "serial-number"):
            with self.subTest(name=name), self.assertRaises(audit.AuditError):
                self.parse(zero_log() + f"metadata {name}=hidden\n")

    def test_c_drive_absolute_and_drive_relative_outputs_rejected(self) -> None:
        for path in (r"C:\report.json", "C:/report.json", "c:report.json", r"\\?\C:\report.json"):
            with self.subTest(path=path), self.assertRaisesRegex(audit.AuditError, "C_DRIVE"):
                audit.checked_output_path(path)

    def test_embedded_relative_c_drive_outputs_rejected(self) -> None:
        for path in ("./C:report.json", "../C:/report.json", r"..\C:\report.json", "nested/C:report.json"):
            with self.subTest(path=path), self.assertRaisesRegex(audit.AuditError, "C_DRIVE"):
                audit.checked_output_path(path)

    def test_mnt_c_case_slash_and_dot_variants_rejected(self) -> None:
        for path in ("/mnt/c/report.json", "/MNT/C/report.json", "/mnt/./c/report.json",
                     "/mnt//c/report.json", "mnt/c/report.json", r"\mnt\c\report.json"):
            with self.subTest(path=path), self.assertRaisesRegex(audit.AuditError, "C_DRIVE"):
                audit.checked_output_path(path)

    def test_resolved_path_to_c_drive_is_rejected_without_touching_c(self) -> None:
        with patch.object(audit.Path, "resolve", return_value=Path("/mnt/c/blocked.json")):
            with self.assertRaisesRegex(audit.AuditError, "C_DRIVE"):
                audit.checked_output_path(self.directory / "report.json")

    def test_output_outside_cache_rejected_before_creating_directories(self) -> None:
        with self.assertRaisesRegex(audit.AuditError, "OUTSIDE_W07"):
            audit.checked_output_path("/tmp/w07-l4-not-created/report.json")

    def test_symlink_redirect_outside_task_cache_rejected(self) -> None:
        link = self.directory / "redirect"
        link.symlink_to(audit.CACHE_ROOT.parent, target_is_directory=True)
        with self.assertRaisesRegex(audit.AuditError, "OUTSIDE_W07"):
            audit.checked_output_path(link / "report.json")

    def test_output_alias_of_log_input_is_rejected_and_preserved(self) -> None:
        path = self.put("log.txt", zero_log()); before = path.read_bytes()
        with self.assertRaisesRegex(audit.AuditError, "OUTPUT_ALIASES_INPUT"):
            audit.run_audit([path], json_out=path)
        self.assertEqual(path.read_bytes(), before)

    def test_json_and_markdown_cannot_alias_each_other(self) -> None:
        path = self.put("log.txt", zero_log())
        with self.assertRaisesRegex(audit.AuditError, "OUTPUTS_ALIAS"):
            audit.run_audit([path], json_out=self.directory / "report", md_out=self.directory / "report")

    def test_json_and_markdown_reports_force_host_evidence_flags(self) -> None:
        source = self.put("log.txt", zero_log())
        json_out, md_out = self.directory / "report.json", self.directory / "report.md"
        report = audit.run_audit([source], json_out=json_out, md_out=md_out)
        self.assertEqual(report, json.loads(json_out.read_text()))
        self.assert_host_only(report)
        self.assertIn("device_verified=false; hardware_acceptance=false", md_out.read_text())
        self.assertIn("| SpringBoard | 20 | 20 | 0 | 100.00%", md_out.read_text())

    def test_forged_noise_claims_never_change_evidence_flags(self) -> None:
        report = self.parse(zero_log() + "device_verified=true hardware_acceptance=true\n")
        self.assertTrue(report["zero_fallback_passed"])
        self.assert_host_only(report)

    def test_utf8_bom_log_is_read_without_losing_first_process(self) -> None:
        path = self.directory / "bom.log"
        path.write_bytes(b"\xef\xbb\xbf" + zero_log().encode())
        self.assertTrue(audit.audit_logs([path], self.catalog)["zero_fallback_passed"])

    def test_invalid_utf8_is_rejected_without_echoing_bytes(self) -> None:
        path = self.directory / "bad.log"; path.write_bytes(b"\xffprivate-data")
        with self.assertRaisesRegex(audit.AuditError, "LOG_READ_OR_ENCODING_FAILED"):
            audit.audit_logs([path], self.catalog)


class CliTests(AuditTestBase):
    def test_cli_multiple_inputs_json_and_markdown_end_to_end(self) -> None:
        a = self.put("a.log", zero_log())
        b = self.put("b.log", route("MPS", "fallback:" + COMPUTE, 4, "compute"))
        json_out, md_out = self.directory / "report.json", self.directory / "report.md"
        result = self.cli(a, "--input", b, "--json-out", json_out, "--md-out", md_out)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report, json.loads(json_out.read_text()))
        self.assertEqual((report["raw_count"], report["fallback_count"]), (35, 4))
        self.assertIn(COMPUTE, md_out.read_text())
        self.assert_host_only(report)

    def test_cli_one_residual_fallback_returns_nonzero_and_writes_report(self) -> None:
        path = self.put("log.txt", zero_log() + route("backboardd", "fallback:" + HEAP))
        output = self.directory / "blocked.json"
        result = self.cli(path, "--json-out", output)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(output.read_text())["zero_fallback_passed"])

    def test_cli_allow_fallbacks_report_mode_returns_zero_but_zero_flag_false(self) -> None:
        path = self.put("log.txt", zero_log() + route("Camera", "fallback:" + HEAP))
        result = self.cli(path, "--allow-fallbacks")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["zero_fallback_passed"])
        self.assert_host_only(report)

    def test_cli_unknown_predicate_cannot_be_allowed(self) -> None:
        path = self.put("log.txt", zero_log() + route("MPS", "fallback:raw:compute.not_registered"))
        result = self.cli(path, "--allow-fallbacks")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["unknown_predicate_count"], 1)

    def test_cli_private_value_never_printed_or_published(self) -> None:
        path = self.put("log.txt", zero_log() + "udid=NEVER_PRINT_THIS_VALUE\n")
        output = self.directory / "private.json"
        result = self.cli(path, "--json-out", output)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("NEVER_PRINT_THIS_VALUE", result.stdout + result.stderr)
        self.assertFalse(output.exists())
        self.assert_host_only(json.loads(result.stdout))

    def test_cli_c_drive_path_rejected_before_log_read(self) -> None:
        result = self.cli(self.directory / "nonexistent.log", "--json-out", "C:forbidden.json")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["errors"][0]["code"], "C_DRIVE_OUTPUT_FORBIDDEN")

    def test_cli_no_inputs_is_safe_failure(self) -> None:
        result = self.cli()
        self.assertEqual(result.returncode, 1)
        self.assert_host_only(json.loads(result.stdout))

    def test_cli_custom_target_and_top_n(self) -> None:
        path = self.put("log.txt", route("MPS", count=9) + route("CoreImage", "fallback:" + HEAP, 2))
        result = self.cli("--input", path, "--zero-fallback-procs", "MPS", "--top-n", 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["zero_fallback_procs"], ["MPS"])


class CppOracleBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        TEST_CACHE.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="real-phase4-cpp-", dir=TEST_CACHE)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.env = safe_env(cls.directory)
        sources = [
            "apps/NezhaGpuTest/ref/cpu_reference.h", "apps/NezhaGpuTest/ref/cpu_reference.cpp",
            "apps/NezhaGpuTest/ref/cpu_reference_phase4.h", "apps/NezhaGpuTest/ref/cpu_reference_phase4.cpp",
            "apps/NezhaGpuTest/test/test_cpu_reference_phase4.cpp",
        ]
        hashes = {name: hashlib.sha256((REPO / name).read_bytes()).hexdigest() for name in sources}
        cls.binary = audit.CACHE_ROOT / "test_cpu_reference_phase4_gcc"
        binding_path = audit.CACHE_ROOT / "gcc-phase4-source-binding.json"
        bound = False
        if cls.binary.is_file() and binding_path.is_file():
            binding = json.loads(binding_path.read_text())
            bound = binding.get("sources") == hashes and binding.get("binary_sha256") == hashlib.sha256(cls.binary.read_bytes()).hexdigest()
        if not bound:
            cls.binary = cls.directory / "test_cpu_reference_phase4"
            command = ["g++-13", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                       "-I" + str(REPO / "apps/NezhaGpuTest/ref"),
                       *(str(REPO / name) for name in sources if name.endswith(".cpp")),
                       "-o", str(cls.binary)]
            built = subprocess.run(command, cwd=REPO, env=cls.env, text=True, capture_output=True, timeout=120, check=False)
            if built.returncode:
                raise AssertionError("Real Phase 4 C++ build failed: " + built.stderr)
        cls.sample = cls.directory / "sample.hwtest"
        cls.executed = subprocess.run([str(cls.binary), "--emit-phase4-hwtest", str(cls.sample)], cwd=REPO,
                                      env=cls.env, text=True, capture_output=True, timeout=60, check=False)
        if cls.executed.returncode:
            raise AssertionError("Real Phase 4 C++ run failed: " + cls.executed.stderr)

    def test_real_cpp_runs_at_least_fifty_five_independent_cases(self) -> None:
        match = re.search(r"PASS: (\d+) independent cases, (\d+) checks", self.executed.stdout)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match[1]), 55)
        self.assertGreaterEqual(int(match[2]), 55)
        self.assertEqual(self.executed.stderr, "")

    def test_real_cpp_emits_all_five_cpu_only_exact_hwtest_cases(self) -> None:
        catalog = audit.load_predicates(PREDICATES)
        report = audit.audit_logs([self.sample], catalog, targets=("NezhaGpuTest",))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(set(report["hwtest_cases"]), set(audit.PHASE4_TOLERANCES))
        self.assertEqual(report["hwtest_samples"], 5)
        self.assertEqual(report["cpu_reference_samples"], 5)
        self.assertEqual(report["runtime_submissions"], 0)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        for line in self.sample.read_text().splitlines():
            self.assertIn("max_err=0 mismatched_px=0", line)
            self.assertTrue(line.endswith("note=cpu_reference_only"))

    def test_real_cpp_samples_through_actual_audit_cli(self) -> None:
        output = self.directory / "cpp-audit.json"
        md = self.directory / "cpp-audit.md"
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--input", str(self.sample),
                                 "--zero-fallback-procs", "NezhaGpuTest", "--json-out", str(output), "--md-out", str(md)],
                                cwd=REPO, env=self.env, text=True, capture_output=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report, json.loads(result.stdout))
        self.assertEqual(report["raw_count"], 5)
        self.assertIn("CPU reference samples remain host evidence", md.read_text())

    def test_real_cpp_cpu_samples_cannot_cover_default_system_targets(self) -> None:
        report = audit.audit_logs([self.sample], audit.load_predicates(PREDICATES))
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["zero_fallback_passed"])
        self.assertEqual(report["missing_target_procs"], sorted(audit.DEFAULT_ZERO_PROCS))


if __name__ == "__main__":
    unittest.main()
