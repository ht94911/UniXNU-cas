#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the Phase 4 host exit gate without certifying or operating a device.

Quick runs all mandatory Wave 7/8 host suites and reports absent Wave 8
siblings as deferred_sibling_not_present. Present siblings must pass.
--with-clang adds Clang 18 ASan/UBSan and strict GCC/Clang PM4 comparisons.
Full additionally runs the two Wave 7 TSan stresses and verify_wave.
All generated files use .cache/w08-l4; all source and Mesa inputs are read only.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
from types import FunctionType

from check_contracts import GateError, run_command
from hwtest_phase4_policy import (CACHE_ROOT, cache_directory, cache_environment,
                                 checked_output_path, evidence_fields, parse_hwtest,
                                 vocabulary, write_json)
import fallback_audit
from phase2_host_gate import (INCLUDES, STRICT_FLAGS, c_sources, execute, json_pass,
                             source_file)
from phase3_host_gate import check_wave_report
from verify_wave import clang_configuration
import verify_wave

KMD = "drivers/Adreno840Kernel"
CORE = "drivers/Adreno840Metal/core"
EMIT = "kits/Adreno840Emit"
SERVICE = "adreno_compile/service"
APP = "apps/NezhaGpuTest"
CHIP = "kits/Adreno840/chip_facts_a840.c"
CONTEXT = ["kits/Adreno840/a840_context_init.c", "kits/Adreno840/a840_abi_v3_host.c"]
PACK_SOURCES = ["adreno_compile/pack/a840_pack_v1.c", "adreno_compile/src/adreno_compile_stub.c"]
KMD_TEST = f"{KMD}/test/test_kmd_phase4_memory.cpp"
SERVICE_TEST = f"{SERVICE}/test_shader_compiler_service.cpp"
RUNTIME_TEST = f"{CORE}/test/test_phase4_heap_argbuf_icb.cpp"
W08_KMD = f"{KMD}/test/test_kmd_phase4_quota_recovery.cpp"
W08_PACK = "tools/seed/test_phase4_pack_closure.py"
W08_PACK_ALTERNATE = "tools/verify/test_phase4_pack_closure.py"
W08_RUNTIME = f"{CORE}/test/test_phase4_msaa_tess_camera.cpp"
KMD_SOURCES = [CONTEXT[1], *(f"{KMD}/{name}.c" for name in
                ("a840_kmd_zero_page", "a840_kmd_sparse_va", "a840_kmd_bo_pool"))]
SERVICE_SOURCES = [f"{SERVICE}/a840_shader_compiler_service.c",
                   f"{SERVICE}/a840_shader_compiler_client.c", "adreno_compile/cache/a840_record_io.c",
                   "adreno_compile/src/adreno_compile_stub.c"]
W08_KMD_FILES = [W08_KMD, *(f"{KMD}/a840_kmd_{stem}.{ext}" for stem in
                           ("quota_recovery", "ca_env_policy") for ext in ("h", "c"))]
W08_PACK_FILES = [f"{SERVICE}/a840_shader_compiler_ir3_backend.{ext}" for ext in ("h", "c")]
W08_PACK_FILES += ["daemon/NezhaShaderCompiler/main.c", "tools/seed/phase4_pack_closure.py"]
W08_RUNTIME_FILES = [W08_RUNTIME, *(f"{EMIT}/a840_emit_msaa_tess_vis.{ext}" for ext in ("h", "c")),
                     *(f"{CORE}/a840_phase4_pipeline.{ext}" for ext in ("h", "c"))]
BASE_STAGES = (
    "contracts", "driver_constants", "kmd_phase4_memory", "kmd_phase4_quota_recovery",
    "shader_compiler_service", "compiler_phase4_pack_closure", "runtime_phase4_heap_argbuf_icb",
    "runtime_phase4_msaa_tess_camera", "cpu_phase4_hwtest", "fallback_audit", "no_fallback",
)
WAVE_UNITTEST_TIMEOUT = 900


