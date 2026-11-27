#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 3 format, exact pixel policy, CPU workload and Apple source checks.

All fixtures and old test modules' runtime cache roots are redirected to
w05-l4-test; no old files are changed. Apple checks are lexical only and do
not establish Apple SDK compilation, UBWC execution or device acceptance.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import hwtest_phase3_policy as policy
import hwtest_parser as phase2
from check_contracts import CACHE_PARENT, GateError, cache_directory, cache_environment, checked_cache_path
from phase2_host_gate import apple_lexical_text
from no_fallback_test import findings
import test_hwtest_parser as old_hw
import test_phase1_gates as old_phase1
import test_phase2_gates as old_phase2
import test_verify_tools as legacy

TEST_CACHE = CACHE_PARENT / "w05-l4-test"
for module in (old_hw, old_phase1, old_phase2, legacy):
    module.TEST_CACHE = TEST_CACHE
REPO = Path(__file__).resolve().parents[2]
ROOT = REPO / "apps/NezhaGpuTest"
PREDICATES = REPO / "contracts/DualPathPredicates.tsv"


def record(case: str, status: str = "PASS", route: str = "raw", maximum: str = "0",
           mismatch: str = "0", time: str = "1", note: str = "fixture") -> str:
    return (f"HWTEST case={case} status={status} path={route} max_err={maximum} "
            f"mismatched_px={mismatch} time_us={time} note={note}\n")


def suite(include: bool = False) -> str:
    cases = tuple(phase2.CASES) + policy.CASES if include else policy.CASES
    return "".join(record(case) for case in cases)


