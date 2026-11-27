#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Require six exact Phase 3 HWTEST cases, optionally all eight Phase 2 cases.

Reuse the frozen HWTEST grammar in an isolated function namespace containing
the Phase 3 case/tolerance tables. The Phase 2 module is never mutated, so
both policies can run concurrently. All routes must be raw; native MDSS
linear resolve is valid, while a driver fallback remains forbidden.
Reported host fixtures never certify hardware. Optional JSON output uses
the existing general .cache/<subdir> guard; no device or network actions.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
from types import FunctionType

import hwtest_parser as phase2
from check_contracts import GateError, checked_cache_path, write_json

CASES = (
    "gpu.gmem.sysmem_equivalence", "gpu.gmem.memoryless_depth",
    "gpu.gmem.tile_boundary_1200x2608", "gpu.gmem.programmable_blend",
    "gpu.ubwc.roundtrip_bgra8", "gpu.ubwc.scanout_compat_fallback",
)
NATIVE_FEATURE_PREDICATES = {
    "raw:render.gmem_required", "raw:render.ubwc_attachment", "raw:render.fbfetch_blend",
}


def parse_hwtest(lines, predicates: set[str], policy: str = "phase3", include_phase2: bool = False) -> dict:
    if policy != "phase3":
        raise ValueError(f"unknown Phase 3 policy: {policy}")
    vocabulary = tuple(phase2.CASES) + CASES if include_phase2 else CASES
    tolerances = dict(phase2.TOLERANCES) if include_phase2 else {}
    tolerances.update(dict.fromkeys(CASES, 0.0))
    grammar = FunctionType(phase2.parse_hwtest.__code__,
                           dict(phase2.parse_hwtest.__globals__, CASES=vocabulary, TOLERANCES=tolerances),
                           "phase3_format_parser", phase2.parse_hwtest.__defaults__)
    exact_errors = []
    if isinstance(lines, str):
        lines = lines.splitlines(keepends=True)

    def checked_lines():
        for number, line in enumerate(lines, 1):
            if isinstance(line, str):
                marker = re.search(r"(?:^|\s)HWTEST(?:\s|$)", line)
                if marker and line.endswith("\n") and len(line[marker.start():].strip()) <= 4096:
                    pairs = [word.split("=", 1) for word in line[marker.start():].split()[1:] if word.count("=") == 1]
                    values = dict(pairs)
                    maximum = values.get("max_err", "")
                    # A nonzero decimal mantissa stays nonzero even when float()
                    # underflows. Keep the inherited grammar/metrics unchanged.
                    if values.get("case") in CASES and phase2.NUMBER.fullmatch(maximum) and \
                            maximum.lower().split("e", 1)[0].replace(".", "").strip("0") and float(maximum) == 0:
                        exact_errors.append({"line": number, "code": "EXACT_ZERO_REQUIRED",
                                             "detail": values["case"] + ":" + maximum})
            yield line

    report = grammar(checked_lines(), predicates, policy="phase2", allow_fallback_summary=False)
    report["errors"].extend(exact_errors)
    for record in report["records"]:
        if record["case"] in CASES and record["path"].removeprefix("fallback:") in NATIVE_FEATURE_PREDICATES:
            report["errors"].append({"line": 0, "code": "PHASE3_NATIVE_FEATURE_FALLBACK",
                                     "detail": record["case"] + ":" + record["path"]})
    report["status"] = "FAIL" if report["errors"] else "PASS"
    report.pop("phase2_exit_pass")
    report.update(policy=policy, include_phase2=include_phase2, required_cases=list(vocabulary),
                  required_case_count=len(vocabulary), hardware_acceptance=False, device_verified=False,
                  phase3_exit_pass=report["status"] == "PASS" and report["fallback_count"] == 0)
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--predicates", required=True, type=Path)
    cli.add_argument("--policy", choices=("phase3",), default="phase3")
    cli.add_argument("--include-phase2", action="store_true")
    cli.add_argument("--input", required=True, type=Path, help="HWTEST log or - for stdin")
    cli.add_argument("--json-out", type=Path)
    args = cli.parse_args(argv)
    try:
        if args.json_out:
            checked_cache_path(args.json_out)
        predicates = phase2.load_predicates(args.predicates)
        if str(args.input) == "-":
            report = parse_hwtest(sys.stdin, predicates, args.policy, args.include_phase2)
        else:
            with args.input.open(encoding="utf-8") as source:
                report = parse_hwtest(source, predicates, args.policy, args.include_phase2)
        if args.json_out:
            write_json(args.json_out, report)
    except (OSError, ValueError, GateError) as exc:
        report = {"status": "FAIL", "policy": "phase3", "phase3_exit_pass": False,
                  "device_verified": False, "hardware_acceptance": False,
                  "errors": [{"code": "INPUT_ERROR", "detail": str(exc)}]}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
