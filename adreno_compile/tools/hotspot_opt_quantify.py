#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wave 10 / Lane 2: Quantify real Mesa ir3 Metal hotspot optimization A/B metrics.

Pure Python 3 standard library. Consumes real A/B compilation output JSON
produced by `test_metal_hotspot_opt --dump-json` (or invokes that binary
directly via --runner), enforces zero-regression (instructions, full/effective
registers, spills, SP waves), push-constant layout invariance, and 100% Apple
XR10 safety preservation gates, aggregates metrics by optimization type
(`opt_group`) and workload scenario (`category`), and writes deterministic
JSON + Markdown reports with device_verified=false and
hardware_acceptance=false.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
from typing import Any

REQUIRED_STATS_KEYS = (
    "total_instrs",
    "cat0_ctrl",
    "cat1_mov",
    "cat234_alu",
    "cat5_tex",
    "cat6_mem",
    "cat7_sync",
    "nop_count",
    "full_regs",
    "half_regs",
    "reg_spills",
    "reg_fills",
    "const_dwords",
    "est_waves_per_sp",
    "robust_policy_flags",
)
POLICY_FLAG_PRESERVE_XR_OOB = 1 << 2
VALID_STAGES = frozenset({"vertex", "fragment", "compute"})


class HotspotQuantifyError(ValueError):
    """Raised when input schema, safety invariants, or regression gates fail."""


def percentile(values: list[float], pct: float) -> float:
    """Deterministic linear-interpolated percentile (0..100)."""
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return float(ordered[lo])
    frac = rank - lo
    return float(ordered[lo] * (1.0 - frac) + ordered[hi] * frac)


def effective_reg_vec4(stats: dict[str, Any]) -> int:
    """Kaanapali A840 register-file footprint in full vec4 equivalents."""
    full_v = int(stats["full_regs"])
    half_v = int(stats["half_regs"])
    half_as_full = (half_v + 1) // 2
    merged = bool(stats.get("merged_regs", True))
    return max(full_v, half_as_full) if merged else (full_v + half_as_full)


def _require_int(obj: dict[str, Any], key: str, ctx: str, *, allow_neg: bool = False) -> int:
    if key not in obj:
        raise HotspotQuantifyError(f"{ctx}: missing required field '{key}'")
    val = obj[key]
    if isinstance(val, bool) or not isinstance(val, int):
        raise HotspotQuantifyError(f"{ctx}: field '{key}' must be an integer")
    if not allow_neg and val < 0:
        raise HotspotQuantifyError(f"{ctx}: field '{key}' must be >= 0 (got {val})")
    return val


def _require_bool(obj: dict[str, Any], key: str, ctx: str, *, default: bool | None = None) -> bool:
    if key not in obj:
        if default is not None:
            return default
        raise HotspotQuantifyError(f"{ctx}: missing required boolean field '{key}'")
    val = obj[key]
    if not isinstance(val, bool):
        raise HotspotQuantifyError(f"{ctx}: field '{key}' must be a boolean")
    return val


def _validate_stats_block(block: Any, ctx: str) -> dict[str, Any]:
    if not isinstance(block, dict):
        raise HotspotQuantifyError(f"{ctx}: must be a JSON object")
    parsed: dict[str, Any] = {}
    for key in REQUIRED_STATS_KEYS:
        parsed[key] = _require_int(block, key, ctx)
    if parsed["total_instrs"] == 0:
        raise HotspotQuantifyError(f"{ctx}: total_instrs must be > 0")
    if parsed["est_waves_per_sp"] == 0:
        raise HotspotQuantifyError(f"{ctx}: est_waves_per_sp must be > 0")
    parsed["merged_regs"] = _require_bool(block, "merged_regs", ctx, default=True)
    parsed["uses_fbfetch"] = _require_bool(block, "uses_fbfetch", ctx, default=False)
    parsed["early_preamble"] = _require_bool(block, "early_preamble", ctx, default=False)
    parsed["effective_vec4"] = effective_reg_vec4(parsed)
    return parsed


