# SPDX-License-Identifier: MIT
"""Phase 5 three-way comparison, scheduling, and real CLI integration tests."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from check_contracts import CACHE_PARENT, GateError, cache_environment
import frame_weights_profiler as fw
import phase5_host_gate as gate
import phase5_perf_compare as perf

REPO = Path(__file__).resolve().parents[2]
MESA = Path(os.environ.get("A840_VERIFY_MESA",
                          "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work"))
APP, SETTINGS, LIBRARY, GLASS = fw.CORE_SCENES
ALL_STAGES = (
    "contracts", "driver_constants", "kmd_phase5_preempt", "kmd_phase5_gmu_mdss",
    "ir3_robustness_and_stats", "ir3_metal_hotspot_opt", "emit_sync_tracker_pm4", "runtime_frame_arena",
    "runtime_cb_chunking_promise", "frame_weights_profiler", "phase5_perf_compare", "no_fallback_final",
    "phase4_non_regression", "verify_wave",
)


def frame(number: int, scene: str = APP, **changes) -> fw.Frame:
    values = {"frame_seq": number, "scene_id": next(key for key, name in fw.SCENES.items() if name == scene),
              "ring_id": 0, "t_vsync_prev_ns": fw.BUDGET_NS * number,
              "pre_create_ns": 2_000_000, "encode_ns": 1_000_000, "submit_ns": 500_000,
              "gpu_exec_ns": 2_000_000, "wait_sync_ns": 500_000,
              "physical_frame_interval_ns": 6_000_000, "wfi_count": 2,
              "preempted_count": 1, "cache_flush_count": 3, "heap_allocs": 0}
    values.update(changes)
    values["scanout_margin_ns"] = fw.BUDGET_NS - sum(values[key] for key in fw.TIMED_STAGES)
    return fw.Frame(**values)


def profile(*frames) -> dict:
    return fw.analyze(fw.Trace("JSONL", list(frames), []))


def four(*, scenes=fw.CORE_SCENES, repeat=1, **changes) -> dict:
    return profile(*(frame(index, scene, **changes)
                     for index, scene in enumerate(scenes * repeat, start=1)))


def scene_report(report: dict, name: str = APP, phase: str = "phase5") -> dict:
    return report["scenes"][name]["phases"][phase]


def trend(report: dict, metric: str, name: str = APP, pair: str = "phase3_to_phase5") -> dict:
    return report["scenes"][name]["comparisons"][metric][pair]


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.base = four()

    def compare(self, candidate=None, **kwargs):
        return perf.compare(self.base, self.base, self.base if candidate is None else candidate, **kwargs)

    def test_full_four_scene_and_three_phase_order(self):
        report = self.compare()
        self.assertTrue(report["all_four_scenes_present"])
        self.assertTrue(report["coverage_complete"])
        self.assertEqual(tuple(report["scenes"]), fw.CORE_SCENES)
        for scene in report["scenes"].values():
            self.assertEqual(tuple(scene["phases"]), perf.PHASES)

    def test_report_provenance_is_fixed(self):
        report = self.compare()
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        self.assertEqual(report["source_evidence_kind"], "unverified_phase_comparison")

    def test_physical_nearest_rank(self):
        candidate = profile(*(frame(i, physical_frame_interval_ns=i * 1000) for i in range(1, 101)))
        timing = scene_report(self.compare(candidate, allow_partial_scenes=True))["physical_intervals"]
        self.assertEqual([timing[name] for name in perf.DISTRIBUTION_FIELDS], [50, 95, 99, 100])

    def test_software_nearest_rank_ignores_reported_physical_intervals(self):
        candidate = profile(*(frame(i, pre_create_ns=i * 1000) for i in range(1, 101)))
        timing = scene_report(self.compare(candidate, allow_partial_scenes=True))["software_pipeline"]
        self.assertEqual([timing[name] for name in perf.DISTRIBUTION_FIELDS], [4050, 4095, 4099, 4100])

    def test_cumulative_buckets_include_each_edge(self):
        edges = (8_333_332, 8_333_333, 16_666_666, 16_666_667, 19_999_999, 20_000_000,
                 99_999_999, 100_000_000)
        candidate = profile(*(frame(i, physical_frame_interval_ns=ns) for i, ns in enumerate(edges, 1)))
        distribution = scene_report(self.compare(candidate, allow_partial_scenes=True))["physical_intervals"]
        self.assertEqual(list(distribution["stall_buckets"].values()), [7, 5, 3, 1])
        self.assertEqual(distribution["within_budget_pct"], 25)

    def test_exact_backend_budget_passes_and_enters_inclusive_bucket(self):
        report = self.compare(four(wait_sync_ns=4_833_333))
        backend = scene_report(report)["backend"]
        self.assertTrue(report["backend_within_120hz_budget"])
        self.assertTrue(backend["within_120hz_budget"])
        self.assertEqual((backend["max_us"], backend["stall_buckets"]["ge_8_333_ms"]), (8333.333, 1))

    def test_one_ns_past_backend_budget_fails(self):
        report = self.compare(four(wait_sync_ns=4_833_334))
        self.assertFalse(report["backend_within_120hz_budget"])
        self.assertEqual(scene_report(report)["backend"]["max_us"], 8333.334)

    def test_backend_uses_per_frame_budget_including_tail(self):
        candidate = profile(*(frame(i + 1, APP, wait_sync_ns=6_000_000 if i == 49 else 500_000)
                              for i in range(50)),
                            *(frame(i + 51, scene) for i, scene in enumerate((SETTINGS, LIBRARY, GLASS))))
        report = self.compare(candidate)
        backend = scene_report(report)["backend"]
        self.assertEqual((backend["p95_us"], backend["max_us"], backend["within_budget_pct"]),
                         (4000, 9500, 98))
        self.assertFalse(report["backend_within_120hz_budget"])

    def test_missing_physical_never_falls_back_to_software(self):
        report = self.compare(four(physical_frame_interval_ns=0))
        values = scene_report(report)
        self.assertEqual(values["physical_intervals"]["sample_count"], 0)
        self.assertIsNone(values["physical_intervals"]["p50_us"])
        self.assertEqual(values["software_pipeline"]["p50_us"], 6000)
        self.assertEqual(trend(report, "physical_intervals.ge_8_333_ms_count"),
                         {"delta": None, "verdict": "UNMEASURED"})

    def test_physical_rate_uses_only_measured_intervals(self):
        candidate = profile(frame(1), frame(2, physical_frame_interval_ns=0),
                            frame(3, physical_frame_interval_ns=20_000_000))
        physical = scene_report(self.compare(candidate, allow_partial_scenes=True))["physical_intervals"]
        self.assertEqual((physical["sample_count"], physical["within_budget_pct"]), (2, 50))

    def test_backend_sums_encode_submit_gpu_wait(self):
        report = self.compare(four(encode_ns=1000, submit_ns=3000, gpu_exec_ns=7000, wait_sync_ns=12_000))
        self.assertEqual(scene_report(report)["backend"]["mean_us"], 23)

    def test_f07_long_stall_and_backend_success_can_coexist(self):
        report = self.compare(four(pre_create_ns=116_000_000, physical_frame_interval_ns=120_000_000))
        scene = scene_report(report, GLASS)
        self.assertTrue(report["backend_within_120hz_budget"])
        self.assertEqual(scene["backend"]["within_budget_pct"], 100)
        self.assertEqual(scene["physical_intervals"]["stall_buckets"]["ge_100_ms"], 1)
        self.assertEqual(scene["f07_pre_create"]["physical_stall_count"], 1)
        self.assertAlmostEqual(scene["f07_pre_create"]["share_pct"], 116 / 1.2)

    def test_f07_can_dominate_while_backend_independently_misses_budget(self):
        report = self.compare(four(pre_create_ns=100_000_000, wait_sync_ns=7_000_000,
                                   physical_frame_interval_ns=120_000_000))
        values = scene_report(report)
        self.assertEqual(values["physical_intervals"]["max_stall"]["attribution"], "F07_PRE_CREATE_DOMINANT")
        self.assertFalse(values["backend"]["within_120hz_budget"])

    def test_unattributed_physical_delay_is_not_counted_as_f07(self):
        scene = scene_report(self.compare(four(physical_frame_interval_ns=110_000_000)))
        self.assertEqual(scene["f07_pre_create"]["physical_stall_count"], 0)
        self.assertEqual(scene["physical_intervals"]["max_stall"]["attribution"], "UNATTRIBUTED_OVERRUN")

    def test_share_rises_when_backend_improves_with_upstream_unchanged(self):
        report = self.compare(four(gpu_exec_ns=1_000_000))
        change = trend(report, "f07_pre_create.share_pct")
        self.assertGreater(change["delta"], 0)
        self.assertEqual(change["verdict"], "NEUTRAL")
        self.assertEqual(trend(report, "f07_pre_create.mean_us")["verdict"], "NEUTRAL")
        self.assertEqual(trend(report, "backend.mean_us")["verdict"], "IMPROVED")
        self.assertFalse(report["regression_detected"])

    def test_reclassifying_a_stall_after_backend_improvement_is_diagnostic(self):
        baseline = four(pre_create_ns=10_000_000, gpu_exec_ns=10_000_000,
                        physical_frame_interval_ns=30_000_000)
        candidate = four(pre_create_ns=10_000_000, gpu_exec_ns=2_000_000,
                         physical_frame_interval_ns=30_000_000)
        report = perf.compare(baseline, baseline, candidate)
        self.assertEqual(trend(report, "f07_pre_create.physical_stall_count"),
                         {"delta": 1, "verdict": "NEUTRAL"})
        self.assertEqual(trend(report, "f07_pre_create.mean_us")["verdict"], "NEUTRAL")
        self.assertEqual(trend(report, "backend.mean_us")["verdict"], "IMPROVED")
        self.assertFalse(report["regression_detected"])

    def test_zero_pipeline_has_null_context_shares(self):
        zero = four(**dict.fromkeys(fw.TIMED_STAGES, 0))
        scene = scene_report(self.compare(zero))
        self.assertIsNone(scene["f07_pre_create"]["share_pct"])
        self.assertIsNone(scene["backend"]["share_pct"])
        self.assertEqual(scene["backend"]["within_budget_pct"], 100)

    def test_zero_frame_scene_is_missing_even_if_listed_in_schema(self):
        with self.assertRaisesRegex(fw.ProfileError, "phase5 DENSE_LIQUID_GLASS_SWIPE"):
            self.compare(four(scenes=(APP, SETTINGS, LIBRARY)))

    def test_partial_flag_lists_missing_phase_and_preserves_other_phases(self):
        report = self.compare(four(scenes=(APP, SETTINGS, LIBRARY)), allow_partial_scenes=True)
        self.assertFalse(report["all_four_scenes_present"])
        self.assertFalse(report["coverage_complete"])
        self.assertFalse(report["backend_within_120hz_budget"])
        self.assertEqual(report["missing_scenes"], {"phase0": [], "phase3": [], "phase5": [GLASS]})
        self.assertEqual(trend(report, "backend.p95_us", GLASS)["verdict"], "UNMEASURED")
        self.assertEqual(trend(report, "backend.p95_us", GLASS, "phase0_to_phase3")["verdict"], "NEUTRAL")

    def test_all_phases_missing_scene_still_has_unmeasured_verdict(self):
        partial = four(scenes=(APP, SETTINGS, LIBRARY))
        report = perf.compare(partial, partial, partial, allow_partial_scenes=True)
        self.assertIsNone(scene_report(report, GLASS))
        self.assertEqual(trend(report, "backend.p99_us", GLASS), {"delta": None, "verdict": "UNMEASURED"})

    def test_phase5_backend_flag_does_not_depend_on_phase0_coverage(self):
        report = perf.compare(four(scenes=(APP,)), self.base, self.base, allow_partial_scenes=True)
        self.assertFalse(report["coverage_complete"])
        self.assertTrue(report["backend_within_120hz_budget"])

    def test_custom_scene_cannot_fill_required_coverage(self):
        with self.assertRaisesRegex(fw.ProfileError, GLASS):
            self.compare(four(scenes=(APP, SETTINGS, LIBRARY, "CUSTOM")))

    def test_scene_missing_from_json_is_ok_only_without_frames_and_with_partial(self):
        candidate = four(scenes=(APP, SETTINGS, LIBRARY))
        del candidate["scenes"][GLASS]
        self.assertEqual(self.compare(candidate, allow_partial_scenes=True)["missing_scenes"]["phase5"], [GLASS])
        with self.assertRaises(fw.ProfileError):
            self.compare(candidate)

    def test_regression_is_attributed_only_to_affected_scene(self):
        candidate = profile(frame(1, APP), frame(2, SETTINGS), frame(3, LIBRARY),
                            frame(4, GLASS, physical_frame_interval_ns=100_000_000))
        report = self.compare(candidate)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(report["regression_detected"])
        self.assertTrue(report["scenes"][GLASS]["regression_detected"])
        self.assertFalse(report["scenes"][APP]["regression_detected"])
        self.assertEqual(trend(report, "physical_intervals.ge_100_ms_count", GLASS)["verdict"], "REGRESSED")

    def test_historical_regression_visible_when_phase5_recovers(self):
        report = perf.compare(self.base, four(gpu_exec_ns=3_000_000), self.base)
        self.assertFalse(report["regression_detected"])
        self.assertEqual(trend(report, "backend.p95_us", pair="phase0_to_phase3")["verdict"], "REGRESSED")
        self.assertEqual(trend(report, "backend.p95_us")["verdict"], "IMPROVED")

    def test_regression_against_phase3_even_if_phase0_improved(self):
        report = perf.compare(four(gpu_exec_ns=3_000_000), four(gpu_exec_ns=1_000_000), self.base)
        self.assertTrue(report["regression_detected"])
        self.assertEqual(trend(report, "backend.p95_us")["verdict"], "REGRESSED")
        self.assertEqual(trend(report, "backend.p95_us", pair="phase0_to_phase5")["verdict"], "IMPROVED")

    def test_regression_against_phase0_even_if_phase3_improved(self):
        report = perf.compare(four(gpu_exec_ns=1_000_000), four(gpu_exec_ns=3_000_000), self.base)
        self.assertTrue(report["regression_detected"])
        self.assertEqual(trend(report, "backend.p95_us")["verdict"], "IMPROVED")
        self.assertEqual(trend(report, "backend.p95_us", pair="phase0_to_phase5")["verdict"], "REGRESSED")

    def test_higher_within_budget_rate_improves(self):
        previous = four(physical_frame_interval_ns=12_000_000)
        report = perf.compare(previous, previous, self.base)
        self.assertEqual(trend(report, "physical_intervals.within_budget_pct"),
                         {"delta": 100, "verdict": "IMPROVED"})

    def test_equal_measured_samples_are_neutral(self):
        report = self.compare()
        self.assertEqual(trend(report, "backend.mean_us"), {"delta": 0, "verdict": "NEUTRAL"})
        self.assertEqual(trend(report, "efficiency.heap_allocs"), {"delta": 0, "verdict": "NEUTRAL"})


    def test_missing_instruction_counts_are_unmeasured(self):
        report = self.compare(four(instruction_count=0))
        self.assertEqual(scene_report(report)["efficiency"]["instruction_count"], 0)
        self.assertIsNone(scene_report(report, phase="phase3")["efficiency"]["instruction_count"])
        self.assertEqual(trend(report, "efficiency.instruction_count")["verdict"], "UNMEASURED")

    def test_sample_count_change_keeps_raw_totals_unmeasured(self):
        report = self.compare(four(repeat=2))
        self.assertFalse(report["regression_detected"])
        self.assertEqual(trend(report, "efficiency.wfi_count")["verdict"], "UNMEASURED")
        self.assertEqual(trend(report, "efficiency.wfi_count_per_frame")["verdict"], "NEUTRAL")
        self.assertEqual(trend(report, "backend.total_us")["verdict"], "UNMEASURED")

    def test_higher_stall_rate_detected_with_different_sample_counts(self):
        short = profile(frame(1, physical_frame_interval_ns=20_000_000), frame(2), frame(3))
        report = perf.compare(short, short, four(physical_frame_interval_ns=20_000_000, repeat=2),
                              allow_partial_scenes=True)
        self.assertEqual(trend(report, "physical_intervals.ge_20_ms_count")["verdict"], "UNMEASURED")
        self.assertEqual(trend(report, "physical_intervals.ge_20_ms_pct")["verdict"], "REGRESSED")

    def test_efficiency_counter_directions_are_lower(self):
        report = self.compare(four(wfi_count=1, cache_flush_count=2, preempted_count=0, heap_allocs=1))
        for metric in ("wfi_count", "cache_flush_count", "preempted_count"):
            self.assertEqual(trend(report, f"efficiency.{metric}")["verdict"], "IMPROVED")
        self.assertEqual(trend(report, "efficiency.heap_allocs")["verdict"], "REGRESSED")

    def test_extra_power_and_thermal_metrics_have_distinct_directions(self):
        report = self.compare(extra_metrics={
            "phase3": {APP: {"avg_gpu_mhz": 578, "thermal_throttled_pct": 30}},
            "phase5": {APP: {"avg_gpu_mhz": 726, "thermal_throttled_pct": 10}},
        })
        self.assertEqual(trend(report, "efficiency.avg_gpu_mhz"), {"delta": 148, "verdict": "NEUTRAL"})
        self.assertEqual(trend(report, "efficiency.thermal_throttled_pct")["verdict"], "IMPROVED")
        self.assertIsNone(scene_report(report, GLASS)["efficiency"]["avg_gpu_mhz"])

    def test_extra_instruction_count_fills_missing_profiler_measurement(self):
        report = self.compare(extra_metrics={
            "phase3": {APP: {"instruction_count": 120}},
            "phase5": {APP: {"instruction_count": 90}},
        })
        self.assertEqual(trend(report, "efficiency.instruction_count"), {"delta": -30, "verdict": "IMPROVED"})

    def test_extra_instruction_count_cannot_override_measured_value(self):
        with self.assertRaisesRegex(fw.ProfileError, "disagrees with the profiler"):
            self.compare(four(instruction_count=30), extra_metrics={"phase5": {APP: {"instruction_count": 1}}})

    def test_extra_metrics_reject_unknown_scene_and_phase(self):
        for extras in ({"phase4": {}}, {"phase5": {"fictional": {"avg_gpu_mhz": 500}}}):
            with self.subTest(extras=extras), self.assertRaises(fw.ProfileError):
                self.compare(extra_metrics=extras)

    def test_extra_metrics_require_captured_frames(self):
        with self.assertRaisesRegex(fw.ProfileError, "require captured scene"):
            self.compare(four(scenes=(APP,)), allow_partial_scenes=True,
                         extra_metrics={"phase5": {GLASS: {"avg_gpu_mhz": 580}}})

    def test_extra_metrics_reject_nonnumeric_and_out_of_range(self):
        for key, value in (("thermal_throttled_pct", 101), ("avg_gpu_mhz", float("inf")),
                           ("avg_gpu_mhz", -1), ("instruction_count", True)):
            with self.subTest(key=key), self.assertRaises(fw.ProfileError):
                self.compare(extra_metrics={"phase5": {APP: {key: value}}})


class SchemaTests(unittest.TestCase):
    def setUp(self):
        self.report = four()

    def rejects(self, edit, message):
        candidate = copy.deepcopy(self.report)
        edit(candidate)
        with self.assertRaisesRegex(fw.ProfileError, message):
            perf.compare(self.report, self.report, candidate, allow_partial_scenes=True)

    def test_reject_device_verified_true(self):
        self.rejects(lambda obj: obj.update(device_verified=True), "device_verified")

    def test_reject_hardware_acceptance_true(self):
        self.rejects(lambda obj: obj.update(hardware_acceptance=True), "hardware_acceptance")

    def test_reject_lookalike_booleans_and_missing_flags(self):
        self.rejects(lambda obj: obj.update(device_verified=0), "device_verified")
        self.rejects(lambda obj: obj.pop("hardware_acceptance"), "hardware_acceptance")

    def test_reject_unrecognized_provenance_tag(self):
        self.rejects(lambda obj: obj.update(source_evidence_kind="device"), "source_evidence_kind")

    def test_reject_unrecognized_source_format(self):
        self.rejects(lambda obj: obj.update(source_format="recreated"), "source_format")

    def test_binary_capture_report_with_no_compiler_counter_supported(self):
        raw = dict(self.report, source_format="A840FW01")
        self.assertEqual(perf.compare(raw, self.report, self.report)["inputs"]["phase0"]["source_format"],
                         "A840FW01")

    def test_reject_bad_schema_version(self):
        self.rejects(lambda obj: obj.update(schema_version=True), "schema_version")

    def test_reject_wrong_refresh_or_budget(self):
        self.rejects(lambda obj: obj.update(target_refresh_hz=60), "target_refresh_hz")
        self.rejects(lambda obj: obj.update(target_budget_us=8300), "target_budget_us")

    def test_reject_failed_or_compare_report(self):
        self.rejects(lambda obj: obj.update(mode="compare"), "mode")
        self.rejects(lambda obj: obj.update(status="FAIL"), "status")

    def test_reject_summary_inconsistent_with_frames(self):
        self.rejects(lambda obj: obj["scenes"][APP]["physical_intervals"].update(p95_us=999), "p95_us")

    def test_reject_stall_bucket_inconsistent_with_frames(self):
        self.rejects(lambda obj: obj["scenes"][APP]["physical_intervals"]["stall_buckets"].update(ge_100_ms=1),
                     "ge_100_ms")

    def test_reject_missing_scene_summary_for_captured_frames(self):
        self.rejects(lambda obj: obj["scenes"].pop(GLASS), "scene summaries missing")

    def test_reject_backend_claim_inconsistent_with_stages(self):
        self.rejects(lambda obj: obj["frames"][0].update(backend_total_us=10), "backend_total_us")

    def test_reject_blank_frames_with_fake_counts(self):
        self.rejects(lambda obj: obj.update(frames=[]), "contain frames")

    def test_reject_bad_frame_algebra_or_order(self):
        self.rejects(lambda obj: obj["frames"][0].update(scanout_margin_us=100), "scanout_margin_us")
        self.rejects(lambda obj: obj["frames"][1].update(frame_seq=1), "out of order")

    def test_reject_invalid_instruction_coverage(self):
        self.rejects(lambda obj: obj["scenes"][APP].update(instruction_count_frames=2), "instruction_count_frames")


class CachedCase(unittest.TestCase):
    def setUp(self):
        root = CACHE_PARENT / "w10-l4" / "tests"
        root.mkdir(parents=True, exist_ok=True)
        storage = tempfile.TemporaryDirectory(prefix="phase5-", dir=root)
        self.addCleanup(storage.cleanup)
        self.cache = Path(storage.name)

    def cli(self, script, *args, timeout=40):
        return subprocess.run([sys.executable, "-B", str(REPO / "tools/verify" / script), *map(str, args)],
                              cwd=REPO, env=cache_environment(self.cache), timeout=timeout,
                              check=False, capture_output=True, text=True)


class CompareCliTests(CachedCase):
    def setUp(self):
        super().setUp()
        self.inputs = {phase: self.cache / f"{phase}.json" for phase in perf.PHASES}
        for path in self.inputs.values():
            path.write_text(json.dumps(four()), encoding="utf-8")
        self.json_path = self.cache / "comparison.json"
        self.md_path = self.cache / "comparison.md"

    def comparison(self, *args):
        inputs = [part for phase, path in self.inputs.items() for part in (f"--{phase}", path)]
        return self.cli("phase5_perf_compare.py", *inputs, *args)

    def test_cli_outputs_json_and_markdown_deterministically(self):
        args = ("--out-json", self.json_path, "--out-md", self.md_path)
        result = self.comparison(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        first = self.json_path.read_bytes(), self.md_path.read_bytes()
        self.assertEqual(json.loads(result.stdout), json.loads(first[0]))
        self.assertEqual(self.comparison(*args).returncode, 0)
        self.assertEqual((self.json_path.read_bytes(), self.md_path.read_bytes()), first)
        md = first[1].decode("utf-8")
        for label in (*fw.CORE_SCENES, "F07", "NEUTRAL", "UNMEASURED", "ge_100_ms", "device_verified=false"):
            self.assertIn(label, md)

    def test_cli_allows_partial_only_with_switch(self):
        self.inputs["phase5"].write_text(json.dumps(four(scenes=(APP,))), encoding="utf-8")
        self.assertEqual(self.comparison().returncode, 1)
        result = self.comparison("--allow-partial-scenes")
        self.assertEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout)["coverage_complete"])

    def test_cli_regression_returns_one_with_reports(self):
        self.inputs["phase5"].write_text(json.dumps(four(gpu_exec_ns=6_000_000)), encoding="utf-8")
        result = self.comparison("--out-json", self.json_path, "--out-md", self.md_path)
        self.assertEqual(result.returncode, 1)
        report = json.loads(self.json_path.read_text())
        self.assertIs(report["regression_detected"], True)
        self.assertEqual(json.loads(result.stdout), report)
        self.assertIn("REGRESSED", self.md_path.read_text(encoding="utf-8"))

    def test_cli_rejects_duplicate_json_keys_before_emitting_files(self):
        self.inputs["phase5"].write_text('{"device_verified":true,"device_verified":false}')
        result = self.comparison("--out-json", self.json_path)
        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate JSON key", json.loads(result.stdout)["error"])
        self.assertFalse(self.json_path.exists())

    def test_cli_rejects_nonfinite_json(self):
        self.inputs["phase5"].write_text('{"frame_count": Infinity}')
        result = self.comparison()
        self.assertEqual(result.returncode, 1)
        self.assertIn("non-finite", json.loads(result.stdout)["error"])

    def test_cli_does_not_overwrite_input(self):
        baseline = self.inputs["phase0"].read_bytes()
        result = self.comparison("--out-json", self.inputs["phase0"])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.inputs["phase0"].read_bytes(), baseline)

    def test_cli_loads_supplemental_metrics_file(self):
        extra = self.cache / "extra.json"
        extra.write_text(json.dumps({"phase5": {APP: {"avg_gpu_mhz": 589.25}}}))
        result = self.comparison("--extra-metrics", extra)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(scene_report(json.loads(result.stdout))["efficiency"]["avg_gpu_mhz"], 589.25)

    def test_cli_bad_usage_returns_two(self):
        self.assertEqual(self.cli("phase5_perf_compare.py", "--phase0", self.inputs["phase0"]).returncode, 2)



class GateTests(CachedCase):
    def setUp(self):
        super().setUp()
        self.repo = self.cache / "repo"
        self.repo.mkdir()

    def siblings(self, *groups):
        for paths in groups:
            for name in paths:
                path = self.repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

    def stub_stages(self, **changes):
        names = ("contracts", "tool", "native", "ir3", "streams", "frame_weights",
                 "perf_fixture", "no_fallback", "phase4", "wave")
        return mock.patch.multiple(gate.HostRun, **(dict.fromkeys(names, mock.DEFAULT) | changes))

    def run_gate(self, mode="quick", **args):
        with self.stub_stages():
            return gate.run_gate(self.repo, self.cache, self.cache / "gate", mode, **args)

    def gate_row(self, report, name):
        return next(row for row in report["results"] if row["name"] == name)

    def test_quick_enumerates_fourteen_stages_and_reports_absent_siblings(self):
        report = self.run_gate()
        self.assertEqual(tuple(row["name"] for row in report["results"]), ALL_STAGES)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["deferred_siblings"], ["W10-L1", "W10-L2", "W10-L3"])
        self.assertFalse(report["phase5_host_ready"])

    def test_quick_marks_only_full_mode_stages_skipped(self):
        report = self.run_gate()
        self.assertEqual([row["name"] for row in report["results"] if row["status"] == gate.SKIPPED],
                         ["phase4_non_regression", "verify_wave"])

    def test_full_with_absent_siblings_is_not_ready(self):
        report = self.run_gate("full")
        self.assertFalse(report["phase5_host_ready"])
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["deferred_siblings"]), 3)
        self.assertTrue(all(row["status"] != gate.SKIPPED for row in report["results"]))

    def test_quick_with_present_siblings_is_not_ready(self):
        self.siblings(gate.GMU_FILES, gate.HOTSPOT_FILES, gate.CHUNK_FILES)
        report = self.run_gate()
        self.assertFalse(report["phase5_host_ready"])
        self.assertEqual(report["deferred_siblings"], [])

    def test_full_with_present_siblings_and_all_pass_is_ready(self):
        self.siblings(gate.GMU_FILES, gate.HOTSPOT_FILES, gate.CHUNK_FILES)
        report = self.run_gate("full", with_clang=True)
        self.assertEqual(report["deferred_siblings"], [])
        self.assertTrue(report["phase5_host_ready"])
        self.assertEqual([row["status"] for row in report["results"]], ["PASS"] * 14)
        self.assertTrue(report["with_clang"])

    def test_partial_sibling_fails_and_is_not_deferred(self):
        self.siblings((gate.GMU_FILES[1],))
        report = self.run_gate()
        self.assertEqual(report["status"], "FAIL")
        self.assertNotIn("W10-L1", report["deferred_siblings"])
        self.assertIn("MISSING_SOURCE", self.gate_row(report, "kmd_phase5_gmu_mdss")["error"])

    def test_broken_sibling_symlink_fails(self):
        test = self.repo / gate.CHUNK_TEST
        test.parent.mkdir(parents=True)
        test.symlink_to(self.cache / "missing.cpp")
        report = self.run_gate()
        self.assertNotIn("W10-L3", report["deferred_siblings"])
        self.assertIn("SYMLINK_SOURCE", self.gate_row(report, "runtime_cb_chunking_promise")["error"])

    def test_present_siblings_invoke_their_source_lists_and_tsan(self):
        self.siblings(gate.GMU_FILES, gate.HOTSPOT_FILES, gate.CHUNK_FILES)
        with self.stub_stages() as stubs:
            gate.run_gate(self.repo, self.cache, self.cache / "gate", "full")
        gmu = next(entry for entry in stubs["native"].call_args_list if entry.args[1] == gate.GMU_TEST)
        self.assertEqual(set(gmu.args[2]), {name for name in gate.GMU_FILES if name.endswith(".c")})
        self.assertTrue(gmu.kwargs["tsan"])
        stubs["ir3"].assert_any_call(mock.ANY, "test_metal_hotspot_opt", "hotspot_opt_quantify")
        chunk = next(entry for entry in stubs["streams"].call_args_list if entry.args[1] == gate.CHUNK_TEST)
        self.assertTrue(chunk.kwargs["tsan"])
        self.assertTrue(set(gate.ARENA) <= set(chunk.args[2]))

    def test_failing_subgate_refuses_readiness_and_other_gates_continue(self):
        self.siblings(gate.GMU_FILES, gate.HOTSPOT_FILES, gate.CHUNK_FILES)
        with self.stub_stages(contracts=mock.Mock(side_effect=GateError("CONTRACT_DRIFT"))) as stubs:
            report = gate.run_gate(self.repo, self.cache, self.cache / "gate", "full")
            stubs["no_fallback"].assert_called_once()
            stubs["wave"].assert_called_once()
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(self.gate_row(report, "contracts")["error"], "CONTRACT_DRIFT")
        self.assertFalse(report["phase5_host_ready"])

    def test_failed_and_passed_stages_have_fixed_flags(self):
        with self.stub_stages(wave=mock.Mock(side_effect=GateError("WAVE_FAILED"))):
            report = gate.run_gate(self.repo, self.cache, self.cache / "gate", "full")
        for row in (report, *report["results"]):
            self.assertIs(row["device_verified"], False)
            self.assertIs(row["hardware_acceptance"], False)
        self.assertFalse(report["phase5_host_ready"])

    def test_custom_out_json_is_identical_to_default_summary(self):
        path = self.cache / "custom.json"
        report = self.run_gate(out_json=path)
        self.assertEqual(json.loads(path.read_text()), report)
        self.assertEqual(path.read_bytes(), Path(report["summary"]).read_bytes())

    def test_repeated_gate_uses_fresh_run_directories(self):
        self.assertNotEqual(self.run_gate()["run_dir"], self.run_gate()["run_dir"])

    def test_windows_c_is_rejected(self):
        for path in ("C:/unused/report.json", r"c:\Temp\unused", "/mnt/c/unused", "/MNT/C/unused"):
            with self.subTest(path=path), self.assertRaises(GateError):
                perf.checked_output_path(path, directory=True)

    def test_tmp_is_rejected(self):
        with self.assertRaises(GateError):
            perf.checked_output_path("/tmp/w10-rejected", directory=True)

    def test_arbitrary_nested_project_cache_is_allowed(self):
        path = CACHE_PARENT / "w10-coord-verify" / "a" / "b"
        self.assertEqual(perf.checked_output_path(path, directory=True), path)

    def test_shared_cache_root_is_refused(self):
        with self.assertRaises(GateError):
            perf.checked_output_path(CACHE_PARENT, directory=True)

    def test_symlink_escape_to_tmp_is_rejected(self):
        alias = self.cache / "escape"
        alias.symlink_to("/tmp", target_is_directory=True)
        with self.assertRaises(GateError):
            self.run_gate(out_json=alias / "no-report.json")

    def test_cache_dir_symlink_escape_is_rejected(self):
        alias = self.cache / "tools"
        alias.symlink_to(REPO / "tools", target_is_directory=True)
        with self.assertRaises(GateError):
            gate.run_gate(self.repo, self.cache, alias / "gate")

    def test_in_cache_directory_symlink_is_safe(self):
        target = self.cache / "destination"
        target.mkdir()
        alias = self.cache / "alias"
        alias.symlink_to(target, target_is_directory=True)
        self.assertEqual(perf.checked_output_path(alias / "report.json"), target / "report.json")

    def test_cli_refuses_invalid_out_path_before_compiling(self):
        path = self.cache / "uncreated-gate-dir"
        result = self.cli("phase5_host_gate.py", "--repo", REPO, "--mesa", MESA, "--cache-dir", path,
                          "--out-json", "/tmp/w10-rejected.json")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(path.exists())
        self.assertIs(json.loads(result.stdout)["phase5_host_ready"], False)

    def test_invalid_mode_and_nonbool_clang_are_refused(self):
        for opts in ({"mode": "unknown"}, {"with_clang": 1}):
            with self.subTest(opts=opts), self.assertRaises(GateError):
                gate.run_gate(self.repo, self.cache, self.cache / "gate", **opts)

    def test_nonzero_child_command_fails(self):
        runner = gate.HostRun(self.repo, self.cache, self.cache / "native", "quick", False)
        row = {"commands": []}
        with self.assertRaises(GateError):
            runner.command(row, [sys.executable, "-B", "-c", "raise SystemExit(7)"])
        self.assertEqual(row["commands"][0]["returncode"], 7)

    def test_tsan_invokes_twenty_rounds_when_full_is_requested(self):
        runner = gate.HostRun(self.repo, self.cache, self.cache / "native", "full", False)
        row = {"commands": []}
        binary = self.cache / "binary"
        with mock.patch.object(runner, "build", return_value=binary) as build, \
                mock.patch.object(runner, "command") as command:
            runner.native(row, "test.cpp", ["source.c"], tsan=True)
        self.assertEqual(build.call_args.args[-1], "thread")
        command.assert_called_with(row, ["setarch", "x86_64", "-R", binary, "--tsan-stress", "20"],
                                   cwd=binary.parent, timeout=600)
        self.assertEqual(row["tsan_rounds"], 20)

    def test_ir3_bootstrap_passes_pinned_environment_and_link_inputs(self):
        builder = "adreno_compile/tools/build_adreno_compile_ir3.py"
        manifest = "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
        self.siblings((builder, manifest))
        (self.repo / builder).write_text(
            """from pathlib import Path
