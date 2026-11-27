# SPDX-License-Identifier: MIT
"""Wire, algebra, statistics and CLI tests for the frame recorder/profiler."""

import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

import frame_weights_profiler as fw

SCRIPT = Path(fw.__file__).resolve()
REPO = SCRIPT.parents[2]
APP, SETTINGS, LIBRARY, GLASS = fw.CORE_SCENES
BUDGET = 8_333_333


def frame(seq=1, scene=APP, **changes):
    row = {
        "type": "frame", "frame_seq": seq, "scene_id": scene, "ring_id": 3,
        "preempted_count": 1, "wfi_count": 2, "cache_flush_count": 3, "heap_allocs": 4,
        "t_vsync_prev_ns": (1 << 55) + seq * BUDGET,
        "pre_create_ns": 2_000_000, "encode_ns": 1_000_000, "submit_ns": 500_000,
        "gpu_exec_ns": 2_000_000, "wait_sync_ns": 500_000, "physical_frame_interval_ns": 0,
    }
    row.update(changes)
    stages = [row[key] for key in ("pre_create_ns", "encode_ns", "submit_ns", "gpu_exec_ns", "wait_sync_ns")]
    row.setdefault("scanout_margin_ns", BUDGET - sum(stages) if all(type(x) is int for x in stages) else 0)
    return row


def draw(seq=1, ticks=192, pipeline_hash=4, pass_id=1, mode="GMEM", **changes):
    start = (1 << 63) + 1234
    row = {
        "type": "draw", "frame_seq": seq, "pass_id": pass_id, "draw_id": 2,
        "pipeline_hash": pipeline_hash, "gmem_mode": mode, "tile_count": 19,
        "draw_verts_or_threads": 123, "start_ticks_19m2": start, "end_ticks_19m2": start + ticks,
        "duration_ns": ticks * 625 // 12, "sync_flags": 0x80000001,
    }
    row.update(changes)
    return row


def jsonl(*rows):
    return "\n".join(map(json.dumps, rows))


def profile(*rows, top_n=10):
    return fw.analyze(fw.parse_jsonl(jsonl(*rows).splitlines()), top_n=top_n)


def binary():
    # Independent fixed-width wire fixture, also checked against the native export below.
    frames = struct.pack("<Q6I6QqQ", 7, 1, 3, 4, 5, 6, 9, (1 << 54) + 1,
                         9_000_000, 100_000, 50_000, 20_000, 30_000, -866667, 20_000_000)
    draws = struct.pack("<QIIQIIIQQQI", 7, 29, 13, (1 << 64) - 1, 0, 17, 199,
                        (1 << 64) - 193, (1 << 64) - 1, 10000, 0xc0)
    payload = frames + draws
    return struct.pack("<8sIIIQIIII", b"A840FW01", 1, 19200000, 120, BUDGET, 0, 1, 1,
                       zlib.crc32(payload)) + payload


def alter(wire, offset, fmt, value, *, fix_crc=False):
    data = bytearray(wire)
    struct.pack_into("<" + fmt, data, offset, value)
    if fix_crc:
        struct.pack_into("<I", data, 40, zlib.crc32(data[44:]))
    return bytes(data)


