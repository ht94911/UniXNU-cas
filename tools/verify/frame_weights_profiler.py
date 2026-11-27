#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Profile A840FW01 or JSONL captures, or compare two analyze JSON reports.

JSONL uses one object per line, with type=frame or type=draw and the named
A840FW01 record fields. Scene and GMEM mode also accept their enum names.
An absent physical_frame_interval_ns means zero (no reported interval).
JSONL frames can additionally carry instruction_count and independent
backend_total_ns/us and total_pipeline_ns/us claims. Pipeline hashes may
be unsigned integers or hexadecimal strings with a 0x prefix.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
from decimal import Decimal
import json
import math
from pathlib import Path
import re
import struct
import sys
from tempfile import NamedTemporaryFile
import zlib

CACHE_ROOT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w09-l4")
MAGIC = b"A840FW01"
VERSION = 1
ALWAYS_ON_HZ = 19_200_000
TARGET_REFRESH_HZ = 120
BUDGET_NS = 8_333_333
MAX_U32 = (1 << 32) - 1
MAX_U64 = (1 << 64) - 1
MIN_I64 = -(1 << 63)
MAX_I64 = (1 << 63) - 1
ALGEBRA_TOLERANCE_NS = 1000
HEADER = struct.Struct("<8sIIIQ4I")
FRAME = struct.Struct("<Q6I6QqQ")
DRAW = struct.Struct("<QIIQIIIQQQI")

SCENES = {
    0: "CUSTOM",
    1: "APP_OPEN_CLOSE",
    2: "SETTINGS_SCROLL",
    3: "APP_LIBRARY_SCROLL",
    4: "DENSE_LIQUID_GLASS_SWIPE",
}
CORE_SCENES = tuple(SCENES[i] for i in (1, 2, 3, 4))
MODES = {0: "GMEM", 1: "SYSMEM", 2: "COMPUTE", 3: "BLIT"}
FRAME_COUNTERS = ("preempted_count", "wfi_count", "cache_flush_count", "heap_allocs")
BACKEND_STAGES = ("encode_ns", "submit_ns", "gpu_exec_ns", "wait_sync_ns")
TIMED_STAGES = ("pre_create_ns", *BACKEND_STAGES)
STAGES = (*TIMED_STAGES, "scanout_margin_ns")
FRAME_U32 = ("scene_id", "ring_id", *FRAME_COUNTERS)
FRAME_U64 = ("frame_seq", "t_vsync_prev_ns", *TIMED_STAGES, "physical_frame_interval_ns")
FRAME_FIELDS = (FRAME_U64[0], *FRAME_U32, *FRAME_U64[1:-1],
                "scanout_margin_ns", FRAME_U64[-1])
DRAW_FIELDS = (
    "frame_seq", "pass_id", "draw_id", "pipeline_hash", "gmem_mode", "tile_count",
    "draw_verts_or_threads", "start_ticks_19m2", "end_ticks_19m2", "duration_ns", "sync_flags",
)
DRAW_U32 = {"pass_id", "draw_id", "gmem_mode", "tile_count", "draw_verts_or_threads", "sync_flags"}
TOTAL_FIELDS = ("backend_total", "total_pipeline")
CLAIMS = {name + unit for name in TOTAL_FIELDS for unit in ("_ns", "_us")}
BUCKETS = {
    "ge_8_333_ms": BUDGET_NS, "ge_16_667_ms": 16_666_667,
    "ge_20_ms": 20_000_000, "ge_100_ms": 100_000_000,
}
HOST_FLAGS = ("device_verified", "hardware_acceptance")


class ProfileError(ValueError):
    """Malformed capture or report."""


@dataclass(frozen=True)
class Frame:
    frame_seq: int
    scene_id: int
    ring_id: int
    preempted_count: int
    wfi_count: int
    cache_flush_count: int
    heap_allocs: int
    t_vsync_prev_ns: int
    pre_create_ns: int
    encode_ns: int
    submit_ns: int
    gpu_exec_ns: int
    wait_sync_ns: int
    scanout_margin_ns: int
    physical_frame_interval_ns: int
    instruction_count: int | None = None

    @property
    def backend_ns(self) -> int:
        return self.encode_ns + self.submit_ns + self.gpu_exec_ns + self.wait_sync_ns

    @property
    def pipeline_ns(self) -> int:
        return self.pre_create_ns + self.backend_ns


