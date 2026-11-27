#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the Wave 9 and Wave 10 Phase 5 host gates.

Quick runs GCC 13 ASan/UBSan, PM4, the Python suites and the symbol audit.
--with-clang adds Clang 18 ASan/UBSan and compares its PM4 bytes with GCC.
Full adds TSan, pinned Mesa ir3 A/B tests, Phase 4 quick and verify_wave.
Only completely absent Wave 10 siblings may be deferred.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import tempfile

from check_contracts import GateError, cache_environment, run_command
import frame_weights_profiler as fw
from phase2_host_gate import INCLUDES, STRICT_FLAGS, c_sources, execute, json_pass, source_file
from phase3_host_gate import check_wave_report
from phase4_host_gate import CACHE_ROOT as PHASE4_CACHE, WAVE_UNITTEST_TIMEOUT, require_pm4, require_pm4_diff
import phase5_perf_compare as perf
from verify_wave import clang_configuration

KMD = "drivers/Adreno840Kernel"
CORE = "drivers/Adreno840Metal/core"
EMIT = "kits/Adreno840Emit"
RECORDER = "apps/NezhaGpuTest/perf"
IR3 = "adreno_compile/ir3"
IR3_TOOLS = "adreno_compile/tools"
PREEMPT = (f"{KMD}/test/test_kmd_phase5_preempt.cpp",
           f"{KMD}/a840_kmd_preempt_ring.c", f"{KMD}/a840_kmd_wakeup_latency.c")
SYNC = (f"{EMIT}/a840_cs.c", f"{EMIT}/a840_emit_sync.c", f"{EMIT}/a840_sync_tracker.c")
ARENA = (*SYNC, f"{CORE}/a840_frame_arena.c")
GMU_TEST = f"{KMD}/test/test_kmd_phase5_gmu_mdss.cpp"
GMU_FILES = (GMU_TEST, *(f"{KMD}/a840_kmd_{name}.{ext}" for name in ("gmu_dcvs", "mdss_sync")
                         for ext in ("c", "h")))
HOTSPOT_FILES = (f"{IR3}/test_metal_hotspot_opt.c", f"{IR3}/a840_metal_opt_pass.c",
                 f"{IR3}/a840_metal_opt_pass.h", f"{IR3_TOOLS}/test_hotspot_opt_quantify.py",
                 f"{IR3_TOOLS}/hotspot_opt_quantify.py")
CHUNK_TEST = f"{CORE}/test/test_cb_chunking_and_early_promise.cpp"
CHUNK_FILES = (CHUNK_TEST, *(f"{CORE}/a840_{name}.{ext}" for name in ("cb_chunker", "early_promise_sched")
                             for ext in ("c", "h")))
SYNC_EXPORTS = {f"{case}_{mode}.pm4" for case in (
    "s1_liquid_glass_4pass", "s2_disjoint_dual_pass", "s3_compute_to_fragment", "s4_blit_camera_nv12")
                for mode in ("naive", "tracked")}
ASAN = "address,undefined"
DEFERRED = "deferred_sibling_not_present"
SKIPPED = "skipped_quick"

# Use the existing pinned Mesa build recipe in its own process. It builds only
# the non-Vulkan libraries and provides the same IR3 defines as its E2E build.
IR3_BOOTSTRAP = """
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
import build_adreno_compile_ir3 as builder
mesa, cache = map(Path, sys.argv[2:4])
env = dict(os.environ, MESA_GIT_SHA1_OVERRIDE=builder.MESA_COMMIT)
env["PATH"] = str(builder.MESON.parent) + os.pathsep + env.get("PATH", "")
source, build, _, libraries = builder.build_mesa_archives(
    mesa, builder.DEFAULT_MANIFEST, cache, 32, env)
translate = builder.ensure_translate_archive(cache, 32, env)
flags = builder.mesa_compile_defines(cache) + builder.mesa_include_flags(source, build)
print(json.dumps([flags, [str(path) for path in [*libraries, translate]]]))
"""

WAVE_BOOTSTRAP = ("import sys; sys.path.insert(0, sys.argv.pop(1)); "
                  "from phase4_host_gate import wave_cli; raise SystemExit(wave_cli(sys.argv[1:]))")


def _require_host(report: dict, **fields) -> None:
    if (report.get("status") != "PASS" or
            any(report.get(key) is not False for key in fw.HOST_FLAGS) or
            any(report.get(key) != value for key, value in fields.items())):
        raise GateError("INVALID_HOST_REPORT")


