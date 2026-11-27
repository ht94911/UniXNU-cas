#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit reported process/scene routes using all 16 frozen TSV predicates.

ROUTE count units and HWTEST samples are counted separately as well as in
the combined reported-route totals. Logs cannot attest physical submissions,
screen timing or device acceptance. Unknown predicates and malformed records
remain fatal in --allow-fallbacks report mode. Missing target-process coverage
cannot pass the zero-fallback gate. All output and temporary files use the
Wave 7 Lane 4 cache; input files are read only.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from collections import Counter, defaultdict
import csv
from dataclasses import dataclass
from decimal import Decimal
import hashlib
import io
import json
import math
import os
from pathlib import Path
import posixpath
import re
import tempfile

CACHE_ROOT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w07-l4")
DEFAULT_PREDICATES = Path(__file__).resolve().parents[2] / "contracts/DualPathPredicates.tsv"
CONTRACT_SHA256 = "5cc51b159550a9371db3b7d6e1d50625ef55e6d094f24e2608ee21d631f7f40d"
CONTRACT_FIELDS = ("predicate", "category", "owner_lane", "target_phase", "status", "description")
PREDICATE_COUNT = 16
DEFAULT_ZERO_PROCS = ("backboardd", "SpringBoard", "Camera")
UINT64_MAX = (1 << 64) - 1
TOKEN = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")
PROCESS = re.compile(r"[A-Za-z0-9_.-]{1,64}\Z")
PREDICATE = re.compile(r"raw:[a-z][a-z0-9_]*(?:\.[a-z0-9][a-z0-9_]*)+\Z")
INTEGER = re.compile(r"[0-9]{1,20}\Z")
NUMBER = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
MARKER = re.compile(r"(?:^|\s)(ROUTE|HWTEST)(?=\s|$)")
FALLBACK_TOKEN = re.compile(r"fallback:(raw:[A-Za-z0-9_.:-]+)")
PRIVATE_FIELD = re.compile(
    r"(?<![A-Za-z0-9])(?:udid|ecid|serial[_-]?number|ap[_-]?nonce|imei|imsi|meid)(?![A-Za-z0-9])",
    re.IGNORECASE,
)
ROUTE_REQUIRED = {"proc", "path"}
ROUTE_OPTIONAL = {"count", "scene"}
HWTEST_REQUIRED = {"case", "status", "path", "max_err", "mismatched_px", "time_us", "note"}
HWTEST_OPTIONAL = {"proc", "scene"}
PHASE4_TOLERANCES = {
    "gpu.heap.aliasing": Decimal(0),
    "gpu.argbuf.bindless": Decimal(1),
    "gpu.indirect.draw_dispatch": Decimal(0),
    "gpu.icb.replay": Decimal(0),
    "gpu.encoder.lifecycle_faults": Decimal(0),
}


class AuditError(ValueError):
    """A safe error code. Never include input log values or input filenames."""


@dataclass(frozen=True)
class PredicateInfo:
    predicate: str
    category: str
    owner_lane: str
    target_phase: str
    status: str
    description: str


@dataclass(frozen=True)
class RouteRecord:
    proc: str
    scene: str
    path: str
    count: int
    kind: str
    case: str = ""
    note: str = ""
    status: str = ""
    maximum: Decimal = Decimal(0)
    mismatches: int = 0
    time_us: int = 0


def load_predicates(path: str | Path = DEFAULT_PREDICATES) -> dict[str, PredicateInfo]:
    """Metadata comes only from the TSV; its frozen hash prevents replacement."""
    try:
        wire = Path(path).read_bytes()
        rows = list(csv.reader(io.StringIO(wire.decode("utf-8"), newline=""), delimiter="\t"))
    except (OSError, UnicodeError, csv.Error) as exc:
        raise AuditError("CONTRACT_READ_FAILED") from exc
    if not rows or tuple(rows[0]) != CONTRACT_FIELDS:
        raise AuditError("CONTRACT_HEADER")
    catalog = {}
    for row in rows[1:]:
        if len(row) != len(CONTRACT_FIELDS) or any(not field for field in row):
            raise AuditError("CONTRACT_ROW")
        info = PredicateInfo(*row)
        if not PREDICATE.fullmatch(info.predicate):
            raise AuditError("CONTRACT_PREDICATE")
        if info.predicate in catalog:
            raise AuditError("CONTRACT_DUPLICATE")
        if (not PROCESS.fullmatch(info.category) or info.owner_lane not in {"L1", "L2", "L3", "L4"} or
                info.target_phase not in {"Phase2", "Phase3", "Phase4"} or
                info.status not in {"open", "closed"}):
            raise AuditError("CONTRACT_METADATA")
        catalog[info.predicate] = info
    if len(catalog) != PREDICATE_COUNT:
        raise AuditError("CONTRACT_REQUIRES_16_PREDICATES")
    if hashlib.sha256(wire).hexdigest() != CONTRACT_SHA256:
        raise AuditError("CONTRACT_FROZEN_SHA256_MISMATCH")
    return catalog