class BinaryTests(unittest.TestCase):
    def test_wire_sizes_and_fields(self):
        trace = fw.decode_binary(binary())
        self.assertEqual((fw.HEADER.size, fw.FRAME.size, fw.DRAW.size), (44, 96, 64))
        self.assertEqual(trace.source_format, "A840FW01")
        row = trace.frames[0]
        self.assertEqual((row.frame_seq, row.scene_id, row.ring_id), (7, 1, 3))
        self.assertEqual((row.preempted_count, row.wfi_count, row.cache_flush_count, row.heap_allocs), (4, 5, 6, 9))
        self.assertEqual(row.t_vsync_prev_ns, (1 << 54) + 1)
        self.assertEqual(row.scanout_margin_ns, -866667)
        command = trace.draws[0]
        self.assertEqual((command.frame_seq, command.pass_id, command.draw_id), (7, 29, 13))
        self.assertEqual((command.pipeline_hash, command.duration_ns, command.sync_flags),
                         ((1 << 64) - 1, 10000, 0xc0))

    def test_wrong_magic(self):
        with self.assertRaisesRegex(fw.ProfileError, "magic"):
            fw.decode_binary(b"B" + binary()[1:])

    def test_wrong_version(self):
        with self.assertRaisesRegex(fw.ProfileError, "version"):
            fw.decode_binary(alter(binary(), 8, "I", 2))

    def test_wrong_clock(self):
        with self.assertRaisesRegex(fw.ProfileError, "clock"):
            fw.decode_binary(alter(binary(), 12, "I", 19_200_001))

    def test_wrong_refresh(self):
        with self.assertRaisesRegex(fw.ProfileError, "refresh"):
            fw.decode_binary(alter(binary(), 16, "I", 60))

    def test_wrong_budget(self):
        with self.assertRaisesRegex(fw.ProfileError, "budget"):
            fw.decode_binary(alter(binary(), 20, "Q", 8333000))

    def test_header_scene_outside_enum(self):
        with self.assertRaisesRegex(fw.ProfileError, "scene_id"):
            fw.decode_binary(alter(binary(), 28, "I", 5))

    def test_frame_scene_disagrees_with_header(self):
        with self.assertRaisesRegex(fw.ProfileError, "disagrees with header"):
            fw.decode_binary(alter(binary(), 28, "I", 2))

    def test_bad_frame_scene_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "frame 1.*scene_id"):
            fw.decode_binary(alter(binary(), 52, "I", 5, fix_crc=True))

    def test_bad_ring_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "frame 1.*ring_id"):
            fw.decode_binary(alter(binary(), 56, "I", 4, fix_crc=True))

    def test_short_header(self):
        with self.assertRaisesRegex(fw.ProfileError, "truncated"):
            fw.decode_binary(binary()[:43])

    def test_short_payload(self):
        with self.assertRaisesRegex(fw.ProfileError, "length"):
            fw.decode_binary(binary()[:-1])

    def test_extra_payload(self):
        with self.assertRaisesRegex(fw.ProfileError, "length"):
            fw.decode_binary(binary() + b"\0")

    def test_large_header_counts(self):
        with self.assertRaisesRegex(fw.ProfileError, "length"):
            fw.decode_binary(alter(binary(), 32, "I", (1 << 32) - 1))

    def test_crc_detects_altered_payload(self):
        with self.assertRaisesRegex(fw.ProfileError, "CRC32"):
            fw.decode_binary(alter(binary(), 140 + 8, "I", 123))

    def test_bad_frame_algebra_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "scanout_margin_ns"):
            fw.decode_binary(alter(binary(), 44 + 80, "q", -866666, fix_crc=True))

    def test_bad_draw_ticks_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "draw 1.*reversed"):
            fw.decode_binary(alter(binary(), 140 + 44, "Q", 7, fix_crc=True))

    def test_bad_draw_duration_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "duration_ns"):
            fw.decode_binary(alter(binary(), 140 + 52, "Q", 9999, fix_crc=True))

    def test_bad_draw_mode_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "gmem_mode"):
            fw.decode_binary(alter(binary(), 140 + 24, "I", 4, fix_crc=True))

    def test_orphan_draw_with_corrected_crc(self):
        with self.assertRaisesRegex(fw.ProfileError, "missing frame_seq"):
            fw.decode_binary(alter(binary(), 140, "Q", 8, fix_crc=True))


