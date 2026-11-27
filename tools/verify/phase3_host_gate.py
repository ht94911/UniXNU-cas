#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run Phase 3 host suites with the pinned Mesa PM4 vocabulary.

Quick runs the dedicated Phase 3 suites and skips the redundant Wave sweep.
Full also runs verify_wave. --with-clang adds Clang 18 ASan/UBSan to the C/C++
suites in either mode. Absent Wave 6 sibling suites are reported individually;
a present suite must pass. All reports describe host evidence only.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from types import FunctionType

from check_contracts import GateError, cache_directory, cache_environment, write_json
from corpus_audit import read_pack
import hwtest_parser
from phase2_host_gate import build_test, c_sources, execute, fresh_output, json_pass, source_file
from verify_wave import SUITES, CLANG_SUITES

KMD = "drivers/Adreno840Kernel"
CORE = "drivers/Adreno840Metal/core"
EMIT = "kits/Adreno840Emit"
PACK = "adreno_compile/pack"
APP = "apps/NezhaGpuTest"
WAVE6_KMD = f"{KMD}/test/test_kmd_phase3_promise_display.cpp"
WAVE6_PACK = f"{PACK}/test_pack_sds_preencode.cpp"
WAVE6_RUNTIME = f"{CORE}/test/test_phase3_runtime_integration.cpp"
CASES = ("gpu.ca.sweep", "ui.glass_live")
PACK_SOURCES = [f"{PACK}/a840_pack_v1.c", "adreno_compile/src/adreno_compile_stub.c"]
CHIP = "kits/Adreno840/chip_facts_a840.c"
CONTEXT = ["kits/Adreno840/a840_context_init.c", "kits/Adreno840/a840_abi_v3_host.c"]


def require_samples(report: dict, count: int) -> None:
    records = report.get("records", [])
    if (report.get("status") != "PASS" or report.get("fallback_count") != 0 or
            len(records) != count or report.get("measurement_kind") != "CPU_REFERENCE_FIXTURE" or
            report.get("hardware_acceptance") is not False or
            any(record.get("note") != "cpu_reference_only" or record.get("path") != "raw" for record in records)):
        raise GateError("HWTEST_CPU_POLICY_FAILED: " + json.dumps(report.get("errors", [])))


def check_ca_glass_samples(lines, predicates: set[str]) -> dict:
    # Reuse the frozen parser with a private vocabulary, as the Phase 3 policy does.
    grammar = FunctionType(hwtest_parser.parse_hwtest.__code__,
                           dict(hwtest_parser.parse_hwtest.__globals__,
                                CASES=CASES, TOLERANCES=dict.fromkeys(CASES, 1.0)))
    report = grammar(lines, predicates, "phase2", False)
    require_samples(report, len(CASES))
    return report


def check_wave_report(report: dict, with_clang: bool) -> dict:
    required = {"py_compile", "contracts", "driver_constants", "unittest", "pm4_xml_index",
                "pm4_decode", "no_fallback", *SUITES}
    if with_clang:
        required.update(f"{suite}:clang18" for suite in CLANG_SUITES)
    results = report.get("results") if isinstance(report, dict) else None
    if (not isinstance(report, dict) or report.get("status") != "PASS" or
            report.get("with_clang") is not with_clang or
            not isinstance(results, list) or len(results) != len(required) or
            any(not isinstance(row, dict) or not isinstance(row.get("name"), str) for row in results)):
        raise GateError("VERIFY_WAVE_INCOMPLETE")
    statuses = {row["name"]: row.get("status") for row in results}
    if statuses.keys() != required or any(status != "PASS" for status in statuses.values()):
        raise GateError("VERIFY_WAVE_INCOMPLETE")
    return statuses


