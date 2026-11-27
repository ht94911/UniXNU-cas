#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wave 9 / Lane 2: Quantify real Mesa ir3 ROBUST_ON vs ROBUST_OFF A/B metrics.

Pure Python 3 standard library. Consumes real A/B compilation output JSON
produced by `test_robustness_and_profile --dump-json` (or invokes that binary
directly via --runner), enforces zero-regression and Apple XR10 safety
preservation gates, aggregates instruction reduction (total, median, max, P95),
register pressure, and SP wave occupancy by category
(compositor / liquid-glass / compute / camera / xr10), and writes deterministic
JSON + Markdown reports with device_verified=false and hardware_acceptance=false.
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

REQUIRED_CATEGORIES = ("compositor", "liquid-glass", "compute", "camera", "xr10")
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
    "const_dwords",
    "est_waves_per_sp",
    "robust_policy_flags",
)
POLICY_FLAG_PRESERVE_XR_OOB = 1 << 2
POLICY_FLAG_ELIDE_BUF_BOUNDS = 1 << 1
POLICY_FLAG_ZERO_PAGE_GUARD = 1 << 3


class QuantifyError(ValueError):
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


def _require_int(obj: dict[str, Any], key: str, ctx: str, *, allow_neg: bool = False) -> int:
    if key not in obj:
        raise QuantifyError(f"{ctx}: missing required field '{key}'")
    val = obj[key]
    if isinstance(val, bool) or not isinstance(val, int):
        raise QuantifyError(f"{ctx}: field '{key}' must be an integer")
    if not allow_neg and val < 0:
        raise QuantifyError(f"{ctx}: field '{key}' must be >= 0 (got {val})")
    return val


def _validate_stats_block(block: Any, ctx: str) -> dict[str, int]:
    if not isinstance(block, dict):
        raise QuantifyError(f"{ctx}: must be a JSON object")
    parsed: dict[str, int] = {}
    for key in REQUIRED_STATS_KEYS:
        parsed[key] = _require_int(block, key, ctx)
    if parsed["total_instrs"] == 0:
        raise QuantifyError(f"{ctx}: total_instrs must be > 0")
    if parsed["est_waves_per_sp"] == 0:
        raise QuantifyError(f"{ctx}: est_waves_per_sp must be > 0")
    return parsed


