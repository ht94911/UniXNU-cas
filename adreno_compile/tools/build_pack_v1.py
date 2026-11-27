#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile all four direct 24A446 corpora into checked, readonly A840PK01 Tier0."""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
CACHE_ROOT = ROOT.parent / ".cache"


def checked_output(path: Path) -> Path:
    raw = str(path).lower()
    result = path.expanduser().resolve()
    if raw.startswith(("c:", "/mnt/c")) or result == CACHE_ROOT or not result.is_relative_to(CACHE_ROOT):
        raise ValueError("output must resolve under project .cache/<any-subdir>")
    return result


def module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError("cannot load read-only helper")
    value = importlib.util.module_from_spec(spec)
    sys.modules[name] = value
    spec.loader.exec_module(value)
    return value


def execute(command, env, log: Path, timeout=600):
    started = time.monotonic()
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(json.dumps({"command": list(map(str, command)), "exit": result.returncode,
                              "seconds": time.monotonic() - started,
                              "stdout": result.stdout, "stderr": result.stderr}, indent=2) + "\n")
    if result.returncode:
        raise RuntimeError("command failed: " + str(log) + "\n" + result.stderr[-4096:])
    return result


def json_line(result):
    for line in reversed(result.stdout.splitlines()):
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                pass
    raise ValueError("driver has no complete JSON result")