class HostRun:
    def __init__(self, repo: Path, mesa: Path, output: Path, mode: str, with_clang: bool):
        self.repo, self.mesa, self.output = repo, mesa, output
        self.full = mode == "full"
        self.with_clang = with_clang
        self.compilers = ("gcc", "clang18") if with_clang else ("gcc",)
        self.env = cache_environment(output / "tmp")
        self.env.update(A840_VERIFY_MESA=str(mesa), CARGO_BUILD_JOBS="32",
                        ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", TSAN_OPTIONS="halt_on_error=1")
        self.toolchains = {}
        self.objects: dict[tuple[Path, str], Path] = {}
        self.ir3_inputs: tuple[list[str], list[str]] | None = None
        self.pm4_index: Path | None = None

    def command(self, row: dict, argv, *, cwd: Path | None = None, timeout: int = 300) -> dict:
        return execute(row, list(map(str, argv)), cwd or self.repo, self.env, timeout=timeout)

    def script(self, relative: str, *args) -> list[str]:
        return [sys.executable, "-B", str(source_file(self.repo, relative)), *map(str, args)]

    def tool(self, row: dict, name: str, *args) -> dict:
        return json_pass(self.command(row, self.script(f"tools/verify/{name}.py", *args), timeout=600))

    def sibling(self, row: dict, lane: str, files: tuple[str, ...]) -> bool:
        present = [file for file in files if (self.repo / file).exists() or (self.repo / file).is_symlink()]
        row["sibling"] = {"name": lane, "required_paths": list(files), "present_paths": present}
        if not present:
            row["status"] = DEFERRED
            return False
        for file in files:
            source_file(self.repo, file)
        return True

    def compiler(self, row: dict, name: str) -> tuple[str, str, list[str]]:
        if name not in self.toolchains:
            cc, cxx, extra = clang_configuration() if name == "clang18" else ("gcc-13", "g++-13", [])
            major = "18" if name == "clang18" else "13"
            for program in (cc, cxx):
                version = self.command(row, (program, "-dumpversion"))["stdout"].strip().split(".")[0]
                if version != major:
                    raise GateError(f"expected compiler major {major}: {program}")
            self.toolchains[name] = (cc, cxx, extra)
        return self.toolchains[name]

    def build(self, row: dict, test: str, sources, compiler: str, sanitizer: str = ASAN,
              *, ir3: bool = False) -> Path:
        cc, cxx, extra = self.compiler(row, compiler)
        output = self.output / Path(test).stem / compiler / sanitizer.replace(",", "-")
        obj_dir = self.output / "objects" / compiler / sanitizer.replace(",", "-")
        output.mkdir(parents=True, exist_ok=True)
        obj_dir.mkdir(parents=True, exist_ok=True)
        flags = [*STRICT_FLAGS, "-g", f"-fsanitize={sanitizer}", *extra]
        link = []
        if ir3:
            ir3_flags, link = self.prepare_ir3(row)
            flags.remove("-pedantic")  # Match the existing Mesa C11 E2E recipe.
            flags += ["-O1", "-Wno-unused-parameter", *ir3_flags]
        else:
            flags += [arg for name in (*INCLUDES, RECORDER) if (self.repo / name).is_dir()
                      for arg in ("-I", str(self.repo / name))]
        if sanitizer == "thread":
            flags.append("-O1")

        objects, pending = [], []
        for relative in sorted({test, *sources}):
            src = source_file(self.repo, relative)
            digest = hashlib.sha256(relative.encode()).hexdigest()[:16]
            obj = obj_dir / f"{src.stem}-{digest}.o"
            key = (obj_dir, relative)
            objects.append(obj)
            if key not in self.objects:
                program, standard = (cc, "c11") if src.suffix == ".c" else (cxx, "c++17")
                argv = [program, "-std=" + standard, *flags, "-c", str(src), "-o", str(obj)]
                pending.append((key, obj, argv))
        with ThreadPoolExecutor(max_workers=32) as pool:
            jobs = [pool.submit(run_command, argv, cwd=output, env=self.env, timeout=300)
                    for _, _, argv in pending]
            results = [job.result() for job in jobs]
        row["commands"].extend(results)
        for (key, obj, _), result in zip(pending, results):
            if result["returncode"] == 0:
                self.objects[key] = obj
        if any(result["returncode"] for result in results):
            raise GateError(f"native source compilation failed: {test}")
        binary = output / "test"
        self.command(row, [cxx, *flags, *objects, *link, "-ldl", "-lm", "-no-pie", "-o", binary], cwd=output)
        return binary

    def native(self, row: dict, test: str, sources, *, tsan: bool = False) -> None:
        for compiler in self.compilers:
            binary = self.build(row, test, sources, compiler)
            self.command(row, [binary], cwd=binary.parent, timeout=600)
        if tsan and self.full:
            self.tsan(row, test, sources)

    def tsan(self, row: dict, test: str, sources) -> None:
        binary = self.build(row, test, sources, "gcc", "thread")
        self.command(row, ["setarch", "x86_64", "-R", binary, "--tsan-stress", "20"],
                     cwd=binary.parent, timeout=600)
        row["tsan_rounds"] = 20

    def unittest(self, row: dict, relative: str) -> None:
        test = source_file(self.repo, relative)
        result = self.command(row, [sys.executable, "-B", "-m", "unittest", "discover",
                                    "-s", test.parent, "-p", test.name, "-v"], timeout=600)
        match = re.search(r"(?m)^Ran ([1-9][0-9]*) tests? in ", result["stderr"])
        if not match:
            raise GateError(f"empty unittest discovery: {relative}")
        row["tests_run"] = int(match[1])

    def contracts(self, row: dict) -> None:
        report = self.tool(row, "check_contracts", "--repo", self.repo, "--cache-dir", self.output / "contracts")
        if (len(report["contracts"]) != 5 or report["errors"] or report["violations"] or
                any(item["repo_sha256"] != item["docs_sha256"] for item in report["contracts"])):
            raise GateError("expected five unchanged frozen contracts")
        row["contracts_checked"] = len(report["contracts"])

    def kmd_gmu(self, row: dict) -> None:
        if self.sibling(row, "W10-L1", GMU_FILES):
            sources = [name for name in GMU_FILES if name.endswith(".c")]
            self.native(row, GMU_TEST, sources, tsan=True)
            for (directory, name), obj in self.objects.items():
                if name not in sources:
                    continue
                symbols = self.command(row, ["nm", "-u", obj])["stdout"]
                if re.search(r"(?m)\b(?:malloc|calloc|realloc|free)(?:@\S+)?$", symbols):
                    raise GateError(f"heap symbol in GMU/MDSS object: {directory.name}/{obj.name}")

    def prepare_ir3(self, row: dict) -> tuple[list[str], list[str]]:
        if self.ir3_inputs is None:
            builder = source_file(self.repo, f"{IR3_TOOLS}/build_adreno_compile_ir3.py")
            cache = self.output / "ir3"
            cache.mkdir(exist_ok=True)
            source_file(self.repo, "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json")
            result = self.command(row, [sys.executable, "-B", "-c", IR3_BOOTSTRAP, builder.parent,
                                        self.mesa, cache], timeout=1800)
            flags, archives = json.loads(result["stdout"].splitlines()[-1])
            self.ir3_inputs = ([*flags, "-I", str(self.repo / "translate/include")],
                               ["-Wl,--start-group", *archives, "-Wl,--end-group", "-lz"])
        return self.ir3_inputs

    def ir3(self, row: dict, test_stem: str, quantifier: str) -> None:
        script = f"{IR3_TOOLS}/{quantifier}.py"
        source_file(self.repo, script)
        self.unittest(row, f"{IR3_TOOLS}/test_{quantifier}.py")
        if not self.full:
            return
        binary = self.build(row, f"{IR3}/{test_stem}.c", c_sources(self.repo, IR3), "gcc", ir3=True)
        raw = binary.parent / "ab.json"
        summary = binary.parent / "quantified.json"
        markdown = binary.parent / "quantified.md"
        self.command(row, [binary, "--dump-json", raw], timeout=600)
        self.command(row, self.script(script, "--input", raw, "--out-json", summary, "--out-md", markdown),
                     timeout=600)
        _require_host(perf.load_json(summary))
        if not markdown.stat().st_size:
            raise GateError("missing IR3 quantification Markdown")
        row.update(ab_json=str(raw), quantification_json=str(summary), quantification_md=str(markdown))

    def ir3_hotspot(self, row: dict) -> None:
        if self.sibling(row, "W10-L2", HOTSPOT_FILES):
            self.ir3(row, "test_metal_hotspot_opt", "hotspot_opt_quantify")

    def streams(self, row: dict, test: str, sources, expected: set[str] | None = None,
                *, tsan: bool = False) -> None:
        paths = {}
        for compiler in self.compilers:
            binary = self.build(row, test, sources, compiler)
            paths[compiler] = self.export_pm4(row, binary, "pm4")
        if self.with_clang:
            second = paths["clang18"]
        else:
            # A second execution checks the bytes and ensures pm4_diff never
            # compares one file with itself even in a GCC-only run.
            second = self.export_pm4(row, binary, "pm4-repeat")

        first = paths["gcc"]
        if not first or first.keys() != second.keys() or (expected is not None and first.keys() != expected):
            raise GateError(f"PM4 export coverage mismatch: {test}")
        if self.pm4_index is None:
            target = self.output / "a840_pm4_index.json"
            self.command(row, self.script("tools/verify/pm4_xml_index.py", "--mesa", self.mesa, "--out", target))
            self.pm4_index = target
        options = ["--index", self.pm4_index, "--chip", "A8XX", "--format", "binary"]
        row["comparison_scope"] = "gcc_clang" if self.with_clang else "gcc_repeat"
        row["streams"] = []
        for name in sorted(first):
            sample, other = first[name], second[name]
            check = self.tool(row, "pm4_check", *options, sample)
            require_pm4(check)
            require_pm4(self.tool(row, "pm4_check", *options, other))
            require_pm4_diff(self.tool(row, "pm4_diff", *options, "--strict-packets",
                                      "--expected", sample, "--actual", other))
            payload = sample.read_bytes()
            if payload != other.read_bytes():
                raise GateError(f"PM4 byte mismatch: {name}")
            row["streams"].append({"name": name, "sha256": hashlib.sha256(payload).hexdigest(),
                                   "packets": check["packet_count"], "dwords": check["dwords"]})
        if tsan and self.full:
            self.tsan(row, test, sources)

    def export_pm4(self, row: dict, binary: Path, name: str) -> dict[str, Path]:
        directory = binary.parent / name
        directory.mkdir()
        self.command(row, [binary, "--dump-pm4", directory], cwd=binary.parent, timeout=600)
        return {path.name: path for path in directory.glob("*.pm4")
                if path.is_file() and not path.is_symlink()}

    def runtime_chunking(self, row: dict) -> None:
        if self.sibling(row, "W10-L3", CHUNK_FILES):
            self.streams(row, CHUNK_TEST, [*ARENA, *(name for name in CHUNK_FILES if name.endswith(".c"))],
                         tsan=True)

    def frame_weights(self, row: dict) -> None:
        row["captures"] = {}
        for compiler in self.compilers:
            binary = self.build(row, f"{RECORDER}/test_frame_weights.cpp",
                                [f"{RECORDER}/a840_frame_weights.c"], compiler)
            sample = binary.parent / "sample.a840fw"
            # The frozen native exporter checks the literal W09 cache path.
            parent = perf.checked_output_path(fw.CACHE_ROOT, directory=True)
            parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="phase5-", dir=parent) as scratch:
                exported = Path(scratch) / sample.name
                self.command(row, [binary, "--dump-sample", exported], cwd=binary.parent)
                sample.write_bytes(exported.read_bytes())
            report = self.tool(row, "frame_weights_profiler", "analyze", "--input", sample)
            _require_host(report, source_format="A840FW01", source_evidence_kind="unverified_capture")
            frame_counts = {scene: report["scenes"][scene]["frame_count"] for scene in fw.CORE_SCENES}
            if not all(frame_counts.values()) or not report["draw_weights"]["draw_count"]:
                raise GateError("incomplete native frame-weight sample")
            row["captures"][compiler] = {"path": str(sample), "frames_by_scene": frame_counts,
                                         "draw_count": report["draw_weights"]["draw_count"]}
        self.unittest(row, "tools/verify/test_frame_weights_profiler.py")

    def perf_fixture(self, row: dict) -> None:
        row["fixture_kind"] = "synthetic_jsonl"
        directory = self.output / "performance"
        directory.mkdir()
        inputs = {}
        for phase, encode in zip((*perf.PHASES, "regression"), (5_000_000, 3_000_000, 1_000_000, 20_000_000)):
            capture = directory / f"{phase}.jsonl"
            capture.write_text(synthetic_frames(encode), encoding="utf-8")
            report = self.tool(row, "frame_weights_profiler", "analyze", "--input", capture)
            inputs[phase] = directory / f"{phase}.json"
            perf.write_output(inputs[phase], perf.json_text(report))
        for phase, expected in (("phase5", 0), ("regression", 1)):
            json_path, md_path = directory / f"compare-{phase}.json", directory / f"compare-{phase}.md"
            command = self.script("tools/verify/phase5_perf_compare.py", "--phase0", inputs["phase0"],
                                  "--phase3", inputs["phase3"], "--phase5", inputs[phase],
                                  "--out-json", json_path, "--out-md", md_path)
            result = run_command(command, cwd=self.repo, env=self.env)
            row["commands"].append(result)
            if result["returncode"] != expected:
                raise GateError(f"unexpected comparison exit: {phase}")
            report = json.loads(result["stdout"])
            if (report.get("status") != ("FAIL" if expected else "PASS") or
                    any(report.get(field) is not False for field in fw.HOST_FLAGS) or
                    report.get("all_four_scenes_present") is not True or
                    report.get("backend_within_120hz_budget") is not (expected == 0) or
                    report.get("regression_detected") is not (expected == 1) or
                    report != json.loads(json_path.read_text(encoding="utf-8")) or
                    md_path.read_text(encoding="utf-8") != perf.markdown(report)):
                raise GateError(f"invalid comparison integration report: {phase}")
            comparison = report["scenes"][fw.CORE_SCENES[-1]]["phases"]["phase5"]
            if comparison["f07_pre_create"]["physical_stall_count"] <= 0:
                raise GateError("synthetic replay lost its F07 long stall")
            row[phase] = {"json": str(json_path), "markdown": str(md_path)}

    def no_fallback(self, row: dict) -> None:
        objects = sorted(set(self.objects.values()))
        if not objects:
            raise GateError("no Phase 5 objects to audit")
        report = self.tool(row, "no_fallback_test", "--profile", "final",
                           *(part for obj in objects for part in ("--artifact", obj)))
        if (report["profile"] != "final" or not report["symbols_scanned"] or
                report["scanned_items"] != len(objects) or report["violations"] or report["errors"]):
            raise GateError("incomplete Phase 5 final symbol audit")
        row.update(objects_scanned=len(objects), symbols_scanned=report["symbols_scanned"], violations=0)

    def phase4(self, row: dict) -> None:
        # Its frozen CLI requires the old literal cache root. Give this run
        # its own directory there and keep the compiler temporaries scoped.
        parent = perf.checked_output_path(PHASE4_CACHE, directory=True)
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="phase5-", dir=parent))
        args = ["--repo", self.repo, "--mesa", self.mesa, "--mode", "quick", "--cache-dir", output]
        if self.with_clang:
            args.append("--with-clang")
        report = self.tool(row, "phase4_host_gate", *args)
        _require_host(report, mode="quick", with_clang=self.with_clang, deferred_siblings=[])
        if report.get("phase4_host_ready") is not True:
            raise GateError("Phase 4 quick did not reach host readiness")
        row["summary"] = str(output / "phase4_host_summary.json")

    def wave(self, row: dict) -> None:
        directory = self.output / "wave"
        tool_dir = source_file(self.repo, "tools/verify/phase4_host_gate.py").parent
        command = [sys.executable, "-B", "-c", WAVE_BOOTSTRAP, tool_dir,
                   "--repo", self.repo, "--mesa", self.mesa, "--cache-dir", directory]
        if self.with_clang:
            command.append("--with-clang")
        self.command(row, command, timeout=3600)
        summary = directory / "verify_wave_summary.json"
        row.update(subgates=check_wave_report(perf.load_json(summary), self.with_clang),
                   summary=str(summary), unittest_timeout_seconds=WAVE_UNITTEST_TIMEOUT)

    def stages(self):
        return (
            ("contracts", self.contracts),
            ("driver_constants", lambda row: self.tool(row, "driver_constants_test", "--root", self.repo)),
            ("kmd_phase5_preempt", lambda row: self.native(row, PREEMPT[0], PREEMPT[1:], tsan=True)),
            ("kmd_phase5_gmu_mdss", self.kmd_gmu),
            ("ir3_robustness_and_stats", lambda row: self.ir3(row, "test_robustness_and_profile",
                                                            "robustness_quantify")),
            ("ir3_metal_hotspot_opt", self.ir3_hotspot),
            ("emit_sync_tracker_pm4", lambda row: self.streams(row, f"{EMIT}/test/test_sync_tracker.cpp",
                                                              SYNC, SYNC_EXPORTS)),
            ("runtime_frame_arena", lambda row: self.native(row, f"{CORE}/test/test_frame_arena_and_sync.cpp",
                                                           ARENA, tsan=True)),
            ("runtime_cb_chunking_promise", self.runtime_chunking),
            ("frame_weights_profiler", self.frame_weights),
            ("phase5_perf_compare", self.perf_fixture),
            ("no_fallback_final", self.no_fallback),
            ("phase4_non_regression", self.phase4 if self.full else None),
            ("verify_wave", self.wave if self.full else None),
        )


