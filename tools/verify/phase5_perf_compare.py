#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the four core scenes in three frame_weights_profiler analyze reports.

--extra-metrics accepts a JSON object keyed by phase0, phase3, phase5, then
scene name. Each scene can supply instruction_count (a total for the scene's
captured frames), avg_gpu_mhz, and thermal_throttled_pct (0 to 100). Omitted or
null fields add no measurement. The CLI exits 1 on invalid data or a Phase 5
regression, and writes both requested reports when a regression is measured.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from collections import defaultdict
from decimal import Decimal
import json
import math
from pathlib import Path
import re

from check_contracts import CACHE_PARENT, GateError
import frame_weights_profiler as fw

PHASES = ("phase0", "phase3", "phase5")
PAIRS = (("phase0", "phase3"), ("phase3", "phase5"), ("phase0", "phase5"))
TIMING_GROUPS = ("physical_intervals", "software_pipeline", "backend")
BACKEND_STAGES = tuple(key.removesuffix("_ns") + "_us" for key in fw.BACKEND_STAGES)
DISTRIBUTION_FIELDS = ("p50_us", "p95_us", "p99_us", "max_us")
EFFICIENCY_COUNTS = (*fw.FRAME_COUNTERS, "instruction_count")
EXTRA_FIELDS = ("instruction_count", "avg_gpu_mhz", "thermal_throttled_pct")
BUDGET_US = fw.BUDGET_NS / 1000
BUCKET_US = {name: ns / 1000 for name, ns in fw.BUCKETS.items()}
EVIDENCE_KIND = "unverified_phase_comparison"


def evidence_fields() -> dict:
    return {"device_verified": False, "hardware_acceptance": False}


def json_text(report: dict) -> str:
    return json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"


def checked_output_path(path: str | Path, *, directory: bool = False) -> Path:
    """Keep output paths in a real subdirectory of the shared project cache."""
    if re.match(r"^(?:[A-Za-z]:|\\\\)", str(path)):
        raise GateError("Windows output paths are forbidden")
    source = Path(path).expanduser().absolute()
    if ".." in source.parts or CACHE_PARENT not in source.parents:
        raise GateError(f"output must be inside {CACHE_PARENT}/: {path}")
    resolved = source.resolve()
    if CACHE_PARENT.resolve() != CACHE_PARENT or CACHE_PARENT not in resolved.parents:
        raise GateError(f"output must be inside {CACHE_PARENT}/: {path}")
    if resolved.exists() and not (resolved.is_dir() if directory else resolved.is_file()):
        raise GateError(f"invalid output type: {path}")
    return resolved


def write_output(path: Path, content: str) -> None:
    fw._write_report(checked_output_path(path), content)


def load_json(path: Path):
    return fw._parse_json(path.read_text(encoding="utf-8"))


def _object(value, field: str) -> dict:
    if type(value) is not dict:
        raise fw.ProfileError(f"expected object: {field}")
    return value


def _count(value, field: str) -> int:
    if type(value) is not int or value < 0:
        raise fw.ProfileError(f"expected nonnegative integer: {field}")
    return value


def _number(value, field: str, *, signed: bool = False) -> float:
    if type(value) in (int, float, Decimal):
        try:
            number = float(value)
            if math.isfinite(number) and (signed or value >= 0):
                return number
        except (OverflowError, ValueError):
            pass
    raise fw.ProfileError(f"expected finite {'number' if signed else 'nonnegative number'}: {field}")


def _check(actual, expected, field: str) -> None:
    """Validate the fields consumed from a profiler result against the frame rows."""
    if isinstance(expected, dict):
        obj = _object(actual, field)
        for name, value in expected.items():
            if name not in obj:
                raise fw.ProfileError(f"missing field: {field}.{name}")
            _check(obj[name], value, f"{field}.{name}")
    elif type(expected) is float:
        if not math.isclose(_number(actual, field, signed=expected < 0), expected,
                            rel_tol=1e-12, abs_tol=1e-9):
            raise fw.ProfileError(f"inconsistent field: {field}")
    elif type(actual) is not type(expected) or actual != expected:
        raise fw.ProfileError(f"inconsistent field: {field}")