@dataclass(frozen=True)
class Draw:
    frame_seq: int
    pass_id: int
    draw_id: int
    pipeline_hash: int
    gmem_mode: int
    tile_count: int
    draw_verts_or_threads: int
    start_ticks_19m2: int
    end_ticks_19m2: int
    duration_ns: int
    sync_flags: int


@dataclass(frozen=True)
class Trace:
    source_format: str
    frames: list[Frame]
    draws: list[Draw]


def _integer(row: dict, key: str, maximum: int, minimum: int = 0) -> int:
    value = row.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProfileError(f"{key} must be an integer in [{minimum}, {maximum}]")
    return value


def _enum(value, names: dict[int, str], key: str, allow_name: bool) -> int:
    if type(value) is int and value in names:
        return value
    if allow_name and isinstance(value, str):
        for number, name in names.items():
            if value == name:
                return number
    raise ProfileError(f"invalid {key}")


def _reported_totals(row: dict, name: str, expected_ns: int) -> list[Decimal]:
    smallest = expected_ns - ALGEBRA_TOLERANCE_NS
    largest = expected_ns + ALGEBRA_TOLERANCE_NS
    ns_key, us_key = name + "_ns", name + "_us"
    totals = []
    if ns_key in row:
        value = _integer(row, ns_key, MAX_U64)
        if not smallest <= value <= largest:
            raise ProfileError(f"{ns_key} identity error exceeds 1.0 us")
        totals.append(Decimal(value).scaleb(-3))
    if us_key in row:
        value = row[us_key]
        if type(value) not in (int, Decimal) or not Decimal(value).is_finite() or value < 0:
            raise ProfileError(f"{us_key} must be a finite nonnegative number")
        if not Decimal(smallest).scaleb(-3) <= value <= Decimal(largest).scaleb(-3):
            raise ProfileError(f"{us_key} identity error exceeds 1.0 us")
        totals.append(Decimal(value))
    return totals


def _frame(row: dict, *, jsonl: bool, header_scene: int = 0) -> Frame:
    scene = _enum(row.get("scene_id"), SCENES, "scene_id", jsonl)
    if header_scene and scene != header_scene:
        raise ProfileError("frame scene_id disagrees with header")
    ring = _integer(row, "ring_id", 3)
    margin = _integer(row, "scanout_margin_ns", MAX_I64, MIN_I64)
    values = {key: _integer(row, key, MAX_U64) for key in FRAME_U64}
    counts = {key: _integer(row, key, MAX_U32) for key in FRAME_COUNTERS}
    instructions = _integer(row, "instruction_count", MAX_U64) if "instruction_count" in row else None
    frame = Frame(**values, **counts, scene_id=scene, ring_id=ring, scanout_margin_ns=margin,
                  instruction_count=instructions)
    expected_margin = BUDGET_NS - frame.pipeline_ns
    if expected_margin < MIN_I64:
        raise ProfileError("stage sum exceeds signed scanout_margin_ns range")
    tolerance = ALGEBRA_TOLERANCE_NS if jsonl else 0
    if abs(margin - expected_margin) > tolerance:
        raise ProfileError("scanout_margin_ns disagrees with budget minus the timed stages")
    if jsonl:
        backends = _reported_totals(row, "backend_total", frame.backend_ns)
        pipelines = _reported_totals(row, "total_pipeline", frame.pipeline_ns)
        pre_create_us = Decimal(frame.pre_create_ns).scaleb(-3)
        if any(abs(total - backend - pre_create_us) > 1 for total in pipelines for backend in backends):
            raise ProfileError("reported total_pipeline and backend_total identity error exceeds 1.0 us")
    return frame


def _draw(row: dict, *, jsonl: bool) -> Draw:
    mode = _enum(row.get("gmem_mode"), MODES, "gmem_mode", jsonl)
    value = row.get("pipeline_hash")
    if jsonl and isinstance(value, str) and re.fullmatch(r"0[xX][0-9a-fA-F]{1,16}", value):
        row = {**row, "pipeline_hash": int(value, 16)}
    fields = {name: _integer(row, name, MAX_U32 if name in DRAW_U32 else MAX_U64)
              for name in DRAW_FIELDS if name != "gmem_mode"}
    draw = Draw(gmem_mode=mode, **fields)
    ticks = draw.end_ticks_19m2 - draw.start_ticks_19m2
    if ticks < 0:
        raise ProfileError("draw tick interval is reversed")
    duration_ns = ticks * 1_000_000_000 // ALWAYS_ON_HZ
    if duration_ns > MAX_U64:
        raise ProfileError("draw duration exceeds uint64")
    if draw.duration_ns != duration_ns:
        raise ProfileError("duration_ns disagrees with the 19.2 MHz tick interval")
    return draw