def is_c_drive_path(path: str | Path) -> bool:
    normalized = posixpath.normpath(str(path).replace("\\", "/")).casefold()
    return bool(re.search(r"(?:^|/)c:", normalized) or
                re.search(r"(?:^|/)mnt/c(?:/|$)", normalized))


def checked_output_path(path: str | Path) -> Path:
    if is_c_drive_path(path):
        raise AuditError("C_DRIVE_OUTPUT_FORBIDDEN")
    try:
        resolved = Path(path).expanduser().resolve()
        if is_c_drive_path(resolved):
            raise AuditError("C_DRIVE_OUTPUT_FORBIDDEN")
        if CACHE_ROOT.resolve() != CACHE_ROOT:
            raise AuditError("CACHE_ROOT_REDIRECT")
        relative = resolved.relative_to(CACHE_ROOT)
        if not relative.parts:
            raise AuditError("OUTPUT_MUST_BE_A_FILE")
    except (OSError, RuntimeError) as exc:
        raise AuditError("OUTPUT_PATH_INVALID") from exc
    except ValueError as exc:
        if isinstance(exc, AuditError):
            raise
        raise AuditError("OUTPUT_OUTSIDE_W07_L4_CACHE") from exc
    return resolved


def zero_processes(value: str | tuple[str, ...] | list[str]) -> tuple[str, ...]:
    procs = tuple(value.split(",")) if isinstance(value, str) else tuple(value)
    if not procs or any(not isinstance(proc, str) or not PROCESS.fullmatch(proc) for proc in procs):
        raise AuditError("ZERO_PROCS_INVALID")
    if len(set(procs)) != len(procs):
        raise AuditError("ZERO_PROCS_DUPLICATE")
    return procs


def uint64(text: str, *, positive: bool = False) -> int:
    if not INTEGER.fullmatch(text):
        raise AuditError("INVALID_INTEGER")
    value = int(text)
    if value > UINT64_MAX or (positive and value == 0):
        raise AuditError("INTEGER_RANGE")
    return value


def fields(tokens: list[str], required: set[str], optional: set[str]) -> dict[str, str]:
    result = {}
    for token in tokens:
        if token.count("=") != 1:
            raise AuditError("MALFORMED_FIELD")
        key, value = token.split("=", 1)
        if key not in required | optional or key in result or not value:
            raise AuditError("UNKNOWN_OR_DUPLICATE_FIELD")
        result[key] = value
    if not required <= result.keys():
        raise AuditError("MISSING_FIELDS")
    return result


