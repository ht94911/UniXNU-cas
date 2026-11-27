#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decide Wave 12 system acceptance from a same-boot device capture.

Use --evidence RUN.json --boot-image BOOT_FINAL.img for device verdicts.
RUN.json pins each scene/action capture, runtime log and regression log as
{"path": "...", "sha256": "..."}. See the W12-L4 receipt for the capture
schema. --self-check runs isolated host fixtures and never issues a device
verdict. An evidence hash preserves the supplied record's identity.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import stat
import sys
import uuid

sys.dont_write_bytecode = True
from system_cutover_packager import (
    BUNDLE_BINARY, BUNDLE_INSTALL_PATH, DAEMON_EXECUTABLE, HOST_FLAGS, LEGACY_NAME,
    check_sha256, checked_path, fields, json_output, parse_json, read_file,
    read_pinned, require, sha256,
)


SCENES = {
    "app_open_close": ("hot_open", "close"),
    "settings_scroll": ("scroll",),
    "app_library_list_scroll": ("list_scroll",),
    "dense_liquid_glass_pages": ("page_move",),
}
DISPLAY = {"width_px": 1200, "height_px": 2608, "refresh_hz": 120}
BUDGET_NS = 8_333_333
DROP_NS = 16_666_667
STOP_NS = 100_000_000
DEVICE_CAPTURE = "device_capture"
HOST_FIXTURE = "host_fixture"
IDENTITY = ("schema_version", "source_kind", "boot_id", "boot_sha256", "timebase")
RUN_FIELDS = (*IDENTITY, "model", "display", "scenes", "runtime_log", "regression_log")
SCENE_FIELDS = (*IDENTITY, "scene", "action", "physical_source", "action_observer",
                "capture_complete", "lost_records", "action_start_ns", "action_end_ns",
                "frames", "f07")
GLASS_FIELDS = ("layout", "liquid_glass_folder_count", "motion_kind", "single_folder_opened")
RUNTIME_COUNTERS = ("turnip_loads", "vulkan_loads", "cp_protect_violations", "jetsam_kills")
REQUIRED_IMAGES = (f"{BUNDLE_INSTALL_PATH}/{BUNDLE_BINARY}", DAEMON_EXECUTABLE)
REGRESSIONS = {"camera155": ("preview", "photo", "review"),
               "wifi": ("association", "transfer"), "power": ("sleep", "wake")}


def uint(value, name: str) -> int:
    require(type(value) is int and 0 <= value < 1 << 64, f"{name} must be an unsigned integer")
    return value


def window(doc: dict, start_key: str, end_key: str) -> tuple[int, int]:
    start, end = uint(doc[start_key], start_key), uint(doc[end_key], end_key)
    require(end > start, f"{end_key} must follow {start_key}")
    return start, end


def verify_identity(doc: dict, identity: dict, name: str) -> None:
    for key, value in identity.items():
        require(type(doc.get(key)) is type(value) and doc[key] == value, f"{name} {key} mismatch")


def verify_run(run: dict, boot_digest: str, source_kind: str) -> dict:
    fields(run, "run", RUN_FIELDS)
    require(type(run["schema_version"]) is int and run["schema_version"] == 1, "unsupported evidence schema")
    require(run["source_kind"] == source_kind and run["timebase"] == "monotonic_ns",
            "required source and same-boot monotonic timebase are missing")
    require(check_sha256(run["boot_sha256"], "observed boot SHA-256") ==
            check_sha256(boot_digest, "expected boot image SHA-256"), "boot image SHA-256 mismatch")
    boot_id = run["boot_id"]
    require(type(boot_id) is str, "required boot instance UUID is missing")
    parsed = uuid.UUID(boot_id)
    require(parsed.int != 0 and str(parsed) == boot_id.lower(), "invalid boot instance UUID")
    display = fields(run["display"], "display", DISPLAY)
    require(run["model"] == "nezha" and all(type(display[key]) is int and display[key] == value
                                          for key, value in DISPLAY.items()),
            "expected nezha 1200×2608 at 120 Hz")
    return {key: run[key] for key in IDENTITY}


