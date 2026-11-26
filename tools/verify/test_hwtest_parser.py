#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""HWTEST policy/format tests and real CPU sample, all in .cache/w03-l4-test.

Only in-memory cache roots of older test modules are redirected; their
source files are untouched. Apple sources receive lexical/structural checks
here, not an Apple SDK compilation or a hardware-acceptance claim.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import hwtest_parser as parser
from check_contracts import CACHE_PARENT, GateError, cache_directory, cache_environment, checked_cache_path
import driver_constants_test as lexical
import test_phase1_gates as phase1
import test_verify_tools as legacy

TEST_CACHE = CACHE_PARENT / "w03-l4-test"
phase1.TEST_CACHE = TEST_CACHE
legacy.TEST_CACHE = TEST_CACHE
REPO = Path(__file__).resolve().parents[2]
PREDICATES = REPO / "contracts/DualPathPredicates.tsv"


def record(case: str, *, status: str = "PASS", path: str = "raw", maximum: str = "0",
           mismatches: str = "0", time: str = "1", note: str = "fixture") -> str:
    return (f"HWTEST case={case} status={status} path={path} max_err={maximum} "
            f"mismatched_px={mismatches} time_us={time} note={note}\n")


def suite() -> str:
    return "".join(record(case) for case in parser.CASES)


