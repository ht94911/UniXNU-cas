#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the eleven Phase 6 host release gates for the A840 native stack.

Quick runs the Phase 3/4/5 regressions and all available release checks.
Full adds Wave discovery and the production KMD TSan stress. Neither mode
certifies the unified system on a device.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import re
import stat
import tempfile

from check_contracts import CACHE_PARENT, GateError, run_command
from cp_protect_audit import audit_paths, discover_files
from no_fallback_test import NM_SYMBOL, string_items
from phase2_host_gate import c_sources, source_file
from phase3_host_gate import check_wave_report
from phase4_host_gate import CACHE_ROOT as PHASE4_CACHE
from phase4_host_gate import CHIP, CONTEXT, PACK_SOURCES, SERVICE_SOURCES, WAVE_UNITTEST_TIMEOUT
from phase5_host_gate import CORE, EMIT, KMD, WAVE_BOOTSTRAP, HostRun
from phase5_perf_compare import checked_output_path, evidence_fields, json_text, write_output

DEFAULT_CACHE = CACHE_PARENT / "w11-l4"
GATES = (
    "contracts", "driver_constants", "kmd_phase6_release_freeze",
    "release_build_graph_zero_turnip", "production_cutover_zero_fallback",
    "cp_protect_pm4_audit", "no_fallback_final_strict",
    "phase3_non_regression", "phase4_non_regression", "phase5_non_regression", "verify_wave",
)
DEFERRED = "deferred_sibling_not_present"
SKIPPED = "skipped_quick"
SIBLINGS = {
    "kmd_phase6_release_freeze": "W11-L1",
    "release_build_graph_zero_turnip": "W11-L2",
    "production_cutover_zero_fallback": "W11-L3",
}

FREEZE = f"{KMD}/a840_kmd_v3_freeze.c"
FREEZE_TEST = f"{KMD}/test/test_kmd_phase6_release_freeze.cpp"
GRAPH_BUILDER = "tools/mac_ci/build_release_macos.py"
GRAPH_TEST = "tools/mac_ci/test_release_build_graph.py"
CUTOVER = f"{CORE}/a840_production_router.c"
CUTOVER_TEST = f"{CORE}/test/test_production_cutover.cpp"
TRANSPORT = "drivers/Adreno840Metal/transport"

GRAPH_REQUIRED = frozenset((
    "NativeMetalTranslate", "adreno_compile", "Adreno840Emit",
    "Adreno840Metal.bundle", "NezhaShaderCompiler", "NezhaGpuTest",
))
GRAPH_FORBIDDEN = frozenset((
    "libvulkan_freedreno", "turnip-darwin-kmd.patch", "metal2vulkan-runtime",
    "src/freedreno/vulkan", "NativeBridgeMetalFrontend",
))
GRAPH_PATTERN = re.compile(
    r"libvulkan_freedreno|turnip-darwin-kmd\.patch|metal2vulkan[-_]runtime|"
    r"src/freedreno/vulkan|NativeBridgeMetalFrontend", re.I
)
# Vulkan API functions use the vkUppercase spelling. Identify standalone
# C driver symbols too. The Rust IR translator has namespaced vk_format
# helpers and Vulkan memory-model metadata without a Vulkan loader.
FORBIDDEN_SYMBOL = re.compile(
    r"(?<![A-Za-z0-9_])(?:__imp_)?_*(?:vk[A-Z]\w*|PFN_vk[A-Z]\w*|tu_\w+|NB\w*|"
    r"(?i:turnip\w*|libvulkan\w*|metal2vulkan[-_]runtime\w*))\b"
    r"|^_*(?:vk_|vulkan_)\w+"
)
HEAP_SYMBOL = re.compile(
    r"(?:^|\s)_?(?:malloc|calloc|realloc|free|aligned_alloc|memalign|posix_memalign|reallocarray)"
    r"(?:@\S+)?$", re.M
)