def percentile(ordered: list[int], rank: int) -> int:
    return ordered[(rank * len(ordered) + 99) // 100 - 1]


def analyze_f07(rows, sequences: set[int]) -> dict:
    require(type(rows) is list and rows, "same-window F07 pre_create samples are required")
    seen, pre_sum, total_sum = set(), 0, 0
    for row in rows:
        fields(row, "F07 sample", ("sequence", "pre_create_ns", "pipeline_ns"))
        seq = uint(row["sequence"], "F07 sequence")
        pre = uint(row["pre_create_ns"], "pre_create_ns")
        total = uint(row["pipeline_ns"], "pipeline_ns")
        require(seq in sequences and seq not in seen and 0 <= pre <= total and total > 0,
                "F07 must join one in-window physical interval with a valid software duration")
        seen.add(seq)
        pre_sum += pre
        total_sum += total
    return {"sample_count": len(seen), "matched_physical_intervals": len(sequences),
            "pre_create_ns": pre_sum, "software_pipeline_ns": total_sum,
            "pre_create_share_pct": 100 * pre_sum / total_sum}


def analyze_action(doc: dict, identity: dict, scene: str, action: str) -> tuple[dict, tuple[int, int]]:
    glass = scene == "dense_liquid_glass_pages"
    fields(doc, f"{scene}/{action}", (*SCENE_FIELDS, *GLASS_FIELDS) if glass else SCENE_FIELDS)
    verify_identity(doc, identity, f"{scene}/{action}")
    require(doc["scene"] == scene and doc["action"] == action, "scene or action identity mismatch")
    require(doc["physical_source"] in ("optical_content_updates", "panel_content_updates"),
            "requires physical content updates from the optical or hardware panel observer")
    observers = ("ui_animation_trace", "optical_action_tracking") if scene == "app_open_close" else (
        "hid_motion", "optical_action_tracking")
    require(doc["action_observer"] in observers, "action continuity is missing or unconfirmed")
    require(doc["capture_complete"] is True and uint(doc["lost_records"], "lost_records") == 0,
            "physical capture has incomplete coverage or lost records")
    if glass:
        require(doc["layout"] == "dense_liquid_glass_folders"
                and uint(doc["liquid_glass_folder_count"], "liquid_glass_folder_count") > 1
                and doc["motion_kind"] in ("page_swipe", "whole_page_motion")
                and doc["single_folder_opened"] is False,
                "requires movement of a dense Liquid Glass Home Screen page")
    start, end = window(doc, "action_start_ns", "action_end_ns")
    rows = doc["frames"]
    require(type(rows) is list and len(rows) >= 2, "physical content timestamps are missing")
    samples, active_holds, sampled_sequences = [], [], set()
    first = last = None
    for row in rows:
        fields(row, "physical update", ("sequence", "present_ns", "content_id"))
        current = uint(row["present_ns"], "present_ns")
        seq = uint(row["sequence"], "physical capture sequence")
        content = row["content_id"]
        require(type(content) is str and content.strip(), "physical content identity is missing")
        if last is None:
            first = current
        else:
            previous, previous_seq, previous_content = last
            require(seq == previous_seq + 1 and current > previous, "physical capture is gapped or out of order")
            require(content != previous_content, "repeated content is not a new physical frame")
            overlap = min(current, end) - max(previous, start)
            if overlap > 0:
                active_holds.append(overlap)
            if start <= previous and current <= end:
                samples.append(current - previous)
                sampled_sequences.add(seq)
        last = current, seq, content
    require(first <= start and last[0] >= end and samples,
            "physical timestamps must bracket the observed action with at least one full interval")

    samples.sort()
    p50_ns, p95_ns = percentile(samples, 50), percentile(samples, 95)
    dropped = sum(sample >= DROP_NS for sample in samples)
    stopped = sum(hold >= STOP_NS for hold in active_holds)
    errors = []
    if p95_ns > BUDGET_NS:
        errors.append("physical P95 exceeds the 120 Hz budget")
    if stopped:
        errors.append("physical operation hold reached 100 ms")
    return {
        "status": "FAIL" if errors else "PASS", "issues": errors,
        "physical_source": doc["physical_source"],
        "physical": {"sample_count": len(samples), "p50_ms": p50_ns / 1e6,
                     "p95_ms": p95_ns / 1e6, "max_ms": samples[-1] / 1e6,
                     "ge_16_667_ms_count": dropped, "ge_16_667_ms_rate_pct": 100 * dropped / len(samples),
                     "ge_100_ms_operation_stops": stopped, "max_operation_hold_ms": max(active_holds) / 1e6},
        "f07_pre_create": analyze_f07(doc["f07"], sampled_sequences),
    }, (start, end)


def runtime_checks(doc: dict, identity: dict, windows: list[tuple[int, int]]) -> dict:
    fields(doc, "runtime log", (*IDENTITY, "coverage", "lost_records", "start_ns", "end_ns",
                               "loaded_images", *RUNTIME_COUNTERS))
    verify_identity(doc, identity, "runtime log")
    start, end = window(doc, "start_ns", "end_ns")
    require(doc["coverage"] == "boot_through_regressions" and uint(doc["lost_records"], "lost_records") == 0
            and all(start <= first and end >= last for first, last in windows),
            "runtime log must cover boot, all physical actions and regressions without loss")
    images = doc["loaded_images"]
    require(type(images) is list and all(type(path) is str and path.startswith("/") for path in images),
            "loaded image paths are required")
    require(all(image in images for image in REQUIRED_IMAGES), "native plugin and compiler runtime loads are required")
    counters = {key: uint(doc[key], key) for key in RUNTIME_COUNTERS}
    forbidden = sorted({path for path in images if LEGACY_NAME.search(path)})
    return {"status": "FAIL" if forbidden or any(counters.values()) else "PASS",
            "native_images": list(REQUIRED_IMAGES), "legacy_loaded_images": forbidden, **counters}


def regression_checks(doc: dict, identity: dict) -> tuple[dict, tuple[int, int]]:
    fields(doc, "regression log", (*IDENTITY, "start_ns", "end_ns", *REGRESSIONS))
    verify_identity(doc, identity, "regression log")
    limits = window(doc, "start_ns", "end_ns")
    checks = {}
    for group, required in REGRESSIONS.items():
        outcomes = fields(doc[group], group, required)
        for outcome in outcomes.values():
            require(outcome in ("PASS", "FAIL", "UNTESTED"), f"invalid {group} result")
        checks[group] = dict(outcomes)
    passed = all(outcome == "PASS" for group in checks.values() for outcome in group.values())
    return {"status": "PASS" if passed else "FAIL", **checks}, limits


def evaluate(run: dict, boot_digest: str, read_document, *, source_kind: str) -> dict:
    identity = verify_run(run, boot_digest, source_kind)
    inputs = fields(run["scenes"], "scenes", SCENES)
    scenes, windows = {}, []
    for name, actions in SCENES.items():
        action_refs = fields(inputs[name], name, actions)
        reports = {}
        for action in actions:
            label = f"{name}/{action}"
            try:
                reports[action], limits = analyze_action(read_document(action_refs[action], label),
                                                         identity, name, action)
            except ValueError as exc:
                raise ValueError(f"{label}: {exc}") from exc
            windows.append(limits)
        passed = all(report["status"] == "PASS" for report in reports.values())
        scenes[name] = {"status": "PASS" if passed else "FAIL", "actions": reports}
    regressions, limits = regression_checks(read_document(run["regression_log"], "regression_log"), identity)
    runtime = runtime_checks(read_document(run["runtime_log"], "runtime_log"), identity, [*windows, limits])
    passed = all(row["status"] == "PASS" for row in (*scenes.values(), runtime, regressions))
    device = source_kind == DEVICE_CAPTURE
    return {
        **identity, "status": "PASS" if passed else "FAIL", "model": "nezha", "display": dict(DISPLAY),
        "device_verified": device, "hardware_acceptance": device and passed,
        "physical_budget_ns": BUDGET_NS, "drop_threshold_ns": DROP_NS, "operation_stop_threshold_ns": STOP_NS,
        "scenes": scenes, "runtime": runtime, "regressions": regressions,
    }


def host_fixture() -> tuple[dict, str]:
    boot = sha256(b"Wave 12 synthetic boot fixture")
    identity = {"schema_version": 1, "source_kind": HOST_FIXTURE, "boot_sha256": boot,
                "boot_id": "00000000-0000-4000-9000-000000000012", "timebase": "monotonic_ns"}
    scenes = {}
    for name, actions in SCENES.items():
        scenes[name] = {}
        for action in actions:
            first = 1_000_000_000
            spacing = 8_300_000
            row = {**identity, "scene": name, "action": action,
                   "physical_source": "optical_content_updates", "action_observer": "optical_action_tracking",
                   "capture_complete": True, "lost_records": 0,
                   "action_start_ns": first, "action_end_ns": first + 3 * spacing,
                   "frames": [{"sequence": i, "present_ns": first + i * spacing, "content_id": f"content-{i}"}
                              for i in range(4)],
                   "f07": [{"sequence": 1, "pre_create_ns": 9_000_000, "pipeline_ns": 10_000_000}]}
            if name == "dense_liquid_glass_pages":
                row.update(layout="dense_liquid_glass_folders", liquid_glass_folder_count=12,
                           motion_kind="page_swipe", single_folder_opened=False)
            scenes[name][action] = row
    runtime = {**identity, "start_ns": 1, "end_ns": 3_000_000_000,
               "coverage": "boot_through_regressions", "lost_records": 0,
               "loaded_images": list(REQUIRED_IMAGES), **dict.fromkeys(RUNTIME_COUNTERS, 0)}
    regressions = {**identity, "start_ns": 2_000_000_000, "end_ns": 2_500_000_000,
                   **{name: dict.fromkeys(keys, "PASS") for name, keys in REGRESSIONS.items()}}
    return {**identity, "model": "nezha", "display": dict(DISPLAY), "scenes": scenes,
            "runtime_log": runtime, "regression_log": regressions}, boot


def self_check() -> dict:
    fixture, boot = host_fixture()

    def measure():
        return evaluate(fixture, boot, lambda doc, name: doc, source_kind=HOST_FIXTURE)

    initial = measure()
    require(initial["status"] == "PASS" and all(initial[key] is value for key, value in HOST_FLAGS.items()),
            "passing host fixture escaped host-only scope")
    physical = fixture["scenes"]["app_open_close"]["hot_open"]
    physical["frames"][-1]["present_ns"] = physical["frames"][-2]["present_ns"] + STOP_NS
    physical["action_end_ns"] = physical["frames"][-1]["present_ns"]
    rejected = measure()
    stop = rejected["scenes"]["app_open_close"]["actions"]["hot_open"]
    require(rejected["status"] == "FAIL" and stop["physical"]["ge_100_ms_operation_stops"] == 1
            and all(rejected[key] is value for key, value in HOST_FLAGS.items()), "100 ms boundary was missed")
    require(stop["f07_pre_create"]["pre_create_share_pct"] == 90.0, "F07 software attribution changed")
    return {"schema_version": 1, "status": "PASS", "mode": "host_self_check", **HOST_FLAGS,
            "checks": {"four_scene_fixture": "PASS", "exact_100_ms_hold_rejected": "PASS",
                       "f07_independent": "PASS", "host_device_flags_false": "PASS"}}


def device_report(evidence: Path, boot_image: Path) -> dict:
    require(stat.S_ISREG(boot_image.stat().st_mode), "boot image must be a regular file")
    with boot_image.open("rb") as image:
        digest = hashlib.file_digest(image, "sha256").hexdigest()
    run = parse_json(read_file(evidence))

    def read_document(ref, name):
        return parse_json(read_pinned(ref, evidence.parent, name))

    return evaluate(run, digest, read_document, source_kind=DEVICE_CAPTURE)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--self-check", action="store_true")
    mode.add_argument("--evidence", metavar="RUN_JSON")
    parser.add_argument("--boot-image", metavar="BOOT_FINAL_IMG")
    args = parser.parse_args(argv)
    try:
        if args.self_check:
            require(args.boot_image is None, "--self-check uses only host fixtures")
            report = self_check()
        else:
            require(args.boot_image is not None, "--evidence requires an independently selected --boot-image")
            report = device_report(checked_path(args.evidence, Path.cwd()), checked_path(args.boot_image, Path.cwd()))
    except (OSError, ValueError, RuntimeError, ArithmeticError) as exc:
        report = {"schema_version": 1, "status": "FAIL", **HOST_FLAGS, "error": str(exc)}
    print(json_output(report), end="")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