def _append_frame(frames: list[Frame], frame: Frame) -> None:
    if frames and (frame.frame_seq <= frames[-1].frame_seq or
                   frame.t_vsync_prev_ns < frames[-1].t_vsync_prev_ns):
        raise ProfileError("frame_seq must increase and t_vsync_prev_ns must be nondecreasing")
    frames.append(frame)


def _trace(source_format: str, frames: list[Frame], draws: list[Draw]) -> Trace:
    frame_ids = {frame.frame_seq for frame in frames}
    for draw in draws:
        if draw.frame_seq not in frame_ids:
            raise ProfileError(f"draw references missing frame_seq {draw.frame_seq}")
    return Trace(source_format, frames, draws)


def decode_binary(data: bytes) -> Trace:
    if len(data) < HEADER.size:
        raise ProfileError("truncated A840FW01 header")
    magic, version, hz, refresh, budget, scene, frame_count, draw_count, crc = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ProfileError("bad A840FW01 magic")
    if (version, hz, refresh, budget) != (VERSION, ALWAYS_ON_HZ, TARGET_REFRESH_HZ, BUDGET_NS):
        raise ProfileError("unsupported A840FW01 version, clock, refresh, or budget")
    _enum(scene, SCENES, "header scene_id", False)
    frame_end = HEADER.size + frame_count * FRAME.size
    if len(data) != frame_end + draw_count * DRAW.size:
        raise ProfileError("A840FW01 length does not match record counts")
    view = memoryview(data)
    if zlib.crc32(view[HEADER.size:]) != crc:
        raise ProfileError("A840FW01 payload CRC32 mismatch")

    frames, draws = [], []
    for index, values in enumerate(FRAME.iter_unpack(view[HEADER.size:frame_end]), 1):
        try:
            row = dict(zip(FRAME_FIELDS, values))
            _append_frame(frames, _frame(row, jsonl=False, header_scene=scene))
        except ProfileError as exc:
            raise ProfileError(f"binary frame {index}: {exc}") from exc
    for index, values in enumerate(DRAW.iter_unpack(view[frame_end:]), 1):
        try:
            draws.append(_draw(dict(zip(DRAW_FIELDS, values)), jsonl=False))
        except ProfileError as exc:
            raise ProfileError(f"binary draw {index}: {exc}") from exc
    return _trace("A840FW01", frames, draws)


def _unique_object(fields: list[tuple]) -> dict:
    row = {}
    for key, value in fields:
        if key in row:
            raise ProfileError("duplicate JSON key")
        row[key] = value
    return row


def _reject_constant(_: str):
    raise ProfileError("non-finite JSON number")


def _parse_json(text: str):
    try:
        return json.loads(text, parse_float=Decimal, object_pairs_hook=_unique_object,
                          parse_constant=_reject_constant)
    except ProfileError:
        raise
    except (ValueError, ArithmeticError, RecursionError) as exc:
        raise ProfileError("invalid JSON") from exc


def parse_jsonl(lines) -> Trace:
    frames, draws = [], []
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = _parse_json(line)
            if type(row) is not dict:
                raise ProfileError("expected a JSON object")
            for flag in HOST_FLAGS:
                if row.pop(flag, False) is not False:
                    raise ProfileError(f"{flag} must be false")
            kind = row.pop("type", None)
            if kind == "frame":
                if row.keys() - (set(FRAME_FIELDS) | CLAIMS | {"instruction_count"}):
                    raise ProfileError("unknown frame field")
                row.setdefault("physical_frame_interval_ns", 0)
                _append_frame(frames, _frame(row, jsonl=True))
            elif kind == "draw":
                if row.keys() - set(DRAW_FIELDS):
                    raise ProfileError("unknown draw field")
                draws.append(_draw(row, jsonl=True))
            else:
                raise ProfileError("expected type=frame or type=draw")
        except ProfileError as exc:
            raise ProfileError(f"JSONL line {number}: {exc}") from exc
    return _trace("JSONL", frames, draws)