def wave_cli(argv: list[str] | None = None) -> int:
    """Run the unchanged Wave CLI with a bounded full-discovery time budget.

    Its 300s default predates the Phase 4 suites. Clone only the runner's
    global namespace, replacing command_gate for this invocation; no source,
    imported module global, test list, or assertion is changed. Other child
    time budgets retain their existing values.
    """
    def bounded_command_gate(name, commands, *, cwd, env, timeout=300):
        return verify_wave.command_gate(name, commands, cwd=cwd, env=env,
                                        timeout=WAVE_UNITTEST_TIMEOUT if name == "unittest" else timeout)

    runner = FunctionType(verify_wave.verify_wave.__code__,
                          dict(verify_wave.verify_wave.__globals__, command_gate=bounded_command_gate),
                          "phase4_bounded_wave", verify_wave.verify_wave.__defaults__)
    entry = FunctionType(verify_wave.main.__code__, dict(verify_wave.main.__globals__, verify_wave=runner),
                         "phase4_bounded_wave_cli", verify_wave.main.__defaults__)
    return entry(argv)


def fresh_output(path: Path) -> Path:
    target = checked_output_path(path)
    cache_directory(target.parent)
    if target.exists():
        if target.stat().st_nlink != 1:
            raise GateError("HARDLINK_OUTPUT_FORBIDDEN")
        target.unlink()
    return target


def require_samples(report: dict, policy: str) -> None:
    required = vocabulary(policy)
    if (not isinstance(report, dict) or report.get("status") != "PASS" or
            report.get("host_policy_passed") is not True or report.get("policy") != policy or
            report.get("fallback_count") != 0 or report.get("required_case_count") != len(required) or
            report.get("required_cases") != list(required) or
            report.get("device_verified") is not False or report.get("hardware_acceptance") is not False or
            report.get("measurement_kind") != "CPU_REFERENCE_FIXTURE"):
        raise GateError("HWTEST_CPU_POLICY_FAILED")
    records = report.get("records")
    if (not isinstance(records, list) or len(records) != len(required) or
            any(not isinstance(record, dict) or record.get("note") != "cpu_reference_only" or
                record.get("path") != "raw" or record.get("status") != "PASS" for record in records) or
            {record["case"] for record in records} != set(required)):
        raise GateError("HWTEST_CPU_COVERAGE_FAILED")


def require_pm4(report: dict) -> None:
    if (not isinstance(report, dict) or report.get("status") != "PASS" or report.get("chip") != "A8XX" or
            type(report.get("packet_count")) is not int or report["packet_count"] <= 0 or
            type(report.get("dwords")) is not int or report["dwords"] <= 0):
        raise GateError("PM4_EMPTY_OR_WRONG_CHIP")


def require_pm4_diff(report: dict) -> None:
    if (not isinstance(report, dict) or report.get("status") != "PASS" or report.get("chip") != "A8XX" or
            report.get("comparison") != "packets" or report.get("first_difference") is not None or
            not report.get("expected_packets") or
            report.get("expected_packets") != report.get("actual_packets") or
            report.get("expected_operations") != report.get("actual_operations") or
            report.get("ignored_register_values") != [] or report.get("ignored_opcode_payloads") != []):
        raise GateError("PM4_STRICT_DIFF_FAILED")


def require_audit(report: dict, count: int) -> None:
    if (not isinstance(report, dict) or report.get("status") != "PASS" or
            report.get("zero_fallback_passed") is not True or report.get("fallback_count") != 0 or
            report.get("unknown_predicate_count") != 0 or report.get("counts_complete") is not True or
            report.get("cpu_reference_samples") != count or report.get("hwtest_samples") != count or
            report.get("runtime_submissions") != 0 or report.get("zero_fallback_procs") != ["NezhaGpuTest"] or
            report.get("device_verified") is not False or report.get("hardware_acceptance") is not False):
        raise GateError("FALLBACK_AUDIT_HOST_FIXTURE_FAILED")


