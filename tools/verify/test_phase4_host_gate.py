#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent policy, path, sibling discovery, and real Phase 4 host checks."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from check_contracts import GateError
import fallback_audit
import hwtest_parser
import hwtest_phase3_policy
import hwtest_phase4_policy as policy
import phase4_host_gate as gate

REPO = Path(__file__).resolve().parents[2]
MESA = Path(os.environ.get("A840_VERIFY_MESA",
    "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work"))


def samples(suite="phase4", *, error="0", note="cpu_reference_only", path="raw") -> str:
    return "".join(f"HWTEST case={case} status=PASS path={path} max_err={error} mismatched_px=0 "
                   f"time_us=7 note={note}\n" for case in policy.vocabulary(suite))


def replace_case(text, index, old, new):
    lines = text.splitlines(keepends=True)
    lines[index] = lines[index].replace(old, new)
    return "".join(lines)


class CachedCase(unittest.TestCase):
    def setUp(self):
        policy.cache_directory(policy.CACHE_ROOT)
        self.scratch = tempfile.TemporaryDirectory(prefix="unittest-", dir=policy.CACHE_ROOT)
        self.addCleanup(self.scratch.cleanup)
        self.cache = Path(self.scratch.name)
        self.predicates = set(fallback_audit.load_predicates(REPO / "contracts/DualPathPredicates.tsv"))

    def cli(self, script, *arguments, source=None, timeout=120):
        return subprocess.run([sys.executable, "-B", str(REPO / "tools/verify" / script), *map(str, arguments)],
                              cwd=REPO, env=policy.cache_environment(self.cache), input=source,
                              capture_output=True, text=True, timeout=timeout, check=False)

    def parse(self, text, suite="phase4", **options):
        return policy.parse_hwtest(text, self.predicates, suite, **options)

    def rejects(self, text, code=None, suite="phase4", **options):
        report = self.parse(text, suite, **options)
        self.assertEqual(report["status"], "FAIL")
        self.assertIs(report["host_policy_passed"], False)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        if code:
            self.assertIn(code, [issue["code"] for issue in report["errors"]])
        return report