def parse_record(line: str, catalog: dict[str, PredicateInfo]) -> RouteRecord | None:
    if not isinstance(line, str):
        raise AuditError("INPUT_MUST_BE_TEXT")
    # Check the entire line, including ignored noise and timestamp prefixes.
    if PRIVATE_FIELD.search(line):
        raise AuditError("PRIVATE_FIELD_REDACTION_REQUIRED")
    for match in FALLBACK_TOKEN.finditer(line):
        if PREDICATE.fullmatch(match.group(1)) and match.group(1) not in catalog:
            raise AuditError("UNKNOWN_PREDICATE")
    markers = list(MARKER.finditer(line))
    if not markers:
        return None
    if len(markers) != 1:
        raise AuditError("AMBIGUOUS_RECORD")
    if not line.endswith("\n"):
        raise AuditError("TRUNCATED_RECORD")
    marker = markers[0]
    text = line[marker.start():].strip()
    if len(text) > 4096:
        raise AuditError("RECORD_TOO_LONG")
    tokens = text.split()
    kind = marker.group(1)
    values = fields(tokens[1:], ROUTE_REQUIRED if kind == "ROUTE" else HWTEST_REQUIRED,
                    ROUTE_OPTIONAL if kind == "ROUTE" else HWTEST_OPTIONAL)
    proc = values.get("proc", "NezhaGpuTest")
    scene = values.get("scene", values.get("case", "unlabelled"))
    if not PROCESS.fullmatch(proc) or not TOKEN.fullmatch(scene):
        raise AuditError("INVALID_PROCESS_OR_SCENE")
    path = values["path"]
    if path != "raw":
        if not path.startswith("fallback:") or not PREDICATE.fullmatch(path[len("fallback:"):]):
            raise AuditError("INVALID_ROUTE")
        if path[len("fallback:"):] not in catalog:
            raise AuditError("UNKNOWN_PREDICATE")
    if kind == "ROUTE":
        return RouteRecord(proc, scene, path, uint64(values.get("count", "1"), positive=True), kind)
    case, note, status = values["case"], values["note"], values["status"]
    if not TOKEN.fullmatch(case) or not TOKEN.fullmatch(note) or status not in {"PASS", "FAIL"}:
        raise AuditError("INVALID_HWTEST_STATUS_CASE_OR_NOTE")
    number = values["max_err"]
    if len(number) > 64 or not NUMBER.fullmatch(number):
        raise AuditError("INVALID_HWTEST_METRICS")
    maximum = Decimal(number)
    if not maximum.is_finite() or not math.isfinite(float(maximum)):
        raise AuditError("INVALID_HWTEST_METRICS")
    return RouteRecord(proc, scene, path, 1, kind, case, note, status, maximum,
                       uint64(values["mismatched_px"]), uint64(values["time_us"]))


def evidence_fields() -> dict:
    return {"source_evidence_kind": "host_log_audit",
            "device_verified": False, "hardware_acceptance": False}


def error_report(code: str) -> dict:
    return {"status": "FAIL", **evidence_fields(), "zero_fallback_passed": False,
            "errors": [{"code": code}], "unknown_predicate_count": int(code == "UNKNOWN_PREDICATE")}


def _consume(lines, catalog: dict[str, PredicateInfo], source_index: int) -> tuple[list, list, dict]:
    records, errors = [], []
    meta = {"source_index": source_index, "lines": 0, "ignored_lines": 0,
            "route_records": 0, "hwtest_records": 0}
    for number, line in enumerate(lines, 1):
        meta["lines"] = number
        try:
            record = parse_record(line, catalog)
        except AuditError as exc:
            if str(exc) == "PRIVATE_FIELD_REDACTION_REQUIRED":
                # Do not publish even a partial report derived from this input.
                raise
            errors.append({"source_index": source_index, "line": number, "code": str(exc)})
            continue
        if record is None:
            meta["ignored_lines"] += 1
            continue
        records.append(record)
        meta["route_records" if record.kind == "ROUTE" else "hwtest_records"] += 1
        if record.kind == "HWTEST":
            code = None
            if record.status != "PASS":
                code = "HWTEST_FAILED"
            elif record.mismatches:
                code = "HWTEST_PIXEL_MISMATCH"
            elif record.case in PHASE4_TOLERANCES and record.maximum > PHASE4_TOLERANCES[record.case]:
                code = "HWTEST_ERROR_TOLERANCE"
            if code:
                errors.append({"source_index": source_index, "line": number, "code": code})
    return records, errors, meta


def _stats(records: list[RouteRecord], catalog: dict[str, PredicateInfo], top_n: int) -> dict:
    total = sum(record.count for record in records)
    raw = sum(record.count for record in records if record.path == "raw")
    counts = Counter()
    for record in records:
        if record.path != "raw":
            counts[record.path[len("fallback:"):]] += record.count
    fallback = total - raw
    top = []
    for predicate in sorted(counts, key=lambda key: (-counts[key], key))[:top_n]:
        info = catalog[predicate]
        top.append({"predicate": predicate, "count": counts[predicate], "owner_lane": info.owner_lane,
                    "target_phase": info.target_phase, "category": info.category,
                    "fallback_share": counts[predicate] / fallback,
                    "submission_share": counts[predicate] / total})
    return {"total_submissions": total, "raw_count": raw, "fallback_count": fallback,
            "raw_ratio": raw / total if total else 0.0, "top_predicates": top,
            "runtime_submissions": sum(record.count for record in records if record.kind == "ROUTE"),
            "hwtest_samples": sum(record.kind == "HWTEST" for record in records),
            "cpu_reference_samples": sum(record.kind == "HWTEST" and record.note == "cpu_reference_only"
                                         for record in records)}