class JsonlTests(unittest.TestCase):
    def test_numeric_and_named_enums(self):
        trace = fw.parse_jsonl(jsonl(frame(scene=2), frame(2, GLASS),
                                    draw(mode=1), draw(2, mode="BLIT")).splitlines())
        self.assertEqual([row.scene_id for row in trace.frames], [2, 4])
        self.assertEqual([row.gmem_mode for row in trace.draws], [1, 3])

    def test_hex_hash_and_large_ticks(self):
        report = profile(frame(), draw(pipeline_hash="0xFFFFFFFFFFFFFFFf", ticks=1))
        self.assertEqual(report["draw_weights"]["by_pipeline_hash"][0]["pipeline_hash"],
                         "0xffffffffffffffff")
        self.assertEqual(report["draw_weights"]["total_gpu_us"], 0.052)

    def test_draw_can_arrive_before_frame(self):
        self.assertEqual(profile(draw(), frame())["draw_weights"]["draw_count"], 1)

    def test_no_physical_field(self):
        row = frame()
        del row["physical_frame_interval_ns"]
        self.assertIsNone(profile(row)["frames"][0]["physical_frame_interval_us"])

    def test_empty_capture_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "no frames"):
            fw.analyze(fw.parse_jsonl([" ", "\n"]))

    def test_bad_json(self):
        with self.assertRaisesRegex(fw.ProfileError, "line 2.*invalid JSON"):
            fw.parse_jsonl(["", "{"])

    def test_json_array_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "expected a JSON object"):
            fw.parse_jsonl(["[]"])

    def test_duplicate_json_key(self):
        with self.assertRaisesRegex(fw.ProfileError, "duplicate JSON key"):
            fw.parse_jsonl(['{"type":"draw","type":"frame"}'])

    def test_unknown_type_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "type=frame"):
            fw.parse_jsonl(['{"type":"scene"}'])

    def test_unknown_field_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "unknown frame field"):
            profile(frame(gpu_exex_ns=1))

    def test_bad_scene_name_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "scene_id"):
            profile(frame(scene="APP_LAUNCH"))

    def test_duplicate_frame_seq_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "frame_seq must increase"):
            profile(frame(), frame())

    def test_decreasing_vsync_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "t_vsync_prev_ns"):
            profile(frame(), frame(2, t_vsync_prev_ns=0))

    def test_boolean_field_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "ring_id"):
            profile(frame(ring_id=True))

    def test_float_ns_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "encode_ns"):
            profile(frame(encode_ns=1.0))

    def test_non_finite_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "non-finite"):
            profile(frame(total_pipeline_us=float("nan")))

    def test_negative_stage_rejected(self):
        for key in ("pre_create_ns", "encode_ns", "submit_ns", "gpu_exec_ns", "wait_sync_ns"):
            with self.subTest(stage=key), self.assertRaisesRegex(fw.ProfileError, key):
                profile(frame(**{key: -1}))

    def test_negative_physical_interval_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "physical_frame_interval_ns"):
            profile(frame(physical_frame_interval_ns=-1))

    def test_stage_sum_unrepresentable(self):
        with self.assertRaisesRegex(fw.ProfileError, "signed scanout_margin_ns range"):
            profile(frame(pre_create_ns=(1 << 63) + BUDGET, scanout_margin_ns=-(1 << 63)))

    def test_large_negative_margin_preserved(self):
        row = frame(pre_create_ns=BUDGET + (1 << 63), encode_ns=0, submit_ns=0, gpu_exec_ns=0,
                    wait_sync_ns=0)
        self.assertEqual(profile(row)["frames"][0]["scanout_margin_us"], -(1 << 63) / 1000)

    def test_draw_counter_greater_than_uint32(self):
        with self.assertRaisesRegex(fw.ProfileError, "tile_count"):
            profile(frame(), draw(tile_count=1 << 32))

    def test_draw_delta_greater_than_uint64_nanoseconds(self):
        with self.assertRaisesRegex(fw.ProfileError, "duration exceeds uint64"):
            profile(frame(), draw(start_ticks_19m2=0, end_ticks_19m2=(1 << 64) - 1))

    def test_forged_hardware_flags_rejected(self):
        for flag in ("device_verified", "hardware_acceptance"):
            with self.subTest(flag=flag), self.assertRaisesRegex(fw.ProfileError, flag):
                profile(frame(**{flag: True}))