class HostRun:
    def __init__(self, repo: Path, mesa: Path, output: Path, with_clang: bool):
        self.repo, self.mesa, self.output = repo, mesa, cache_directory(output)
        self.compilers = ("gcc", "clang18") if with_clang else ("gcc",)
        self.env = cache_environment(output)
        self.env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                        TSAN_OPTIONS="halt_on_error=1", A840_VERIFY_MESA=str(mesa),
                        A840_PHASE4_CACHE_DIR=str(output), CARGO_BUILD_JOBS="32")
        self.objects: dict[tuple[str, str, str], Path] = {}
        self.native_objects: dict[str, Path] = {}
        self.full_samples: Path | None = None
        self.toolchains: dict[str, tuple[str, str, list[str]]] = {}

    def tool(self, row: dict, script: str, *arguments) -> dict:
        command = [sys.executable, "-B", str(source_file(self.repo, f"tools/verify/{script}.py")),
                   *map(str, arguments)]
        return json_pass(execute(row, command, self.repo, self.env, timeout=600))

    def present(self, row: dict, name: str, paths: list[str], tests: tuple[str, ...]) -> str | None:
        present = [relative for relative in paths if (self.repo / relative).exists() or
                   (self.repo / relative).is_symlink()]
        row["sibling"] = {"name": name, "paths": paths, "present_paths": present}
        if not present:
            row.update(status="deferred_sibling_not_present")
            row["sibling"]["status"] = "deferred_sibling_not_present"
            return None
        matches = [relative for relative in tests if relative in present]
        if len(matches) != 1:
            raise GateError("SIBLING_PARTIAL_OR_AMBIGUOUS: " + name)
        for relative in paths:
            if relative not in tests or relative == matches[0]:
                source_file(self.repo, relative)
        row["sibling"].update(status="present_required", test=matches[0])
        return matches[0]

    def compiler(self, row: dict, name: str) -> tuple[str, str, list[str]]:
        if name not in self.toolchains:
            cc, cxx, extra = clang_configuration() if name == "clang18" else ("gcc-13", "g++-13", [])
            for program in (cc, cxx):
                if not shutil.which(program):
                    raise GateError("REQUIRED_COMPILER_NOT_FOUND")
                result = execute(row, [program, "-dumpversion"], self.repo, self.env)
                if result["stdout"].strip().split(".")[0] != ("18" if name == "clang18" else "13"):
                    raise GateError("REQUIRED_COMPILER_VERSION_MISMATCH")
            self.toolchains[name] = (cc, cxx, extra)
        return self.toolchains[name]

    def build(self, row: dict, test: str, sources: list[str], compiler: str,
              sanitizer: str = "address,undefined") -> Path:
        cc, cxx, extra = self.compiler(row, compiler)
        output = cache_directory(self.output / Path(test).stem / compiler / sanitizer.replace(",", "-"))
        objects_dir = cache_directory(self.output / "objects" / compiler / sanitizer.replace(",", "-"))
        includes = (*INCLUDES, SERVICE, "adreno_compile/pack", "adreno_compile/layout")
        flags = [*STRICT_FLAGS, "-g", "-fsanitize=" + sanitizer, *extra,
                 *(item for relative in includes if (self.repo / relative).is_dir()
                   for item in ("-I", str(self.repo / relative)))]
        # Match the existing ASan suite recipe. Clang -O1 inlines the many
        # large KMD case-local fixtures into one stack frame; TSan uses the
        # established optimized stress recipe separately.
        if sanitizer == "thread":
            flags.append("-O1")
        pending, selected = [], []
        for relative in sorted(set(sources)):
            source = source_file(self.repo, relative)
            key = (compiler, sanitizer, relative)
            if key in self.objects:
                selected.append(self.objects[key])
                continue
            identity = hashlib.sha256(relative.encode()).hexdigest()[:16]
            target = fresh_output(objects_dir / (source.stem + "-" + identity + ".o"))
            program, standard = (cc, "c11") if source.suffix == ".c" else (cxx, "c++17")
            command = [program, "-std=" + standard, *flags, "-c", str(source), "-o", str(target)]
            pending.append((key, target, command))
            selected.append(target)
        # Each translation unit has its own output. Preserve source-order logs
        # and collect all visible compiler failures before refusing the link.
        with ThreadPoolExecutor(max_workers=32) as pool:
            jobs = [pool.submit(run_command, command, cwd=output, env=self.env, timeout=300)
                    for _, _, command in pending]
            outcomes = [job.result() for job in jobs]
        row["commands"].extend(outcomes)
        if any(result["returncode"] for result in outcomes):
            raise GateError("NATIVE_SOURCE_COMPILE_FAILED")
        for (key, target, _), result in zip(pending, outcomes):
            if result["returncode"] == 0:
                self.objects[key] = target
                if compiler == "gcc" and sanitizer == "address,undefined" and key[2].endswith(".c"):
                    self.native_objects.setdefault(key[2], target)
        target = fresh_output(output / "test")
        execute(row, [cxx, "-std=c++17", *flags, str(source_file(self.repo, test)),
                      *map(str, selected), "-ldl", "-lm", "-no-pie", "-o", str(target)], output, self.env)
        return target

    def run_native(self, row: dict, test: str, sources: list[str]) -> None:
        for compiler in self.compilers:
            binary = self.build(row, test, sources, compiler)
            execute(row, [str(binary)], binary.parent, self.env, timeout=600)

    def kmd(self, row: dict) -> None:
        self.run_native(row, KMD_TEST, KMD_SOURCES)

    def kmd_sibling(self, row: dict) -> None:
        test = self.present(row, "W08-L1", W08_KMD_FILES, (W08_KMD,))
        if test:
            self.run_native(row, test, [*KMD_SOURCES, f"{KMD}/a840_kmd_fault_capture.c",
                                        *(relative for relative in W08_KMD_FILES
                                          if relative.endswith(".c"))])

    def service(self, row: dict) -> None:
        self.run_native(row, SERVICE_TEST, SERVICE_SOURCES)

    def pack_sibling(self, row: dict) -> None:
        test = self.present(row, "W08-L2", [W08_PACK, W08_PACK_ALTERNATE, *W08_PACK_FILES],
                            (W08_PACK, W08_PACK_ALTERNATE))
        if test:
            # This test lives in tools/seed on the actual W08-L2 task card.
            # Discovery requires at least one executed test, not just exit 0.
            result = execute(row, [sys.executable, "-B", "-m", "unittest", "discover",
                                  "-s", str(source_file(self.repo, test).parent),
                                  "-p", Path(test).name, "-v"], self.repo, self.env, timeout=600)
            import re
            count = re.search(r"\bRan ([1-9][0-9]*) tests?\b", result["stderr"] + result["stdout"])
            if not count:
                raise GateError("SIBLING_TEST_DISCOVERY_EMPTY")
            row["tests_run"] = int(count.group(1))

    def runtime_sources(self) -> list[str]:
        return [CHIP, *CONTEXT, *c_sources(self.repo, EMIT), *c_sources(self.repo, CORE),
                *PACK_SOURCES, *SERVICE_SOURCES]

    def runtime(self, row: dict) -> None:
        self.run_streams(row, RUNTIME_TEST,
                         (("--dump-indirect-pm4", "indirect.pm4"), ("--dump-icb-pm4", "icb.pm4")))

    def runtime_sibling(self, row: dict) -> None:
        test = self.present(row, "W08-L3", W08_RUNTIME_FILES, (W08_RUNTIME,))
        if test:
            self.run_streams(row, test, (("--dump-msaa-vis-pm4", "msaa-vis.pm4"),
                                        ("--dump-tess-amp-pm4", "tess-amp.pm4"),
                                        ("--dump-camera-mps-pm4", "camera-mps.pm4")))

    def run_streams(self, row: dict, test: str, exports: tuple[tuple[str, str], ...]) -> None:
        samples: dict[str, list[Path]] = {name: [] for _, name in exports}
        row["streams"], row["comparisons"] = [], []
        for compiler in self.compilers:
            binary = self.build(row, test, self.runtime_sources(), compiler)
            paths = [fresh_output(binary.parent / name) for _, name in exports]
            command = [str(binary), *(part for (flag, _), path in zip(exports, paths)
                                      for part in (flag, str(path)))]
            execute(row, command, binary.parent, self.env, timeout=600)
            for (_, name), path in zip(exports, paths):
                report = self.tool(row, "pm4_check", "--mesa", self.mesa, "--chip", "A8XX",
                                   "--format", "binary", path)
                require_pm4(report)
                samples[name].append(path)
                row["streams"].append({"name": name, "compiler": compiler, "path": str(path),
                                       "dwords": report["dwords"], "packets": report["packet_count"],
                                       "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        for name, paths in samples.items():
            cross = len(paths) == 2
            report = self.tool(row, "pm4_diff", "--mesa", self.mesa, "--chip", "A8XX",
                               "--format", "binary", "--strict-packets", "--expected", paths[0],
                               "--actual", paths[-1])
            require_pm4_diff(report)
            if cross and paths[0].read_bytes() != paths[1].read_bytes():
                raise GateError("PM4_GCC_CLANG_BYTE_MISMATCH")
            row["comparisons"].append({"name": name, "status": "PASS", "strict_packets": True,
                                       "gcc_clang_compared": cross,
                                       "scope": "gcc_clang" if cross else "single_compiler_parse_validation"})

    def pixel_hwtest(self, row: dict) -> None:
        predicates = source_file(self.repo, "contracts/DualPathPredicates.tsv")
        catalog = set(fallback_audit.load_predicates(predicates))
        suites = (("", 8), ("_phase3", 6), ("_phase4", 5), ("_phase4_ext", 5))
        row["sample_counts"], row["sample_reports"] = {}, []
        for compiler in self.compilers:
            samples = []
            for suffix, count in suites:
                prefix = f"{APP}/ref/cpu_reference"
                sources = [prefix + ".cpp"] + ([prefix + suffix + ".cpp"] if suffix else [])
                binary = self.build(row, f"{APP}/test/test_cpu_reference{suffix}.cpp", sources, compiler)
                sample = fresh_output(binary.parent / "sample.hwtest")
                command = [str(binary)]
                if not suffix:
                    command += ["--emit-sample-hwtest", str(sample)]
                elif suffix == "_phase3":
                    command += ["--emit-sample-phase3-hwtest", str(sample)]
                elif suffix == "_phase4_ext":
                    command += ["--emit-phase4-ext-hwtest", str(sample)]
                outcome = execute(row, command, binary.parent, self.env, timeout=600)
                # The frozen W07 exporter only accepts w07-l4 output paths.
                # Its stdout contains the same five samples, so no old file or
                # global output guard is changed to collect them under w08-l4.
                lines = (sample.read_text(encoding="utf-8") if suffix in {"", "_phase3"} else
                         "".join(line + "\n" for line in outcome["stdout"].splitlines()
                                 if line.startswith("HWTEST ")))
                if len(lines.splitlines()) != count:
                    raise GateError("CPU_SAMPLE_EXPORT_COUNT")
                if suffix == "_phase4_ext" and sample.read_text(encoding="utf-8") != lines:
                    raise GateError("CPU_SAMPLE_EXPORT_MISMATCH")
                sample.write_text(lines, encoding="ascii")
                samples.append(lines)
            for policy, text in (("phase4", "".join(samples[2:])), ("full_system", "".join(samples))):
                report = parse_hwtest(text, catalog, policy)
                require_samples(report, policy)
                joint = fresh_output(self.output / f"{compiler}-{policy}.hwtest")
                joint.write_text(text, encoding="ascii")
                report = self.tool(row, "hwtest_phase4_policy", "--predicates", predicates,
                                   "--policy", policy, "--input", joint)
                require_samples(report, policy)
                saved = write_json(self.output / f"{compiler}-{policy}-policy.json", report)
                row["sample_reports"].append(str(saved))
                if compiler == "gcc" and policy == "full_system":
                    self.full_samples = joint
            row["sample_counts"][compiler] = {"phase4": 10, "phase4_ext": 5, "full_system": 24}

    def audit(self, row: dict) -> None:
        if self.full_samples is None:
            raise GateError("MISSING_FULL_SYSTEM_CPU_SAMPLES")
        report = self.tool(row, "fallback_audit", "--input", self.full_samples,
                           "--predicates", source_file(self.repo, "contracts/DualPathPredicates.tsv"),
                           "--zero-fallback-procs", "NezhaGpuTest")
        require_audit(report, 24)
        row.update(cpu_reference_samples=24, runtime_submissions=0,
                   zero_fallback_procs=["NezhaGpuTest"], **evidence_fields())
        row["report"] = str(write_json(self.output / "fallback_audit.json", report))

    def no_fallback(self, row: dict) -> None:
        if not self.native_objects:
            raise GateError("EMPTY_NATIVE_OBJECT_SCAN")
        archive = fresh_output(self.output / "liba840_phase4_host.a")
        execute(row, ["ar", "rcs", str(archive), *map(str, self.native_objects.values())], self.output, self.env)
        report = self.tool(row, "no_fallback_test", "--profile", "final", "--artifact", archive)
        if report.get("profile") != "final" or not report.get("symbols_scanned"):
            raise GateError("EMPTY_FINAL_SYMBOL_SCAN")
        row.update(archive=str(archive), native_objects=len(self.native_objects),
                   symbols_scanned=report["symbols_scanned"])

    def tsan(self, row: dict, test: str, sources: list[str]) -> None:
        binary = self.build(row, test, sources, "gcc", "thread")
        execute(row, ["setarch", "x86_64", "-R", str(binary), "--tsan-stress", "20"],
                binary.parent, self.env, timeout=600)
        row["tsan_rounds"] = 20

    def wave(self, row: dict, with_clang: bool) -> None:
        output = cache_directory(self.output / "wave")
        summary = fresh_output(output / "verify_wave_summary.json")
        # Load the actual repo's unchanged Wave entrypoint through the new
        # bounded adapter, in a separate process with its own module globals.
        tools_dir = source_file(self.repo, "tools/verify/verify_wave.py").parent
        bootstrap = ("import sys; sys.path.insert(0, sys.argv.pop(1)); "
                     "from phase4_host_gate import wave_cli; raise SystemExit(wave_cli(sys.argv[1:]))")
        command = [sys.executable, "-B", "-c", bootstrap, str(tools_dir),
                   "--repo", str(self.repo), "--mesa", str(self.mesa), "--cache-dir", str(output)]
        if with_clang:
            command.append("--with-clang")
        outcome = run_command(command, cwd=self.repo, env=self.env, timeout=3600)
        row["commands"].append(outcome)
        if outcome["returncode"]:
            raise GateError("VERIFY_WAVE_COMMAND_FAILED")
        report = json.loads(summary.read_text(encoding="utf-8"))
        row["statuses"] = check_wave_report(report, with_clang)
        row["summary"] = str(summary)
        row["unittest_timeout_seconds"] = WAVE_UNITTEST_TIMEOUT


def result_base(mode: str, with_clang: bool) -> dict:
    return {"status": "FAIL", "phase4_host_ready": False, **evidence_fields(),
            "mode": mode, "with_clang": with_clang, "source_evidence_kind": "host_suites",
            "results": [], "deferred_siblings": [], "tsan_requested": mode == "full",
            "verify_wave_skipped": mode == "quick"}


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path,
             mode: str = "quick", with_clang: bool = False) -> dict:
    if mode not in {"quick", "full"} or type(with_clang) is not bool:
        raise GateError("INVALID_GATE_MODE_OR_CLANG_OPTION")
    # Reject every output path before starting any compiler or child suite.
    cache = cache_directory(cache_dir)
    summary = checked_output_path(cache / "phase4_host_summary.json")
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    if not repo.is_dir() or not mesa.is_dir():
        raise GateError("REPO_OR_MESA_DIRECTORY_MISSING")
    output = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    run = HostRun(repo, mesa, output, with_clang)
    report = result_base(mode, with_clang)
    report.update(repo=str(repo), cache_dir=str(cache), run_dir=str(output), summary=str(summary))

    def stage(name, action):
        row = {"name": name, "status": "PASS", "commands": [], **evidence_fields()}
        report["results"].append(row)
        try:
            action(row)
        except (OSError, ValueError, RuntimeError, KeyError, TypeError) as exc:
            # Child stdout/stderr are preserved in command records. Gate error
            # text is only an error code or a public source-relative path.
            row.update(status="FAIL", error=str(exc))

    stage("contracts", lambda row: run.tool(row, "check_contracts", "--repo", repo,
                                            "--cache-dir", output / "contracts"))
    stage("driver_constants", lambda row: run.tool(row, "driver_constants_test", "--root", repo))
    stage("kmd_phase4_memory", run.kmd)
    stage("kmd_phase4_quota_recovery", run.kmd_sibling)
    stage("shader_compiler_service", run.service)
    stage("compiler_phase4_pack_closure", run.pack_sibling)
    stage("runtime_phase4_heap_argbuf_icb", run.runtime)
    stage("runtime_phase4_msaa_tess_camera", run.runtime_sibling)
    stage("cpu_phase4_hwtest", run.pixel_hwtest)
    stage("fallback_audit", run.audit)
    stage("no_fallback", run.no_fallback)
    if mode == "full":
        stage("kmd_phase4_memory_tsan", lambda row: run.tsan(row, KMD_TEST, KMD_SOURCES))
        stage("shader_compiler_service_tsan", lambda row: run.tsan(row, SERVICE_TEST, SERVICE_SOURCES))
        stage("verify_wave", lambda row: run.wave(row, with_clang))
    report["deferred_siblings"] = [row["sibling"]["name"] for row in report["results"]
                                   if row["status"] == "deferred_sibling_not_present"]
    report["phase4_host_ready"] = all(row["status"] in {"PASS", "deferred_sibling_not_present"}
                                      for row in report["results"])
    report["status"] = "PASS" if report["phase4_host_ready"] else "FAIL"
    write_json(summary, report)
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--repo", required=True, type=Path)
    cli.add_argument("--mesa", required=True, type=Path)
    cli.add_argument("--cache-dir", required=True, type=Path)
    cli.add_argument("--mode", choices=("quick", "full"), default="quick")
    cli.add_argument("--with-clang", action="store_true")
    args = cli.parse_args(argv)
    try:
        report = run_gate(args.repo, args.mesa, args.cache_dir, args.mode, args.with_clang)
    except (OSError, ValueError, RuntimeError) as exc:
        report = result_base(args.mode, args.with_clang)
        report["error"] = str(exc) if isinstance(exc, GateError) else "GATE_INPUT_ERROR"
    # Full command output is retained in the on-disk summary, not repeated in
    # CLI output. Both formats keep the hardware flags fixed at false.
    concise = dict(report, results=[{**{key: value for key, value in row.items() if key != "commands"},
                                   "command_count": len(row["commands"])} for row in report["results"]])
    print(json.dumps(concise, sort_keys=True, indent=2))
    return 0 if report["phase4_host_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