class Phase4PolicyTests(CachedCase):
    def test_phase4_ten_exact_required_cases(self):
        report = self.parse(samples())
        gate.require_samples(report, "phase4")
        self.assertEqual(report["required_case_count"], 10)
        self.assertEqual(len(set(report["required_cases"])), 10)

    def test_full_system_exact_24_disjoint_cases(self):
        report = self.parse(samples("full_system"), "full_system")
        gate.require_samples(report, "full_system")
        self.assertEqual(len(report["records"]), 24)
        self.assertTrue(report["full_system_exit_pass"])

    def test_phase4_does_not_claim_full_system_coverage(self):
        self.assertIs(self.parse(samples())["full_system_exit_pass"], False)

    def test_phase4_in_full_system_policy_reports_fourteen_missing(self):
        report = self.rejects(samples(), "MISSING_CASE", "full_system")
        self.assertEqual(len(report["missing_cases"]), 14)

    def test_full_system_records_do_not_pass_phase4_only_policy(self):
        self.rejects(samples("full_system"), "UNKNOWN_CASE")

    def test_case_order_is_independent(self):
        lines = samples().splitlines(keepends=True)
        self.assertTrue(self.parse("".join(reversed(lines)))["host_policy_passed"])

    def test_missing_one_required_case(self):
        self.rejects("".join(samples().splitlines(keepends=True)[:-1]), "MISSING_CASE")

    def test_duplicate_case_refused(self):
        self.rejects(samples() + samples().splitlines(keepends=True)[0], "DUPLICATE_CASE")

    def test_unknown_case_refused(self):
        self.rejects(samples().replace(policy.CASES[0], "gpu.unknown", 1), "UNKNOWN_CASE")

    def test_truncated_final_record_refused(self):
        self.rejects(samples()[:-1], "TRUNCATED_RECORD")

    def test_registered_fallback_refused(self):
        self.rejects(samples(path="fallback:" + min(self.predicates)), "FALLBACK_FORBIDDEN")

    def test_unknown_fallback_refused(self):
        self.rejects(samples(path="fallback:raw:unknown.feature"), "FALLBACK_FORBIDDEN")

    def test_fallback_in_ignored_prefix_noise_refused(self):
        self.rejects("noise fallback:raw:render.msaa_resolve\n" + samples(), "FALLBACK_FORBIDDEN")

    def test_failed_case_refused(self):
        self.rejects(samples().replace("status=PASS", "status=FAIL", 1), "CASE_FAILED")

    def test_nonzero_pixel_mismatch_refused(self):
        self.rejects(samples().replace("mismatched_px=0", "mismatched_px=1", 1), "PIXEL_MISMATCH")

    def test_exact_query_rejects_positive_underflow(self):
        self.rejects(replace_case(samples(), 6, "max_err=0", "max_err=1e-999"),
                     "ERROR_TOLERANCE_EXCEEDED")

    def test_exact_phase3_full_system_underflow_refused(self):
        self.rejects(replace_case(samples("full_system"), 8, "max_err=0", "max_err=1e-999"),
                     "ERROR_TOLERANCE_EXCEEDED", "full_system")

    def test_decimal_over_one_lsb_cannot_round_down_to_pass(self):
        self.rejects(replace_case(samples(), 5, "max_err=0", "max_err=1.000000000000000000000000001"),
                     "ERROR_TOLERANCE_EXCEEDED")

    def test_msaa_one_lsb_tolerance_passes(self):
        self.assertTrue(self.parse(replace_case(samples(), 5, "max_err=0", "max_err=1"))["host_policy_passed"])

    def test_msaa_more_than_one_lsb_fails(self):
        self.rejects(replace_case(samples(), 5, "max_err=0", "max_err=1.01"), "ERROR_TOLERANCE_EXCEEDED")

    def test_camera_one_lsb_tolerance_passes(self):
        self.assertTrue(self.parse(replace_case(samples(), 8, "max_err=0", "max_err=1"))["host_policy_passed"])

    def test_full_system_fp32_tolerance_preserved(self):
        self.assertTrue(self.parse(replace_case(samples("full_system"), 3, "max_err=0", "max_err=0.00001"),
                                   "full_system")["host_policy_passed"])

    def test_full_system_fp32_error_above_limit_fails(self):
        self.rejects(replace_case(samples("full_system"), 3, "max_err=0", "max_err=0.000011"),
                     "ERROR_TOLERANCE_EXCEEDED", "full_system")

    def test_nan_maximum_refused(self):
        self.rejects(samples().replace("max_err=0", "max_err=nan", 1), "INVALID_METRICS")

    def test_overlong_numeric_field_cannot_hide_rounded_overflow(self):
        self.rejects(replace_case(samples(), 5, "max_err=0", "max_err=1." + "0" * 70 + "1"),
                     "INVALID_METRICS")

    def test_infinite_numeric_maximum_refused(self):
        self.rejects(samples().replace("max_err=0", "max_err=1e999", 1), "INVALID_METRICS")

    def test_uint64_time_overflow_refused(self):
        self.rejects(samples().replace("time_us=7", f"time_us={1 << 64}", 1), "INVALID_METRICS")

    def test_uint64_maximum_time_is_valid_host_metric(self):
        self.assertTrue(self.parse(samples().replace("time_us=7", f"time_us={(1 << 64) - 1}", 1))[
            "host_policy_passed"])

    def test_duplicate_field_refused(self):
        self.rejects(samples().replace("note=cpu_reference_only", "note=cpu_reference_only time_us=8", 1),
                     "UNKNOWN_OR_DUPLICATE_FIELD")

    def test_missing_field_refused(self):
        self.rejects(samples().replace(" time_us=7", "", 1), "MISSING_FIELDS")

    def test_private_field_in_ignored_line_discards_partial_report(self):
        report = self.rejects(samples() + "noise serial_number=DO_NOT_ECHO\n", "PRIVATE_FIELD_REDACTION_REQUIRED")
        self.assertEqual(report["records"], [])
        self.assertNotIn("DO_NOT_ECHO", json.dumps(report))

    def test_private_field_in_timestamp_prefix_refused(self):
        self.rejects("ECID=123 " + samples(), "PRIVATE_FIELD_REDACTION_REQUIRED")

    def test_identity_fields_are_case_insensitive(self):
        for field in ("UDID", "ApNonce", "imei", "IMSI", "meid", "serial-number"):
            with self.subTest(field=field):
                self.rejects(field + "=private\n" + samples(), "PRIVATE_FIELD_REDACTION_REQUIRED")

    def test_cpu_samples_cannot_request_device_verification(self):
        self.rejects(samples(), "HARDWARE_CLAIM_FORBIDDEN", device_verified=True)

    def test_cpu_samples_cannot_request_hardware_acceptance(self):
        self.rejects(samples(), "HARDWARE_CLAIM_FORBIDDEN", hardware_acceptance=True)

    def test_truthy_integer_hardware_claim_refused(self):
        self.rejects(samples(), "HARDWARE_CLAIM_FORBIDDEN", device_verified=1)

    def test_hardware_claim_in_log_metadata_refused(self):
        self.rejects("device_verified=true\n" + samples(), "HARDWARE_CLAIM_FORBIDDEN")

    def test_json_hardware_claim_refused(self):
        self.rejects('{"hardware_acceptance": true}\n' + samples(), "HARDWARE_CLAIM_FORBIDDEN")

    def test_false_hardware_metadata_stays_false(self):
        report = self.parse("device_verified=false hardware_acceptance=false\n" + samples())
        self.assertTrue(report["host_policy_passed"])
        self.assertIs(report["device_verified"], False)

    def test_mixed_cpu_and_reported_provenance_refused(self):
        self.rejects(samples().replace("note=cpu_reference_only", "note=reported_hwtest", 1),
                     "MIXED_CPU_AND_REPORTED_EVIDENCE")

    def test_reported_samples_still_never_certify_hardware(self):
        report = self.parse(samples(note="reported_hwtest"))
        self.assertTrue(report["host_policy_passed"])
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        with self.assertRaises(GateError):
            gate.require_samples(report, "phase4")

    def test_non_text_record_is_a_policy_failure(self):
        self.rejects([b"HWTEST\n"], "ENCODING")

    def test_unknown_policy_is_refused(self):
        with self.assertRaisesRegex(GateError, "UNKNOWN_PHASE4_POLICY"):
            self.parse(samples(), "phase5")

    def test_parser_globals_stay_unchanged_with_concurrent_policies(self):
        before = (hwtest_parser.CASES, dict(hwtest_parser.TOLERANCES), hwtest_phase3_policy.CASES)
        with ThreadPoolExecutor(max_workers=8) as pool:
            reports = list(pool.map(lambda mode: self.parse(samples(mode), mode), ["phase4", "full_system"] * 12))
        self.assertTrue(all(report["host_policy_passed"] for report in reports))
        self.assertEqual(before, (hwtest_parser.CASES, hwtest_parser.TOLERANCES, hwtest_phase3_policy.CASES))