class HostRun:
    def __init__(self, repo: Path, mesa: Path, output: Path, with_clang: bool):
        self.repo, self.mesa, self.output = repo, mesa, output
        self.compilers = ("gcc", "clang18") if with_clang else ("gcc",)
        self.env = cache_environment(output)
        self.env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                        TSAN_OPTIONS="halt_on_error=1", A840_VERIFY_MESA=str(mesa))
        self.native_objects: dict[str, Path] = {}
        self.pack_sample: Path | None = None

    def tool(self, row: dict, script: str, *arguments) -> dict:
        command = [sys.executable, "-B", str(source_file(self.repo, f"tools/verify/{script}.py")),
                   *map(str, arguments)]
        return json_pass(execute(row, command, self.repo, self.env))

    def present(self, row: dict, test: str) -> bool:
        path = self.repo / test
        exists = path.exists() or path.is_symlink()
        row["optional_suite"] = {"test": test, "status": "PRESENT" if exists else "NOT_PRESENT"}
        return exists

    def build(self, row: dict, test: str, sources: list[str], compiler: str,
              sanitizer: str = "address,undefined") -> Path:
        cache = self.output / Path(test).stem
        if sanitizer == "thread":
            cache /= "tsan"
        binary = build_test(row, self.repo, cache, self.env, sources, test, compiler, sanitizer=sanitizer)
        if compiler == "gcc" and sanitizer == "address,undefined":
            for index, relative in enumerate(sorted(set(sources))):
                if relative.endswith(".c"):
                    self.native_objects.setdefault(relative, binary.parent / f"source-{index}.o")
        return binary

    def kmd(self, row: dict) -> None:
        base = [CHIP, f"{KMD}/a840_kmd_ubwc_surface.c", f"{KMD}/a840_kmd_tier0_pack.c"]
        suites = [(f"{KMD}/test/test_kmd_phase3_ubwc_pack.cpp", base)]
        if self.present(row, WAVE6_KMD):
            sources = base + ["kits/Adreno840/a840_abi_v3_host.c"] + [f"{KMD}/{name}.c" for name in (
                "a840_kmd_wait_any", "a840_kmd_fault_capture", "a840_kmd_writer_promise", "a840_kmd_display_handoff")]
            suites.append((WAVE6_KMD, sources))
        for test, sources in suites:
            for compiler in self.compilers:
                binary = self.build(row, test, sources, compiler)
                execute(row, [str(binary)], binary.parent, self.env)
            binary = self.build(row, test, sources, "gcc", "thread")
            execute(row, ["setarch", "x86_64", "-R", str(binary), "--tsan-stress", "20"],
                    binary.parent, self.env, timeout=600)
        row["tsan_rounds_per_suite"] = 20

    def pack(self, row: dict) -> None:
        test = f"{PACK}/test_pack_v1.cpp"
        for compiler in self.compilers:
            binary = self.build(row, test, PACK_SOURCES, compiler)
            output = cache_directory(binary.parent / "pack")
            execute(row, [str(binary), str(output)], binary.parent, self.env)
            if compiler == "gcc":
                self.pack_sample = output / "readonly.pack.bin"
        if self.present(row, WAVE6_PACK):
            sources = [*PACK_SOURCES, CHIP, *c_sources(self.repo, EMIT),
                       f"{PACK}/a840_pack_sds_preencode.c", "adreno_compile/cache/a840_record_io.c",
                       "adreno_compile/layout/a840_binding_plan.c"]
            for compiler in self.compilers:
                binary = self.build(row, WAVE6_PACK, sources, compiler)
                execute(row, [str(binary)], binary.parent, self.env)

    def gmem(self, row: dict) -> None:
        emit_sources = [CHIP, *CONTEXT, *c_sources(self.repo, EMIT)]
        runtime_sources = [*emit_sources, *c_sources(self.repo, CORE)]
        has_wave6 = self.present(row, WAVE6_RUNTIME)
        if has_wave6:
            runtime_sources += PACK_SOURCES
        suites = [
            (f"{EMIT}/test/test_emit_gmem.cpp", emit_sources, "--dump-gmem-pm4"),
            (f"{CORE}/test/test_render_pass_gmem.cpp", runtime_sources, "--dump-rp-pm4"),
        ]
        if has_wave6:
            suites.append((WAVE6_RUNTIME, runtime_sources, "--dump-ca-glass-pm4"))
        row["streams"] = []
        for test, sources, dump_flag in suites:
            samples = []
            for compiler in self.compilers:
                binary = self.build(row, test, sources, compiler)
                sample = fresh_output(binary.parent / "submitted.pm4")
                execute(row, [str(binary), dump_flag, str(sample)], binary.parent, self.env)
                pm4 = self.tool(row, "pm4_check", "--mesa", self.mesa, "--chip", "A8XX", "--format", "binary", sample)
                if pm4.get("chip") != "A8XX" or not pm4.get("packet_count") or not pm4.get("dwords"):
                    raise GateError("PM4_EMPTY_OR_WRONG_CHIP")
                payload = sample.read_bytes()
                samples.append(payload)
                row["streams"].append({"test": test, "compiler": compiler, "dwords": pm4["dwords"],
                                       "packets": pm4["packet_count"], "sha256": hashlib.sha256(payload).hexdigest()})
            if len(samples) == 2 and samples[0] != samples[1]:
                raise GateError("PM4_GCC_CLANG_MISMATCH: " + test)

    def pixel_hwtest(self, row: dict) -> None:
        ref = f"{APP}/ref/cpu_reference"
        suites = (("", "--emit-sample-hwtest"), ("_phase3", "--emit-sample-phase3-hwtest"),
                  ("_ca_glass", "--emit-ca-glass-hwtest"))
        predicates = source_file(self.repo, "contracts/DualPathPredicates.tsv")
        vocabulary = hwtest_parser.load_predicates(predicates)
        row["sample_counts"] = {}
        for compiler in self.compilers:
            samples = []
            for suffix, emit_flag in suites:
                sources = [f"{ref}.cpp"]
                if suffix:
                    sources.append(f"{ref}{suffix}.cpp")
                test = f"{APP}/test/test_cpu_reference{suffix}.cpp"
                binary = self.build(row, test, sources, compiler)
                path = fresh_output(binary.parent / "sample.hwtest")
                execute(row, [str(binary), emit_flag, str(path)], binary.parent, self.env)
                samples.append(path)
            phase2, phase3, ca_glass = samples
            with ca_glass.open(encoding="utf-8") as source:
                check_ca_glass_samples(source, vocabulary)
            joint = fresh_output(phase3.parent / "joint.hwtest")
            joint.write_bytes(phase2.read_bytes() + phase3.read_bytes())
            for path, count, args in ((phase3, 6, ()), (joint, 14, ("--include-phase2",))):
                report = self.tool(row, "hwtest_phase3_policy", "--predicates", predicates, "--input", path, *args)
                require_samples(report, count)
                if report.get("device_verified") is not False or report.get("phase3_exit_pass") is not True:
                    raise GateError("HWTEST_PHASE3_EVIDENCE")
            row["sample_counts"][compiler] = {"phase3": 6, "ca_glass": 2, "joint_phase2_phase3": 14}

    def corpus(self, row: dict) -> None:
        if self.pack_sample is None:
            raise GateError("MISSING_GENERATED_PACK_SAMPLE")
        pack = read_pack(self.pack_sample)
        if pack.tier != 1 or not pack.keys:
            raise GateError("PACK_FIXTURE_REQUIRES_TIER1")
        present = min(pack.keys)
        frequent = hashlib.sha256(b"w06-frequent-miss").hexdigest()
        expensive = hashlib.sha256(b"w06-expensive-miss").hexdigest()
        corpus = fresh_output(self.output / "corpus.log")
        fixture = [
            json.dumps({"pipeline_key_sha256": present, "hit_tier": 1, "compile_us": 4, "process_kind": "app"}),
            f"pipeline_key_sha256={present} hit_tier=tier0 compile_us=6 process_kind=render_server",
            f"pipeline_key_sha256={frequent} hit_tier=miss compile_us=5 process_kind=app",
            f"pipeline_key_sha256={frequent} hit_tier=miss compile_us=7 process_kind=app",
            f"pipeline_key_sha256={expensive} hit_tier=miss compile_us=38 process_kind=app",
        ]
        corpus.write_text("\n".join(fixture) + "\n", encoding="ascii")
        output = fresh_output(self.output / "corpus_audit.json")
        report = self.tool(row, "corpus_audit", "--pack", self.pack_sample, "--corpus", corpus,
                           "--json-out", output, "--top-n", 2)
        expected = {"tier0_hits": 1, "tier1_hits": 1, "tier0_hit_rate": 0.2, "tier1_hit_rate": 0.2,
                    "record_count": 5, "cold_compile_misses": 3, "cold_compile_us": 50,
                    "cold_compile_time_share": 50 / 60}
        ranks = {"top_misses_by_count": [frequent, expensive], "top_misses_by_compile_us": [expensive, frequent]}
        if (report != json.loads(output.read_text(encoding="utf-8")) or
                any(report.get(name) != value for name, value in expected.items()) or
                any([item["pipeline_key_sha256"] for item in report.get(name, [])] != keys
                    for name, keys in ranks.items()) or report["pack"]["file_sha256"] != pack.sha256 or
                report["pack"]["highest_bank_bit"] != 16):
            raise GateError("CORPUS_REAL_FIXTURE_MISMATCH")
        row["sample_report"] = str(output)
        row["records"] = report["record_count"]

    def no_fallback(self, row: dict) -> None:
        artifacts = [arg for path in self.native_objects.values() for arg in ("--artifact", path)]
        report = self.tool(row, "no_fallback_test", "--profile", "final", *artifacts)
        if report.get("profile") != "final" or not report.get("symbols_scanned"):
            raise GateError("EMPTY_FINAL_SYMBOL_SCAN")
        row["symbols_scanned"] = report["symbols_scanned"]

    def wave(self, row: dict, with_clang: bool) -> None:
        output = cache_directory(self.output / "wave")
        summary = fresh_output(output / "verify_wave_summary.json")
        command = [sys.executable, "-B", str(source_file(self.repo, "tools/verify/verify_wave.py")),
                   "--repo", str(self.repo), "--mesa", str(self.mesa), "--cache-dir", str(output)]
        if with_clang:
            command.append("--with-clang")
        execute(row, command, self.repo, self.env, timeout=1800)
        row["subgates"] = check_wave_report(json.loads(summary.read_text(encoding="utf-8")), with_clang)
        row["summary"] = str(summary)