class AlgebraTests(unittest.TestCase):
    def test_both_identities_and_six_stages(self):
        report = profile(frame(backend_total_ns=4_000_000, total_pipeline_us=6000))
        row = report["frames"][0]
        self.assertEqual(row["backend_total_us"], 4000)
        self.assertEqual(row["total_pipeline_us"], 6000)
        self.assertEqual(row["scanout_margin_us"], 2333.333)
        self.assertEqual((row["pre_create_us"], row["encode_us"], row["submit_us"],
                          row["gpu_exec_us"], row["wait_sync_us"]), (2000, 1000, 500, 2000, 500))

    def test_identity_tolerance_inclusive(self):
        report = profile(frame(backend_total_ns=4_001_000, total_pipeline_us=6001,
                               scanout_margin_ns=2334333))
        self.assertEqual(report["frames"][0]["total_pipeline_us"], 6000)

    def test_bad_backend_identity(self):
        with self.assertRaisesRegex(fw.ProfileError, "backend_total_ns identity"):
            profile(frame(backend_total_ns=4_001_001))

    def test_bad_total_identity_decimal(self):
        with self.assertRaisesRegex(fw.ProfileError, "total_pipeline_us identity"):
            profile(frame(total_pipeline_us=6001.00001))

    def test_redundant_totals_must_also_agree_with_each_other(self):
        with self.assertRaisesRegex(fw.ProfileError, "reported total_pipeline and backend_total"):
            profile(frame(backend_total_ns=4_001_000, total_pipeline_us=5999))

    def test_negative_claim_rejected(self):
        with self.assertRaisesRegex(fw.ProfileError, "nonnegative"):
            profile(frame(backend_total_us=-1))

    def test_scanout_margin_independently_checks_the_stage_sum(self):
        with self.assertRaisesRegex(fw.ProfileError, "scanout_margin_ns disagrees"):
            profile(frame(scanout_margin_ns=2334334))