def _summarize(records: list[RouteRecord], errors: list[dict], sources: list[dict],
               catalog: dict[str, PredicateInfo], targets, top_n: int, allow_fallbacks: bool) -> dict:
    targets = zero_processes(targets)
    if type(top_n) is not int or top_n < 1:
        raise AuditError("TOP_N_MUST_BE_POSITIVE")
    if not records:
        errors.append({"code": "NO_VALID_ROUTE_RECORDS"})
    proc_groups, scene_groups = defaultdict(list), defaultdict(list)
    proc_scene = defaultdict(lambda: defaultdict(list))
    for record in records:
        proc_groups[record.proc].append(record)
        scene_groups[record.scene].append(record)
        proc_scene[record.proc][record.scene].append(record)
    by_proc = {proc: _stats(proc_groups[proc], catalog, top_n) for proc in sorted(proc_groups)}
    by_scene = {scene: _stats(scene_groups[scene], catalog, top_n) for scene in sorted(scene_groups)}
    unknown = sum(error["code"] == "UNKNOWN_PREDICATE" for error in errors)
    missing = sorted(set(targets) - by_proc.keys())
    blockers = [{"proc": proc, "reason": "missing_coverage"} for proc in missing]
    blockers += [{"proc": proc, "reason": "fallback_remaining", "fallback_count": by_proc[proc]["fallback_count"]}
                 for proc in sorted(targets) if proc in by_proc and by_proc[proc]["fallback_count"]]
    if unknown:
        blockers.append({"reason": "unknown_predicate", "record_count": unknown})
    zero_pass = not errors and not blockers
    return {"status": "PASS" if not errors and (zero_pass or allow_fallbacks) else "FAIL",
            **evidence_fields(), **_stats(records, catalog, top_n),
            "predicate_count": len(catalog), "contract_sha256": CONTRACT_SHA256,
            "zero_fallback_procs": list(targets), "zero_fallback_passed": zero_pass,
            "zero_fallback_blockers": blockers, "missing_target_procs": missing,
            "unknown_predicate_count": unknown, "allow_fallbacks": bool(allow_fallbacks),
            "by_process": by_proc, "by_scene": by_scene,
            "by_process_scene": {proc: {scene: _stats(group, catalog, top_n)
                                       for scene, group in sorted(proc_scene[proc].items())}
                                 for proc in sorted(proc_scene)},
            "sources": sources, "errors": errors, "counts_complete": not errors,
            "count_semantics": "ROUTE count units plus one reported route per HWTEST sample",
            "hwtest_cases": dict(sorted(Counter(record.case for record in records if record.kind == "HWTEST").items()))}


def audit_lines(lines, catalog: dict[str, PredicateInfo], targets=DEFAULT_ZERO_PROCS,
                top_n: int = 10, allow_fallbacks: bool = False) -> dict:
    if isinstance(lines, str):
        lines = lines.splitlines(keepends=True)
    records, errors, meta = _consume(lines, catalog, 1)
    return _summarize(records, errors, [meta], catalog, targets, top_n, allow_fallbacks)


def audit_logs(paths, catalog: dict[str, PredicateInfo], targets=DEFAULT_ZERO_PROCS,
               top_n: int = 10, allow_fallbacks: bool = False) -> dict:
    records, errors, sources = [], [], []
    if not paths:
        raise AuditError("LOG_INPUT_REQUIRED")
    for index, path in enumerate(paths, 1):
        digest = hashlib.sha256()
        try:
            with Path(path).open("rb") as source:
                def lines():
                    for number, wire in enumerate(source, 1):
                        digest.update(wire)
                        line = wire.decode("utf-8")
                        yield line.removeprefix("\ufeff") if number == 1 else line
                batch, issues, meta = _consume(lines(), catalog, index)
        except (OSError, UnicodeError) as exc:
            raise AuditError("LOG_READ_OR_ENCODING_FAILED") from exc
        meta["sha256"] = digest.hexdigest()
        records.extend(batch); errors.extend(issues); sources.append(meta)
    return _summarize(records, errors, sources, catalog, targets, top_n, allow_fallbacks)