class ParserTests(unittest.TestCase):
    def setUp(self) -> None:
        cache_directory(TEST_CACHE)
        temporary = tempfile.TemporaryDirectory(prefix="parser-", dir=TEST_CACHE)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = cache_environment(self.directory)
        self.predicates = parser.load_predicates(PREDICATES)

    def parse(self, text: str, allow: bool = False) -> dict:
        return parser.parse_hwtest(io.StringIO(text), self.predicates, allow_fallback_summary=allow)

    def codes(self, text: str, allow: bool = False) -> set[str]:
        return {error["code"] for error in self.parse(text, allow)["errors"]}

    def replace(self, case: str, **kwargs) -> str:
        return suite().replace(record(case), record(case, **kwargs))

    def test_complete_raw_suite_passes(self) -> None:
        report = self.parse(suite())
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["records"]), 8)
        self.assertEqual(report["fallback_count"], 0)

    def test_all_eight_required_cases_are_fixed(self) -> None:
        self.assertEqual(len(parser.CASES), 8)
        self.assertIn("gpu.compute.nonuniform_grid", parser.CASES)
        self.assertIn("gpu.sync.fence_and_order", parser.CASES)

    def test_single_missing_case_is_rejected(self) -> None:
        report = self.parse(suite().replace(record(parser.CASES[0]), ""))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["missing_cases"], [parser.CASES[0]])

    def test_duplicate_case_is_rejected(self) -> None:
        self.assertIn("DUPLICATE_CASE", self.codes(suite() + record(parser.CASES[0])))

    def test_fail_status_is_rejected(self) -> None:
        self.assertIn("CASE_FAILED", self.codes(self.replace(parser.CASES[1], status="FAIL")))

    def test_single_mismatched_pixel_is_rejected_even_with_pass_status(self) -> None:
        self.assertIn("PIXEL_MISMATCH", self.codes(self.replace(parser.CASES[2], mismatches="1")))

    def test_exact_case_one_lsb_error_is_rejected(self) -> None:
        self.assertIn("ERROR_TOLERANCE_EXCEEDED", self.codes(self.replace(parser.CASES[1], maximum="1")))

    def test_bilinear_one_lsb_tolerance_is_allowed(self) -> None:
        self.assertEqual(self.parse(self.replace(parser.CASES[2], maximum="1"))["status"], "PASS")

    def test_bilinear_over_tolerance_is_rejected(self) -> None:
        self.assertIn("ERROR_TOLERANCE_EXCEEDED", self.codes(self.replace(parser.CASES[2], maximum="1.01")))

    def test_float_tolerance_and_scientific_notation(self) -> None:
        self.assertEqual(self.parse(self.replace(parser.CASES[3], maximum="1e-6"))["status"], "PASS")
        self.assertIn("ERROR_TOLERANCE_EXCEEDED", self.codes(self.replace(parser.CASES[3], maximum="2e-5")))

    def test_legal_fallback_is_forbidden_by_phase2(self) -> None:
        text = self.replace(parser.CASES[0], path="fallback:raw:blit.unsupported_format")
        report = self.parse(text)
        self.assertEqual(report["fallback_count"], 1)
        self.assertIn("FALLBACK_FORBIDDEN", {r["code"] for r in report["errors"]})

    def test_illegal_predicate_is_rejected_in_debug_too(self) -> None:
        text = self.replace(parser.CASES[0], path="fallback:raw:not.registered")
        self.assertIn("INVALID_ROUTE_OR_PREDICATE", self.codes(text, allow=True))

    def test_debug_summary_counts_predicates(self) -> None:
        text = self.replace(parser.CASES[0], path="fallback:raw:blit.unsupported_format")
        text = text.replace(record(parser.CASES[1]), record(parser.CASES[1], path="fallback:raw:blit.unsupported_format"))
        report = self.parse(text, allow=True)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["fallback_predicates"], {"raw:blit.unsupported_format": 2})
        self.assertFalse(report["phase2_exit_pass"])

    def test_debug_mode_keeps_fail_and_coverage_gates(self) -> None:
        text = self.replace(parser.CASES[0], status="FAIL", path="fallback:raw:blit.unsupported_format")
        self.assertEqual(self.parse(text, True)["status"], "FAIL")
        self.assertEqual(self.parse(text.replace(record(parser.CASES[1]), ""), True)["status"], "FAIL")

    def test_fallback_requires_full_raw_predicate(self) -> None:
        self.assertIn("INVALID_ROUTE_OR_PREDICATE", self.codes(self.replace(parser.CASES[0], path="fallback:blit.unsupported_format")))

    def test_truncated_last_line_is_rejected(self) -> None:
        self.assertIn("TRUNCATED_RECORD", self.codes(suite().rstrip("\n")))

    def test_truncated_missing_note_is_rejected(self) -> None:
        text = suite().replace("note=fixture\n", "\n", 1)
        self.assertIn("MISSING_FIELDS", self.codes(text))

    def test_partial_marker_does_not_make_empty_log_pass(self) -> None:
        self.assertEqual(self.parse("HWTEST")["status"], "FAIL")
        self.assertEqual(self.parse("HWTES")["status"], "FAIL")

    def test_unknown_case_is_rejected(self) -> None:
        self.assertIn("UNKNOWN_CASE", self.codes(suite().replace(parser.CASES[0], "gpu.fake", 1)))

    def test_invalid_status_is_rejected(self) -> None:
        self.assertIn("INVALID_STATUS_OR_NOTE", self.codes(self.replace(parser.CASES[0], status="SKIP")))

    def test_duplicate_field_is_rejected(self) -> None:
        self.assertIn("UNKNOWN_OR_DUPLICATE_FIELD", self.codes(suite().replace("path=raw", "path=raw path=raw", 1)))

    def test_unknown_field_is_rejected(self) -> None:
        self.assertIn("UNKNOWN_OR_DUPLICATE_FIELD", self.codes(suite().replace("note=fixture", "note=fixture extra=1", 1)))

    def test_invalid_numeric_and_nonfinite_fields(self) -> None:
        for value in ("nan", "inf", "1e999", "-1", "garbage"):
            with self.subTest(value=value):
                self.assertIn("INVALID_METRICS", self.codes(self.replace(parser.CASES[0], maximum=value)))

    def test_negative_and_fractional_integer_metrics(self) -> None:
        for value in ("-1", "1.5"):
            self.assertIn("INVALID_METRICS", self.codes(self.replace(parser.CASES[0], mismatches=value)))
            self.assertIn("INVALID_METRICS", self.codes(self.replace(parser.CASES[0], time=value)))

    def test_uint64_metric_overflow_is_rejected(self) -> None:
        self.assertIn("INVALID_METRICS", self.codes(self.replace(parser.CASES[0], time=str(1 << 64))))

    def test_note_whitespace_and_injection_are_rejected(self) -> None:
        self.assertIn("MALFORMED_FIELD", self.codes(self.replace(parser.CASES[0], note="two words")))
        self.assertIn("INVALID_STATUS_OR_NOTE", self.codes(self.replace(parser.CASES[0], note="bad/route")))

    def test_crlf_and_prefix_log_lines_are_supported(self) -> None:
        text = "".join("[123.5] " + line for line in suite().splitlines(keepends=True)).replace("\n", "\r\n")
        self.assertEqual(self.parse(text)["status"], "PASS")

    def test_field_order_is_not_significant(self) -> None:
        text = "".join("HWTEST " + " ".join(reversed(line.split()[1:])) + "\n" for line in suite().splitlines())
        self.assertEqual(self.parse(text)["status"], "PASS")

    def test_large_noisy_log_uses_line_stream(self) -> None:
        def lines():
            for _ in range(50000): yield "ordinary device console message\n"
            yield from suite().splitlines(keepends=True)
        report = parser.parse_hwtest(lines(), self.predicates)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["ignored_lines"], 50000)

    def test_cpu_fixture_does_not_claim_hardware_acceptance(self) -> None:
        report = self.parse(suite().replace("note=fixture", "note=cpu_reference_only"))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["measurement_kind"], "CPU_REFERENCE_FIXTURE")
        self.assertFalse(report["hardware_acceptance"])

    def test_empty_log_is_rejected(self) -> None:
        self.assertEqual(self.parse("")["missing_cases"], sorted(parser.CASES))

    def test_malformed_record_and_excess_length_rejected(self) -> None:
        self.assertIn("MALFORMED_FIELD", self.codes(suite().replace("time_us=1", "time_us", 1)))
        self.assertIn("RECORD_TOO_LONG", self.codes(self.replace(parser.CASES[0], note="x" * 5000)))

    def test_predicate_file_is_real_16_entry_vocabulary(self) -> None:
        self.assertEqual(len(self.predicates), 16)
        self.assertIn("raw:compute.uncompiled_kernel", self.predicates)

    def test_duplicate_predicate_vocabulary_rejected(self) -> None:
        path = self.directory / "predicates.tsv"
        path.write_text("predicate\nraw:a\nraw:a\n", encoding="ascii")
        with self.assertRaises(ValueError): parser.load_predicates(path)

    def test_bad_predicate_header_and_empty_vocabulary_rejected(self) -> None:
        path = self.directory / "predicates.tsv"
        for text in ("wrong\nraw:a\n", "predicate\n"):
            path.write_text(text, encoding="ascii")
            with self.assertRaises(ValueError): parser.load_predicates(path)

    def test_parser_cli_exit_codes_and_help(self) -> None:
        path = self.directory / "fixture.hwtest"; path.write_text(suite(), encoding="ascii")
        command = [sys.executable, "-B", str(REPO / "tools/verify/hwtest_parser.py"), "--predicates", str(PREDICATES),
                   "--policy", "phase2", "--input", str(path)]
        result = subprocess.run(command, env=self.env, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path.write_text(suite().replace("status=PASS", "status=FAIL", 1), encoding="ascii")
        result = subprocess.run(command, env=self.env, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 1)
        result = subprocess.run(command[:3] + ["--help"], env=self.env, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0)

    def test_new_cache_roots_redirect_legacy_without_source_changes(self) -> None:
        self.assertEqual(phase1.TEST_CACHE, TEST_CACHE)
        self.assertEqual(legacy.TEST_CACHE, TEST_CACHE)
        with self.assertRaises(GateError): checked_cache_path("/mnt/c/forbidden")

    def test_real_cpu_executable_sample_passes_parser(self) -> None:
        output = self.directory / "cpu-reference"
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-fsanitize=address,undefined",
                   "-fno-pie", "-no-pie", "-I", str(REPO / "apps/NezhaGpuTest/ref"),
                   str(REPO / "apps/NezhaGpuTest/ref/cpu_reference.cpp"), str(REPO / "apps/NezhaGpuTest/test/test_cpu_reference.cpp"),
                   "-o", str(output)]
        self.env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        built = subprocess.run(command, cwd=self.directory, env=self.env, capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(built.returncode, 0, built.stderr)
        sample = self.directory / "sample.hwtest"
        ran = subprocess.run([str(output), "--emit-sample-hwtest", str(sample)], cwd=self.directory, env=self.env,
                             capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        report = self.parse(sample.read_text())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["measurement_kind"], "CPU_REFERENCE_FIXTURE")

    def test_apple_sources_balanced_and_all_shader_functions_wired(self) -> None:
        root = REPO / "apps/NezhaGpuTest"
        shader = (root / "shaders/NezhaGpuTest.metal").read_text()
        runner = (root / "ios/NezhaGpuTestRunner.mm").read_text()
        for file in (root / "shaders/NezhaGpuTest.metal", root / "ios/NezhaGpuTestRunner.mm", root / "ios/main.mm"):
            clean = lexical.strip_literals(file.read_text())
            stack = []
            pairs = {"}": "{", "]": "[", ")": "("}
            for token in clean:
                if token in "{[(": stack.append(token)
                elif token in "}])":
                    self.assertTrue(stack and stack.pop() == pairs[token], str(file))
            self.assertFalse(stack, str(file))
        for name in ("add_vec", "nonuniform_grid", "clear_bgra8", "scale_texture", "fence_step", "fence_verify", "test_vertex", "triangle_fragment", "textured_fragment"):
            self.assertIn(name, shader)
            self.assertIn('@"' + name + '"', runner)
        self.assertIn("dispatchThreads:MTLSizeMake(width, height, 1)", runner)
        self.assertIn("a840_hwtest_path_for_command_buffer", runner)
        self.assertIn("fallback:raw:unimplemented.global", runner)
        self.assertIn("std::make_shared<CompletionState>", runner)
        self.assertIn("[verifier waitForFence:fence]", runner)
        self.assertIn("destinationOffset:offset + 12", runner)


if __name__ == "__main__":
    parser_cli = argparse.ArgumentParser(description=__doc__)
    parser_cli.add_argument("-v", "--verbose", action="store_true")
    arguments = parser_cli.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2 if arguments.verbose else 1)