class PolicyCliAndOutputTests(CachedCase):
    def command(self, text=None, *, output=None, suite="phase4", source=None):
        path = self.cache / "sample.hwtest"
        if text is not None:
            path.write_text(text, encoding="utf-8")
        arguments = ["--predicates", REPO / "contracts/DualPathPredicates.tsv", "--policy", suite,
                     "--input", "-" if source is not None else path]
        if output is not None:
            arguments += ["--json-out", output]
        return self.cli("hwtest_phase4_policy.py", *arguments, source=source)

    def test_cli_stdin_full_system_policy(self):
        result = self.command(suite="full_system", source=samples("full_system"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["required_case_count"], 24)

    def test_cli_writes_identical_safe_json(self):
        output = self.cache / "policy.json"
        result = self.command(samples(), output=output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), json.loads(output.read_text()))

    def test_cli_private_fields_never_echo(self):
        result = self.command(samples() + "serial_number=DO_NOT_ECHO\n")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("DO_NOT_ECHO", result.stdout + result.stderr)

    def test_windows_c_output_refused_before_mkdir(self):
        for path in ("C:/blocked.json", r"c:\Users\blocked.json", "/mnt/c/blocked.json", "/MNT/C/blocked.json"):
            with self.subTest(path=path):
                result = self.command(samples(), output=path)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(json.loads(result.stdout)["errors"][0]["code"], "C_DRIVE_OUTPUT_FORBIDDEN")

    def test_output_outside_lane_cache_refused(self):
        output = policy.CACHE_ROOT.parent / "blocked-w08-l4.json"
        result = self.command(samples(), output=output)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(output.exists())

    def test_worktree_output_refused(self):
        result = self.command(samples(), output=REPO / "blocked-w08-l4.json")
        self.assertEqual(result.returncode, 1)
        self.assertFalse((REPO / "blocked-w08-l4.json").exists())

    def test_symlink_file_output_refused(self):
        output = self.cache / "link.json"
        output.symlink_to(self.cache / "new.json")
        result = self.command(samples(), output=output)
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.cache / "new.json").exists())

    def test_symlink_parent_output_refused(self):
        target = self.cache / "real"
        target.mkdir()
        link = self.cache / "link"
        link.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(GateError, "SYMLINK_OUTPUT_FORBIDDEN"):
            policy.checked_output_path(link / "report.json")

    def test_cache_root_cannot_be_redirected(self):
        link = self.cache / "root-link"
        link.symlink_to(self.cache, target_is_directory=True)
        with mock.patch.object(policy, "CACHE_ROOT", link):
            with self.assertRaisesRegex(GateError, "CACHE_ROOT_REDIRECT"):
                policy.checked_output_path(link / "report.json")

    def test_output_cannot_overwrite_input(self):
        path = self.cache / "sample.hwtest"
        result = self.command(samples(), output=path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(path.read_text(), samples())

    def test_hardlink_alias_to_input_is_refused(self):
        source = self.cache / "sample.hwtest"
        source.write_text(samples())
        output = self.cache / "hardlink.json"
        os.link(source, output)
        result = self.command(output=output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(output.read_text(), samples())

    def test_missing_input_has_safe_failure_flags(self):
        result = self.command()
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)

    def test_cache_root_is_allowed_as_directory_only(self):
        self.assertEqual(policy.checked_output_path(policy.CACHE_ROOT, directory=True), policy.CACHE_ROOT)
        with self.assertRaises(GateError):
            policy.checked_output_path(policy.CACHE_ROOT)

    def test_atomic_json_keeps_previous_on_replace_failure(self):
        output = self.cache / "report.json"
        output.write_text("previous")
        with mock.patch.object(policy.os, "replace", side_effect=OSError("replace failed")):
            with self.assertRaises(OSError):
                policy.write_json(output, {"status": "PASS"})
        self.assertEqual(output.read_text(), "previous")
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), ["report.json"])