def _pct(part, total) -> float | None:
    return 100 * part / total if total else None


def _dist(samples: list[float]) -> dict:
    ordered = sorted(samples)
    count = len(ordered)
    return {
        "sample_count": count,
        **{f"p{p}_us": fw._nearest_rank(ordered, p) if count else None for p in (50, 95, 99)},
        "max_us": ordered[-1] if count else None,
        "within_budget_pct": _pct(sum(value <= BUDGET_US for value in ordered), count),
        "stall_buckets": {key: sum(value >= threshold for value in ordered)
                          for key, threshold in BUCKET_US.items()},
    }


def _attribution(interval: float, upstream: float, backend: float, pipeline: float) -> str:
    if interval <= BUDGET_US:
        return "WITHIN_120HZ_BUDGET"
    if pipeline > BUDGET_US and upstream > backend:
        return "F07_PRE_CREATE_DOMINANT"
    if backend > BUDGET_US:
        return "BACKEND_OVER_BUDGET"
    return "UNATTRIBUTED_OVERRUN"


def _frame(row, previous: dict | None, source_format: str) -> dict:
    obj = _object(row, "frame")
    seq = _count(obj.get("frame_seq"), "frame_seq")
    vsync = _count(obj.get("t_vsync_prev_ns"), "t_vsync_prev_ns")
    name = obj.get("scene")
    if name not in (*fw.CORE_SCENES, "CUSTOM"):
        raise fw.ProfileError("unknown frame scene")
    if _count(obj.get("ring_id"), "ring_id") > 3:
        raise fw.ProfileError("invalid ring_id")
    if previous and (seq <= previous["frame_seq"] or vsync < previous["t_vsync_prev_ns"]):
        raise fw.ProfileError("frame_seq or t_vsync_prev_ns is out of order")
    stages = {key: _number(obj.get(key), key) for key in ("pre_create_us", *BACKEND_STAGES)}
    upstream = stages["pre_create_us"]
    backend = _number(obj.get("backend_total_us"), "backend_total_us")
    pipeline = _number(obj.get("total_pipeline_us"), "total_pipeline_us")
    _check(backend, math.fsum(stages[key] for key in BACKEND_STAGES), "backend_total_us")
    _check(pipeline, math.fsum(stages.values()), "total_pipeline_us")
    margin = _number(obj.get("scanout_margin_us"), "scanout_margin_us", signed=True)
    tolerance = fw.ALGEBRA_TOLERANCE_NS / 1000 if source_format == "JSONL" else 0
    if not math.isclose(margin, BUDGET_US - pipeline, rel_tol=1e-12, abs_tol=tolerance + 1e-9):
        raise fw.ProfileError("scanout_margin_us disagrees with the software pipeline")
    physical = obj.get("physical_frame_interval_us")
    if physical is not None:
        physical = _number(physical, "physical_frame_interval_us")
        if physical <= 0:
            raise fw.ProfileError("physical_frame_interval_us must be positive or null")
    expected = {
        "pre_create_share_pct": _pct(upstream, pipeline), "backend_share_pct": _pct(backend, pipeline),
        "software_attribution": _attribution(pipeline, upstream, backend, pipeline),
        "physical_attribution": _attribution(physical, upstream, backend, pipeline)
        if physical is not None else None,
    }
    _check(obj, {**expected, "physical_frame_interval_us": physical}, "frame")
    if any(obj.get(key, False) is not False for key in fw.HOST_FLAGS):
        raise fw.ProfileError("frame provenance flags must be false")
    return {**obj, **stages, **expected, "backend_total_us": backend,
            "total_pipeline_us": pipeline, "physical_frame_interval_us": physical,
            "scanout_margin_us": margin}