import os
MESA_COMMIT = "fixture-pin"
MESON = Path(__file__).with_name("meson")
DEFAULT_MANIFEST = Path(__file__).with_name("manifest.json")

def build_mesa_archives(mesa, manifest, cache, jobs, env):
    assert manifest == DEFAULT_MANIFEST and jobs == 32
    assert env["MESA_GIT_SHA1_OVERRIDE"] == MESA_COMMIT
    assert env["PATH"].split(os.pathsep)[0] == str(MESON.parent)
    return mesa, cache, {}, [cache / "libir3.a"]

def ensure_translate_archive(cache, jobs, env):
    assert jobs == 32 and env["MESA_GIT_SHA1_OVERRIDE"] == MESA_COMMIT
    return cache / "libtranslate.a"

def mesa_compile_defines(cache):
    return ["-DTEST_IR3=1"]

def mesa_include_flags(source, build):
    return ["-isystem", str(source)]
""", encoding="utf-8")
        output = self.cache / "native"
        runner = gate.HostRun(self.repo, self.cache, output, "full", False)
        flags, link = runner.prepare_ir3({"commands": []})
        self.assertEqual(flags, ["-DTEST_IR3=1", "-isystem", str(self.cache),
                                 "-I", str(self.repo / "translate/include")])
        self.assertEqual(link, ["-Wl,--start-group", str(output / "ir3/libir3.a"),
                                str(output / "ir3/libtranslate.a"), "-Wl,--end-group", "-lz"])

    def test_zero_unittest_discovery_fails(self):
        self.siblings((gate.HOTSPOT_FILES[-2],))
        runner = gate.HostRun(self.repo, self.cache, self.cache / "native", "quick", False)
        with mock.patch.object(runner, "command", return_value={"stderr": "Ran 0 tests in 0.01s"}):
            with self.assertRaisesRegex(GateError, "empty unittest"):
                runner.unittest({"commands": []}, gate.HOTSPOT_FILES[-2])


class QuickIntegrationTests(CachedCase):
    def test_real_quick_gate_exercises_native_pm4_profiler_and_comparison(self):
        out_json = self.cache / "quick.json"
        result = self.cli("phase5_host_gate.py", "--repo", REPO, "--mesa", MESA,
                          "--mode", "quick", "--cache-dir", self.cache / "real-quick",
                          "--out-json", out_json, timeout=420)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(out_json.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "PASS")
        self.assertFalse(report["phase5_host_ready"])
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        rows = {row["name"]: row for row in report["results"]}
        self.assertEqual(tuple(rows), ALL_STAGES)
        for name, files in (("W10-L1", gate.GMU_FILES), ("W10-L2", gate.HOTSPOT_FILES),
                            ("W10-L3", gate.CHUNK_FILES)):
            self.assertEqual(name in report["deferred_siblings"], not any((REPO / path).exists() for path in files))
        pm4 = rows["emit_sync_tracker_pm4"]
        self.assertEqual(pm4["comparison_scope"], "gcc_repeat")
        self.assertEqual(len(pm4["streams"]), 8)
        counts = rows["frame_weights_profiler"]["captures"]["gcc"]["frames_by_scene"]
        self.assertEqual(counts, dict.fromkeys(fw.CORE_SCENES, 3))
        self.assertGreater(rows["no_fallback_final"]["objects_scanned"], 0)
        self.assertEqual(rows["no_fallback_final"]["violations"], 0)
        good = json.loads(Path(rows["phase5_perf_compare"]["phase5"]["json"]).read_text())
        bad = json.loads(Path(rows["phase5_perf_compare"]["regression"]["json"]).read_text())
        self.assertFalse(good["regression_detected"])
        self.assertTrue(good["backend_within_120hz_budget"])
        self.assertTrue(bad["regression_detected"])
        self.assertEqual(Path(rows["phase5_perf_compare"]["phase5"]["markdown"]).read_text(), perf.markdown(good))
        commands = [entry for row in report["results"] for entry in row["commands"]]
        self.assertTrue(any(entry["command"][0] == "gcc-13" and "-Werror" in entry["command"] and
                            "-fsanitize=address,undefined" in entry["command"] for entry in commands))


if __name__ == "__main__":
    unittest.main()
