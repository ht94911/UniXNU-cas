#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run Wave gates and discovered host suites, with JSON and table results.

Discovery supports test_*.py, test/test_*.{c,cpp}, tests/test_*.{c,cpp} and
Cargo.toml. C/C++ suites compile with -Werror and ASan/UBSan; --with-clang
adds Clang 18 using the pre-extracted compiler-rt. Cargo sources and the
host registry are copied into the approved cache, preventing source or
registry mutation. Generated compiler archives also pass no_fallback.
Missing placeholders are SKIP_NOT_YET_MERGED; populated untested modules fail.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import tomllib

from check_contracts import (DEFAULT_CACHE, GateError, cache_directory,
                             cache_environment, run_command, write_json)
from pm4_check import type4_header, type7_header
from pm4_xml_index import DEFAULT_MESA, MESA_COMMIT, load_index

SUITES = ("kits/Adreno840", "kits/Adreno840Emit", "adreno_compile", "translate")
CLANG_SUITES = SUITES[:3]
CLANG_RUNTIME = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/clang-rt18/usr/lib/llvm-18/lib/clang/18")
REFERENCE_PM4 = Path("/home/hua/nezha-work/UniXNU/kits/AdrenoPM4")
SOURCE_EXTENSIONS = {".c", ".cpp", ".h", ".rs", ".py", ".mm"}
IGNORE_DIRS = {".git", "__pycache__", "target", "build", "out", ".cache"}
NATIVE_FLAGS = ["-Wall", "-Wextra", "-Werror", "-pedantic", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-fno-pie"]


def command_gate(name: str, commands: list[list[str]], *, cwd: Path, env: dict[str, str],
                 timeout: int = 300) -> dict:
    result = {"name": name, "status": "PASS", "commands": []}
    for command in commands:
        outcome = run_command(command, cwd=cwd, env=env, timeout=timeout)
        result["commands"].append(outcome)
        if outcome["returncode"]:
            result["status"] = "FAIL"
            break
    return result


def source_files(module: Path) -> list[Path]:
    return sorted(path for path in module.rglob("*") if path.is_file() and
                  not any(part in IGNORE_DIRS for part in path.relative_to(module).parts))


def cargo_program() -> str:
    program = shutil.which("cargo")
    if program:
        return program
    local = Path("/home/hua/.cargo/bin/cargo")
    return str(local) if local.is_file() else "cargo"


def prepare_cargo_home(home: Path, registry: Path | None = None) -> dict:
    """Copy registry caches instead of linking Cargo-writable paths to sources."""
    home = cache_directory(home)
    target = home / "registry"
    source = (registry if registry is not None else Path.home() / ".cargo/registry").resolve()
    if target.is_symlink():
        raise GateError(f"Cargo registry must be an independent cache copy: {target}")
    if target.exists():
        if not target.is_dir():
            raise GateError(f"Cargo registry cache is not a directory: {target}")
        return {"registry_source": str(source), "registry_cache": str(target), "registry_status": "REUSED"}
    if not source.is_dir():
        return {"registry_source": str(source), "registry_cache": str(target), "registry_status": "ABSENT"}
    shutil.copytree(source, target)
    return {"registry_source": str(source), "registry_cache": str(target), "registry_status": "COPIED"}


def clang_configuration() -> tuple[str, str, list[str]]:
    cc, cxx = shutil.which("clang-18"), shutil.which("clang++-18")
    runtime = CLANG_RUNTIME / "lib/linux"
    if not cc or not cxx or not (runtime / "libclang_rt.asan-x86_64.a").is_file() or \
            not (runtime / "libclang_rt.ubsan_standalone-x86_64.a").is_file():
        raise GateError("--with-clang requires Clang 18 and the existing .cache/clang-rt18 ASan/UBSan libraries")
    result = subprocess.run([cc, "-print-resource-dir"], capture_output=True, text=True, timeout=30, check=False)
    headers = Path(result.stdout.strip()) / "include"
    if result.returncode or not (headers / "stddef.h").is_file():
        raise GateError("Clang 18 system builtin headers are unavailable")
    # compiler-rt Debian archive contains sanitizer headers, not all builtin headers.
    return cc, cxx, ["-resource-dir", str(CLANG_RUNTIME), "-isystem", str(headers)]


def optional_suite(repo: Path, relative: str, cache: Path, env: dict[str, str],
                   with_clang: bool = False,
                   extra_archives: dict[str, str] | None = None) -> dict:
    module = repo / relative
    result = {"name": relative + (":clang18" if with_clang else ""), "status": "PASS", "commands": [],
              "artifacts": [], "cargo_manifests": []}
    if not module.is_dir():
        result.update(status="SKIP_NOT_YET_MERGED", reason="suite directory absent")
        return result
    if module.is_symlink():
        result.update(status="FAIL", reason="symlinked suite directory")
        return result
    files = source_files(module)
    if any(path.is_symlink() for path in files):
        result.update(status="FAIL", reason="symlinked suite source")
        return result
    python_tests = [path for path in files if path.name.startswith("test_") and path.suffix == ".py"]
    native_tests = [path for path in files if path.name.startswith("test_") and path.suffix in {".c", ".cpp"}
                    and any(part in {"test", "tests"} for part in path.relative_to(module).parts[:-1])]
    manifest = module / "Cargo.toml"
    if not python_tests and not native_tests and not manifest.is_file():
        populated = any(path.suffix in SOURCE_EXTENSIONS for path in files)
        result.update(status="FAIL" if populated else "SKIP_NOT_YET_MERGED",
                      reason="source module has no discoverable tests" if populated else "placeholder only; tests not yet merged")
        return result
    output = cache_directory(cache / (relative.replace("/", "-") + ("-clang18" if with_clang else "")))
    suite_env = cache_environment(output, env)
    suite_env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                     UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def execute(command: list[str], cwd: Path = repo) -> bool:
        outcome = run_command(command, cwd=cwd, env=suite_env, timeout=600)
        result["commands"].append(outcome)
        if outcome["returncode"]:
            result["status"] = "FAIL"
            return False
        return True

    for directory in sorted({path.parent for path in python_tests}):
        if not execute([sys.executable, "-B", "-m", "unittest", "discover", "-s", str(directory),
                        "-p", "test_*.py", "-v"]):
            return result

    if manifest.is_file():
        with manifest.open("rb") as source:
            cargo_config = tomllib.load(source)
        needs_archive = "staticlib" in cargo_config.get("lib", {}).get("crate-type", [])
        copied = output / "cargo-source"
        if copied.exists():
            shutil.rmtree(copied)
        shutil.copytree(module, copied, ignore=shutil.ignore_patterns(*IGNORE_DIRS))
        suite_env["CARGO_TARGET_DIR"] = str(output / "cargo-target")
        cargo_home = output / "cargo-home"
        result["cargo_cache"] = prepare_cargo_home(cargo_home)
        suite_env["CARGO_HOME"] = str(cargo_home)
        suite_env["CARGO_NET_OFFLINE"] = "true"
        suite_env["CARGO_BUILD_JOBS"] = "32"
        cargo = cargo_program()
        if Path(cargo).is_absolute():
            suite_env["PATH"] = str(Path(cargo).parent) + os.pathsep + suite_env.get("PATH", "")
        if not execute([cargo, "test", "--offline", "--manifest-path", str(copied / "Cargo.toml")], copied):
            return result
        if (native_tests or needs_archive) and not execute([cargo, "build", "--offline", "--release", "--manifest-path",
                                                            str(copied / "Cargo.toml")], copied):
            return result
        result["cargo_manifests"] = [str(copied / "Cargo.toml")]
        result["artifacts"] = [str(path) for path in sorted((output / "cargo-target/release").glob("lib*.a"))]
        if needs_archive and not result["artifacts"]:
            result.update(status="FAIL", reason="Cargo staticlib build produced no release archive")
            return result

    if native_tests:
        production = [path for path in files if path.suffix in {".c", ".cpp"} and
                      not any(part in {"test", "tests", "gen"} for part in path.relative_to(module).parts[:-1]) and
                      not path.name.startswith("test_")]
        includes = [repo / "contracts", repo / "kits/Adreno840", repo / "kits/Adreno840/gen", module, module / "include", module / "src"]
        def needs_pm4_reference(path: Path) -> bool:
            return relative == "kits/Adreno840Emit" and bool(
                re.search(r"\bAdrenoPM4\w*\b", path.read_text(encoding="utf-8")))

        production_uses_reference = any(needs_pm4_reference(source) for source in production)
        reference_tests = {test for test in native_tests if needs_pm4_reference(test)}
        if production_uses_reference or reference_tests:
            includes.append(REFERENCE_PM4)
        flags = [flag for directory in includes if directory.is_dir() for flag in ("-I", str(directory))]
        cc, cxx, extra_flags = clang_configuration() if with_clang else ("gcc", "g++", [])
        flags = extra_flags + flags
        objects = []
        for number, source in enumerate(production):
            compiler, standard = (cc, "c11") if source.suffix == ".c" else (cxx, "c++17")
            target = output / f"source-{number}.o"
            if not execute([compiler, "-std=" + standard, *NATIVE_FLAGS, *flags,
                            "-c", str(source), "-o", str(target)], output):
                return result
            objects.append(str(target))
        reference_object = output / "reference-pm4.o"
        if production_uses_reference or reference_tests:
            if not execute([cc, "-std=c11", *NATIVE_FLAGS, *flags, "-c", str(REFERENCE_PM4 / "AdrenoPM4.c"),
                            "-o", str(reference_object)], output):
                return result
        if relative == "adreno_compile" and objects:
            archive = output / "libadreno_compile.a"
            if archive.exists():
                archive.unlink()
            if not execute(["ar", "rcs", str(archive), *objects], output):
                return result
            result["artifacts"].append(str(archive))
        libraries = []
        if manifest.is_file():
            libraries = [str(path) for path in sorted((output / "cargo-target/release").glob("lib*.a"))]
            if not libraries:
                result.update(status="FAIL", reason="Cargo C ABI suite has no release static library")
                return result
        for number, test in enumerate(native_tests):
            compiler, standard = (cc, "c11") if test.suffix == ".c" else (cxx, "c++17")
            if any(path.suffix == ".cpp" for path in production):
                compiler = cxx
                standard = "c++17"
            target = output / f"test-{number}"
            references = [str(reference_object)] if production_uses_reference or test in reference_tests else []
            if extra_archives and "adreno_compile" in extra_archives and re.search(r"\ba840_compile_stage\b", test.read_text(encoding="utf-8")):
                references.append(extra_archives["adreno_compile"])
            if not execute([compiler, "-std=" + standard, *NATIVE_FLAGS, *flags,
                            str(test), *objects, *references, *libraries, "-lpthread", "-ldl", "-lm", "-no-pie",
                            "-o", str(target)], output) or not execute([str(target)], output):
                return result
    return result


def verify_wave(repo: str | Path, mesa: str | Path = DEFAULT_MESA,
                cache_dir: str | Path = DEFAULT_CACHE, with_clang: bool = False) -> dict:
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    if not repo.is_dir():
        raise GateError(f"missing repository: {repo}")
    cache = cache_directory(cache_dir)
    env = cache_environment(cache)
    env["A840_VERIFY_MESA"] = str(mesa)
    tools = repo / "tools/verify"
    started, results = time.monotonic(), []
    scripts = sorted(tools.glob("*.py"))
    if not scripts:
        raise GateError(f"missing verification scripts: {tools}")
    results.append(command_gate("py_compile", [[sys.executable, "-X", "pycache_prefix=" + str(cache / "pycache"),
                                                 "-m", "py_compile", *map(str, scripts)]], cwd=cache, env=env))
    for name, command in (
        ("contracts", [sys.executable, str(tools / "check_contracts.py"), "--repo", str(repo),
                       "--cache-dir", str(cache / "contracts")]),
        ("driver_constants", [sys.executable, str(tools / "driver_constants_test.py"), "--root", str(repo)]),
        ("unittest", [sys.executable, "-B", "-m", "unittest", "discover", "-s", str(tools), "-p", "test_*.py", "-v"]),
        ("pm4_xml_index", [sys.executable, str(tools / "pm4_xml_index.py"), "--mesa", str(mesa),
                           "--out", str(cache / "a840_pm4_index.json")]),
    ):
        results.append(command_gate(name, [command], cwd=repo, env=env))
    if results[-1]["status"] == "PASS":
        try:
            index = load_index(cache / "a840_pm4_index.json")
            stream = cache / "pm4-smoke.hex"
            # XML checks the names; header builders are checked against real C in unittest.
            words = [type4_header(0x84c, 1), 0,
                     type7_header(index["opcodes"]["CP_SET_DRAW_STATE"], 3), 0, 0, 0]
            stream.write_text(" ".join(f"{word:08x}" for word in words) + "\n", encoding="ascii")
            results.append(command_gate("pm4_decode", [[sys.executable, str(tools / "pm4_check.py"),
                                                         str(stream), "--format", "hex", "--index",
                                                         str(cache / "a840_pm4_index.json")]], cwd=repo, env=env))
        except (GateError, OSError) as exc:
            results.append({"name": "pm4_decode", "status": "FAIL", "reason": str(exc), "commands": []})
    else:
        results.append({"name": "pm4_decode", "status": "FAIL", "reason": "PM4 XML gate failed", "commands": []})
    built_archives: dict[str, str] = {}
    for suite in SUITES:
        outcome = optional_suite(repo, suite, cache / "suites", env, extra_archives=built_archives)
        if suite == "adreno_compile" and outcome["status"] == "PASS" and outcome.get("artifacts"):
            built_archives["adreno_compile"] = outcome["artifacts"][0]
        results.append(outcome)
    if with_clang:
        for suite in CLANG_SUITES:
            try:
                results.append(optional_suite(repo, suite, cache / "suites", env, with_clang=True))
            except (GateError, OSError, subprocess.TimeoutExpired) as exc:
                results.append({"name": suite + ":clang18", "status": "FAIL", "reason": str(exc), "commands": []})
    command = [sys.executable, str(tools / "no_fallback_test.py"), "--profile", "compiler"]
    for artifact in sorted({path for row in results for path in row.get("artifacts", [])}):
        command.extend(["--artifact", artifact])
    manifests = {path for row in results for path in row.get("cargo_manifests", [])}
    if (repo / "translate/Cargo.toml").is_file():
        manifests.add(str(repo / "translate/Cargo.toml"))
    for manifest in sorted(manifests):
        command.extend(["--cargo-manifest", manifest])
    manifest = repo / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
    if manifest.is_file():
        command.extend(["--json-manifest", str(manifest)])
    results.append(command_gate("no_fallback", [command], cwd=repo, env=env))
    report = {"status": "PASS" if all(row["status"] in {"PASS", "SKIP_NOT_YET_MERGED"} for row in results) else "FAIL",
              "repo": str(repo), "mesa": str(mesa), "expected_mesa_commit": MESA_COMMIT,
              "cache_dir": str(cache), "with_clang": with_clang,
              "seconds": round(time.monotonic() - started, 4), "results": results}
    write_json(cache / "verify_wave_summary.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mesa", type=Path, default=DEFAULT_MESA)
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--with-clang", action="store_true", help="also run three native suites with Clang 18 ASan/UBSan")
    args = parser.parse_args(argv)
    try:
        report = verify_wave(args.repo, args.mesa, args.cache_dir, args.with_clang)
    except (GateError, OSError) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    print("\nGATE                             STATUS                  COMMANDS")
    for row in report["results"]:
        print(f"{row['name']:<33}{row['status']:<24}{len(row['commands'])}")
    print(f"Wave: {report['status']} | JSON: {report['cache_dir']}/verify_wave_summary.json")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