def _reported_distribution(frames: list[dict], interval: str, label: str) -> tuple[dict, int]:
    rows = [row for row in frames if row[interval] is not None]
    stats = _dist([row[interval] for row in rows])
    worst = max(rows, key=lambda row: row[interval]) if rows else None
    stall = None
    if worst and worst[interval] >= BUDGET_US:
        stall = {"frame_seq": worst["frame_seq"], "duration_us": worst[interval],
                 "attribution": worst[label], "pre_create_share_pct": worst["pre_create_share_pct"],
                 "backend_share_pct": worst["backend_share_pct"]}
    return {**stats, "max_stall": stall}, sum(row[label] == "F07_PRE_CREATE_DOMINANT" for row in rows)


def _scene(source: dict, frames: list[dict], extra: dict, source_format: str) -> dict:
    count = len(frames)
    physical, physical_f07 = _reported_distribution(frames, "physical_frame_interval_us", "physical_attribution")
    software, software_f07 = _reported_distribution(frames, "total_pipeline_us", "software_attribution")
    stages = ("pre_create_us", *BACKEND_STAGES, "scanout_margin_us")
    totals = {key.removesuffix("_us"): math.fsum(row[key] for row in frames) for key in stages}
    backend = _dist([row["backend_total_us"] for row in frames])
    backend_total = math.fsum(row["backend_total_us"] for row in frames)
    upstream_total = totals["pre_create"]
    pipeline_total = math.fsum(row["total_pipeline_us"] for row in frames)
    _check(source, {"frame_count": count, "physical_intervals": physical, "software_pipeline": software,
                    "stage_totals_us": totals, "backend_total_us": backend_total,
                    "backend_avg_us": backend_total / count if count else None}, "scene")

    counters = {key: _count(source.get(key), key) for key in fw.FRAME_COUNTERS}
    coverage = _count(source.get("instruction_count_frames"), "instruction_count_frames")
    if coverage > count or (source_format == "A840FW01" and coverage):
        raise fw.ProfileError("invalid instruction_count_frames")
    instructions = source.get("instruction_count")
    if count and coverage == count:
        instructions = _count(instructions, "instruction_count")
    elif "instruction_count" not in source or instructions is not None:
        raise fw.ProfileError("incomplete instruction coverage requires null instruction_count")
    if not count and any(counters.values()):
        raise fw.ProfileError("nonzero counter without scene frames")

    supplemental = extra.get("instruction_count")
    if supplemental is not None and instructions is not None and supplemental != instructions:
        raise fw.ProfileError("supplemental instruction_count disagrees with the profiler")
    counters["instruction_count"] = instructions if supplemental is None else supplemental
    efficiency = {key: value if count else None for key, value in counters.items()}
    efficiency.update({f"{key}_per_frame": value / count if count and value is not None else None
                       for key, value in counters.items()})
    efficiency.update({key: extra.get(key) for key in EXTRA_FIELDS[1:]})
    return {
        "frame_count": count, "physical_intervals": physical, "software_pipeline": software,
        "backend": {**backend, "total_us": backend_total if count else None,
                    "mean_us": backend_total / count if count else None,
                    "share_pct": _pct(backend_total, pipeline_total),
                    "within_120hz_budget": backend["max_us"] <= BUDGET_US if count else None},
        "f07_pre_create": {
            "total_us": upstream_total if count else None,
            "mean_us": upstream_total / count if count else None,
            "share_pct": _pct(upstream_total, pipeline_total),
            "software_stall_count": software_f07 if count else None,
            "software_stall_pct": _pct(software_f07, count),
            "physical_stall_count": physical_f07 if physical["sample_count"] else None,
            "physical_stall_pct": _pct(physical_f07, physical["sample_count"]),
        },
        "efficiency": efficiency,
    }