PROBE006_CASES = (
    "gpu.blit.buffer_copy", "gpu.blit.tex_clear_bgra8", "gpu.blit.tex_blit_scale",
    "gpu.compute.add_vec", "gpu.compute.nonuniform_grid", "gpu.render.solid_triangle",
    "gpu.render.indexed_quad_textured", "gpu.sync.fence_and_order",
)
PENDING_CASES = (
    ("app_open_close", "App open and close animations"),
    ("settings_scroll", "Settings list scrolling"),
    ("app_library_list_scroll", "App Library list scrolling"),
    ("dense_liquid_glass_pages", "Paging or moving the Home Screen with many Liquid Glass folders"),
    ("camera155_preview", "Camera 155 main-camera live preview in the unified build"),
    ("camera155_photo", "Camera 155 capture and save in the unified build"),
    ("camera155_review", "Camera 155 review of the newly saved photo in the unified build"),
    ("wifi_regression", "Wi-Fi association and data transfer in the unified build"),
    ("power_regression", "Power management and sleep/wake in the unified build"),
)


def real_device_checklist() -> dict:
    return {
        "probe006": {
            "scope": "Phase 2 silicon probe",
            "boot_sha256_prefix": "40df7f811030",
            "status": "PASS",
            "passed": 8,
            "total": 8,
            "cases": [{"name": case, "status": "PASS"} for case in PROBE006_CASES],
        },
        "unified_system": {
            "status": "pending_real_device",
            "target_refresh_hz": 120,
            "physical_frame_budget_ms": 1000 / 120,
            "cases": [{"name": name, "scenario": scenario, "status": "pending_real_device"}
                      for name, scenario in PENDING_CASES],
        },
    }


def result_base(mode: str, with_clang: bool) -> dict:
    return {"status": "FAIL", "mode": mode, "with_clang": with_clang, **evidence_fields(),
            "phase6_host_release_ready": False, "results": [], "deferred_siblings": [],
            "real_device_signoff_checklist": real_device_checklist()}


def summarize(report: dict) -> None:
    results = report["results"]
    report["deferred_siblings"] = [SIBLINGS[row["name"]] for row in results
                                   if row["name"] in SIBLINGS and row["status"] == DEFERRED]
    if tuple(row["name"] for row in results) != GATES:
        return
    ready = report["mode"] == "full" and all(row["status"] == "PASS" for row in results)
    quick_pass = report["mode"] == "quick" and all(
        row["status"] == "PASS"
        or (row["name"] == "verify_wave" and row["status"] == SKIPPED)
        or (row["name"] in SIBLINGS and row["status"] == DEFERRED)
        for row in results
    )
    report.update(status="PASS" if ready or quick_pass else "FAIL", phase6_host_release_ready=ready)


def require_host_regression(report: dict, phase: int, with_clang: bool) -> None:
    if (report.get("status") != "PASS" or report.get("mode") != "quick" or
            report.get("with_clang") is not with_clang or
            any(report.get(field) is not False for field in evidence_fields())):
        raise GateError(f"invalid Phase {phase} host report")
    if phase in (3, 4) and report.get(f"phase{phase}_host_ready") is not True:
        raise GateError(f"Phase {phase} quick did not reach host readiness")
    if phase in (4, 5) and report.get("deferred_siblings") != []:
        raise GateError(f"Phase {phase} has unverified pre-Wave-11 siblings")

    rows = report["results"]
    names = [row["name"] for row in rows]
    expected_count = {3: 8, 4: 11, 5: 14}[phase]
    if len(names) != expected_count or len(set(names)) != expected_count:
        raise GateError(f"incomplete Phase {phase} coverage")
    for row in rows:
        expected = (SKIPPED if phase == 5 and row["name"] in ("phase4_non_regression", "verify_wave")
                    else "PASS")
        if row["status"] != expected:
            raise GateError(f"Phase {phase} gate did not pass: {row['name']}")
    if phase == 5:
        for name, expected in (("emit_sync_tracker_pm4", 8), ("runtime_cb_chunking_promise", 10)):
            if len(next((row.get("streams", []) for row in rows if row["name"] == name), [])) != expected:
                raise GateError(f"incomplete Phase 5 PM4 export: {name}")