def _validate_opt_report(block: Any, ctx: str) -> dict[str, Any]:
    if block is None:
        return {
            "fp16_roundtrip_elided": 0,
            "fp16_alu_narrowed": 0,
            "fp16_rtz_aligned": 0,
            "fp16_xr10_guarded": 0,
            "push_const_loads_merged": 0,
            "push_const_offsets_folded": 0,
            "uniform_widens_elided": 0,
            "sink_moves_applied": 0,
            "peak_live_vec4_before": 0,
            "peak_live_vec4_after": 0,
            "xr10_detected": False,
            "xr10_safe": True,
            "push_layout_preserved": True,
        }
    if not isinstance(block, dict):
        raise HotspotQuantifyError(f"{ctx}: must be a JSON object")
    int_keys = (
        "fp16_roundtrip_elided",
        "fp16_alu_narrowed",
        "fp16_rtz_aligned",
        "fp16_xr10_guarded",
        "push_const_loads_merged",
        "push_const_offsets_folded",
        "uniform_widens_elided",
        "sink_moves_applied",
        "peak_live_vec4_before",
        "peak_live_vec4_after",
    )
    parsed: dict[str, Any] = {}
    for k in int_keys:
        parsed[k] = _require_int(block, k, ctx) if k in block else 0
    parsed["xr10_detected"] = _require_bool(block, "xr10_detected", ctx, default=False)
    parsed["xr10_safe"] = _require_bool(block, "xr10_safe", ctx, default=True)
    parsed["push_layout_preserved"] = _require_bool(
        block, "push_layout_preserved", ctx, default=True
    )
    return parsed