def synthetic_frames(encode_ns: int) -> str:
    lines = []
    for index, scene in enumerate(fw.CORE_SCENES, start=1):
        pre_create = (1_000_000, 12_000_000, 2_000_000, 110_000_000)[index - 1]
        pipeline = pre_create + encode_ns + 2_500_000
        lines.append(json.dumps({
            "type": "frame", "frame_seq": index, "scene_id": scene, "ring_id": 0,
            "t_vsync_prev_ns": index * fw.BUDGET_NS, "pre_create_ns": pre_create,
            "encode_ns": encode_ns, "submit_ns": 250_000, "gpu_exec_ns": 2_000_000,
            "wait_sync_ns": 250_000, "scanout_margin_ns": fw.BUDGET_NS - pipeline,
            "physical_frame_interval_ns": pipeline, "preempted_count": 0,
            "wfi_count": encode_ns // 1_000_000, "cache_flush_count": 0, "heap_allocs": 0,
        }))
    return "\n".join(lines) + "\n"


def result_base(mode: str, with_clang: bool) -> dict:
    return {"status": "FAIL", "phase5_host_ready": False, **perf.evidence_fields(),
            "mode": mode, "with_clang": with_clang, "source_evidence_kind": "host_suites",
            "results": [], "deferred_siblings": []}


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path, mode: str = "quick",
             with_clang: bool = False, *, out_json: str | Path | None = None) -> dict:
    if mode not in {"quick", "full"} or type(with_clang) is not bool:
        raise GateError("invalid gate mode or Clang option")
    cache = perf.checked_output_path(cache_dir, directory=True)
    summary = perf.checked_output_path(cache / "phase5_host_summary.json")
    requested = perf.checked_output_path(out_json) if out_json is not None else summary
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    if not repo.is_dir() or not mesa.is_dir():
        raise GateError("repository or Mesa directory is missing")
    cache.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    run = HostRun(repo, mesa, output, mode, with_clang)
    report = result_base(mode, with_clang)
    report.update(repo=str(repo), mesa=str(mesa), cache_dir=str(cache), run_dir=str(output), summary=str(summary))

    for name, action in run.stages():
        row = {"name": name, "status": "PASS", "commands": [], **perf.evidence_fields()}
        report["results"].append(row)
        if action is None:
            row["status"] = SKIPPED
            continue
        try:
            action(row)
        except (OSError, ValueError, RuntimeError, KeyError, TypeError, IndexError) as exc:
            row.update(status="FAIL", error=str(exc))
        if row["status"] == DEFERRED:
            report["deferred_siblings"].append(row["sibling"]["name"])

    report["status"] = "PASS" if all(row["status"] in {"PASS", DEFERRED, SKIPPED}
                                     for row in report["results"]) else "FAIL"
    report["phase5_host_ready"] = mode == "full" and all(row["status"] == "PASS" for row in report["results"])
    payload = perf.json_text(report)
    perf.write_output(summary, payload)
    if requested != summary:
        perf.write_output(requested, payload)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--mesa", required=True, type=Path)
    parser.add_argument("--mode", choices=("quick", "full"), default="quick")
    parser.add_argument("--with-clang", action="store_true")
    parser.add_argument("--cache-dir", required=True, type=Path)
    parser.add_argument("--out-json", type=Path)
    args = parser.parse_args(argv)
    try:
        report = run_gate(args.repo, args.mesa, args.cache_dir, args.mode, args.with_clang, out_json=args.out_json)
    except (OSError, ValueError, RuntimeError) as exc:
        report = result_base(args.mode, args.with_clang)
        report["error"] = str(exc)
    concise = dict(report, results=[{**{key: value for key, value in row.items() if key != "commands"},
                                     "command_count": len(row["commands"])} for row in report["results"]])
    sys.stdout.write(perf.json_text(concise))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