def check_release_graph(report: dict) -> None:
    if not isinstance(report, dict) or report.get("status") != "PASS":
        raise GateError("release build graph did not report PASS")
    targets = report.get("targets")
    if (not isinstance(targets, dict) or targets.keys() != GRAPH_REQUIRED or
            report.get("violations") != [] or not report.get("scanned_source_count") or
            any(report.get(field) is not True for field in
                ("audit_only", "zero_turnip_verified", "zero_fallback_verified")) or
            any(report.get(field) is not False for field in evidence_fields())):
        raise GateError("release build graph audit is incomplete")
    declarations = report.get("forbidden_targets")
    if (not isinstance(declarations, list) or not all(isinstance(item, str) for item in declarations)
            or set(declarations) != GRAPH_FORBIDDEN):
        raise GateError("invalid release graph forbidden target declarations")
    graph = {key: item for key, item in report.items() if key != "forbidden_targets"}
    for location, value in string_items(graph):
        if GRAPH_PATTERN.search(value.replace("\\", "/")):
            raise GateError(f"forbidden release graph dependency at {location}: {value}")


class ReleaseRun(HostRun):
    def __init__(self, repo: Path, mesa: Path, output: Path, mode: str, with_clang: bool):
        # Both native Wave 11 sibling checks require GCC and Clang in either mode.
        super().__init__(repo, mesa, output, mode, with_clang=True)
        self.regression_clang = with_clang
        self.scan_roots = [repo, output]
        self.wave_output = output / "wave"
        extra_includes = os.pathsep.join(str(repo / path) for path in (
            f"{KMD}/xnu", "adreno_compile/include", "adreno_compile/pack",
            "adreno_compile/layout", "adreno_compile/service", "translate/include",
        ))
        self.env.update(C_INCLUDE_PATH=extra_includes, CPLUS_INCLUDE_PATH=extra_includes)

    def driver_constants(self, row: dict) -> None:
        report = self.tool(row, "driver_constants_test", "--root", self.repo)
        if report.get("violations") != [] or not report.get("files_scanned"):
            raise GateError("driver constants audit incomplete")
        row["files_scanned"] = report["files_scanned"]

    def freeze(self, row: dict) -> None:
        if not self.sibling(row, "W11-L1", (FREEZE, FREEZE.removesuffix(".c") + ".h", FREEZE_TEST)):
            return
        kmd_sources = [CHIP, *CONTEXT, f"{EMIT}/a840_cs.c", *c_sources(self.repo, KMD)]
        glue_test = f"{KMD}/test/test_xnu_v3_glue.cpp"
        glue_sources = [*kmd_sources, f"{KMD}/xnu/NativeAdrenoV3Glue.cpp"]
        self.native(row, FREEZE_TEST, glue_sources, tsan=True)
        self.native(row, f"{KMD}/test/test_kmd_v3.cpp", kmd_sources)
        for compiler in self.compilers:
            binary = self.build(row, glue_test, [*glue_sources, *c_sources(self.repo, EMIT)], compiler)
            self.command(row, [binary, "--require-real-emitters"], cwd=binary.parent, timeout=600)

        objects = [path for (_, name), path in self.objects.items() if name == FREEZE]
        row["heap_objects_checked"] = len(objects)
        for obj in objects:
            symbols = self.command(row, ["nm", "-u", obj])["stdout"]
            if HEAP_SYMBOL.search(symbols):
                raise GateError(f"KMD freeze object refers to a heap allocator: {obj}")
        if len(objects) != (3 if self.full else 2):
            raise GateError("KMD heap audit did not see every compiler configuration")

    def release_graph(self, row: dict) -> None:
        if not self.sibling(row, "W11-L2", (GRAPH_BUILDER, GRAPH_TEST)):
            return
        summary = self.output / "release-build-graph.json"
        strict = self.full and all((self.repo / file).is_file() for file in (FREEZE_TEST, CUTOVER_TEST))
        command = self.script(GRAPH_BUILDER, "--audit-graph-only", "--repo", self.repo, "--out-json", summary)
        if strict:
            command.append("--strict-merged")
        self.command(row, command, timeout=600)
        row["summary"] = str(summary)
        graph = json.loads(summary.read_text(encoding="utf-8"))
        check_release_graph(graph)
        if strict and graph.get("pending_sibling_sources") != []:
            raise GateError("release build graph is missing merged sibling sources")
        self.unittest(row, GRAPH_TEST)
        if row["tests_run"] < 30:
            raise GateError("release build graph test coverage below 30 tests")

    def cutover(self, row: dict) -> None:
        if not self.sibling(row, "W11-L3", (CUTOVER, CUTOVER.removesuffix(".c") + ".h", CUTOVER_TEST)):
            return
        sources = [CHIP, *CONTEXT, *c_sources(self.repo, EMIT), *c_sources(self.repo, CORE),
                   *c_sources(self.repo, TRANSPORT), *PACK_SOURCES, *SERVICE_SOURCES]
        self.streams(row, CUTOVER_TEST, sources)
        if len(row["streams"]) < 3 or row["comparison_scope"] != "gcc_clang":
            raise GateError("missing GCC/Clang 3D, MSAA and Tessellation cutover exports")

    def regress(self, row: dict, phase: int) -> None:
        if phase == 4:
            # Phase 4's frozen cache guard requires this literal legacy root.
            parent = checked_output_path(PHASE4_CACHE, directory=True)
            if parent != PHASE4_CACHE:
                raise GateError("redirected Phase 4 cache root")
            parent.mkdir(parents=True, exist_ok=True)
            output = Path(tempfile.mkdtemp(prefix="phase6-", dir=parent))
            self.scan_roots.append(output)
        else:
            output = self.output / f"phase{phase}"
        args = ["--repo", self.repo, "--mesa", self.mesa, "--mode", "quick", "--cache-dir", output]
        if self.regression_clang:
            args.append("--with-clang")
        report = self.tool(row, f"phase{phase}_host_gate", *args)
        require_host_regression(report, phase, self.regression_clang)
        row.update(summary=str(output / f"phase{phase}_host_summary.json"),
                   subgates={stage["name"]: stage["status"] for stage in report["results"]})

    def wave(self, row: dict) -> None:
        directory = self.wave_output
        tool_dir = source_file(self.repo, "tools/verify/phase4_host_gate.py").parent
        command = [sys.executable, "-B", "-c", WAVE_BOOTSTRAP, tool_dir,
                   "--repo", self.repo, "--mesa", self.mesa, "--cache-dir", directory]
        if self.regression_clang:
            command.append("--with-clang")
        self.command(row, command, timeout=3600)
        summary = directory / "verify_wave_summary.json"
        row.update(subgates=check_wave_report(json.loads(summary.read_text(encoding="utf-8")),
                                             self.regression_clang),
                   summary=str(summary), unittest_timeout_seconds=WAVE_UNITTEST_TIMEOUT)

    def audit_pm4(self, row: dict) -> None:
        audit = audit_paths(self.scan_roots)
        summary = self.output / "cp-protect-audit.json"
        write_output(summary, json_text(audit))
        row.update(summary=str(summary), files_scanned=audit["file_count"],
                   packets_scanned=audit["packet_count"], violations=audit["violation_count"],
                   errors=audit["errors"],
                   failed_files=[item for item in audit["files"] if item["status"] != "PASS"])
        if audit["status"] != "PASS":
            raise GateError("CP_PROTECT PM4 audit failed")

    def no_fallback(self, row: dict) -> None:
        # verify_wave copies Cargo's registry, including vendor test
        # archives for other platforms. Build outputs live in cargo-target.
        registry_input = self.wave_output / "suites/translate/cargo-home/registry"
        artifacts = sorted({path for root in self.scan_roots
                            for path in discover_files(root, {".o", ".a"}, excluded_dirs={registry_input})})
        if not artifacts or any(not stat.S_ISREG(path.lstat().st_mode) for path in artifacts):
            raise GateError("native artifact inventory is empty or contains nonregular files")
        command = self.script("tools/verify/no_fallback_test.py", "--profile", "final",
                              *(part for path in artifacts for part in ("--artifact", path)))
        result = run_command(command, cwd=self.repo, env=self.env, timeout=600)
        row["commands"].append(result)
        try:
            audit = json.loads(result["stdout"])
        except ValueError as exc:
            raise GateError("missing final symbol audit JSON") from exc
        row.update(profile=audit["profile"], artifacts_scanned=audit["scanned_items"],
                   symbols_scanned=audit["symbols_scanned"], violations=audit["violations"],
                   errors=audit["errors"])
        if (result["returncode"] or audit["status"] != "PASS" or audit["profile"] != "final" or
                audit["scanned_items"] != len(artifacts) or not audit["symbols_scanned"] or
                audit["violations"] or audit["errors"]):
            raise GateError("incomplete or failing final-profile symbol audit")

        for path in artifacts:
            result = self.command(row, ["nm", "-g", "-C", "--", path])
            for line in result.pop("stdout").splitlines():
                symbol = NM_SYMBOL.match(line)
                if symbol and (match := FORBIDDEN_SYMBOL.search(symbol[2])):
                    row["violations"].append({"path": str(path), "symbol": symbol[2],
                                              "rule": "FORBIDDEN_RELEASE_SYMBOL", "match": match[0]})
        if row["violations"]:
            raise GateError("forbidden release symbols")

    def stages(self):
        # Run the exporters before auditing the artifact inventory.
        return (
            ("contracts", self.contracts),
            ("driver_constants", self.driver_constants),
            ("kmd_phase6_release_freeze", self.freeze),
            ("release_build_graph_zero_turnip", self.release_graph),
            ("production_cutover_zero_fallback", self.cutover),
            ("phase3_non_regression", lambda row: self.regress(row, 3)),
            ("phase4_non_regression", lambda row: self.regress(row, 4)),
            ("phase5_non_regression", lambda row: self.regress(row, 5)),
            ("verify_wave", self.wave if self.full else None),
            ("cp_protect_pm4_audit", self.audit_pm4),
            ("no_fallback_final_strict", self.no_fallback),
        )


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path = DEFAULT_CACHE,
             mode: str = "quick", with_clang: bool = False, *,
             out_json: str | Path | None = None) -> dict:
    if mode not in {"quick", "full"} or type(with_clang) is not bool:
        raise GateError("invalid mode or Clang option")
    cache = checked_output_path(cache_dir, directory=True)
    summary = checked_output_path(cache / "phase6_release_summary.json")
    requested = checked_output_path(out_json) if out_json is not None else summary
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    if not repo.is_dir() or not mesa.is_dir():
        raise GateError("repository or Mesa directory is missing")

    cache.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    run = ReleaseRun(repo, mesa, output, mode, with_clang)
    report = result_base(mode, with_clang)
    rows = {}
    for name, action in run.stages():
        row = {"name": name, "status": "PASS", "commands": []}
        rows[name] = row
        if action is None:
            row["status"] = SKIPPED
            continue
        try:
            action(row)
        except (OSError, ValueError, RuntimeError, TypeError, KeyError, IndexError) as exc:
            row.update(status="FAIL", error=str(exc))
    report.update(repo=str(repo), mesa=str(mesa), cache_dir=str(cache), run_dir=str(output),
                  summary=str(summary), results=[rows[name] for name in GATES])
    summarize(report)
    payload = json_text(report)
    write_output(summary, payload)
    if requested != summary:
        write_output(requested, payload)
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--repo", type=Path, required=True)
    cli.add_argument("--mesa", type=Path, required=True)
    cli.add_argument("--mode", choices=("quick", "full"), default="quick")
    cli.add_argument("--with-clang", action="store_true")
    cli.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE)
    cli.add_argument("--out-json", type=Path)
    args = cli.parse_args(argv)
    try:
        report = run_gate(args.repo, args.mesa, args.cache_dir, args.mode, args.with_clang, out_json=args.out_json)
    except (OSError, ValueError, RuntimeError) as exc:
        report = result_base(args.mode, args.with_clang)
        report["error"] = str(exc)
    concise = dict(report, results=[{**{key: value for key, value in row.items() if key != "commands"},
                                   "command_count": len(row["commands"])} for row in report["results"]])
    sys.stdout.write(json_text(concise))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
