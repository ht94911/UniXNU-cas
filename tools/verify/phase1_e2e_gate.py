#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 1 host exit gate; dry-run readiness is separate from full completion.

Full mode runs the ir3 builder with --run-e2e and a bounded cache destination,
checks its archive with no_fallback, runs the registers generator in --check
mode, and checks nonempty emitter .pm4 samples. Existing test_emit_ops binaries
are reused with --dump-pm4. Samples may reside under kits/Adreno840Emit or in
this run's pm4 directory. <name>.golden.pm4 is an optional expected stream;
otherwise the oracle is explicitly SELF_CONSISTENCY, not a Turnip gold result.
All writes are confined to .cache/w02-l4*. No device or Git mutation commands.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path

from check_contracts import GateError, cache_environment, run_command
from stack_key import (compute_stack_key, header_bytes, check_header, wave2_cache_directory,
                       wave2_cache_path, write_bytes, write_report)

REQUIRED_SUITES = ("kits/Adreno840", "kits/Adreno840Emit", "adreno_compile", "translate")


def repo_file(repo: Path, relative: str) -> Path:
    path = repo / relative
    current = repo
    for part in Path(relative).parts:
        current /= part
        if current.is_symlink():
            raise GateError(f"symlinked gate input: {relative}")
    if not path.is_file():
        raise GateError(f"missing gate input: {relative}")
    return path


def command_json(command: dict) -> dict:
    if command["returncode"]:
        raise GateError(f"command failed (exit {command['returncode']}): {command['stderr'].strip()}")
    try:
        result = json.loads(command["stdout"])
    except ValueError as exc:
        raise GateError("missing or invalid JSON command report") from exc
    if not isinstance(result, dict) or result.get("status") != "PASS":
        raise GateError("subordinate JSON gate did not report PASS")
    return result


def execute(stage: dict, command: list[str], repo: Path, env: dict[str, str], timeout: int = 120) -> dict:
    result = run_command(command, cwd=repo, env=env, timeout=timeout)
    stage["commands"].append(result)
    if result["returncode"]:
        detail = result["stderr"].strip() or result["stdout"].strip()
        raise GateError(f"command failed (exit {result['returncode']}): {detail}")
    return result