class Phase3PolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        cache_directory(TEST_CACHE)
        temporary = tempfile.TemporaryDirectory(prefix="phase3-", dir=TEST_CACHE)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = cache_environment(self.directory)
        self.env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        self.predicates = phase2.load_predicates(PREDICATES)

    def parse(self, text, include: bool = False) -> dict:
        return policy.parse_hwtest(text, self.predicates, include_phase2=include)

    def replace(self, **kwargs) -> str:
        return suite().replace(record(policy.CASES[0]), record(policy.CASES[0], **kwargs))

    def codes(self, text, include: bool = False) -> set[str]:
        return {row["code"] for row in self.parse(text, include)["errors"]}

    def test_six_required_cases_are_fixed(self) -> None:
        self.assertEqual(len(policy.CASES), 6)
        self.assertEqual(policy.CASES[-1], "gpu.ubwc.scanout_compat_fallback")
        self.assertEqual(len(self.predicates), 16)

    def test_six_raw_cases_pass(self) -> None:
        result = self.parse(suite())
        self.assertEqual(result["status"], "PASS")
        self.assertTrue(result["phase3_exit_pass"])
        self.assertEqual(result["required_case_count"], 6)
        self.assertEqual(result["fallback_count"], 0)

    def test_all_fourteen_joint_cases_pass(self) -> None:
        result = self.parse(suite(True), True)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["required_case_count"], 14)
        self.assertEqual(len(result["records"]), 14)

    def test_missing_phase3_case_rejected(self) -> None:
        result = self.parse(suite().replace(record(policy.CASES[1]), ""))
        self.assertEqual(result["missing_cases"], [policy.CASES[1]])
        self.assertEqual(result["status"], "FAIL")

    def test_joint_requires_phase2_coverage(self) -> None:
        result = self.parse(suite(), True)
        self.assertEqual(result["missing_cases"], sorted(phase2.CASES))
        self.assertFalse(result["phase3_exit_pass"])

    def test_joint_requires_phase3_coverage(self) -> None:
        self.assertEqual(self.parse("".join(record(c) for c in phase2.CASES), True)["missing_cases"], sorted(policy.CASES))

    def test_phase2_extra_cases_require_include_option(self) -> None:
        self.assertIn("UNKNOWN_CASE", self.codes(suite(True)))

    def test_duplicate_phase3_case_rejected(self) -> None:
        self.assertIn("DUPLICATE_CASE", self.codes(suite() + record(policy.CASES[1])))

    def test_duplicate_phase2_case_rejected_in_joint(self) -> None:
        self.assertIn("DUPLICATE_CASE", self.codes(suite(True) + record(phase2.CASES[0]), True))

    def test_fail_case_rejected(self) -> None:
        self.assertIn("CASE_FAILED", self.codes(self.replace(status="FAIL")))

    def test_pass_with_one_pixel_mismatch_rejected(self) -> None:
        self.assertIn("PIXEL_MISMATCH", self.codes(self.replace(mismatch="1")))

    def test_all_six_require_exact_zero_error(self) -> None:
        for case in policy.CASES:
            with self.subTest(case=case):
                text = suite().replace(record(case), record(case, maximum="0.0000001"))
                self.assertIn("ERROR_TOLERANCE_EXCEEDED", self.codes(text))

    def test_joint_keeps_phase2_float_tolerance(self) -> None:
        text = suite(True).replace(record(phase2.CASES[3]), record(phase2.CASES[3], maximum="1e-6"))
        self.assertEqual(self.parse(text, True)["status"], "PASS")
        self.assertEqual(self.parse(text.replace("1e-6", "2e-5"), True)["status"], "FAIL")

    def test_joint_keeps_phase2_one_lsb_tolerance(self) -> None:
        text = suite(True).replace(record(phase2.CASES[2]), record(phase2.CASES[2], maximum="1"))
        self.assertEqual(self.parse(text, True)["status"], "PASS")

    def test_all_registered_fallback_routes_forbidden(self) -> None:
        for predicate in self.predicates:
            with self.subTest(predicate=predicate):
                result = self.parse(self.replace(route="fallback:" + predicate))
                self.assertEqual(result["fallback_count"], 1)
                self.assertEqual(result["fallback_predicates"], {predicate: 1})
                self.assertIn("FALLBACK_FORBIDDEN", {row["code"] for row in result["errors"]})

    def test_native_feature_fallbacks_are_explicitly_flagged(self) -> None:
        for predicate in policy.NATIVE_FEATURE_PREDICATES:
            self.assertIn("PHASE3_NATIVE_FEATURE_FALLBACK", self.codes(self.replace(route="fallback:" + predicate)))

    def test_bare_native_predicate_is_not_raw(self) -> None:
        self.assertIn("INVALID_ROUTE_OR_PREDICATE", self.codes(self.replace(route="raw:render.gmem_required")))

    def test_unknown_fallback_predicate_rejected(self) -> None:
        self.assertIn("INVALID_ROUTE_OR_PREDICATE", self.codes(self.replace(route="fallback:raw:unregistered")))

    def test_joint_phase2_fallback_forbidden(self) -> None:
        text = suite(True).replace(record(phase2.CASES[0]), record(phase2.CASES[0], route="fallback:raw:blit.unsupported_format"))
        self.assertIn("FALLBACK_FORBIDDEN", self.codes(text, True))

    def test_native_mdss_linear_case_still_requires_raw(self) -> None:
        text = suite().replace(record(policy.CASES[-1]), record(policy.CASES[-1], note="mdss_native_linear"))
        self.assertEqual(self.parse(text)["status"], "PASS")

    def test_nonfinite_signed_and_invalid_error_rejected(self) -> None:
        for value in ("NaN", "inf", "1e999", "-0", "+1", "bad"):
            self.assertIn("INVALID_METRICS", self.codes(self.replace(maximum=value)))

    def test_integer_overflow_and_fraction_rejected(self) -> None:
        for value in (str(1 << 64), "-1", "1.5", "0x1"):
            self.assertIn("INVALID_METRICS", self.codes(self.replace(time=value)))
            self.assertIn("INVALID_METRICS", self.codes(self.replace(mismatch=value)))

    def test_zero_time_and_scientific_zero_valid(self) -> None:
        self.assertEqual(self.parse(self.replace(maximum="0e10", time="0"))["status"], "PASS")

    def test_positive_error_underflow_cannot_become_exact_zero(self) -> None:
        self.assertIn("EXACT_ZERO_REQUIRED", self.codes(self.replace(maximum="1e-9999")))
        self.assertIn("EXACT_ZERO_REQUIRED", self.codes(self.replace(maximum="0.00001e-9999")))
        self.assertEqual(self.parse(self.replace(maximum="0.00000e-9999"))["status"], "PASS")

    def test_unknown_case_and_status_rejected(self) -> None:
        self.assertIn("UNKNOWN_CASE", self.codes(suite().replace(policy.CASES[0], "gpu.gmem.fake", 1)))
        self.assertIn("INVALID_STATUS_OR_NOTE", self.codes(self.replace(status="SKIP")))

    def test_missing_field_rejected(self) -> None:
        self.assertIn("MISSING_FIELDS", self.codes(suite().replace("note=fixture", "", 1)))

    def test_duplicate_and_unknown_field_rejected(self) -> None:
        self.assertIn("UNKNOWN_OR_DUPLICATE_FIELD", self.codes(suite().replace("path=raw", "path=raw path=raw", 1)))
        self.assertIn("UNKNOWN_OR_DUPLICATE_FIELD", self.codes(suite().replace("note=fixture", "note=fixture extra=1", 1)))

    def test_note_injection_and_malformed_field_rejected(self) -> None:
        self.assertIn("MALFORMED_FIELD", self.codes(self.replace(note="two words")))
        self.assertIn("INVALID_STATUS_OR_NOTE", self.codes(self.replace(note="bad/name")))

    def test_truncated_record_and_empty_log_rejected(self) -> None:
        self.assertIn("TRUNCATED_RECORD", self.codes(suite().rstrip("\n")))
        self.assertEqual(self.parse("")["status"], "FAIL")

    def test_long_record_rejected(self) -> None:
        self.assertIn("RECORD_TOO_LONG", self.codes(self.replace(note="x" * 5000)))

    def test_prefix_crlf_and_field_order_supported(self) -> None:
        lines = ["[1.0] HWTEST " + " ".join(reversed(line.split()[1:])) + "\r\n" for line in suite().splitlines()]
        self.assertEqual(self.parse(lines)["status"], "PASS")

    def test_streaming_noise_and_type_errors(self) -> None:
        def lines():
            for _ in range(10000): yield "ordinary log\n"
            yield from suite().splitlines(keepends=True)
        result = self.parse(lines()); self.assertEqual(result["status"], "PASS"); self.assertEqual(result["ignored_lines"], 10000)
        self.assertIn("ENCODING", self.codes([b"HWTEST"] + suite().splitlines(keepends=True)))

    def test_cpu_fixture_never_claims_device(self) -> None:
        result = self.parse(suite().replace("note=fixture", "note=cpu_reference_only"))
        self.assertEqual(result["measurement_kind"], "CPU_REFERENCE_FIXTURE")
        self.assertFalse(result["hardware_acceptance"]); self.assertFalse(result["device_verified"])

    def test_unknown_policy_rejected(self) -> None:
        with self.assertRaises(ValueError): policy.parse_hwtest(suite(), self.predicates, policy="phase2")

    def test_phase2_globals_unchanged_and_concurrent(self) -> None:
        original_cases, original_tolerances = phase2.CASES, dict(phase2.TOLERANCES)
        with ThreadPoolExecutor(max_workers=4) as workers:
            futures = [workers.submit(self.parse, suite(True), True), workers.submit(self.parse, suite()),
                       workers.submit(phase2.parse_hwtest, "".join(record(c) for c in phase2.CASES), self.predicates)]
            self.assertTrue(all(future.result()["status"] == "PASS" for future in futures))
        self.assertIs(phase2.CASES, original_cases); self.assertEqual(phase2.TOLERANCES, original_tolerances)

    def test_cli_file_stdin_joint_and_json_output(self) -> None:
        source = self.directory / "joint.hwtest"; source.write_text(suite(True))
        output = self.directory / "report.json"
        command = [sys.executable, "-B", str(REPO / "tools/verify/hwtest_phase3_policy.py"), "--predicates", str(PREDICATES),
                   "--policy", "phase3", "--include-phase2", "--input", str(source), "--json-out", str(output)]
        result = subprocess.run(command, env=self.env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr); self.assertEqual(json.loads(output.read_text())["required_case_count"], 14)
        command[command.index(str(source))] = "-"
        result = subprocess.run(command, input=suite(True), env=self.env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_cli_failure_and_cache_boundary(self) -> None:
        command = [sys.executable, "-B", str(REPO / "tools/verify/hwtest_phase3_policy.py"), "--predicates", str(PREDICATES), "--input", "-"]
        result = subprocess.run(command, input=self.replace(route="fallback:raw:render.gmem_required"), env=self.env,
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1); self.assertFalse(json.loads(result.stdout)["phase3_exit_pass"])
        result = subprocess.run(command + ["--json-out", "/mnt/c/blocked-phase3.json"], input=suite(), env=self.env,
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(checked_cache_path(CACHE_PARENT / "future-phase/report.json"), CACHE_PARENT / "future-phase/report.json")
        with self.assertRaises(GateError): checked_cache_path(CACHE_PARENT)

    def test_cli_help_and_missing_input(self) -> None:
        program = [sys.executable, "-B", str(REPO / "tools/verify/hwtest_phase3_policy.py")]
        self.assertEqual(subprocess.run(program + ["--help"], env=self.env, capture_output=True).returncode, 0)
        result = subprocess.run(program + ["--predicates", str(PREDICATES), "--input", str(self.directory / "missing")],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1); self.assertEqual(json.loads(result.stdout)["errors"][0]["code"], "INPUT_ERROR")

    def test_real_phase3_cpu_sample_compiles_and_passes(self) -> None:
        binary = self.directory / "cpu-phase3"; sample = self.directory / "sample.hwtest"
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-fsanitize=address,undefined",
                   "-fno-pie", "-no-pie", "-I", str(ROOT / "ref"), str(ROOT / "ref/cpu_reference.cpp"),
                   str(ROOT / "ref/cpu_reference_phase3.cpp"), str(ROOT / "test/test_cpu_reference_phase3.cpp"), "-o", str(binary)]
        built = subprocess.run(command, cwd=self.directory, env=self.env, capture_output=True, text=True, timeout=90)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = subprocess.run([str(binary), "--emit-sample-phase3-hwtest", str(sample)], cwd=self.directory, env=self.env,
                             capture_output=True, text=True, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertIn("61 independent cases", ran.stdout)
        result = self.parse(sample.read_text()); self.assertEqual(result["status"], "PASS", result)
        self.assertEqual(result["measurement_kind"], "CPU_REFERENCE_FIXTURE")
        self.assertEqual(len(result["records"]), 6)

    def test_new_test_cache_redirects_all_old_modules(self) -> None:
        for module in (old_hw, old_phase1, old_phase2, legacy): self.assertEqual(module.TEST_CACHE, TEST_CACHE)

    def test_apple_sources_balanced_and_no_forbidden_symbols(self) -> None:
        for path in (ROOT / "shaders/NezhaGpuTestPhase3.metal", ROOT / "ios/NezhaGpuTestPhase3Runner.mm"):
            clean, strings = apple_lexical_text(path.read_text(), path.name)
            self.assertEqual(findings(clean + "\n" + strings, "final"), [])
            stack = []; pairs = {"}": "{", "]": "[", ")": "("}
            for token in clean:
                if token in "{[(": stack.append(token)
                elif token in "}])": self.assertTrue(stack and stack.pop() == pairs[token], path.name)
            self.assertFalse(stack, path.name)

    def test_metal_functions_memoryless_and_fetch_are_wired(self) -> None:
        shader = (ROOT / "shaders/NezhaGpuTestPhase3.metal").read_text()
        runner = (ROOT / "ios/NezhaGpuTestPhase3Runner.mm").read_text()
        for name in ("p3_scene_vertex", "p3_fullscreen_vertex", "p3_flat_fragment", "p3_boundary_fragment",
                     "p3_fetch_fragment", "p3_ubwc_write", "p3_ubwc_sample_fragment"):
            self.assertIn(name, shader); self.assertIn('@"' + name + '"', runner)
        self.assertIn("previous [[color(0), raster_order_group(0)]]", shader)
        self.assertIn("rgba [[color(0), raster_order_group(0)]]", shader)
        self.assertIn("MTLStorageModeMemoryless", runner); self.assertIn("MTLPixelFormatDepth32Float", runner)
        self.assertIn("actual.fbfetch_draws == 4", runner)
        self.assertIn("actual.tile_width == 96 && actual.tile_height == 32", runner)
        self.assertIn("require_tiles(evidence(cb), 1200, 2608)", runner)
        self.assertIn("a840_hwtest_path_for_command_buffer", runner)

    def test_runner_requires_real_ubwc_and_mdss_evidence(self) -> None:
        runner = (ROOT / "ios/NezhaGpuTestPhase3Runner.mm").read_text()
        self.assertIn("actual.compressed_blocks > 0 && actual.escape_blocks > 0", runner)
        self.assertIn("actual.highest_bank_bit == 16", runner)
        self.assertIn("a840_hwtest_phase3_encode_scanout", runner)
        self.assertIn("Phase 3 executed feature evidence unavailable", runner)
        self.assertNotIn("encode_ubwc_model(", runner)
        self.assertIn("pending Mac CI", runner)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("-v", "--verbose", action="store_true")
    args = cli.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2 if args.verbose else 1)