def _read_phase(report, extras: dict) -> tuple[dict, dict]:
    obj = _object(report, "analyze report")
    _check(obj, {"schema_version": fw.VERSION, "mode": "analyze", "status": "PASS",
                 "target_refresh_hz": fw.TARGET_REFRESH_HZ, "target_budget_us": BUDGET_US,
                 "source_evidence_kind": "unverified_capture", **evidence_fields()}, "report")
    source_format = obj.get("source_format")
    if source_format not in ("JSONL", "A840FW01"):
        raise fw.ProfileError("unknown report source_format")
    draws = _object(obj.get("draw_weights"), "draw_weights")
    _count(draws.get("draw_count"), "draw_count")
    raw_frames = obj.get("frames")
    if type(raw_frames) is not list or not raw_frames:
        raise fw.ProfileError("analyze report must contain frames")
    _check(obj.get("frame_count"), len(raw_frames), "frame_count")
    grouped = defaultdict(list)
    previous = None
    for raw in raw_frames:
        previous = _frame(raw, previous, source_format)
        grouped[previous["scene"]].append(previous)
    raw_scenes = _object(obj.get("scenes"), "scenes")
    if raw_scenes.keys() - {*fw.CORE_SCENES, "CUSTOM"} or grouped.keys() - raw_scenes.keys():
        raise fw.ProfileError("invalid scenes or scene summaries missing for captured frames")

    scenes = {}
    for name, raw in raw_scenes.items():
        try:
            if extras.get(name) and not grouped[name]:
                raise fw.ProfileError("supplemental metrics require captured scene frames")
            scenes[name] = _scene(_object(raw, name), grouped[name], extras.get(name, {}), source_format)
        except fw.ProfileError as exc:
            raise fw.ProfileError(f"{name}: {exc}") from exc
    if extras.keys() - raw_scenes.keys():
        raise fw.ProfileError("supplemental metrics require captured scene frames")
    metadata = {"source_format": source_format, "source_evidence_kind": obj["source_evidence_kind"],
                "frame_count": len(raw_frames)}
    return {name: scenes.get(name) if grouped[name] else None for name in fw.CORE_SCENES}, metadata


def _extras(extra_metrics) -> dict:
    if extra_metrics is None:
        return {}
    obj = _object(extra_metrics, "extra metrics")
    if obj.keys() - set(PHASES):
        raise fw.ProfileError("unknown supplemental phase")
    result = {}
    for phase, scenes in obj.items():
        result[phase] = {}
        for name, fields in _object(scenes, phase).items():
            if name not in fw.CORE_SCENES:
                raise fw.ProfileError("unknown supplemental scene")
            fields = _object(fields, name)
            if fields.keys() - set(EXTRA_FIELDS):
                raise fw.ProfileError("unknown supplemental metric")
            row = {}
            for key, value in fields.items():
                if value is not None:
                    row[key] = _count(value, key) if key == "instruction_count" else _number(value, key)
            if row.get("thermal_throttled_pct", 0) > 100:
                raise fw.ProfileError("thermal_throttled_pct must be at most 100")
            if row:
                result[phase][name] = row
    return result


def _change(before, after, direction: str, comparable: bool) -> dict:
    if before is None or after is None or not comparable:
        return {"delta": None, "verdict": "UNMEASURED"}
    delta = after - before
    if delta == 0 or direction == "context":
        verdict = "NEUTRAL"
    else:
        verdict = "IMPROVED" if (delta < 0) == (direction == "lower") else "REGRESSED"
    return {"delta": delta, "verdict": verdict}