def load_trace(path: Path) -> Trace:
    data = path.read_bytes()
    if data.startswith(MAGIC) or path.suffix.lower() == ".a840fw":
        return decode_binary(data)
    return parse_jsonl(data.decode("utf-8-sig").splitlines())


def _us(ns: int) -> float:
    return ns / 1000


def _percentage(part: int, total: int) -> float | None:
    return 100 * part / total if total else None


def _nearest_rank(sorted_values: list[int], percent: int) -> int:
    return sorted_values[(len(sorted_values) * percent + 99) // 100 - 1]


def _draw_summary(draws: list[Draw], all_ns: int) -> dict:
    samples = sorted(draw.duration_ns for draw in draws)
    total_ns = sum(samples)
    by_mode = dict.fromkeys(MODES.values(), 0)
    for draw in draws:
        by_mode[MODES[draw.gmem_mode]] += draw.duration_ns
    return {
        "draw_count": len(draws), "total_gpu_us": _us(total_ns),
        "avg_draw_us": _us(total_ns) / len(draws) if draws else None,
        "p95_draw_us": _us(_nearest_rank(samples, 95)) if draws else None,
        "gpu_share_pct": _percentage(total_ns, all_ns),
        "mode_share_pct": {name: _percentage(ns, total_ns) for name, ns in by_mode.items()},
    }


def _draw_weights(draws: list[Draw], top_n: int) -> dict:
    by_pipeline, by_pass_mode = defaultdict(list), defaultdict(list)
    for draw in draws:
        by_pipeline[draw.pipeline_hash].append(draw)
        by_pass_mode[draw.pass_id, draw.gmem_mode].append(draw)
    all_ns = sum(draw.duration_ns for draw in draws)

    def ranked(groups):
        return sorted(groups, key=lambda key: (-sum(draw.duration_ns for draw in groups[key]), key))[:top_n]

    return {
        **_draw_summary(draws, all_ns),
        "by_pipeline_hash": [
            {"pipeline_hash": f"0x{key:016x}", **_draw_summary(by_pipeline[key], all_ns)}
            for key in ranked(by_pipeline)
        ],
        "by_pass_gmem_mode": [
            {"pass_id": key[0], "gmem_mode": MODES[key[1]], **_draw_summary(by_pass_mode[key], all_ns)}
            for key in ranked(by_pass_mode)
        ],
    }


def _attribution(frame: Frame, interval_ns: int) -> str:
    if interval_ns <= BUDGET_NS:
        return "WITHIN_120HZ_BUDGET"
    if frame.pipeline_ns > BUDGET_NS and frame.pre_create_ns > frame.backend_ns:
        return "F07_PRE_CREATE_DOMINANT"
    if frame.backend_ns > BUDGET_NS:
        return "BACKEND_OVER_BUDGET"
    # A long physical interval can also contain time outside the recorded stages.
    return "UNATTRIBUTED_OVERRUN"


def _frame_summary(frame: Frame) -> dict:
    interval = frame.physical_frame_interval_ns
    return {
        "frame_seq": frame.frame_seq, "scene": SCENES[frame.scene_id], "ring_id": frame.ring_id,
        "t_vsync_prev_ns": frame.t_vsync_prev_ns,
        **{key.removesuffix("_ns") + "_us": _us(getattr(frame, key)) for key in STAGES},
        "backend_total_us": _us(frame.backend_ns), "total_pipeline_us": _us(frame.pipeline_ns),
        "pre_create_share_pct": _percentage(frame.pre_create_ns, frame.pipeline_ns),
        "backend_share_pct": _percentage(frame.backend_ns, frame.pipeline_ns),
        "software_attribution": _attribution(frame, frame.pipeline_ns),
        "physical_frame_interval_us": _us(interval) if interval else None,
        "physical_attribution": _attribution(frame, interval) if interval else None,
    }


def _timing_stats(samples: list[tuple[int, dict]], attribution_key: str) -> dict:
    values = sorted(ns for ns, _ in samples)
    max_stall = None
    if values and values[-1] >= BUDGET_NS:
        ns, frame = max(samples, key=lambda sample: sample[0])
        max_stall = {
            "frame_seq": frame["frame_seq"], "duration_us": _us(ns),
            "attribution": frame[attribution_key],
            "pre_create_share_pct": frame["pre_create_share_pct"],
            "backend_share_pct": frame["backend_share_pct"],
        }
    return {
        "sample_count": len(values),
        **{f"p{p}_us": _us(_nearest_rank(values, p)) if values else None for p in (50, 95, 99)},
        "max_us": _us(values[-1]) if values else None,
        "within_budget_pct": _percentage(sum(ns <= BUDGET_NS for ns in values), len(values)),
        "stall_buckets": {name: sum(ns >= boundary for ns in values)
                          for name, boundary in BUCKETS.items()},
        "max_stall": max_stall,
    }


def _scene_stats(rows: list[tuple[Frame, dict]]) -> dict:
    physical = [(frame.physical_frame_interval_ns, summary)
                for frame, summary in rows if frame.physical_frame_interval_ns]
    software = [(frame.pipeline_ns, summary) for frame, summary in rows]
    backend_ns = sum(frame.backend_ns for frame, _ in rows)
    instruction_counts = [frame.instruction_count for frame, _ in rows
                          if frame.instruction_count is not None]
    return {
        "frame_count": len(rows),
        "physical_intervals": _timing_stats(physical, "physical_attribution"),
        "software_pipeline": _timing_stats(software, "software_attribution"),
        "stage_totals_us": {key.removesuffix("_ns"): _us(sum(getattr(frame, key) for frame, _ in rows))
                            for key in STAGES},
        "backend_total_us": _us(backend_ns),
        "backend_avg_us": _us(backend_ns) / len(rows) if rows else None,
        **{key: sum(getattr(frame, key) for frame, _ in rows) for key in FRAME_COUNTERS},
        "instruction_count_frames": len(instruction_counts),
        "instruction_count": sum(instruction_counts) if rows and len(instruction_counts) == len(rows) else None,
    }


def _report_fields(mode: str) -> dict:
    return {
        "schema_version": VERSION, "target_budget_us": _us(BUDGET_NS),
        "device_verified": False, "hardware_acceptance": False, "mode": mode,
    }


def analyze(trace: Trace, top_n: int = 10) -> dict:
    if type(top_n) is not int or top_n < 1:
        raise ProfileError("top_n must be positive")
    if not trace.frames:
        raise ProfileError("capture contains no frames")
    by_scene = {name: [] for name in CORE_SCENES}
    rows = []
    for frame in trace.frames:
        summary = _frame_summary(frame)
        by_scene.setdefault(summary["scene"], []).append((frame, summary))
        rows.append(summary)
    return {
        **_report_fields("analyze"), "status": "PASS", "source_format": trace.source_format,
        "source_evidence_kind": "unverified_capture",
        "target_refresh_hz": TARGET_REFRESH_HZ, "frame_count": len(rows), "frames": rows,
        "draw_weights": _draw_weights(trace.draws, top_n),
        "scenes": {name: _scene_stats(group) for name, group in by_scene.items()},
    }


def _metric(row: dict, name: str, *, counter: bool = False, nullable: bool = False):
    if name not in row:
        raise ProfileError(f"report field missing: {name}")
    value = row[name]
    if value is None and nullable:
        return None
    if counter:
        if type(value) is int and value >= 0:
            return value
    elif type(value) in (int, float, Decimal):
        try:
            converted = float(value)
        except (OverflowError, ValueError):
            converted = math.inf
        if math.isfinite(converted) and value >= 0:
            return converted
    raise ProfileError(f"invalid report field: {name}")


def _report_scenes(report: dict) -> dict:
    if (type(report) is not dict or type(report.get("schema_version")) is not int or
            report["schema_version"] != VERSION or report.get("mode") != "analyze" or
            report.get("status") != "PASS" or _metric(report, "target_budget_us") != _us(BUDGET_NS)):
        raise ProfileError("expected a successful schema_version 1 analyze report at 120 Hz")
    if any(report.get(flag) is not False for flag in HOST_FLAGS):
        raise ProfileError("report must have device_verified=false and hardware_acceptance=false")
    scenes = report.get("scenes")
    if (type(scenes) is not dict or not set(CORE_SCENES) <= scenes.keys() or
            scenes.keys() - set(SCENES.values()) or any(type(row) is not dict for row in scenes.values())):
        raise ProfileError("invalid report scenes")
    return scenes


def _change(baseline, candidate) -> dict:
    return {"baseline": baseline, "candidate": candidate,
            "delta": candidate - baseline if candidate is not None and baseline is not None else None}


def _timing_change(baseline: dict, candidate: dict, key: str) -> dict:
    a, b = baseline.get(key), candidate.get(key)
    if type(a) is not dict or type(b) is not dict:
        raise ProfileError(f"invalid report distribution: {key}")
    return {
        "sample_count": _change(_metric(a, "sample_count", counter=True),
                                _metric(b, "sample_count", counter=True)),
        **{name: _change(_metric(a, name, nullable=True), _metric(b, name, nullable=True))
           for name in ("p50_us", "p95_us", "p99_us", "max_us", "within_budget_pct")},
    }


def compare(baseline: dict, candidate: dict) -> dict:
    base_scenes, candidate_scenes = _report_scenes(baseline), _report_scenes(candidate)
    names = list(CORE_SCENES)
    if "CUSTOM" in base_scenes or "CUSTOM" in candidate_scenes:
        names.append("CUSTOM")
    empty = _scene_stats([])
    scenes = {}
    for name in names:
        a, b = base_scenes.get(name, empty), candidate_scenes.get(name, empty)
        a_count = _metric(a, "frame_count", counter=True)
        b_count = _metric(b, "frame_count", counter=True)
        changes = {"frame_count": _change(a_count, b_count)}
        for key in ("wfi_count", "instruction_count", "instruction_count_frames",
                    "backend_total_us", "backend_avg_us"):
            counter = key in ("wfi_count", "instruction_count", "instruction_count_frames")
            nullable = key in ("instruction_count", "backend_avg_us")
            a_metric = _metric(a, key, counter=counter, nullable=nullable)
            b_metric = _metric(b, key, counter=counter, nullable=nullable)
            if key != "instruction_count_frames":
                a_metric = a_metric if a_count else None
                b_metric = b_metric if b_count else None
            changes[key] = _change(a_metric, b_metric)
        for key in ("physical_intervals", "software_pipeline"):
            changes[key] = _timing_change(a, b, key)
        scenes[name] = changes
    return {**_report_fields("compare"), "status": "PASS", "source_evidence_kind": "unverified_comparison",
            "scenes": scenes}


def _display(value) -> str:
    if value is None:
        return "—"
    if type(value) in (int, str):
        return str(value)
    return f"{value:.3f}"


def markdown(report: dict) -> str:
    lines = ["# Adreno 840 frame weights", "",
             "Evidence device_verified=false, hardware_acceptance=false.", "",
             "120 Hz budget 8.333333 ms. Physical intervals are unverified input values.", ""]
    if report["mode"] == "compare":
        lines += ["Candidate minus baseline. Times and their changes are in µs.", "",
                  "| Scene | Basis | Δ samples | Δ P50 | Δ P95 | Δ P99 |",
                  "| --- | --- | ---: | ---: | ---: | ---: |"]
        for scene, row in report["scenes"].items():
            for key, label in (("physical_intervals", "Reported physical"), ("software_pipeline", "Software")):
                values = [_display(row[key][field]["delta"])
                          for field in ("sample_count", "p50_us", "p95_us", "p99_us")]
                lines.append(f"| {scene} | {label} | " + " | ".join(values) + " |")
        lines += ["", "| Scene | Δ instructions | Δ frames with instruction counts | Δ WFI | Δ backend total µs | Δ backend mean µs |",
                  "| --- | ---: | ---: | ---: | ---: | ---: |"]
        for scene, row in report["scenes"].items():
            values = [_display(row[key]["delta"]) for key in
                      ("instruction_count", "instruction_count_frames", "wfi_count", "backend_total_us", "backend_avg_us")]
            lines.append(f"| {scene} | " + " | ".join(values) + " |")
        return "\n".join(lines) + "\n"

    lines += [
        "Software totals sum the five timed stages. Draw shares use the sum of the recorded Draw durations.",
        "Percentiles use nearest rank. Threshold counts are cumulative.",
        "",
        "| Scene | Basis | Frames | Samples | P50 µs | P95 µs | P99 µs | Max µs | Within budget % | ≥8.333 ms | ≥16.667 ms | ≥20 ms | ≥100 ms |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    basis = (("physical_intervals", "Reported physical"), ("software_pipeline", "Software"))
    stalls = []
    for scene, row in report["scenes"].items():
        for key, label in basis:
            stats = row[key]
            values = [_display(stats[field]) for field in
                      ("sample_count", "p50_us", "p95_us", "p99_us", "max_us", "within_budget_pct")]
            values.extend(str(stats["stall_buckets"][bucket]) for bucket in BUCKETS)
            lines.append(f"| {scene} | {label} | {row['frame_count']} | " + " | ".join(values) + " |")
            stall = stats["max_stall"]
            if stall:
                details = [_display(stall[field]) for field in
                           ("frame_seq", "duration_us", "attribution", "pre_create_share_pct", "backend_share_pct")]
                stalls.append(f"| {scene} | {label} | " + " | ".join(details) + " |")
    if stalls:
        lines += [
            "", "| Scene | Basis | Frame | Max stall µs | Attribution | pre_create % | Backend % |",
            "| --- | --- | ---: | ---: | --- | ---: | ---: |", *stalls,
        ]
    stage_names = [name.removesuffix("_ns") for name in STAGES]
    lines += ["", "Six-stage scene totals in µs.", "",
              "| Scene | " + " | ".join(stage_names) + " | Backend total |",
              "| --- |" + " ---: |" * (len(stage_names) + 1)]
    for scene, row in report["scenes"].items():
        if row["frame_count"]:
            values = [row["stage_totals_us"][name] for name in stage_names]
            values.append(row["backend_total_us"])
            lines.append(f"| {scene} | " + " | ".join(map(_display, values)) + " |")

    weights = report["draw_weights"]
    mode_shares = ", ".join(f"{name} {_display(pct)}%" if pct is not None else f"{name} —"
                            for name, pct in weights["mode_share_pct"].items())
    lines += ["", f"Recorded Draw time {weights['total_gpu_us']:.3f} µs ({mode_shares})."]
    for key, label in (("by_pipeline_hash", "Pipeline hash"), ("by_pass_gmem_mode", "Pass / Mode")):
        lines += ["", f"| {label} | Draws | Total µs | Mean µs | P95 µs | GPU time % | GMEM % | SYSMEM % |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for row in weights[key]:
            name = row["pipeline_hash"] if key == "by_pipeline_hash" else f"{row['pass_id']} / {row['gmem_mode']}"
            values = [row[field] for field in ("draw_count", "total_gpu_us", "avg_draw_us", "p95_draw_us",
                                               "gpu_share_pct")]
            values.extend(row["mode_share_pct"][mode] for mode in ("GMEM", "SYSMEM"))
            lines.append(f"| {name} | " + " | ".join(map(_display, values)) + " |")
    return "\n".join(lines) + "\n"


def _output_path(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if (re.match(r"^[A-Za-z]:", str(path)) or CACHE_ROOT.resolve() != CACHE_ROOT or
            CACHE_ROOT not in resolved.parents):
        raise ProfileError("report output must be inside the W09 Lane 4 cache")
    return resolved


def _write_report(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".frame-weights-",
                                dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("--baseline", "--candidate"):
        argv.insert(0, "compare")

    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    analysis = modes.add_parser("analyze", help="profile one binary or JSONL capture")
    analysis.add_argument("--input", type=Path, required=True)
    analysis.add_argument("--top-n", type=int, default=10)
    comparison = modes.add_parser("compare", help="compare two JSON reports from analyze")
    comparison.add_argument("--baseline", type=Path, required=True)
    comparison.add_argument("--candidate", type=Path, required=True)
    for mode in (analysis, comparison):
        mode.add_argument("--out-json", type=Path)
        mode.add_argument("--out-md", type=Path)
    args = parser.parse_args(argv)

    try:
        inputs = [args.input] if args.mode == "analyze" else [args.baseline, args.candidate]
        outputs = [_output_path(path) if path is not None else None
                   for path in (args.out_json, args.out_md)]
        targets = [path for path in outputs if path is not None]
        if len(set(targets)) != len(targets) or set(targets) & {path.resolve() for path in inputs}:
            raise ProfileError("report paths must be distinct from each other and the inputs")
        if args.mode == "analyze":
            report = analyze(load_trace(args.input), top_n=args.top_n)
        else:
            reports = [_parse_json(path.read_text(encoding="utf-8")) for path in inputs]
            report = compare(*reports)
        payload = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if outputs[0] is not None:
            _write_report(outputs[0], payload)
        if outputs[1] is not None:
            _write_report(outputs[1], markdown(report))
    except (OSError, ValueError, RuntimeError) as exc:
        report = {**_report_fields(args.mode), "status": "FAIL", "error": str(exc)}
        payload = json.dumps(report, indent=2) + "\n"
    sys.stdout.write(payload)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