def emitter_streams(repo: Path, cache: Path, wave_report: dict, stage: dict, env: dict[str, str]) -> list[Path]:
    output = wave2_cache_directory(cache / "pm4")
    for row in wave_report.get("results", []):
        suite_name = row.get("name")
        if suite_name not in {"kits/Adreno840Emit", "kits/Adreno840"} or row.get("status") != "PASS":
            continue
        for command in row.get("commands", []):
            words = command.get("command", [])
            if "-o" not in words:
                continue
            if suite_name == "kits/Adreno840Emit" and any(Path(word).name == "test_emit_ops.cpp" for word in words):
                binary = wave2_cache_path(words[words.index("-o") + 1])
                if not binary.is_file():
                    raise GateError("emitter test binary is missing")
                sample = output / "emitted.pm4"
                if sample.exists() or sample.is_symlink():
                    sample.unlink()
                execute(stage, [str(binary), "--dump-pm4", str(sample)], repo, env)
                if not sample.is_file():
                    raise GateError("PM4_SAMPLE_MISSING: emitter did not create the requested stream")
            elif suite_name == "kits/Adreno840Emit" and any(Path(word).name == "test_emit_3d_compute.cpp" for word in words):
                binary = wave2_cache_path(words[words.index("-o") + 1])
                if not binary.is_file():
                    raise GateError("3d/compute emitter test binary is missing")
                comp = output / "compute.pm4"
                gfx = output / "graphics3d.pm4"
                for p in (comp, gfx):
                    if p.exists() or p.is_symlink():
                        p.unlink()
                execute(stage, [str(binary), "--dump-compute-pm4", str(comp), "--dump-3d-pm4", str(gfx)], repo, env)
                if not comp.is_file() or not gfx.is_file():
                    raise GateError("PM4_SAMPLE_MISSING: 3d/compute emitter did not create requested streams")
            elif suite_name == "kits/Adreno840" and any(Path(word).name == "test_context_init_and_abi3.cpp" for word in words):
                binary = wave2_cache_path(words[words.index("-o") + 1])
                if not binary.is_file():
                    raise GateError("context_init test binary is missing")
                ctx = output / "context_init.pm4"
                if ctx.exists() or ctx.is_symlink():
                    ctx.unlink()
                execute(stage, [str(binary), "--dump-pm4", str(ctx)], repo, env)
                if not ctx.is_file():
                    raise GateError("PM4_SAMPLE_MISSING: context_init emitter did not create requested stream")
    streams = set()
    for directory in (repo / "kits/Adreno840Emit", output):
        for path in directory.rglob("*.pm4"):
            repo_file(repo, path.relative_to(repo).as_posix()) if path.is_relative_to(repo) else wave2_cache_path(path)
            if path.is_symlink():
                raise GateError(f"symlinked PM4 sample: {path}")
            if not path.name.endswith(".golden.pm4"):
                streams.add(path)
    if not streams:
        raise GateError("PM4_SAMPLE_MISSING: no emitted .pm4 stream")
    return sorted(streams)


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path,
             mode: str = "dry-run", with_clang: bool = False) -> dict:
    if mode not in {"dry-run", "full"}:
        raise GateError(f"unknown Phase 1 mode: {mode}")
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    cache = wave2_cache_directory(cache_dir)
    env = cache_environment(cache)
    env["A840_VERIFY_MESA"] = str(mesa)
    env["A840_PHASE1_CACHE_ROOT"] = str(cache)
    report = {"status": "PASS", "mode": mode, "with_clang": with_clang, "phase1_complete": False,
              "repo": str(repo), "mesa": str(mesa), "cache_dir": str(cache), "results": []}

    def stage(name: str) -> dict:
        result = {"name": name, "status": "PASS", "commands": []}
        report["results"].append(result)
        return result

    def fail(row: dict, exc: Exception) -> None:
        row.update(status="FAIL", error=str(exc))

    key_stage = stage("stack_key")
    try:
        key = compute_stack_key(repo)
        repeated = compute_stack_key(repo)
        if key != repeated:
            raise GateError("STACK_KEY_NONDETERMINISTIC: consecutive source snapshots differ")
        write_report(cache / "stack_key.json", key)
        write_bytes(cache / "A840StackKey.h", header_bytes(key))
        check_header(cache / "A840StackKey.h", repeated)
        key_stage.update(stack_key_hex=key["stack_key_hex"], file_count=key["file_count"],
                         stack_digest_sha256=key["stack_digest_sha256"], repeat_equal=True)
    except (GateError, OSError, ValueError) as exc:
        fail(key_stage, exc)
        report["status"] = "FAIL"
        write_report(cache / "phase1_e2e_summary.json", report)
        return report

    wave_stage = stage("verify_wave")
    wave_report = {}
    try:
        wave_summary = wave2_cache_path(cache / "wave/verify_wave_summary.json")
        if wave_summary.exists() or wave_summary.is_symlink():
            wave_summary.unlink()
        command = [sys.executable, str(repo_file(repo, "tools/verify/verify_wave.py")), "--repo", str(repo),
                   "--mesa", str(mesa), "--cache-dir", str(cache / "wave")]
        if with_clang:
            command.append("--with-clang")
        execute(wave_stage, command, repo, env, timeout=1800)
        wave_report = json.loads(wave_summary.read_text(encoding="utf-8"))
        if not isinstance(wave_report, dict) or wave_report.get("status") != "PASS" or \
                not isinstance(wave_report.get("results"), list) or \
                any(not isinstance(row, dict) for row in wave_report["results"]):
            raise GateError("VERIFY_WAVE_FAILED: invalid host-suite report")
        suites = {row.get("name"): row.get("status") for row in wave_report["results"]}
        if mode == "full" and (any(status != "PASS" for status in suites.values()) or
                                any(suites.get(name) != "PASS" for name in REQUIRED_SUITES)):
            raise GateError("VERIFY_WAVE_FAILED: required suites did not all pass")
        wave_stage["summary"] = str(cache / "wave/verify_wave_summary.json")
    except (GateError, OSError, ValueError) as exc:
        fail(wave_stage, exc)

    ir3_stage = stage("adreno_ir3_e2e")
    builder = repo / "adreno_compile/tools/build_adreno_compile_ir3.py"
    try:
        if not builder.exists() and not builder.is_symlink():
            if mode == "full":
                raise GateError("IR3_BACKEND_MISSING: adreno_compile/tools/build_adreno_compile_ir3.py")
            ir3_stage["status"] = "PENDING_W02_L2"
        elif mode == "dry-run":
            repo_file(repo, "adreno_compile/tools/build_adreno_compile_ir3.py")
            ir3_stage["status"] = "READY_NOT_RUN_DRY_RUN"
        else:
            if wave_stage["status"] != "PASS":
                raise GateError("VERIFY_WAVE_FAILED: ir3 execution requires passing host suites")
            ir3_cache = wave2_cache_directory(cache / "ir3")
            disasm = wave2_cache_directory(cache / "disasm")
            archive = ir3_cache / "libadreno_compile_ir3.a"
            if archive.exists() or archive.is_symlink():
                archive.unlink()
            execute(ir3_stage, [sys.executable, str(repo_file(repo, "adreno_compile/tools/build_adreno_compile_ir3.py")),
                               "--mesa", str(mesa), "--cache-dir", str(ir3_cache), "--jobs", "32", "--run-e2e",
                               "--dump-disasm", str(disasm)], repo, env, timeout=1800)
            if archive.is_symlink():
                raise GateError("IR3_ARCHIVE_SYMLINK: expected a newly emitted archive")
            archive = wave2_cache_path(archive)
            if not archive.is_file() or not archive.stat().st_size:
                raise GateError("IR3_ARCHIVE_MISSING: libadreno_compile_ir3.a")
            checked = command_json(execute(ir3_stage, [sys.executable, str(repo_file(repo, "tools/verify/no_fallback_test.py")),
                                                       "--profile", "compiler", "--artifact", str(archive)], repo, env))
            if not checked.get("symbols_scanned"):
                raise GateError("IR3_ARCHIVE_EMPTY: archive has no global symbols")
            ir3_stage.update(archive=str(archive), archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                             no_fallback_symbols=checked["symbols_scanned"])
    except (GateError, OSError, ValueError) as exc:
        fail(ir3_stage, exc)

    emit_stage = stage("a840_regs_and_emit")
    required = ("kits/Adreno840/gen/Adreno840Regs.h", "kits/Adreno840Emit/a840_emit_ops.h")
    try:
        missing = [relative for relative in required if not (repo / relative).exists() and not (repo / relative).is_symlink()]
        if missing:
            emit_stage["missing"] = missing
            if mode == "full":
                raise GateError("REGS_EMIT_MISSING: " + ", ".join(missing))
            emit_stage["status"] = "PENDING_W01_L1_L3"
        elif mode == "dry-run":
            for relative in required:
                repo_file(repo, relative)
            emit_stage["status"] = "READY_NOT_RUN_DRY_RUN"
        else:
            if wave_stage["status"] != "PASS":
                raise GateError("VERIFY_WAVE_FAILED: PM4 validation requires passing host suites")
            for relative in required:
                repo_file(repo, relative)
            scratch = wave2_cache_directory(cache / "reggen")
            execute(emit_stage, [sys.executable, str(repo_file(repo, "kits/Adreno840/tools/gen_caps_and_regs.py")),
                                 "--mesa", str(mesa), "--out-dir", str(repo / "kits/Adreno840/gen"),
                                 "--check", "--tmp-dir", str(scratch)], repo, env)
            emit_stage["samples"] = []
            for sample in emitter_streams(repo, cache, wave_report, emit_stage, env):
                checked = command_json(execute(emit_stage, [sys.executable, str(repo_file(repo, "tools/verify/pm4_check.py")),
                                                             str(sample), "--mesa", str(mesa), "--chip", "A8XX"], repo, env))
                if not checked.get("packet_count"):
                    raise GateError(f"PM4_SAMPLE_EMPTY: {sample}")
                golden = sample.with_name(sample.stem + ".golden.pm4")
                if golden.is_symlink():
                    raise GateError(f"symlinked PM4 golden sample: {golden}")
                if golden.exists() or golden.is_symlink():
                    repo_file(repo, golden.relative_to(repo).as_posix()) if golden.is_relative_to(repo) else wave2_cache_path(golden)
                compared = command_json(execute(emit_stage, [sys.executable, str(repo_file(repo, "tools/verify/pm4_diff.py")),
                                                              "--expected", str(golden if golden.is_file() else sample),
                                                              "--actual", str(sample), "--mesa", str(mesa), "--chip", "A8XX"], repo, env))
                emit_stage["samples"].append({"path": str(sample), "sha256": hashlib.sha256(sample.read_bytes()).hexdigest(),
                                              "packets": checked["packet_count"], "oracle": "GOLDEN" if golden.is_file() else "SELF_CONSISTENCY",
                                              "operations": compared["actual_operations"]})
    except (GateError, OSError, ValueError) as exc:
        fail(emit_stage, exc)

    final_key = stage("source_identity_after_gates")
    try:
        if compute_stack_key(repo) != key:
            raise GateError("SOURCE_IDENTITY_CHANGED: stack sources changed during gate execution")
    except (GateError, OSError, ValueError) as exc:
        fail(final_key, exc)
    report["status"] = "FAIL" if any(row["status"] == "FAIL" for row in report["results"]) else "PASS"
    report["phase1_complete"] = mode == "full" and report["status"] == "PASS"
    write_report(cache / "phase1_e2e_summary.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--mesa", required=True, type=Path)
    parser.add_argument("--cache-dir", required=True, type=Path)
    parser.add_argument("--mode", choices=("dry-run", "full"), default="dry-run")
    parser.add_argument("--with-clang", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = run_gate(args.repo, args.mesa, args.cache_dir, args.mode, args.with_clang)
    except (GateError, OSError, ValueError) as exc:
        print(json.dumps({"status": "FAIL", "error": str(exc)}, indent=2, sort_keys=True))
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    print("\nGATE                            STATUS")
    for row in report["results"]:
        print(f"{row['name']:<32}{row['status']}")
    print(f"Phase1 host gate: {report['status']} | mode={report['mode']} | phase1_complete={report['phase1_complete']}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