def validate_and_normalize_input(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise HotspotQuantifyError("top-level JSON must be an object")
    if raw.get("schema_version") != 1:
        raise HotspotQuantifyError(
            f"unsupported or missing schema_version: {raw.get('schema_version')!r}"
        )
    shaders = raw.get("shaders")
    if not isinstance(shaders, list) or len(shaders) == 0:
        raise HotspotQuantifyError("'shaders' must be a non-empty list")

    seen_names: set[str] = set()
    normalized: list[dict[str, Any]] = []

    for idx, entry in enumerate(shaders):
        ctx = f"shaders[{idx}]"
        if not isinstance(entry, dict):
            raise HotspotQuantifyError(f"{ctx}: must be an object")
        name = entry.get("name")
        category = entry.get("category")
        opt_group = entry.get("opt_group")
        stage = entry.get("stage")
        if not isinstance(name, str) or not name.strip():
            raise HotspotQuantifyError(f"{ctx}: 'name' must be a non-empty string")
        if name in seen_names:
            raise HotspotQuantifyError(f"{ctx}: duplicate shader name '{name}'")
        seen_names.add(name)
        if not isinstance(category, str) or not category.strip():
            raise HotspotQuantifyError(f"{ctx}: 'category' must be a non-empty string")
        if not isinstance(opt_group, str) or not opt_group.strip():
            raise HotspotQuantifyError(f"{ctx}: 'opt_group' must be a non-empty string")
        if stage not in VALID_STAGES:
            raise HotspotQuantifyError(f"{ctx}: invalid stage {stage!r}")
        is_xr10 = _require_bool(entry, "is_xr10", ctx, default=False)
        expects_improvement = _require_bool(entry, "expects_improvement", ctx, default=False)

        off_s = _validate_stats_block(entry.get("opt_off"), f"{ctx}.opt_off")
        on_s = _validate_stats_block(entry.get("opt_on"), f"{ctx}.opt_on")
        opt_rep = _validate_opt_report(entry.get("opt_report"), f"{ctx}.opt_report")

        saved_instrs = off_s["total_instrs"] - on_s["total_instrs"]
        reduction_pct = (100.0 * saved_instrs) / float(off_s["total_instrs"])
        delta_cat1 = on_s["cat1_mov"] - off_s["cat1_mov"]
        delta_alu = on_s["cat234_alu"] - off_s["cat234_alu"]
        delta_tex = on_s["cat5_tex"] - off_s["cat5_tex"]
        delta_mem = on_s["cat6_mem"] - off_s["cat6_mem"]
        delta_full = on_s["full_regs"] - off_s["full_regs"]
        delta_half = on_s["half_regs"] - off_s["half_regs"]
        delta_eff = on_s["effective_vec4"] - off_s["effective_vec4"]
        delta_spills = on_s["reg_spills"] - off_s["reg_spills"]
        delta_const = on_s["const_dwords"] - off_s["const_dwords"]
        delta_waves = on_s["est_waves_per_sp"] - off_s["est_waves_per_sp"]

        off_xr = (off_s["robust_policy_flags"] & POLICY_FLAG_PRESERVE_XR_OOB) != 0
        on_xr = (on_s["robust_policy_flags"] & POLICY_FLAG_PRESERVE_XR_OOB) != 0
        xr_preserved = (
            opt_rep["xr10_safe"]
            and ((not off_xr) or on_xr)
            and ((not is_xr10) or (off_xr and on_xr and opt_rep["fp16_alu_narrowed"] == 0))
        )

        # Register regression: full_regs increased OR effective register-file
        # vec4 footprint increased. Note that FP16 narrowing legitimately trades
        # full_regs down for half_regs up while keeping effective_vec4 <= before.
        reg_regressed = (delta_full > 0) or (delta_eff > 0)
        instr_regressed = saved_instrs < 0
        spill_regressed = delta_spills > 0
        wave_regressed = delta_waves < 0
        push_layout_ok = bool(opt_rep["push_layout_preserved"])

        improved = (
            saved_instrs > 0
            or delta_cat1 < 0
            or delta_full < 0
            or delta_spills < 0
            or delta_waves > 0
            or delta_const < 0
        )

        normalized.append({
            "name": name,
            "category": category,
            "opt_group": opt_group,
            "source": str(entry.get("source", "unknown")),
            "stage": stage,
            "is_xr10": is_xr10,
            "expects_improvement": expects_improvement,
            "opt_off": off_s,
            "opt_on": on_s,
            "opt_report": opt_rep,
            "saved_total_instrs": saved_instrs,
            "reduction_pct": round(reduction_pct, 4),
            "delta_cat1_mov": delta_cat1,
            "delta_cat234_alu": delta_alu,
            "delta_cat5_tex": delta_tex,
            "delta_cat6_mem": delta_mem,
            "delta_full_regs": delta_full,
            "delta_half_regs": delta_half,
            "delta_effective_vec4": delta_eff,
            "delta_reg_spills": delta_spills,
            "delta_const_dwords": delta_const,
            "delta_est_waves_per_sp": delta_waves,
            "improved": improved,
            "instr_regressed": instr_regressed,
            "reg_regressed": reg_regressed,
            "spill_regressed": spill_regressed,
            "wave_regressed": wave_regressed,
            "xr_preserved": xr_preserved,
            "push_layout_preserved": push_layout_ok,
        })

    return {
        "mesa_commit": str(raw.get("mesa_commit", "85fda3b36405bcaea8bffc64aa4f2651dc90770d")),
        "target_chip_id": str(raw.get("target_chip_id", "0x44050a31")),
        "shaders": normalized,
    }


def _aggregate_group(items: list[dict[str, Any]]) -> dict[str, Any]:
    if not items:
        return {
            "shader_count": 0,
            "improved_count": 0,
            "regressed_count": 0,
            "total_instrs_off": 0,
            "total_instrs_on": 0,
            "total_saved_instrs": 0,
            "total_reduction_pct": 0.0,
            "median_reduction_pct": 0.0,
            "max_reduction_pct": 0.0,
            "p95_reduction_pct": 0.0,
            "total_delta_cat1_mov": 0,
            "total_delta_cat234_alu": 0,
            "total_delta_cat6_mem": 0,
            "total_delta_full_regs": 0,
            "max_delta_full_regs": 0,
            "min_delta_full_regs": 0,
            "total_delta_reg_spills": 0,
            "total_delta_const_dwords": 0,
            "avg_waves_off": 0.0,
            "avg_waves_on": 0.0,
            "max_delta_waves": 0,
            "min_delta_waves": 0,
            "xr_preserved_count": 0,
            "xr_preservation_rate_pct": 100.0,
        }

    total_off = sum(s["opt_off"]["total_instrs"] for s in items)
    total_on = sum(s["opt_on"]["total_instrs"] for s in items)
    total_saved = total_off - total_on
    total_red_pct = (100.0 * total_saved / float(total_off)) if total_off > 0 else 0.0
    red_pcts = [float(s["reduction_pct"]) for s in items]
    improved = sum(1 for s in items if s["improved"])
    regressed = sum(
        1 for s in items
        if s["instr_regressed"] or s["reg_regressed"] or s["spill_regressed"] or s["wave_regressed"]
    )
    xr_ok = sum(1 for s in items if s["xr_preserved"])

    return {
        "shader_count": len(items),
        "improved_count": improved,
        "regressed_count": regressed,
        "total_instrs_off": total_off,
        "total_instrs_on": total_on,
        "total_saved_instrs": total_saved,
        "total_reduction_pct": round(total_red_pct, 4),
        "median_reduction_pct": round(float(statistics.median(red_pcts)), 4),
        "max_reduction_pct": round(max(red_pcts), 4),
        "p95_reduction_pct": round(percentile(red_pcts, 95.0), 4),
        "total_delta_cat1_mov": sum(s["delta_cat1_mov"] for s in items),
        "total_delta_cat234_alu": sum(s["delta_cat234_alu"] for s in items),
        "total_delta_cat6_mem": sum(s["delta_cat6_mem"] for s in items),
        "total_delta_full_regs": sum(s["delta_full_regs"] for s in items),
        "max_delta_full_regs": max(s["delta_full_regs"] for s in items),
        "min_delta_full_regs": min(s["delta_full_regs"] for s in items),
        "total_delta_reg_spills": sum(s["delta_reg_spills"] for s in items),
        "total_delta_const_dwords": sum(s["delta_const_dwords"] for s in items),
        "avg_waves_off": round(
            sum(s["opt_off"]["est_waves_per_sp"] for s in items) / float(len(items)), 2
        ),
        "avg_waves_on": round(
            sum(s["opt_on"]["est_waves_per_sp"] for s in items) / float(len(items)), 2
        ),
        "max_delta_waves": max(s["delta_est_waves_per_sp"] for s in items),
        "min_delta_waves": min(s["delta_est_waves_per_sp"] for s in items),
        "xr_preserved_count": xr_ok,
        "xr_preservation_rate_pct": round((100.0 * xr_ok) / float(len(items)), 2),
    }


def quantify(raw: Any, *, enforce_gates: bool = True) -> dict[str, Any]:
    norm = validate_and_normalize_input(raw)
    shaders = norm["shaders"]

    violations: list[str] = []
    for s in shaders:
        name = s["name"]
        if s["instr_regressed"]:
            violations.append(
                f"{name}: instruction regression "
                f"({s['opt_off']['total_instrs']} -> {s['opt_on']['total_instrs']})"
            )
        if s["reg_regressed"]:
            violations.append(
                f"{name}: register pressure regression "
                f"(full {s['opt_off']['full_regs']}->{s['opt_on']['full_regs']}, "
                f"eff_vec4 {s['opt_off']['effective_vec4']}->{s['opt_on']['effective_vec4']})"
            )
        if s["spill_regressed"]:
            violations.append(
                f"{name}: register spill regression "
                f"({s['opt_off']['reg_spills']} -> {s['opt_on']['reg_spills']})"
            )
        if s["wave_regressed"]:
            violations.append(
                f"{name}: SP wave occupancy regression "
                f"({s['opt_off']['est_waves_per_sp']} -> {s['opt_on']['est_waves_per_sp']})"
            )
        if not s["xr_preserved"]:
            violations.append(f"{name}: Apple XR10 safety boundary violated")
        if not s["push_layout_preserved"]:
            violations.append(f"{name}: push-constant layout contract violated")
        if s["expects_improvement"] and not s["improved"]:
            violations.append(f"{name}: targeted hotspot shader showed zero measurable improvement")

    by_opt_group: dict[str, Any] = {}
    for grp in sorted({s["opt_group"] for s in shaders}):
        by_opt_group[grp] = _aggregate_group([s for s in shaders if s["opt_group"] == grp])

    by_category: dict[str, Any] = {}
    for cat in sorted({s["category"] for s in shaders}):
        by_category[cat] = _aggregate_group([s for s in shaders if s["category"] == cat])

    overall = _aggregate_group(shaders)
    if overall["improved_count"] == 0:
        violations.append("no shader demonstrated measurable optimization gain")

    status = "PASS" if not violations else "FAIL"
    report = {
        "schema_version": 1,
        "wave": "W10-L2",
        "status": status,
        "mesa_commit": norm["mesa_commit"],
        "target_chip_id": norm["target_chip_id"],
        "device_verified": False,
        "hardware_acceptance": False,
        "overall": overall,
        "by_opt_group": by_opt_group,
        "by_category": by_category,
        "violations": violations,
        "shaders": sorted(shaders, key=lambda x: (x["opt_group"], x["category"], x["name"])),
    }

    if enforce_gates and violations:
        raise HotspotQuantifyError("; ".join(violations))
    return report


def render_markdown(report: dict[str, Any]) -> str:
    ov = report["overall"]
    lines = [
        "# Adreno 840 ir3 Metal Hotspot Optimization A/B Quantification Report (W10-L2)",
        "",
        f"- **Status**: `{report['status']}`",
        f"- **Mesa Commit**: `{report['mesa_commit']}` (non-Vulkan ir3+fdl)",
        f"- **Target Chip**: `{report['target_chip_id']}`",
        "- **device_verified**: `false`",
        "- **hardware_acceptance**: `false`",
        "",
        "## Overall Summary",
        "",
        "| Metric | Value |",
        "| --- | --- |",
        f"| Shaders Evaluated | {ov['shader_count']} (improved: {ov['improved_count']}, regressed: {ov['regressed_count']}) |",
        f"| Total Instructions (OPT_OFF -> OPT_ON) | {ov['total_instrs_off']} -> {ov['total_instrs_on']} ({-ov['total_saved_instrs']:+d}, {ov['total_reduction_pct']:.2f}%) |",
        f"| Median / Max / P95 Reduction (%) | {ov['median_reduction_pct']:.2f}% / {ov['max_reduction_pct']:.2f}% / {ov['p95_reduction_pct']:.2f}% |",
        f"| Cat1 Mov / ALU / Mem Delta | {ov['total_delta_cat1_mov']:+d} / {ov['total_delta_cat234_alu']:+d} / {ov['total_delta_cat6_mem']:+d} |",
        f"| FullReg Total / Min / Max Delta | {ov['total_delta_full_regs']:+d} / {ov['min_delta_full_regs']:+d} / {ov['max_delta_full_regs']:+d} |",
        f"| Reg Spills / Const Dwords Delta | {ov['total_delta_reg_spills']:+d} / {ov['total_delta_const_dwords']:+d} |",
        f"| Avg Est Waves/SP (OFF -> ON) | {ov['avg_waves_off']:.2f} -> {ov['avg_waves_on']:.2f} (max Δ {ov['max_delta_waves']:+d}) |",
        f"| XR10 Safety Preservation Rate | {ov['xr_preservation_rate_pct']:.2f}% ({ov['xr_preserved_count']}/{ov['shader_count']}) |",
        "",
        "## By Optimization Group",
        "",
        "| Opt Group | Shaders | Instrs OFF | Instrs ON | Saved | Total % | Cat1 Δ | FullReg Δ | Spills Δ | Waves OFF->ON | XR10 Safe |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for grp, agg in report["by_opt_group"].items():
        lines.append(
            f"| {grp} | {agg['shader_count']} | {agg['total_instrs_off']} | {agg['total_instrs_on']} "
            f"| {agg['total_saved_instrs']} | {agg['total_reduction_pct']:.2f}% | {agg['total_delta_cat1_mov']:+d} "
            f"| {agg['total_delta_full_regs']:+d} | {agg['total_delta_reg_spills']:+d} "
            f"| {agg['avg_waves_off']:.1f}->{agg['avg_waves_on']:.1f} | {agg['xr_preservation_rate_pct']:.1f}% |"
        )

    lines.extend([
        "",
        "## By Workload Category",
        "",
        "| Category | Shaders | Instrs OFF | Instrs ON | Saved | Total % | Median % | Max % | FullReg Δ | Waves OFF->ON | XR10 Safe |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    for cat, agg in report["by_category"].items():
        lines.append(
            f"| {cat} | {agg['shader_count']} | {agg['total_instrs_off']} | {agg['total_instrs_on']} "
            f"| {agg['total_saved_instrs']} | {agg['total_reduction_pct']:.2f}% | {agg['median_reduction_pct']:.2f}% "
            f"| {agg['max_reduction_pct']:.2f}% | {agg['total_delta_full_regs']:+d} "
            f"| {agg['avg_waves_off']:.1f}->{agg['avg_waves_on']:.1f} | {agg['xr_preservation_rate_pct']:.1f}% |"
        )

    lines.extend([
        "",
        "## Per-Shader Detail",
        "",
        "| Shader | Opt Group | Category | Stage | OFF | ON | Saved (%) | Cat1 Δ | ALU Δ | FullReg | HalfReg | Spills | ConstDW | Waves | XR10 |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ])
    for s in report["shaders"]:
        off_s = s["opt_off"]
        on_s = s["opt_on"]
        lines.append(
            f"| `{s['name']}` | {s['opt_group']} | {s['category']} | {s['stage']} "
            f"| {off_s['total_instrs']} | {on_s['total_instrs']} | {s['saved_total_instrs']} ({s['reduction_pct']:.2f}%) "
            f"| {s['delta_cat1_mov']:+d} | {s['delta_cat234_alu']:+d} "
            f"| {off_s['full_regs']}->{on_s['full_regs']} | {off_s['half_regs']}->{on_s['half_regs']} "
            f"| {off_s['reg_spills']}->{on_s['reg_spills']} | {off_s['const_dwords']}->{on_s['const_dwords']} "
            f"| {off_s['est_waves_per_sp']}->{on_s['est_waves_per_sp']} "
            f"| {'OK' if s['xr_preserved'] else 'FAIL'} |"
        )

    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    src_group = parser.add_mutually_exclusive_group(required=True)
    src_group.add_argument("--input", type=Path, help="Input A/B JSON from test_metal_hotspot_opt")
    src_group.add_argument("--runner", type=Path, help="Path to test_metal_hotspot_opt binary")
    parser.add_argument("--dump-ab-json", type=Path, default=None,
                        help="Where --runner writes raw A/B JSON (required with --runner)")
    parser.add_argument("--out-json", type=Path, required=True, help="Output quantified report JSON")
    parser.add_argument("--out-md", type=Path, required=True, help="Output quantified report Markdown")
    parser.add_argument("--no-fail-on-gate", action="store_true",
                        help="Write report even if violations exist (still exits non-zero)")
    args = parser.parse_args(argv)

    try:
        if args.runner is not None:
            if args.dump_ab_json is None:
                raise HotspotQuantifyError("--dump-ab-json is required when using --runner")
            args.dump_ab_json.parent.mkdir(parents=True, exist_ok=True)
            proc = subprocess.run(
                [str(args.runner), "--dump-json", str(args.dump_ab_json)],
                capture_output=True, text=True, check=False, timeout=600,
            )
            if proc.returncode != 0:
                raise HotspotQuantifyError(
                    f"runner exited with {proc.returncode}: {proc.stderr.strip()}"
                )
            input_path = args.dump_ab_json
        else:
            input_path = args.input

        if not input_path.is_file():
            raise HotspotQuantifyError(f"input JSON does not exist: {input_path}")
        try:
            raw = json.loads(input_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise HotspotQuantifyError(f"invalid JSON in {input_path}: {exc}") from exc

        report = quantify(raw, enforce_gates=not args.no_fail_on_gate)
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        args.out_md.write_text(render_markdown(report), encoding="utf-8")

        ov = report["overall"]
        print(
            f"hotspot_opt_quantify: {report['status']} shaders={ov['shader_count']} "
            f"instrs={ov['total_instrs_off']}->{ov['total_instrs_on']} "
            f"({-ov['total_saved_instrs']:+d}, {ov['total_reduction_pct']:.2f}%, "
            f"cat1={ov['total_delta_cat1_mov']:+d}, full_regs={ov['total_delta_full_regs']:+d}, "
            f"waves={ov['avg_waves_off']:.2f}->{ov['avg_waves_on']:.2f}) "
            f"xr_safe={ov['xr_preservation_rate_pct']:.1f}% device_verified=false"
        )
        return 0 if report["status"] == "PASS" else 1
    except (HotspotQuantifyError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"hotspot_opt_quantify: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