def validate_and_normalize_input(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise QuantifyError("top-level JSON must be an object")
    if raw.get("schema_version") != 1:
        raise QuantifyError(f"unsupported or missing schema_version: {raw.get('schema_version')!r}")
    shaders = raw.get("shaders")
    if not isinstance(shaders, list) or len(shaders) == 0:
        raise QuantifyError("'shaders' must be a non-empty list")

    seen_names: set[str] = set()
    normalized: list[dict[str, Any]] = []

    for idx, entry in enumerate(shaders):
        ctx = f"shaders[{idx}]"
        if not isinstance(entry, dict):
            raise QuantifyError(f"{ctx}: must be an object")
        name = entry.get("name")
        category = entry.get("category")
        stage = entry.get("stage")
        if not isinstance(name, str) or not name.strip():
            raise QuantifyError(f"{ctx}: 'name' must be a non-empty string")
        if name in seen_names:
            raise QuantifyError(f"{ctx}: duplicate shader name '{name}'")
        seen_names.add(name)
        if not isinstance(category, str) or not category.strip():
            raise QuantifyError(f"{ctx}: 'category' must be a non-empty string")
        if stage not in {"vertex", "fragment", "compute"}:
            raise QuantifyError(f"{ctx}: invalid stage {stage!r}")
        is_xr10 = entry.get("is_xr10", False)
        if not isinstance(is_xr10, bool):
            raise QuantifyError(f"{ctx}: 'is_xr10' must be boolean")

        on_s = _validate_stats_block(entry.get("robust_on"), f"{ctx}.robust_on")
        off_s = _validate_stats_block(entry.get("robust_off"), f"{ctx}.robust_off")

        saved_instrs = on_s["total_instrs"] - off_s["total_instrs"]
        reduction_pct = (100.0 * saved_instrs) / float(on_s["total_instrs"])
        delta_full = off_s["full_regs"] - on_s["full_regs"]
        delta_half = off_s["half_regs"] - on_s["half_regs"]
        delta_waves = off_s["est_waves_per_sp"] - on_s["est_waves_per_sp"]

        # XR10 preservation verification: both ON and OFF must preserve XR OOB
        # whenever the shader is marked is_xr10 or had PRESERVE_XR_OOB under ROBUST_ON.
        on_xr = (on_s["robust_policy_flags"] & POLICY_FLAG_PRESERVE_XR_OOB) != 0
        off_xr = (off_s["robust_policy_flags"] & POLICY_FLAG_PRESERVE_XR_OOB) != 0
        xr_preserved = (not is_xr10 or (on_xr and off_xr)) and ((not on_xr) or off_xr)

        # Also cross-check explicit diff.xr_preserved if present in input.
        raw_diff = entry.get("diff")
        if isinstance(raw_diff, dict) and "xr_preserved" in raw_diff:
            if not isinstance(raw_diff["xr_preserved"], bool):
                raise QuantifyError(f"{ctx}.diff.xr_preserved must be boolean")
            xr_preserved = xr_preserved and raw_diff["xr_preserved"]

        normalized.append({
            "name": name,
            "category": category,
            "source": str(entry.get("source", "unknown")),
            "stage": stage,
            "is_xr10": is_xr10,
            "robust_on": on_s,
            "robust_off": off_s,
            "saved_total_instrs": saved_instrs,
            "reduction_pct": round(reduction_pct, 4),
            "delta_cat0_ctrl": off_s["cat0_ctrl"] - on_s["cat0_ctrl"],
            "delta_cat1_mov": off_s["cat1_mov"] - on_s["cat1_mov"],
            "delta_cat234_alu": off_s["cat234_alu"] - on_s["cat234_alu"],
            "delta_cat5_tex": off_s["cat5_tex"] - on_s["cat5_tex"],
            "delta_cat6_mem": off_s["cat6_mem"] - on_s["cat6_mem"],
            "delta_cat7_sync": off_s["cat7_sync"] - on_s["cat7_sync"],
            "delta_full_regs": delta_full,
            "delta_half_regs": delta_half,
            "delta_est_waves_per_sp": delta_waves,
            "instr_regressed": saved_instrs < 0,
            "reg_regressed": delta_full > 0 or delta_half > 0,
            "wave_regressed": delta_waves < 0,
            "xr_preserved": xr_preserved,
            "zero_page_guarded": (off_s["robust_policy_flags"] & POLICY_FLAG_ZERO_PAGE_GUARD) != 0,
        })

    return {
        "schema_version": 1,
        "mesa_commit": str(raw.get("mesa_commit", "85fda3b36405bcaea8bffc64aa4f2651dc90770d")),
        "target_chip_id": str(raw.get("target_chip_id", "0x44050a31")),
        "zero_page_guard_bytes": int(raw.get("zero_page_guard_bytes", 65536)),
        "shaders": normalized,
    }


def _aggregate_group(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if not rows:
        return {
            "shader_count": 0,
            "total_instrs_on": 0,
            "total_instrs_off": 0,
            "total_saved_instrs": 0,
            "total_reduction_pct": 0.0,
            "median_reduction_pct": 0.0,
            "max_reduction_pct": 0.0,
            "p95_reduction_pct": 0.0,
            "total_delta_alu": 0,
            "total_delta_ctrl": 0,
            "total_delta_mem": 0,
            "max_delta_full_regs": 0,
            "max_delta_half_regs": 0,
            "min_delta_waves_per_sp": 0,
            "max_delta_waves_per_sp": 0,
            "avg_waves_on": 0.0,
            "avg_waves_off": 0.0,
            "xr_preserved_count": 0,
            "xr_preservation_rate_pct": 100.0,
            "improved_count": 0,
            "regressed_count": 0,
        }

    total_on = sum(r["robust_on"]["total_instrs"] for r in rows)
    total_off = sum(r["robust_off"]["total_instrs"] for r in rows)
    total_saved = total_on - total_off
    total_pct = (100.0 * total_saved / float(total_on)) if total_on > 0 else 0.0
    pcts = [float(r["reduction_pct"]) for r in rows]

    xr_ok = sum(1 for r in rows if r["xr_preserved"])
    improved = sum(1 for r in rows if r["saved_total_instrs"] > 0)
    regressed = sum(
        1 for r in rows if r["instr_regressed"] or r["reg_regressed"] or r["wave_regressed"]
    )

    return {
        "shader_count": len(rows),
        "total_instrs_on": total_on,
        "total_instrs_off": total_off,
        "total_saved_instrs": total_saved,
        "total_reduction_pct": round(total_pct, 4),
        "median_reduction_pct": round(float(statistics.median(pcts)), 4),
        "max_reduction_pct": round(float(max(pcts)), 4),
        "p95_reduction_pct": round(percentile(pcts, 95.0), 4),
        "total_delta_alu": sum(r["delta_cat234_alu"] for r in rows),
        "total_delta_ctrl": sum(r["delta_cat0_ctrl"] for r in rows),
        "total_delta_mem": sum(r["delta_cat6_mem"] for r in rows),
        "max_delta_full_regs": max(r["delta_full_regs"] for r in rows),
        "max_delta_half_regs": max(r["delta_half_regs"] for r in rows),
        "min_delta_waves_per_sp": min(r["delta_est_waves_per_sp"] for r in rows),
        "max_delta_waves_per_sp": max(r["delta_est_waves_per_sp"] for r in rows),
        "avg_waves_on": round(sum(r["robust_on"]["est_waves_per_sp"] for r in rows) / len(rows), 4),
        "avg_waves_off": round(sum(r["robust_off"]["est_waves_per_sp"] for r in rows) / len(rows), 4),
        "xr_preserved_count": xr_ok,
        "xr_preservation_rate_pct": round(100.0 * xr_ok / len(rows), 4),
        "improved_count": improved,
        "regressed_count": regressed,
    }


def quantify(raw: Any, *, enforce_gates: bool = True) -> dict[str, Any]:
    norm = validate_and_normalize_input(raw)
    shaders = norm["shaders"]

    violations: list[dict[str, Any]] = []
    for r in shaders:
        if r["instr_regressed"]:
            violations.append({
                "shader": r["name"],
                "rule": "ZERO_INSTR_REGRESSION",
                "detail": f"total_instrs increased from {r['robust_on']['total_instrs']} to {r['robust_off']['total_instrs']}",
            })
        if r["reg_regressed"]:
            violations.append({
                "shader": r["name"],
                "rule": "ZERO_REG_REGRESSION",
                "detail": f"full_regs delta={r['delta_full_regs']}, half_regs delta={r['delta_half_regs']}",
            })
        if r["wave_regressed"]:
            violations.append({
                "shader": r["name"],
                "rule": "ZERO_WAVE_REGRESSION",
                "detail": f"est_waves_per_sp delta={r['delta_est_waves_per_sp']}",
            })
        if not r["xr_preserved"]:
            violations.append({
                "shader": r["name"],
                "rule": "XR10_SAFETY_PRESERVED",
                "detail": "ROBUST_OFF dropped PRESERVE_XR_OOB protection",
            })

    categories_seen = sorted({r["category"] for r in shaders})
    ordered_cats = list(REQUIRED_CATEGORIES) + [
        c for c in categories_seen if c not in REQUIRED_CATEGORIES
    ]
    by_category: dict[str, Any] = {}
    for cat in ordered_cats:
        cat_rows = [r for r in shaders if r["category"] == cat]
        if cat_rows or cat in REQUIRED_CATEGORIES:
            by_category[cat] = _aggregate_group(cat_rows)

    overall = _aggregate_group(shaders)
    status = "PASS" if not violations else "FAIL"

    report: dict[str, Any] = {
        "schema_version": 1,
        "status": status,
        "mesa_commit": norm["mesa_commit"],
        "target_chip_id": norm["target_chip_id"],
        "zero_page_guard_bytes": norm["zero_page_guard_bytes"],
        "measurement_kind": "REAL_MESA_IR3_STATIC_AB",
        "device_verified": False,
        "hardware_acceptance": False,
        "overall": overall,
        "by_category": by_category,
        "violations": violations,
        "shaders": sorted(shaders, key=lambda r: (r["category"], r["name"])),
    }

    if enforce_gates and violations:
        raise QuantifyError(
            "Robustness quantification gate failed: "
            + "; ".join(f"{v['shader']}[{v['rule']}]: {v['detail']}" for v in violations)
        )
    return report


def render_markdown(report: dict[str, Any]) -> str:
    ov = report["overall"]
    lines = [
        "# Adreno 840 ir3 Robustness A/B Static Quantification Report",
        "",
        f"- **Status**: `{report['status']}`",
        f"- **Mesa Commit**: `{report['mesa_commit']}` (non-Vulkan ir3+fdl)",
        f"- **Target Chip**: `{report['target_chip_id']}` (Zero-Page Guard: `{report['zero_page_guard_bytes']}` bytes)",
        f"- **device_verified**: `{str(report['device_verified']).lower()}`",
        f"- **hardware_acceptance**: `{str(report['hardware_acceptance']).lower()}`",
        "",
        "## Overall Summary",
        "",
        "| Metric | Value |",
        "| --- | --- |",
        f"| Shaders Evaluated | {ov['shader_count']} (improved: {ov['improved_count']}, regressed: {ov['regressed_count']}) |",
        f"| Total Instructions (ROBUST_ON -> ROBUST_OFF) | {ov['total_instrs_on']} -> {ov['total_instrs_off']} (-{ov['total_saved_instrs']}, {ov['total_reduction_pct']:.2f}%) |",
        f"| Median / Max / P95 Reduction (%) | {ov['median_reduction_pct']:.2f}% / {ov['max_reduction_pct']:.2f}% / {ov['p95_reduction_pct']:.2f}% |",
        f"| ALU / Ctrl / Mem Delta | {ov['total_delta_alu']:+d} / {ov['total_delta_ctrl']:+d} / {ov['total_delta_mem']:+d} |",
        f"| Max Full / Half Reg Delta | {ov['max_delta_full_regs']:+d} / {ov['max_delta_half_regs']:+d} |",
        f"| Avg Est Waves/SP (ON -> OFF) | {ov['avg_waves_on']:.2f} -> {ov['avg_waves_off']:.2f} |",
        f"| XR10 Safety Preservation Rate | {ov['xr_preservation_rate_pct']:.2f}% ({ov['xr_preserved_count']}/{ov['shader_count']}) |",
        "",
        "## By Category",
        "",
        "| Category | Shaders | Instrs ON | Instrs OFF | Saved | Total % | Median % | Max % | P95 % | Max RegF Δ | Waves ON->OFF | XR10 Safe |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for cat, agg in report["by_category"].items():
        lines.append(
            f"| {cat} | {agg['shader_count']} | {agg['total_instrs_on']} | {agg['total_instrs_off']} "
            f"| {agg['total_saved_instrs']} | {agg['total_reduction_pct']:.2f}% | {agg['median_reduction_pct']:.2f}% "
            f"| {agg['max_reduction_pct']:.2f}% | {agg['p95_reduction_pct']:.2f}% | {agg['max_delta_full_regs']:+d} "
            f"| {agg['avg_waves_on']:.1f}->{agg['avg_waves_off']:.1f} | {agg['xr_preservation_rate_pct']:.1f}% |"
        )

    lines.extend([
        "",
        "## Per-Shader Detail",
        "",
        "| Shader | Category | Stage | ON | OFF | Saved (%) | ALU Δ | Mem Δ | FullReg ON->OFF | Waves ON->OFF | XR10 |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ])
    for s in report["shaders"]:
        on_s = s["robust_on"]
        off_s = s["robust_off"]
        lines.append(
            f"| `{s['name']}` | {s['category']} | {s['stage']} | {on_s['total_instrs']} | {off_s['total_instrs']} "
            f"| {s['saved_total_instrs']} ({s['reduction_pct']:.2f}%) | {s['delta_cat234_alu']:+d} | {s['delta_cat6_mem']:+d} "
            f"| {on_s['full_regs']}->{off_s['full_regs']} | {on_s['est_waves_per_sp']}->{off_s['est_waves_per_sp']} "
            f"| {'OK' if s['xr_preserved'] else 'FAIL'} |"
        )

    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    src_group = parser.add_mutually_exclusive_group(required=True)
    src_group.add_argument("--input", type=Path, help="Input A/B JSON produced by test_robustness_and_profile")
    src_group.add_argument("--runner", type=Path, help="Path to test_robustness_and_profile binary to invoke")
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
                raise QuantifyError("--dump-ab-json is required when using --runner")
            args.dump_ab_json.parent.mkdir(parents=True, exist_ok=True)
            proc = subprocess.run(
                [str(args.runner), "--dump-json", str(args.dump_ab_json)],
                capture_output=True, text=True, check=False, timeout=600,
            )
            if proc.returncode != 0:
                raise QuantifyError(f"runner exited with {proc.returncode}: {proc.stderr.strip()}")
            input_path = args.dump_ab_json
        else:
            input_path = args.input

        if not input_path.is_file():
            raise QuantifyError(f"input JSON does not exist: {input_path}")
        try:
            raw = json.loads(input_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise QuantifyError(f"invalid JSON in {input_path}: {exc}") from exc

        report = quantify(raw, enforce_gates=not args.no_fail_on_gate)
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        args.out_md.write_text(render_markdown(report), encoding="utf-8")

        ov = report["overall"]
        print(
            f"robustness_quantify: {report['status']} shaders={ov['shader_count']} "
            f"instrs={ov['total_instrs_on']}->{ov['total_instrs_off']} "
            f"(-{ov['total_saved_instrs']}, {ov['total_reduction_pct']:.2f}%, "
            f"max={ov['max_reduction_pct']:.2f}%, p95={ov['p95_reduction_pct']:.2f}%) "
            f"xr_safe={ov['xr_preservation_rate_pct']:.1f}% device_verified=false"
        )
        return 0 if report["status"] == "PASS" else 1
    except (QuantifyError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"robustness_quantify: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