class HostGateTests(CachedCase):
    def stub_suites(self, **overrides):
        methods = {name: mock.DEFAULT for name in ("tool", "kmd", "kmd_sibling", "service", "pack_sibling",
                   "runtime", "runtime_sibling", "pixel_hwtest", "audit", "no_fallback", "tsan", "wave")}
        methods.update(overrides)
        return mock.patch.multiple(gate.HostRun, **methods)

    def test_quick_runs_every_required_stage(self):
        with self.stub_suites() as stubs:
            report = gate.run_gate(REPO, MESA, self.cache / "quick")
        self.assertEqual(tuple(row["name"] for row in report["results"]), gate.BASE_STAGES)
        self.assertTrue(report["phase4_host_ready"])
        stubs["tsan"].assert_not_called()
        stubs["wave"].assert_not_called()

    def test_full_requests_both_tsan_and_clang_wave(self):
        with self.stub_suites() as stubs:
            report = gate.run_gate(REPO, MESA, self.cache / "full", "full", True)
        self.assertTrue(report["phase4_host_ready"])
        self.assertEqual(stubs["tsan"].call_count, 2)
        self.assertIs(stubs["wave"].call_args.args[1], True)
        self.assertFalse(report["verify_wave_skipped"])

    def test_child_failure_is_saved_and_other_stages_continue(self):
        failure = mock.Mock(side_effect=GateError("KMD_FAILURE"))
        with self.stub_suites(kmd=failure) as stubs:
            report = gate.run_gate(REPO, MESA, self.cache / "failure")
            stubs["runtime"].assert_called_once()
        self.assertFalse(report["phase4_host_ready"])
        row = next(row for row in report["results"] if row["name"] == "kmd_phase4_memory")
        self.assertEqual(row["error"], "KMD_FAILURE")
        self.assertEqual(json.loads(Path(report["summary"]).read_text()), report)

    def test_failure_and_pass_flags_are_fixed_at_every_stage(self):
        with self.stub_suites(service=mock.Mock(side_effect=GateError("SERVICE_FAIL"))):
            report = gate.run_gate(REPO, MESA, self.cache / "flags")
        for row in [report, *report["results"]]:
            self.assertIs(row["device_verified"], False)
            self.assertIs(row["hardware_acceptance"], False)

    def test_full_wave_failure_refuses_ready(self):
        with self.stub_suites(wave=mock.Mock(side_effect=GateError("WAVE_FAILED"))):
            report = gate.run_gate(REPO, MESA, self.cache / "full", "full")
        self.assertFalse(report["phase4_host_ready"])

    def test_invalid_gate_mode_fails_before_output(self):
        with self.assertRaisesRegex(GateError, "INVALID_GATE_MODE"):
            gate.run_gate(REPO, MESA, self.cache / "bad", "dry-run")
        self.assertFalse((self.cache / "bad").exists())

    def test_non_boolean_clang_option_refused(self):
        with self.assertRaises(GateError):
            gate.run_gate(REPO, MESA, self.cache / "bad", with_clang=1)

    def test_cli_bad_output_keeps_both_false_flags(self):
        result = self.cli("phase4_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--cache-dir", "/mnt/c/blocked-phase4", "--mode", "quick")
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertFalse(report["phase4_host_ready"])
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)

    def test_cli_unknown_mode_is_argument_error(self):
        result = self.cli("phase4_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--cache-dir", self.cache, "--mode", "dry-run")
        self.assertEqual(result.returncode, 2)

    def test_cli_help_describes_clang_and_full(self):
        result = self.cli("phase4_host_gate.py", "--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--with-clang", result.stdout)
        self.assertIn("quick,full", result.stdout)

    def test_absent_sibling_is_deferred_and_never_hardware_verified(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        row = {"commands": []}
        self.assertIsNone(run.present(row, "W08-L1", gate.W08_KMD_FILES, (gate.W08_KMD,)))
        self.assertEqual(row["status"], "deferred_sibling_not_present")

    def test_partial_sibling_sources_fail_closed(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        file = self.cache / gate.W08_KMD_FILES[1]
        file.parent.mkdir(parents=True)
        file.write_text("/* partial sibling */")
        with self.assertRaisesRegex(GateError, "SIBLING_PARTIAL_OR_AMBIGUOUS"):
            run.present({"commands": []}, "W08-L1", gate.W08_KMD_FILES, (gate.W08_KMD,))

    def test_present_sibling_has_required_source_set(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        for relative in gate.W08_KMD_FILES:
            file = self.cache / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("/* host source fixture */")
        row = {"commands": []}
        with mock.patch.object(run, "run_native") as native:
            run.kmd_sibling(row)
        native.assert_called_once()
        self.assertEqual(native.call_args.args[1], gate.W08_KMD)
        self.assertIn(gate.W08_KMD_FILES[2], native.call_args.args[2])
        self.assertEqual(row["sibling"]["status"], "present_required")

    def test_present_sibling_failure_propagates(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        for relative in gate.W08_RUNTIME_FILES:
            file = self.cache / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("/* host source fixture */")
        with mock.patch.object(run, "run_streams", side_effect=GateError("SIBLING_FAIL")):
            with self.assertRaisesRegex(GateError, "SIBLING_FAIL"):
                run.runtime_sibling({"commands": []})

    def test_broken_sibling_symlink_cannot_be_deferred(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        path = self.cache / gate.W08_KMD
        path.parent.mkdir(parents=True)
        path.symlink_to(self.cache / "absent.cpp")
        with self.assertRaisesRegex(GateError, "SYMLINK_SOURCE"):
            run.present({"commands": []}, "W08-L1", gate.W08_KMD_FILES, (gate.W08_KMD,))

    def test_two_pack_test_locations_are_ambiguous(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        for relative in (gate.W08_PACK, gate.W08_PACK_ALTERNATE):
            path = self.cache / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# fixture")
        with self.assertRaisesRegex(GateError, "SIBLING_PARTIAL_OR_AMBIGUOUS"):
            run.pack_sibling({"commands": []})

    def test_pack_discovery_cannot_pass_zero_tests(self):
        run = gate.HostRun(self.cache, MESA, self.cache / "run", False)
        for relative in (gate.W08_PACK, *gate.W08_PACK_FILES):
            path = self.cache / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# fixture" if path.suffix == ".py" else "/* fixture */")
        with mock.patch.object(gate, "execute", return_value={"stdout": "", "stderr": "Ran 0 tests\nOK"}):
            with self.assertRaisesRegex(GateError, "SIBLING_TEST_DISCOVERY_EMPTY"):
                run.pack_sibling({"commands": []})

    def test_host_samples_checker_rejects_true_hardware_flag(self):
        report = self.parse(samples())
        report["device_verified"] = True
        with self.assertRaises(GateError):
            gate.require_samples(report, "phase4")

    def test_host_samples_checker_rejects_missing_record(self):
        report = self.parse(samples())
        report["records"].pop()
        with self.assertRaises(GateError):
            gate.require_samples(report, "phase4")

    def test_pm4_gate_requires_nonempty_a8xx(self):
        good = {"status": "PASS", "chip": "A8XX", "packet_count": 2, "dwords": 4}
        gate.require_pm4(good)
        for changed in ({"chip": "A7XX"}, {"packet_count": 0}, {"dwords": 0}, {"packet_count": True}):
            with self.subTest(changed=changed), self.assertRaises(GateError):
                gate.require_pm4(dict(good, **changed))

    def test_pm4_diff_must_be_strict_and_unmasked(self):
        good = {"status": "PASS", "chip": "A8XX", "comparison": "packets", "first_difference": None,
                "expected_packets": 2, "actual_packets": 2, "expected_operations": 3, "actual_operations": 3,
                "ignored_register_values": [], "ignored_opcode_payloads": []}
        gate.require_pm4_diff(good)
        for changed in ({"comparison": "operations"}, {"ignored_register_values": ["MASKED"]},
                        {"actual_packets": 1}, {"first_difference": {"index": 0}}):
            with self.subTest(changed=changed), self.assertRaises(GateError):
                gate.require_pm4_diff(dict(good, **changed))

    def test_reference_audit_cannot_claim_runtime_process_coverage(self):
        report = fallback_audit.audit_lines(samples("full_system"), fallback_audit.load_predicates(),
                                            targets=("NezhaGpuTest",))
        gate.require_audit(report, 24)
        report["runtime_submissions"] = 1
        with self.assertRaises(GateError):
            gate.require_audit(report, 24)

    def test_empty_native_symbol_scan_refused(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        with self.assertRaisesRegex(GateError, "EMPTY_NATIVE_OBJECT_SCAN"):
            run.no_fallback({"commands": []})

    def test_missing_cpu_samples_refuse_fallback_audit(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        with self.assertRaisesRegex(GateError, "MISSING_FULL_SYSTEM_CPU_SAMPLES"):
            run.audit({"commands": []})

    def test_unique_run_directories_do_not_reuse_prior_pass(self):
        with self.stub_suites():
            first = gate.run_gate(REPO, MESA, self.cache / "shared")
            second = gate.run_gate(REPO, MESA, self.cache / "shared")
        self.assertNotEqual(first["run_dir"], second["run_dir"])
        self.assertEqual(json.loads(Path(second["summary"]).read_text()), second)

    def test_environment_redirects_all_temporaries_to_lane_cache(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        for key in ("TMPDIR", "TMP", "TEMP", "PYTHONPYCACHEPREFIX", "XDG_CACHE_HOME"):
            Path(run.env[key]).relative_to(policy.CACHE_ROOT)
        self.assertEqual(run.env["PYTHONDONTWRITEBYTECODE"], "1")

    def test_tsan_uses_twenty_rounds_and_aslr_control(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        binary = self.cache / "test"
        row = {"commands": []}
        with mock.patch.object(run, "build", return_value=binary) as build, mock.patch.object(gate, "execute") as execute:
            run.tsan(row, gate.KMD_TEST, gate.KMD_SOURCES)
        self.assertEqual(build.call_args.args[-1], "thread")
        self.assertEqual(execute.call_args.args[1], ["setarch", "x86_64", "-R", str(binary), "--tsan-stress", "20"])
        self.assertEqual(row["tsan_rounds"], 20)

    def test_native_build_keeps_strict_flags_without_inlining_large_asan_fixture(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        outcome = {"command": [], "returncode": 0, "stdout": "", "stderr": "", "seconds": 0}
        row = {"commands": []}
        with mock.patch.object(run, "compiler", return_value=("gcc-13", "g++-13", [])), \
                mock.patch.object(gate, "execute", return_value=outcome) as execute:
            run.build(row, gate.KMD_TEST, [], "gcc")
        command = execute.call_args.args[1]
        self.assertIn("-Werror", command)
        self.assertIn("-pedantic", command)
        self.assertIn("-fsanitize=address,undefined", command)
        self.assertNotIn("-O1", command)

    def test_wave_invokes_existing_cli_and_reads_its_fixed_summary_name(self):
        run = gate.HostRun(REPO, MESA, self.cache / "run", False)
        row = {"commands": []}

        def child(command, **_options):
            self.assertNotIn("--json-out", command)
            self.assertIn("--with-clang", command)
            output = Path(command[command.index("--cache-dir") + 1])
            (output / "verify_wave_summary.json").write_text('{"status":"PASS"}')
            return {"returncode": 0, "stdout": "", "stderr": ""}

        with mock.patch.object(gate, "run_command", side_effect=child), \
                mock.patch.object(gate, "check_wave_report", return_value={"all": "PASS"}):
            run.wave(row, True)
        self.assertEqual(Path(row["summary"]).name, "verify_wave_summary.json")

    def test_wave_budget_extension_changes_only_discovery_timeout(self):
        original_runner = gate.verify_wave.verify_wave
        original_main = gate.verify_wave.main

        def entry(_argv):
            runner = cloned["runner"]
            command_gate = runner.__globals__["command_gate"]
            command_gate("unittest", [["python3", "-m", "unittest", "discover"]], cwd=REPO, env={})
            command_gate("contracts", [["python3", "check_contracts.py"]], cwd=REPO, env={}, timeout=123)
            return 0

        cloned = {}
        real_function_type = gate.FunctionType

        def capture(code, globals_, name, defaults):
            function = real_function_type(code, globals_, name, defaults)
            if name == "phase4_bounded_wave":
                cloned["runner"] = function
                return function
            return entry

        with mock.patch.object(gate, "FunctionType", side_effect=capture), \
                mock.patch.object(gate.verify_wave, "command_gate") as command:
            self.assertEqual(gate.wave_cli([]), 0)
        self.assertEqual(command.call_args_list[0].kwargs["timeout"], 900)
        self.assertEqual(command.call_args_list[1].kwargs["timeout"], 123)
        self.assertIs(gate.verify_wave.verify_wave, original_runner)
        self.assertIs(gate.verify_wave.main, original_main)

    def test_real_quick_gate_builds_executes_and_saves_host_only_summary(self):
        output = self.cache / "real-quick"
        result = self.cli("phase4_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--cache-dir", output, "--mode", "quick", timeout=600)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        concise = json.loads(result.stdout)
        self.assertTrue(concise["phase4_host_ready"])
        self.assertIs(concise["device_verified"], False)
        self.assertIs(concise["hardware_acceptance"], False)
        saved = json.loads((output / "phase4_host_summary.json").read_text())
        rows = {row["name"]: row for row in saved["results"]}
        self.assertEqual(tuple(rows), gate.BASE_STAGES)
        self.assertEqual(rows["cpu_phase4_hwtest"]["sample_counts"]["gcc"],
                         {"phase4": 10, "phase4_ext": 5, "full_system": 24})
        self.assertEqual(len(rows["runtime_phase4_heap_argbuf_icb"]["streams"]), 2)
        self.assertTrue(all(stream["packets"] > 0 for stream in rows["runtime_phase4_heap_argbuf_icb"]["streams"]))
        self.assertGreater(rows["no_fallback"]["symbols_scanned"], 0)
        self.assertEqual(rows["fallback_audit"]["runtime_submissions"], 0)
        commands = [command for row in saved["results"] for command in row["commands"]]
        self.assertTrue(any("-Werror" in command["command"] and "-fsanitize=address,undefined" in command["command"]
                            and command["returncode"] == 0 for command in commands))
        self.assertFalse(any("--tsan-stress" in command["command"] for command in commands))


if __name__ == "__main__":
    unittest.main()
