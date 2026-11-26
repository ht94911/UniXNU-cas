#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate complete HWTEST records against frozen predicates and Phase 2 policy.

Only eight required cases form a phase2 run. Each appears exactly once and
passes its CPU comparator. --allow-fallback-summary permits registered
fallback routes for debugging, while keeping all failure/coverage gates.
This parser verifies reported measurements; a CPU fixture is explicitly
marked cpu_reference_only and cannot establish real hardware acceptance.
Input is processed line by line, preserving truncated-record detection.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import csv
import json
import math
from pathlib import Path
import re

CASES = (
    "gpu.blit.buffer_copy", "gpu.blit.tex_clear_bgra8", "gpu.blit.tex_blit_scale",
    "gpu.compute.add_vec", "gpu.compute.nonuniform_grid", "gpu.render.solid_triangle",
    "gpu.render.indexed_quad_textured", "gpu.sync.fence_and_order",
)
TOLERANCES = dict(zip(CASES, (0.0, 0.0, 1.0, 1e-5, 0.0, 1.0, 1.0, 0.0)))
FIELDS = {"case", "status", "path", "max_err", "mismatched_px", "time_us", "note"}
TOKEN = re.compile(r"[A-Za-z0-9_.:-]+\Z")
NUMBER = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
INTEGER = re.compile(r"[0-9]+\Z")


def load_predicates(path: str | Path) -> set[str]:
    with Path(path).open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if not reader.fieldnames or reader.fieldnames[0] != "predicate" or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("invalid predicate TSV header")
        predicates = set()
        for line, row in enumerate(reader, 2):
            predicate = row.get("predicate", "")
            if not predicate.startswith("raw:") or not TOKEN.fullmatch(predicate) or predicate in predicates:
                raise ValueError(f"invalid or duplicate predicate at TSV line {line}")
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"malformed predicate TSV row {line}")
            predicates.add(predicate)
        if not predicates:
            raise ValueError("empty predicate vocabulary")
        return predicates