def build(mesa: Path, cache: Path, jobs: int, env: dict):
    bench = module("a840_w05_corpus", ROOT / "adreno_compile/tools/corpus_bench.py")
    _reference_driver, manifest = bench.build_driver(mesa, cache, jobs, env)
    common = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie",
              "-I", ROOT / "contracts", "-I", ROOT / "translate/include",
              "-I", ROOT / "adreno_compile/cache", "-I", ROOT / "adreno_compile/layout",
              "-I", ROOT / "adreno_compile/pack"]
    pack = cache / "a840_pack_v1.o"
    driver = cache / "pack_builder_driver.o"
    execute(["gcc", *common, "-c", ROOT / "adreno_compile/pack/a840_pack_v1.c", "-o", pack],
            env, cache / "pack-compile.json")
    execute(["gcc", *common, "-DA840_BUILD_PACK_DRIVER=1", "-c",
             ROOT / "adreno_compile/pack/pack_builder_driver.c", "-o", driver],
            env, cache / "pack-driver-compile.json")
    binary = cache / "pack_builder_driver"
    execute(["g++", "-g", "-fsanitize=address,undefined", "-no-pie",
             driver, pack, cache / "record_io.o", cache / "binding_plan.o",
             "-Wl,--start-group", manifest["ir3_archive"],
             *[item["path"] for item in manifest["mesa_archives"]],
             manifest["translator_archive"], "-Wl,--end-group",
             "-lz", "-lpthread", "-ldl", "-lm", "-o", binary],
            env, cache / "pack-driver-link.json")
    return binary, bench, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=32)
    parser.add_argument("--pack-out", type=Path, required=True)
    parser.add_argument("--report-out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = {"status": "FAIL", "container": "A840PK01", "tier": 0, "pipelines": []}
    destination = None
    try:
        cache = checked_output(args.cache_dir)
        pack = checked_output(args.pack_out)
        destination = checked_output(args.report_out)
        if pack == destination:
            raise ValueError("pack/report outputs must be distinct")
        if not 1 <= args.jobs <= 32:
            raise ValueError("jobs must be 1..32")
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "tmp").mkdir(exist_ok=True)
        pack.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env.update(TMPDIR=str(cache / "tmp"), TMP=str(cache / "tmp"), TEMP=str(cache / "tmp"),
                   PYTHONDONTWRITEBYTECODE="1", PYTHONPYCACHEPREFIX=str(cache / "pycache"),
                   ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", CCACHE_DISABLE="1",
                   CARGO_BUILD_JOBS=str(args.jobs), GIT_OPTIONAL_LOCKS="0")
        sys.path.insert(0, str(ROOT / "tools/verify"))
        stack = module("a840_w05_stack", ROOT / "tools/verify/stack_key.py")
        identity = stack.compute_stack_key(ROOT)
        (cache / "stack-key.json").write_text(json.dumps(identity, indent=2) + "\n")
        binary, bench, build_report = build(args.mesa.resolve(), cache, args.jobs, env)
        samples = [bench.SAMPLES[i] for i in (0, 1, 3, 4)]
        libraries, cases = [], []
        for path in samples:
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            functions = bench.container_functions(data)
            unique = {item["name"]: item for item in functions}
            vertex = next((f["name"] for f in functions if f["type"] == 0), None)
            fragment = next((f["name"] for f in functions if f["type"] == 1), None)
            libraries.append({"name": path.name, "sha256": digest, "bytes": len(data),
                              "unique_function_count": len(unique)})
            for item in sorted(unique.values(), key=lambda f: f["name"]):
                name, kind = item["name"], item["type"]
                if kind == 2:
                    mode, entry, other = "compute", name, "-"
                elif kind == 0 and fragment:
                    mode, entry, other = "render", name, fragment
                elif kind == 1 and vertex:
                    mode, entry, other = "render", vertex, name
                else:
                    raise ValueError("direct corpus has an unsupported/unpaired stage")
                cases.append({"library": str(path), "library_sha256": digest,
                              "function": name, "kind": kind,
                              "mode": mode, "entry": entry, "other": other})
        if len(cases) != 33:
            raise ValueError(f"expected exact direct corpus33, got {len(cases)}")
        parts = cache / "parts"
        parts.mkdir(exist_ok=True)

        def attempt(index, case):
            output = parts / f"pipeline-{index:03d}.pack.bin"
            command = [binary, "compile", case["library"], case["mode"],
                       case["entry"], case["other"], output, identity["stack_key_hex"]]
            result = execute(command, env, cache / "logs" / f"compile-{index:03d}.json", timeout=180)
            value = json_line(result)
            if value.get("status") != "PASS" or not value.get("compiled_ok") or not value.get("binding_plan_ok"):
                raise ValueError("real pipeline compile/binding gate failed")
            return index, {**case, **value, "part": str(output)}

        rows = [None] * len(cases)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(attempt, i, case) for i, case in enumerate(cases)]
            for number, future in enumerate(as_completed(futures), 1):
                index, row = future.result()
                rows[index] = row
                if number % 8 == 0 or number == len(cases):
                    print(f"AOT real pipelines {number}/{len(cases)}", flush=True)
        merge = execute([binary, "merge", pack, identity["stack_key_hex"], *[r["part"] for r in rows]],
                        env, cache / "merge-mmap-verify.json", timeout=180)
        verified = json_line(merge)
        if verified.get("status") != "PASS" or verified.get("lookup_verified") != 33 or \
                not all(verified.get(k) for k in ("mmap_readonly", "all_isa_128b_aligned", "sha256_verified")):
            raise ValueError("pack/mmap/lookup verification gate failed")
        if stack.compute_stack_key(ROOT) != identity:
            raise ValueError("source bytes changed during AOT build")
        if any(hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]
               for path, item in zip(samples, libraries)):
            raise ValueError("read-only direct corpus changed")
        digest = hashlib.sha256(pack.read_bytes()).hexdigest()
        report.update(status="PASS", attempted_pipelines=33, compiled_ok=33, binding_plan_ok=33,
                      libraries=libraries, pipelines=rows, mmap_verification=verified,
                      build=build_report, stack_key=identity["stack_key_hex"],
                      stack_digest_sha256=identity["stack_digest_sha256"],
                      pack_out=str(pack), pack_sha256=digest, pack_bytes=pack.stat().st_size,
                      distinct_pipeline_keys=len({r["pipeline_key_sha256"] for r in rows}))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    if destination is not None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        temporary.replace(destination)
    summary = {k: report[k] for k in ("status", "attempted_pipelines", "compiled_ok",
               "binding_plan_ok", "distinct_pipeline_keys", "pack_bytes", "pack_sha256", "stack_key", "error") if k in report}
    if "mmap_verification" in report:
        summary["mmap_verification"] = report["mmap_verification"]
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