def _metrics(row: dict) -> dict:
    """Supply values, preferred directions and denominators for raw totals."""
    values = {}
    frames = row["frame_count"]
    for group in TIMING_GROUPS:
        data = row[group]
        count = data["sample_count"]
        for key in DISTRIBUTION_FIELDS:
            values[f"{group}.{key}"] = (data[key], "lower", None)
        values[f"{group}.within_budget_pct"] = (data["within_budget_pct"], "higher", None)
        for key, hits in data["stall_buckets"].items():
            values[f"{group}.{key}_count"] = (hits if count else None, "lower", count)
            values[f"{group}.{key}_pct"] = (_pct(hits, count), "lower", None)
    for group in ("backend", "f07_pre_create"):
        data = row[group]
        values[f"{group}.total_us"] = (data["total_us"], "lower", frames)
        values[f"{group}.mean_us"] = (data["mean_us"], "lower", None)
        values[f"{group}.share_pct"] = (data["share_pct"], "context", None)
    f07 = row["f07_pre_create"]
    for basis, count in (("software", frames), ("physical", row["physical_intervals"]["sample_count"])):
        for unit, denominator in (("count", count), ("pct", None)):
            key = f"{basis}_stall_{unit}"
            values[f"f07_pre_create.{key}"] = (f07[key], "context", denominator)
    for key, value in row["efficiency"].items():
        values[f"efficiency.{key}"] = (value, "context" if key == "avg_gpu_mhz" else "lower",
                                        frames if key in EFFICIENCY_COUNTS else None)
    return values


def _comparisons(phases: dict) -> dict:
    measured = {phase: _metrics(row) for phase, row in phases.items() if row is not None}
    # Preserve all metric keys even when the scene is absent in every phase.
    reference = next(iter(measured.values()), None)
    if reference is None:
        reference = _metrics(_scene(fw._scene_stats([]), [], {}, "JSONL"))
    comparisons = {}
    for key, (_, direction, _) in reference.items():
        samples = {phase: measured.get(phase, {}).get(key, (None, direction, None)) for phase in PHASES}
        comparisons[key] = {"values": {phase: samples[phase][0] for phase in PHASES},
                            "direction": direction,
                            **{f"{a}_to_{b}": _change(samples[a][0], samples[b][0], direction,
                                                     samples[a][2] == samples[b][2]) for a, b in PAIRS}}
    return comparisons


def compare(phase0: dict, phase3: dict, phase5: dict, *, extra_metrics=None,
            allow_partial_scenes: bool = False) -> dict:
    """Return a closeout report using Phase 0 and Phase 3 as Phase 5 baselines."""
    supplements = _extras(extra_metrics)
    phases, metadata = {}, {}
    for phase, report in zip(PHASES, (phase0, phase3, phase5)):
        try:
            phases[phase], metadata[phase] = _read_phase(report, supplements.get(phase, {}))
        except fw.ProfileError as exc:
            raise fw.ProfileError(f"{phase}: {exc}") from exc
    missing = {phase: [name for name in fw.CORE_SCENES if phases[phase][name] is None] for phase in PHASES}
    complete = not any(missing.values())
    if not complete and not allow_partial_scenes:
        detail = "; ".join(f"{phase} {', '.join(names)}" for phase, names in missing.items() if names)
        raise fw.ProfileError(f"missing core scenes: {detail}; use --allow-partial-scenes for partial coverage")

    scenes = {}
    for name in fw.CORE_SCENES:
        samples = {phase: rows[name] for phase, rows in phases.items()}
        trends = _comparisons(samples)
        regressed = any(metric[f"{base}_to_phase5"]["verdict"] == "REGRESSED"
                        for metric in trends.values() for base in PHASES[:2])
        scenes[name] = {"phases": samples, "comparisons": trends, "regression_detected": regressed}
    regressed = any(row["regression_detected"] for row in scenes.values())
    backend_ready = not missing["phase5"] and all(
        row["backend"]["within_120hz_budget"] for row in phases["phase5"].values())
    return {
        "schema_version": 1, "status": "FAIL" if regressed else "PASS",
        **evidence_fields(), "source_evidence_kind": EVIDENCE_KIND, "target_budget_us": BUDGET_US,
        "inputs": metadata, "coverage_complete": complete, "all_four_scenes_present": complete,
        "missing_scenes": missing, "backend_within_120hz_budget": backend_ready,
        "regression_detected": regressed, "scenes": scenes,
    }