def markdown_summary(report: dict) -> str:
    lines = ["# Process fallback audit", "", f"Status: {report['status']}",
             "Evidence: host_log_audit; device_verified=false; hardware_acceptance=false.",
             f"Zero-fallback targets passed: {str(report['zero_fallback_passed']).lower()}.", "",
             "Counts combine ROUTE count units and one reported route per HWTEST sample.",
             "CPU reference samples remain host evidence.", "",
             "| Process | Total | Raw | Fallback | Raw ratio |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for proc, row in report.get("by_process", {}).items():
        lines.append(f"| {proc} | {row['total_submissions']} | {row['raw_count']} | "
                     f"{row['fallback_count']} | {row['raw_ratio']:.2%} |")
    lines += ["", "| Scene | Total | Raw | Fallback | Raw ratio |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for scene, row in report.get("by_scene", {}).items():
        lines.append(f"| {scene} | {row['total_submissions']} | {row['raw_count']} | "
                     f"{row['fallback_count']} | {row['raw_ratio']:.2%} |")
    lines += ["", "| Predicate | Count | Owner | Target | Fallback share |",
              "| --- | ---: | --- | --- | ---: |"]
    for row in report.get("top_predicates", []):
        lines.append(f"| {row['predicate']} | {row['count']} | {row['owner_lane']} | "
                     f"{row['target_phase']} | {row['fallback_share']:.2%} |")
    if report.get("zero_fallback_blockers"):
        lines += ["", "Zero-fallback blockers:"]
        for row in report["zero_fallback_blockers"]:
            lines.append(f"- {row.get('proc', 'all processes')}: {row['reason']}")
    if report.get("errors"):
        lines += ["", "Invalid input records:"]
        lines += [f"- {row['code']}" for row in report["errors"]]
    return "\n".join(lines) + "\n"


def _write_output(path: Path, text: str) -> None:
    # Resolve again before creating a temporary beside the final output.
    path = checked_output_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".fallback-audit-",
                                         dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(text)
        os.replace(temporary, path)
    except OSError as exc:
        raise AuditError("REPORT_WRITE_FAILED") from exc
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def run_audit(paths, predicates: str | Path = DEFAULT_PREDICATES, targets=DEFAULT_ZERO_PROCS,
              top_n: int = 10, allow_fallbacks: bool = False,
              json_out: str | Path | None = None, md_out: str | Path | None = None) -> dict:
    paths = [Path(path) for path in paths]
    outputs = [checked_output_path(path) for path in (json_out, md_out) if path is not None]
    if len(set(outputs)) != len(outputs):
        raise AuditError("OUTPUTS_ALIAS_EACH_OTHER")
    inputs = {Path(predicates).resolve(), *(path.resolve() for path in paths)}
    if any(path in inputs for path in outputs):
        raise AuditError("OUTPUT_ALIASES_INPUT")
    report = audit_logs(paths, load_predicates(predicates), targets, top_n, allow_fallbacks)
    if json_out is not None:
        _write_output(checked_output_path(json_out), json.dumps(report, indent=2, sort_keys=True) + "\n")
    if md_out is not None:
        _write_output(checked_output_path(md_out), markdown_summary(report))
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("logs", nargs="*", type=Path)
    cli.add_argument("--input", action="append", default=[], type=Path, help="repeatable log file")
    cli.add_argument("--predicates", type=Path, default=DEFAULT_PREDICATES)
    cli.add_argument("--zero-fallback-procs", default=",".join(DEFAULT_ZERO_PROCS))
    cli.add_argument("--top-n", type=int, default=10)
    cli.add_argument("--allow-fallbacks", action="store_true", help="report mode for registered fallbacks")
    cli.add_argument("--json-out", type=Path)
    cli.add_argument("--md-out", type=Path)
    args = cli.parse_args(argv)
    try:
        report = run_audit([*args.logs, *args.input], args.predicates, args.zero_fallback_procs,
                           args.top_n, args.allow_fallbacks, args.json_out, args.md_out)
    except AuditError as exc:
        report = error_report(str(exc))
    except (OSError, ValueError, RuntimeError):
        report = error_report("AUDIT_INPUT_ERROR")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
