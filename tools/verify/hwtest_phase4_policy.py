#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate ten Phase 4 or all 24 full-system HWTEST cases as host evidence.

The frozen Phase 2 grammar and Phase 3 vocabulary are reused in an isolated
namespace. No fallback, missing/duplicate case, private identity field, or
claim of hardware acceptance can pass. CPU fixtures may pass the host policy
but never establish device_verified or hardware_acceptance. Outputs and
temporary files are restricted to the Wave 8 Lane 4 cache.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from decimal import Decimal, InvalidOperation
import json
import os
from pathlib import Path
import re
import tempfile
from types import FunctionType

from check_contracts import GateError, cache_environment as base_environment
import fallback_audit
import hwtest_parser as phase2
import hwtest_phase3_policy as phase3

CACHE_ROOT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w08-l4")
PHASE4_BASE_CASES = (
    "gpu.heap.aliasing", "gpu.argbuf.bindless", "gpu.indirect.draw_dispatch",
    "gpu.icb.replay", "gpu.encoder.lifecycle_faults",
)
EXT_CASES = (
    "gpu.render.msaa4x_resolve", "gpu.render.visibility_query", "gpu.render.tess_amplify",
    "gpu.camera155.nv12_preview_photo", "gpu.mps.gemm_ci_blur",
)
CASES = PHASE4_BASE_CASES + EXT_CASES
FULL_SYSTEM_CASES = tuple(phase2.CASES) + tuple(phase3.CASES) + CASES
# GEMM checks relative error in the CPU comparator; this aggregate HWTEST
# maximum also includes BGRA8 blur pixels, whose channel tolerance is one.
TOLERANCES = dict(zip(CASES, (0, 1, 0, 0, 0, 1, 0, 1, 1, 1)))
TOLERANCES = {**phase2.TOLERANCES, **dict.fromkeys(phase3.CASES, 0), **TOLERANCES}
PRIVATE_FIELD = fallback_audit.PRIVATE_FIELD
HARDWARE_CLAIM = re.compile(
    r"\b(?:device_verified|hardware_acceptance)\b[\"']?\s*[:=]\s*[\"']?(?:true|1)(?![A-Za-z0-9_.])",
    re.IGNORECASE,
)
C_DRIVE = re.compile(r"(?:^|/)c:|(?:^|/)mnt/c(?:/|$)", re.IGNORECASE)


def evidence_fields() -> dict:
    return {"device_verified": False, "hardware_acceptance": False}


def vocabulary(policy: str) -> tuple[str, ...]:
    if policy not in {"phase4", "full_system"}:
        raise GateError("UNKNOWN_PHASE4_POLICY")
    return CASES if policy == "phase4" else FULL_SYSTEM_CASES


def failure_report(code: str, policy: str = "phase4") -> dict:
    required = vocabulary(policy)
    return {"status": "FAIL", "policy": policy, **evidence_fields(),
            "phase4_exit_pass": False, "full_system_exit_pass": False,
            "host_policy_passed": False, "required_cases": list(required),
            "required_case_count": len(required), "records": [], "fallback_count": 0,
            "errors": [{"code": code}], "missing_cases": list(required),
            "measurement_kind": "UNVERIFIED"}


def checked_output_path(path: str | Path, *, directory: bool = False) -> Path:
    raw = str(path).replace("\\", "/")
    if C_DRIVE.search(raw):
        raise GateError("C_DRIVE_OUTPUT_FORBIDDEN")
    try:
        source = Path(path).expanduser().absolute()
        resolved = source.resolve()
        if C_DRIVE.search(str(resolved)):
            raise GateError("C_DRIVE_OUTPUT_FORBIDDEN")
        if CACHE_ROOT.resolve() != CACHE_ROOT:
            raise GateError("CACHE_ROOT_REDIRECT")
        relative = resolved.relative_to(CACHE_ROOT)
        if not relative.parts and not directory:
            raise GateError("OUTPUT_MUST_BE_A_FILE")
        if any(part.is_symlink() for part in (source, *source.parents)):
            raise GateError("SYMLINK_OUTPUT_FORBIDDEN")
        if resolved.exists() and (resolved.is_dir() if not directory else not resolved.is_dir()):
            raise GateError("OUTPUT_KIND_INVALID")
    except (OSError, RuntimeError) as exc:
        raise GateError("OUTPUT_PATH_INVALID") from exc
    except ValueError as exc:
        if isinstance(exc, GateError):
            raise
        raise GateError("OUTPUT_OUTSIDE_W08_L4_CACHE") from exc
    return resolved


def cache_directory(path: str | Path) -> Path:
    target = checked_output_path(path, directory=True)
    target.mkdir(parents=True, exist_ok=True)
    return checked_output_path(target, directory=True)