def result_base(mode: str, with_clang: bool) -> dict:
    return {"status": "FAIL", "mode": mode, "with_clang": with_clang, "phase3_host_ready": False,
            "device_verified": False, "hardware_acceptance": False, "results": []}


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path,
             mode: str = "quick", with_clang: bool = False) -> dict:
    if mode not in {"quick", "full"}:
        raise GateError("INVALID_PHASE3_GATE_MODE")
    if Path(repo).is_symlink():
        raise GateError("SYMLINK_REPOSITORY")
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    if not repo.is_dir():
        raise GateError("MISSING_REPOSITORY")
    cache = cache_directory(cache_dir)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    run = HostRun(repo, mesa, output, with_clang)
    report = result_base(mode, with_clang)
    report.update(repo=str(repo), cache_dir=str(cache), summary=str(cache / "phase3_host_summary.json"))

    def stage(name, action):
        row = {"name": name, "status": "PASS", "commands": []}
        report["results"].append(row)
        try:
            action(row)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            row.update(status="FAIL", error=str(exc))

    stage("contracts", lambda row: run.tool(row, "check_contracts", "--repo", repo,
                                            "--cache-dir", output / "contracts"))
    stage("driver_constants", lambda row: run.tool(row, "driver_constants_test", "--root", repo))
    stage("kmd_phase3", run.kmd)
    stage("pack_v1", run.pack)
    stage("gmem", run.gmem)
    stage("pixel_hwtest", run.pixel_hwtest)
    stage("corpus_audit", run.corpus)
    stage("no_fallback", run.no_fallback)
    if mode == "full":
        stage("verify_wave", lambda row: run.wave(row, with_clang))

    report["phase3_host_ready"] = all(row["status"] == "PASS" for row in report["results"])
    report["status"] = "PASS" if report["phase3_host_ready"] else "FAIL"
    report["verify_wave_skipped"] = mode == "quick"
    # Publish a complete report after the last gate and keep concurrent runs isolated.
    target = write_json(output / "phase3_host_summary.json", report)
    target.replace(cache / target.name)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("quick", "full"), default="quick")
    parser.add_argument("--with-clang", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = run_gate(args.repo, args.mesa, args.cache_dir, args.mode, args.with_clang)
    except (OSError, ValueError, RuntimeError) as exc:
        report = result_base(args.mode, args.with_clang)
        report["error"] = str(exc)
    report["results"] = [{**{key: value for key, value in row.items() if key != "commands"},
                          "command_count": len(row["commands"])} for row in report["results"]]
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["phase3_host_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