def parse_hwtest(lines, predicates: set[str], policy: str = "phase2", allow_fallback_summary: bool = False) -> dict:
    if policy != "phase2":
        raise ValueError(f"unknown HWTEST policy: {policy}")
    if isinstance(lines, str):
        lines = lines.splitlines(keepends=True)
    report = {"status": "PASS", "policy": policy, "allow_fallback_summary": allow_fallback_summary,
              "records": [], "errors": [], "missing_cases": [], "fallback_count": 0,
              "fallback_predicates": {}, "lines_read": 0, "ignored_lines": 0,
              "hardware_acceptance": False}
    seen = set()

    def error(line: int, code: str, detail: str) -> None:
        report["errors"].append({"line": line, "code": code, "detail": detail})

    for line_number, line in enumerate(lines, 1):
        report["lines_read"] = line_number
        if not isinstance(line, str):
            error(line_number, "ENCODING", "HWTEST input must be decoded text")
            continue
        marker = re.search(r"(?:^|\s)HWTEST(?:\s|$)", line)
        if not marker:
            report["ignored_lines"] += 1
            continue
        if not line.endswith("\n"):
            error(line_number, "TRUNCATED_RECORD", "HWTEST record lacks terminating newline")
            continue
        record_text = line[marker.start():].strip()
        if len(record_text) > 4096:
            error(line_number, "RECORD_TOO_LONG", "HWTEST record exceeds 4096 characters")
            continue
        tokens = record_text.split()
        if not tokens or tokens[0] != "HWTEST":
            error(line_number, "MALFORMED_RECORD", "invalid marker")
            continue
        values = {}
        invalid = False
        for token in tokens[1:]:
            if token.count("=") != 1:
                error(line_number, "MALFORMED_FIELD", token); invalid = True; break
            key, value = token.split("=", 1)
            if key not in FIELDS or key in values or not value:
                error(line_number, "UNKNOWN_OR_DUPLICATE_FIELD", token); invalid = True; break
            values[key] = value
        if invalid:
            continue
        if set(values) != FIELDS:
            error(line_number, "MISSING_FIELDS", ",".join(sorted(FIELDS - values.keys())))
            continue
        if values["case"] not in CASES:
            error(line_number, "UNKNOWN_CASE", values["case"]); continue
        if values["status"] not in {"PASS", "FAIL"} or not TOKEN.fullmatch(values["note"]):
            error(line_number, "INVALID_STATUS_OR_NOTE", values["status"] + ":" + values["note"]); continue
        if not NUMBER.fullmatch(values["max_err"]) or not INTEGER.fullmatch(values["mismatched_px"]) or \
                not INTEGER.fullmatch(values["time_us"]):
            error(line_number, "INVALID_METRICS", "nonfinite, signed, or malformed numeric fields"); continue
        maximum = float(values["max_err"])
        if not math.isfinite(maximum) or len(values["mismatched_px"]) > 20 or len(values["time_us"]) > 20:
            error(line_number, "INVALID_METRICS", "out-of-range numeric fields"); continue
        mismatches, elapsed = int(values["mismatched_px"]), int(values["time_us"])
        if mismatches > (1 << 64) - 1 or elapsed > (1 << 64) - 1:
            error(line_number, "INVALID_METRICS", "uint64 overflow"); continue
        route = values["path"]
        if route == "raw":
            predicate = None
        elif route.startswith("fallback:") and route[len("fallback:"):] in predicates:
            predicate = route[len("fallback:"):]
            report["fallback_count"] += 1
            report["fallback_predicates"][predicate] = report["fallback_predicates"].get(predicate, 0) + 1
        else:
            error(line_number, "INVALID_ROUTE_OR_PREDICATE", route); continue
        case = values["case"]
        if case in seen:
            error(line_number, "DUPLICATE_CASE", case)
        seen.add(case)
        record = {"case": case, "status": values["status"], "path": route,
                  "max_err": maximum, "mismatched_px": mismatches, "time_us": elapsed, "note": values["note"]}
        report["records"].append(record)
        if values["status"] != "PASS":
            error(line_number, "CASE_FAILED", case)
        if mismatches:
            error(line_number, "PIXEL_MISMATCH", f"{case}: {mismatches}")
        if maximum > TOLERANCES[case]:
            error(line_number, "ERROR_TOLERANCE_EXCEEDED", f"{case}: {maximum} > {TOLERANCES[case]}")
        if predicate is not None and not allow_fallback_summary:
            error(line_number, "FALLBACK_FORBIDDEN", predicate)
    report["missing_cases"] = sorted(set(CASES) - seen)
    for case in report["missing_cases"]:
        error(0, "MISSING_CASE", case)
    report["fallback_predicates"] = dict(sorted(report["fallback_predicates"].items()))
    if report["errors"]:
        report["status"] = "FAIL"
    report["phase2_exit_pass"] = report["status"] == "PASS" and report["fallback_count"] == 0
    # Notes are provenance claims, not an independent attestation of routing.
    report["measurement_kind"] = "CPU_REFERENCE_FIXTURE" if any(
        record["note"] == "cpu_reference_only" for record in report["records"]) else "REPORTED_HWTEST"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predicates", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path, help="HWTEST log path or - for stdin")
    parser.add_argument("--policy", choices=("phase2",), default="phase2")
    parser.add_argument("--allow-fallback-summary", action="store_true")
    args = parser.parse_args(argv)
    try:
        predicates = load_predicates(args.predicates)
        if str(args.input) == "-":
            report = parse_hwtest(sys.stdin, predicates, args.policy, args.allow_fallback_summary)
        else:
            with args.input.open(encoding="utf-8") as source:
                report = parse_hwtest(source, predicates, args.policy, args.allow_fallback_summary)
    except (OSError, ValueError) as exc:
        report = {"status": "FAIL", "errors": [{"code": "INPUT_ERROR", "detail": str(exc)}]}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