def _display(value) -> str:
    if value is None:
        return "—"
    if type(value) is float:
        return f"{value:.12g}"
    return str(value)


def markdown(report: dict) -> str:
    lines = [
        "# Adreno 840 Phase 5 performance comparison", "",
        "device_verified=false, hardware_acceptance=false. Physical intervals are unverified input samples.",
        "The 120 Hz budget is 8,333.333 µs. Threshold counts are cumulative.",
        "GPU backend includes encode, submit, GPU execution and wait. F07 covers upstream pre_create.",
        "Raw totals and threshold counts require equal sample counts for a verdict. Rates use measured samples.",
        "Frequency, stage shares and F07 attribution counts describe context.",
        "Phase 5 regression checks use both baselines.", "",
        f"- Four scenes in all phases: {report['all_four_scenes_present']}",
        f"- All Phase 5 backend samples within budget across the four scenes: "
        f"{report['backend_within_120hz_budget']}",
        f"- Phase 5 regression detected: {report['regression_detected']}",
    ]
    for name in fw.CORE_SCENES:
        scene = report["scenes"][name]
        lines.extend(["", f"## {name}", "",
                      "| Phase | Frames | Reported physical samples | Backend within budget |",
                      "| --- | ---: | ---: | --- |"])
        for phase in PHASES:
            data = scene["phases"][phase]
            values = (data["frame_count"], data["physical_intervals"]["sample_count"],
                      data["backend"]["within_120hz_budget"]) if data is not None else (None,) * 3
            lines.append(f"| {phase} | " + " | ".join(map(_display, values)) + " |")
        if scene["comparisons"]:
            lines += ["", "| Metric | Phase 0 | Phase 3 | Phase 5 | 0 → 3 | 3 → 5 | 0 → 5 |",
                      "| --- | ---: | ---: | ---: | --- | --- | --- |"]
            for key, metric in sorted(scene["comparisons"].items()):
                values = [_display(metric["values"][phase]) for phase in PHASES]
                for a, b in PAIRS:
                    change = metric[f"{a}_to_{b}"]
                    values.append(f"{change['verdict']} ({_display(change['delta'])})")
                lines.append(f"| {key} | " + " | ".join(values) + " |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for phase in PHASES:
        parser.add_argument(f"--{phase}", required=True, type=Path)
    parser.add_argument("--extra-metrics", type=Path)
    parser.add_argument("--allow-partial-scenes", action="store_true")
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    args = parser.parse_args(argv)
    try:
        inputs = [getattr(args, phase) for phase in PHASES]
        outputs = [checked_output_path(path) if path is not None else None
                   for path in (args.out_json, args.out_md)]
        targets = [path for path in outputs if path is not None]
        input_paths = inputs + ([args.extra_metrics] if args.extra_metrics else [])
        if len(set(targets)) != len(targets) or set(targets) & {path.resolve() for path in input_paths}:
            raise fw.ProfileError("output paths must be distinct from one another and every input")
        report = compare(*(load_json(path) for path in inputs),
                         extra_metrics=load_json(args.extra_metrics) if args.extra_metrics else None,
                         allow_partial_scenes=args.allow_partial_scenes)
        payload = json_text(report)
        if outputs[0] is not None:
            write_output(outputs[0], payload)
        if outputs[1] is not None:
            write_output(outputs[1], markdown(report))
    except (OSError, ValueError, RuntimeError, ArithmeticError) as exc:
        report = {"status": "FAIL", **evidence_fields(), "source_evidence_kind": EVIDENCE_KIND,
                  "coverage_complete": False, "all_four_scenes_present": False,
                  "backend_within_120hz_budget": False, "regression_detected": False, "error": str(exc)}
        payload = json_text(report)
    sys.stdout.write(payload)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