def cache_environment(path: str | Path, base: dict | None = None) -> dict:
    return base_environment(cache_directory(path), base)


def write_json(path: str | Path, report: dict) -> Path:
    target = checked_output_path(path)
    cache_directory(target.parent)
    scratch = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".phase4-",
                                         dir=target.parent, delete=False) as stream:
            scratch = Path(stream.name)
            json.dump(report, stream, sort_keys=True, indent=2)
            stream.write("\n")
        os.replace(scratch, checked_output_path(target))
    finally:
        if scratch is not None and scratch.exists():
            scratch.unlink()
    return target


def parse_hwtest(lines, predicates: set[str], policy: str = "phase4", *,
                 device_verified: bool = False, hardware_acceptance: bool = False) -> dict:
    required = vocabulary(policy)
    if device_verified is not False or hardware_acceptance is not False:
        return failure_report("HARDWARE_CLAIM_FORBIDDEN", policy)
    if isinstance(lines, str):
        lines = lines.splitlines(keepends=True)
    checked, issues = [], []
    for number, line in enumerate(lines, 1):
        if isinstance(line, str):
            # Refuse private data before invoking a grammar that echoes bad tokens.
            if PRIVATE_FIELD.search(line):
                return failure_report("PRIVATE_FIELD_REDACTION_REQUIRED", policy)
            if HARDWARE_CLAIM.search(line):
                issues.append({"line": number, "code": "HARDWARE_CLAIM_FORBIDDEN"})
            if "fallback:" in line:
                issues.append({"line": number, "code": "FALLBACK_FORBIDDEN"})
            marker = re.search(r"(?:^|\s)HWTEST(?:\s|$)", line)
            if marker:
                fields = dict(word.split("=", 1) for word in line[marker.start():].split()[1:]
                              if word.count("=") == 1)
                maximum, case = fields.get("max_err", ""), fields.get("case", "")
                if len(maximum) > 64:
                    issues.append({"line": number, "code": "INVALID_METRICS"})
                elif case in required and phase2.NUMBER.fullmatch(maximum):
                    try:
                        if Decimal(maximum) > Decimal(str(TOLERANCES[case])):
                            issues.append({"line": number, "code": "ERROR_TOLERANCE_EXCEEDED"})
                    except InvalidOperation:
                        issues.append({"line": number, "code": "INVALID_METRICS"})
        checked.append(line)
    grammar = FunctionType(phase2.parse_hwtest.__code__,
                           dict(phase2.parse_hwtest.__globals__, CASES=required,
                                TOLERANCES={case: TOLERANCES[case] for case in required}),
                           "phase4_format_parser", phase2.parse_hwtest.__defaults__)
    report = grammar(checked, predicates, "phase2", False)
    notes = {record["note"] for record in report["records"]}
    if "cpu_reference_only" in notes and notes != {"cpu_reference_only"}:
        issues.append({"line": 0, "code": "MIXED_CPU_AND_REPORTED_EVIDENCE"})
    report["errors"] = [{"line": error.get("line", 0), "code": error["code"]}
                        for error in report["errors"]] + issues
    report.pop("phase2_exit_pass")
    passed = not report["errors"] and report["fallback_count"] == 0
    report.update(status="PASS" if passed else "FAIL", policy=policy,
                  required_cases=list(required), required_case_count=len(required),
                  host_policy_passed=passed, phase4_exit_pass=passed,
                  full_system_exit_pass=passed and policy == "full_system", **evidence_fields())
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--predicates", required=True, type=Path)
    cli.add_argument("--policy", choices=("phase4", "full_system"), default="phase4")
    cli.add_argument("--input", required=True, type=Path, help="HWTEST log, or - for stdin")
    cli.add_argument("--json-out", type=Path)
    args = cli.parse_args(argv)
    try:
        if args.json_out:
            output = checked_output_path(args.json_out)
            for path in (args.predicates, args.input):
                if str(path) != "-" and (output == path.resolve() or
                        (output.exists() and path.exists() and os.path.samefile(output, path))):
                    raise GateError("OUTPUT_ALIASES_INPUT")
        predicates = set(fallback_audit.load_predicates(args.predicates))
        if str(args.input) == "-":
            report = parse_hwtest(sys.stdin, predicates, args.policy)
        else:
            with args.input.open(encoding="utf-8") as stream:
                report = parse_hwtest(stream, predicates, args.policy)
        if args.json_out:
            write_json(args.json_out, report)
    except (OSError, ValueError, RuntimeError) as exc:
        code = str(exc) if isinstance(exc, (GateError, fallback_audit.AuditError)) else "INPUT_ERROR"
        report = failure_report(code, args.policy)
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0 if report["host_policy_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