class StatisticsTests(unittest.TestCase):
    def test_pipeline_ranking_sums_and_breaks_ties(self):
        weights = profile(frame(), draw(ticks=192, pipeline_hash=10), draw(ticks=384, pipeline_hash=2),
                          draw(ticks=192, pipeline_hash=10), draw(ticks=192, pipeline_hash=20))["draw_weights"]
        rows = weights["by_pipeline_hash"]
        self.assertEqual([row["pipeline_hash"] for row in rows], ["0x%016x" % x for x in (2, 10, 20)])
        self.assertEqual([row["total_gpu_us"] for row in rows], [20, 20, 10])
        self.assertEqual([row["gpu_share_pct"] for row in rows], [40, 40, 20])
        self.assertEqual((rows[1]["draw_count"], rows[1]["avg_draw_us"]), (2, 10))

    def test_pass_mode_grouping_uses_both_keys(self):
        draws = [draw(ticks=192, mode="GMEM", pass_id=1), draw(ticks=384, mode="SYSMEM", pass_id=1),
                 draw(ticks=576, mode="GMEM", pass_id=2), draw(ticks=192, mode="SYSMEM", pass_id=1)]
        rows = profile(frame(), *draws)["draw_weights"]["by_pass_gmem_mode"]
        self.assertEqual([(row["pass_id"], row["gmem_mode"], row["total_gpu_us"]) for row in rows],
                         [(1, "SYSMEM", 30), (2, "GMEM", 30), (1, "GMEM", 10)])

    def test_top_n_retains_full_denominator(self):
        weights = profile(frame(), draw(ticks=384), draw(pipeline_hash=0, mode="SYSMEM"),
                          top_n=1)["draw_weights"]
        self.assertEqual(len(weights["by_pass_gmem_mode"]), 1)
        self.assertEqual(len(weights["by_pipeline_hash"]), 1)
        self.assertEqual(weights["total_gpu_us"], 30)
        self.assertAlmostEqual(weights["by_pipeline_hash"][0]["gpu_share_pct"], 200 / 3)

    def test_invalid_top_n(self):
        for n in (0, -1, True):
            with self.subTest(n=n), self.assertRaisesRegex(fw.ProfileError, "top_n"):
                profile(frame(), top_n=n)

    def test_mode_shares_for_all_four_modes(self):
        base = [draw(mode=mode, ticks=(i + 1) * 192)
                for i, mode in enumerate(("GMEM", "SYSMEM", "COMPUTE", "BLIT"))]
        weights = profile(frame(), *base)["draw_weights"]
        self.assertEqual(weights["mode_share_pct"], {"GMEM": 10, "SYSMEM": 20, "COMPUTE": 30, "BLIT": 40})
        self.assertEqual(weights["by_pipeline_hash"][0]["mode_share_pct"], weights["mode_share_pct"])

    def test_nearest_rank_draw_p95(self):
        weights = profile(frame(), *(draw(ticks=i * 192) for i in range(1, 21)))["draw_weights"]
        self.assertEqual((weights["avg_draw_us"], weights["p95_draw_us"]), (105, 190))

    def test_zero_duration_draws_have_no_gpu_weight_denominator(self):
        weights = profile(frame(), draw(ticks=0))["draw_weights"]
        self.assertEqual(weights["total_gpu_us"], 0)
        self.assertIsNone(weights["by_pipeline_hash"][0]["gpu_share_pct"])

    def test_frame_only_has_empty_draw_rankings(self):
        weights = profile(frame())["draw_weights"]
        self.assertEqual(weights["by_pipeline_hash"], [])
        self.assertEqual(weights["by_pass_gmem_mode"], [])
        self.assertIsNone(weights["p95_draw_us"])

    def test_independent_scene_stats(self):
        report = profile(*(frame(i, scene, physical_frame_interval_ns=i * 10_000_000)
                           for i, scene in enumerate(fw.CORE_SCENES, 1)))
        self.assertEqual(set(report["scenes"]), set(fw.CORE_SCENES))
        self.assertEqual([report["scenes"][s]["physical_intervals"]["p95_us"] for s in fw.CORE_SCENES],
                         [10_000, 20_000, 30_000, 40_000])

    def test_custom_and_missing_scenes(self):
        scenes = profile(frame(scene="CUSTOM"))["scenes"]
        self.assertEqual(scenes["CUSTOM"]["frame_count"], 1)
        for name in fw.CORE_SCENES:
            self.assertEqual(scenes[name]["frame_count"], 0)
            self.assertIsNone(scenes[name]["software_pipeline"]["within_budget_pct"])

    def test_missing_physical_never_substitutes_pipeline(self):
        scene = profile(frame(physical_frame_interval_ns=20_000_000), frame(2))["scenes"][APP]
        self.assertEqual(scene["physical_intervals"]["sample_count"], 1)
        self.assertEqual(scene["physical_intervals"]["p50_us"], 20_000)
        self.assertEqual(scene["software_pipeline"]["sample_count"], 2)
        self.assertEqual(scene["software_pipeline"]["p50_us"], 6000)

    def test_physical_nearest_rank_percentiles(self):
        trace = [frame(i, physical_frame_interval_ns=i * 1000) for i in range(1, 101)]
        stats = profile(*trace)["scenes"][APP]["physical_intervals"]
        self.assertEqual([stats[key] for key in ("p50_us", "p95_us", "p99_us", "max_us")], [50, 95, 99, 100])

    def test_cumulative_buckets_have_inclusive_edges(self):
        intervals = [8_333_332, 8_333_333, 16_666_666, 16_666_667,
                     19_999_999, 20_000_000, 99_999_999, 100_000_000]
        trace = [frame(i, physical_frame_interval_ns=ns) for i, ns in enumerate(intervals, 1)]
        stats = profile(*trace)["scenes"][APP]["physical_intervals"]
        self.assertEqual(list(stats["stall_buckets"].values()), [7, 5, 3, 1])
        self.assertEqual(stats["within_budget_pct"], 25)
        self.assertEqual(stats["max_stall"]["frame_seq"], 8)

    def test_f07_takes_largest_upstream_share(self):
        sample = frame(pre_create_ns=16_000_000, physical_frame_interval_ns=20_000_000)
        report = profile(sample)
        row = report["frames"][0]
        self.assertEqual(row["physical_attribution"], "F07_PRE_CREATE_DOMINANT")
        self.assertEqual((row["pre_create_share_pct"], row["backend_share_pct"]), (80, 20))
        self.assertEqual(report["scenes"][APP]["physical_intervals"]["max_stall"]["attribution"],
                         "F07_PRE_CREATE_DOMINANT")

    def test_backend_includes_wait_sync_and_overruns(self):
        row = profile(frame(wait_sync_ns=7_000_000, physical_frame_interval_ns=16_666_667))["frames"][0]
        self.assertEqual(row["physical_attribution"], "BACKEND_OVER_BUDGET")
        self.assertEqual(row["backend_total_us"], 10_500)
        self.assertAlmostEqual(row["pre_create_share_pct"] + row["backend_share_pct"], 100)

    def test_observed_gap_within_budget_wins_physical_label(self):
        row = profile(frame(pre_create_ns=20_000_000, physical_frame_interval_ns=BUDGET))["frames"][0]
        self.assertEqual(row["physical_attribution"], "WITHIN_120HZ_BUDGET")
        self.assertEqual(row["software_attribution"], "F07_PRE_CREATE_DOMINANT")

    def test_unexplained_physical_gap_stays_unattributed(self):
        row = profile(frame(physical_frame_interval_ns=120_000_000))["frames"][0]
        self.assertEqual(row["physical_attribution"], "UNATTRIBUTED_OVERRUN")
        self.assertEqual(row["software_attribution"], "WITHIN_120HZ_BUDGET")

    def test_combined_overrun_without_dominant_pre_or_backend_overrun(self):
        row = profile(frame(gpu_exec_ns=5_000_000))["frames"][0]
        self.assertEqual(row["total_pipeline_us"], 9000)
        self.assertEqual(row["software_attribution"], "UNATTRIBUTED_OVERRUN")
        self.assertIsNone(row["physical_attribution"])

    def test_zero_total_has_no_stage_shares(self):
        row = profile(frame(pre_create_ns=0, encode_ns=0, submit_ns=0, gpu_exec_ns=0,
                            wait_sync_ns=0))["frames"][0]
        self.assertIsNone(row["pre_create_share_pct"])
        self.assertIsNone(row["backend_share_pct"])

    def test_instruction_counter_coverage(self):
        complete = profile(frame(instruction_count=42), frame(2, instruction_count=0))["scenes"][APP]
        self.assertEqual((complete["instruction_count"], complete["instruction_count_frames"]), (42, 2))
        partial = profile(frame(instruction_count=42), frame(2))["scenes"][APP]
        self.assertEqual((partial["instruction_count"], partial["instruction_count_frames"]), (None, 1))


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.baseline = profile(frame(physical_frame_interval_ns=10_000_000, instruction_count=100))
        self.candidate = profile(frame(physical_frame_interval_ns=8_000_000, instruction_count=75,
                                       encode_ns=500_000, wfi_count=1))

    def test_delta_direction_and_coverage_per_scene(self):
        app = fw.compare(self.baseline, self.candidate)["scenes"][APP]
        self.assertEqual(app["wfi_count"]["delta"], -1)
        self.assertEqual(app["instruction_count"]["delta"], -25)
        self.assertEqual(app["backend_total_us"]["delta"], -500)
        self.assertEqual(app["backend_avg_us"]["delta"], -500)
        for percentile in ("p50_us", "p95_us", "p99_us"):
            self.assertEqual(app["physical_intervals"][percentile]["delta"], -2000)
            self.assertEqual(app["software_pipeline"][percentile]["delta"], -500)

    def test_unknown_instruction_delta_and_missing_physical(self):
        app = fw.compare(self.baseline, profile(frame()))["scenes"][APP]
        self.assertIsNone(app["instruction_count"]["delta"])
        self.assertIsNone(app["physical_intervals"]["p95_us"]["delta"])
        self.assertEqual(app["physical_intervals"]["sample_count"]["delta"], -1)

    def test_custom_left_or_right_only(self):
        report = fw.compare(profile(frame(scene="CUSTOM")), self.candidate)
        self.assertEqual(report["scenes"]["CUSTOM"]["frame_count"]["delta"], -1)
        self.assertIsNone(report["scenes"]["CUSTOM"]["software_pipeline"]["p99_us"]["delta"])
        self.assertIsNone(report["scenes"]["CUSTOM"]["backend_total_us"]["delta"])
        self.assertIsNone(report["scenes"]["CUSTOM"]["wfi_count"]["delta"])

    def test_rejects_forged_reports(self):
        for flag in ("hardware_acceptance", "device_verified"):
            with self.subTest(flag=flag):
                invalid = {**self.baseline, flag: True}
                with self.assertRaisesRegex(fw.ProfileError, "must have device_verified=false"):
                    fw.compare(invalid, self.candidate)

    def test_rejects_schema_and_budget_mismatch(self):
        for change in ({"schema_version": True}, {"schema_version": 2}, {"target_budget_us": 8333}):
            with self.subTest(change=change), self.assertRaisesRegex(fw.ProfileError, "schema_version"):
                fw.compare({**self.baseline, **change}, self.candidate)

    def test_rejects_non_numeric_distribution_value(self):
        malformed = copy.deepcopy(self.baseline)
        malformed["scenes"][APP]["physical_intervals"]["p50_us"] = "10000"
        with self.assertRaisesRegex(fw.ProfileError, "invalid report field: p50_us"):
            fw.compare(malformed, self.candidate)

    def test_report_flags_and_report_schema(self):
        for report in (self.baseline, fw.compare(self.baseline, self.candidate)):
            self.assertEqual(report["schema_version"], 1)
            self.assertEqual(report["target_budget_us"], 8333.333)
            self.assertIs(report["device_verified"], False)
            self.assertIs(report["hardware_acceptance"], False)


class CliTests(unittest.TestCase):
    def setUp(self):
        fw.CACHE_ROOT.mkdir(parents=True, exist_ok=True)
        scratch = tempfile.TemporaryDirectory(prefix="profiler-test-", dir=fw.CACHE_ROOT)
        self.addCleanup(scratch.cleanup)
        self.directory = Path(scratch.name)
        self.capture = self.directory / "sample.a840fw"
        self.capture.write_bytes(binary())

    def cli(self, *args):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), *map(str, args)], cwd=REPO,
                              capture_output=True, text=True, timeout=30, check=False)

    def assert_failure(self, result, message):
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIn(message, report["error"])
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)

    def test_analyze_binary_json_and_markdown(self):
        report_file = self.directory / "report.json"
        md = self.directory / "report.md"
        result = self.cli("analyze", "--input", self.capture, "--out-json", report_file, "--out-md", md)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row = json.loads(result.stdout)
        self.assertEqual(row["source_format"], "A840FW01")
        self.assertEqual(row, json.loads(report_file.read_text()))
        self.assertIs(row["device_verified"], False)
        self.assertIs(row["hardware_acceptance"], False)
        text = md.read_text(encoding="utf-8")
        for expected in ("device_verified=false", "hardware_acceptance=false", "Reported physical",
                         "F07_PRE_CREATE_DOMINANT", "0xffffffffffffffff", "Six-stage scene totals"):
            self.assertIn(expected, text)

    def test_analyze_utf8_bom_jsonl(self):
        path = self.directory / "frame.jsonl"
        path.write_text(jsonl(frame(), draw()), encoding="utf-8-sig")
        result = self.cli("analyze", "--input", path, "--top-n", 1)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["source_format"], "JSONL")

    def test_magic_detection_without_suffix(self):
        path = self.directory / "capture"
        path.write_bytes(self.capture.read_bytes())
        result = self.cli("analyze", "--input", path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["source_format"], "A840FW01")

    def test_corrupt_file_does_not_emit_report(self):
        self.capture.write_bytes(alter(binary(), 140 + 8, "I", 15))
        report_file = self.directory / "report.json"
        self.assert_failure(self.cli("analyze", "--input", self.capture, "--out-json", report_file), "CRC32")
        self.assertFalse(report_file.exists())

    def test_bad_encoding_reports_failure(self):
        path = self.directory / "bad.jsonl"
        path.write_bytes(b"\x80\x80")
        self.assert_failure(self.cli("analyze", "--input", path), "decode")

    def test_missing_input_reports_failure(self):
        self.assert_failure(self.cli("analyze", "--input", self.directory / "absent"), "No such file")

    def test_invalid_top_n_exit_code(self):
        self.assert_failure(self.cli("analyze", "--input", self.capture, "--top-n", 0), "top_n")

    def test_usage_exit_code(self):
        self.assertEqual(self.cli("analyze").returncode, 2)

    def test_forbidden_output_paths_and_symlink(self):
        link = self.directory / "escape"
        link.symlink_to("/tmp")
        for outside in ("/tmp/w09-l4-forbidden.json", "/mnt/c/w09-l4-forbidden.json", link / "w09.json"):
            with self.subTest(path=outside):
                result = self.cli("analyze", "--input", self.capture, "--out-json", outside)
                self.assert_failure(result, "inside the W09 Lane 4 cache")

    def test_duplicate_outputs_and_input_alias_fail_before_write(self):
        for target in (self.capture, self.directory / "report.json"):
            with self.subTest(path=target):
                result = self.cli("analyze", "--input", self.capture, "--out-json", target, "--out-md", target)
                self.assert_failure(result, "must be distinct")
        self.assertEqual(self.capture.read_bytes(), binary())

    def test_compare_cli_uses_reports_and_writes_both_formats(self):
        a, b = self.directory / "a.json", self.directory / "b.json"
        a.write_text(json.dumps(profile(frame(instruction_count=10))))
        b.write_text(json.dumps(profile(frame(instruction_count=12, wfi_count=0))))
        json_out, md = self.directory / "comparison.json", self.directory / "comparison.md"
        result = self.cli("compare", "--baseline", a, "--candidate", b,
                          "--out-json", json_out, "--out-md", md)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(json_out.read_text())
        self.assertEqual(report["scenes"][APP]["instruction_count"]["delta"], 2)
        self.assertEqual(report["scenes"][APP]["wfi_count"]["delta"], -2)
        self.assertIs(report["device_verified"], False)
        self.assertIs(report["hardware_acceptance"], False)
        self.assertIn("Candidate minus baseline", md.read_text(encoding="utf-8"))
        alias = self.cli("--baseline", a, "--candidate", b)
        self.assertEqual(json.loads(alias.stdout), report)

    def test_compare_cli_rejects_false_provenance_claim(self):
        a, b = self.directory / "a.json", self.directory / "b.json"
        baseline = profile(frame())
        a.write_text(json.dumps(baseline))
        b.write_text(json.dumps({**baseline, "device_verified": True}))
        self.assert_failure(self.cli("compare", "--baseline", a, "--candidate", b), "device_verified=false")


class NativeBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fw.CACHE_ROOT.mkdir(parents=True, exist_ok=True)
        scratch = tempfile.TemporaryDirectory(prefix="fw-cpp-bridge-", dir=fw.CACHE_ROOT)
        cls.addClassCleanup(scratch.cleanup)
        directory = Path(scratch.name)
        src = REPO / "apps/NezhaGpuTest/perf"
        obj, exe, sample = directory / "recorder.o", directory / "test_fw", directory / "sample.a840fw"
        env = {**os.environ, "TMPDIR": str(directory), "TMP": str(directory), "TEMP": str(directory)}
        flags = ("-O2", "-Wall", "-Wextra", "-Werror")
        for command in (
            ["gcc", "-std=c11", *flags, "-c", src / "a840_frame_weights.c", "-o", obj],
            ["g++", "-std=c++17", *flags, src / "test_frame_weights.cpp", obj, "-o", exe],
            [exe, "--dump-sample", sample],
        ):
            result = subprocess.run(command, cwd=REPO, env=env, capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f"{command[0]} failed: {result.stdout}\n{result.stderr}")
        cls.trace = fw.load_trace(sample)
        cls.report = fw.analyze(cls.trace)

    def test_native_export_has_all_four_scenes(self):
        self.assertEqual(self.trace.source_format, "A840FW01")
        self.assertEqual((len(self.trace.frames), len(self.trace.draws)), (12, 24))
        self.assertEqual([self.report["scenes"][name]["frame_count"] for name in fw.CORE_SCENES],
                         [3, 3, 3, 3])

    def test_native_timing_and_ranking(self):
        self.assertEqual(self.report["draw_weights"]["total_gpu_us"], 720)
        top = self.report["draw_weights"]["by_pipeline_hash"][0]
        self.assertEqual(top["pipeline_hash"], "0xfedcba0102030405")
        self.assertEqual(top["total_gpu_us"], 600)
        self.assertEqual(self.trace.draws[0].duration_ns, 10000)
        self.assertGreater(self.trace.draws[0].start_ticks_19m2, 1 << 63)

    def test_native_budget_and_separate_causes(self):
        def physical(scene):
            return self.report["scenes"][scene]["physical_intervals"]

        self.assertEqual(physical(APP)["max_stall"]["attribution"], "F07_PRE_CREATE_DOMINANT")
        self.assertEqual(physical(SETTINGS)["max_stall"]["attribution"], "BACKEND_OVER_BUDGET")
        self.assertEqual(physical(LIBRARY)["max_stall"]["attribution"], "UNATTRIBUTED_OVERRUN")
        self.assertEqual(physical(GLASS)["max_us"], 120_000)
        self.assertEqual(physical(GLASS)["stall_buckets"]["ge_100_ms"], 1)
        self.assertEqual(physical(GLASS)["sample_count"], 2)
        self.assertIsNone(self.report["scenes"][GLASS]["instruction_count"])
        self.assertIs(self.report["device_verified"], False)
        self.assertIs(self.report["hardware_acceptance"], False)


if __name__ == "__main__":
    unittest.main()
