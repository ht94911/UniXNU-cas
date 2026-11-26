#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Benchmark real read-only 24A446 functions through translate, IR3 and A840PR01.

Compare the same inputs and pipeline pairs with the Wave 3 report. Record
unsupported stages, translator/backend errors and binding plan errors separately.
Private libraries remain at their original paths.
"""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
CACHE_ROOT = ROOT.parent / ".cache"
DEFAULT_BASELINE = CACHE_ROOT / "w03-l2/corpus_report.json"
WAVE3_FUNCTION_COUNT = 66
PINNED_CACHE = ROOT.parent / ".cache/coord-phase1-full-01/ir3"
SAMPLES = (
    Path("/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib"),
    Path("/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MetalFilter.metallib"),
    Path("/home/hua/nezha-work/private/ios27/camera27-c0/registration088-research/ci_filters.metallib"),
    Path("/home/hua/nezha-work/private/ios27/camera27-c0/registration093-research/VideoToolbox-default.metallib"),
    Path("/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib"),
    Path("/home/hua/nezha-work/ios27/gpu27-astra/private/metal-full058-context-signposts-result/logs/stitch-modules/stitched-distinct.metallib"),
)


def checked_output(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved == CACHE_ROOT or not resolved.is_relative_to(CACHE_ROOT):
        raise ValueError(f"output must be beneath {CACHE_ROOT}: {resolved}")
    return resolved


def digest_file(path: Path) -> str:
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256")
    return digest.hexdigest()


def module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def execute(argv: list[str | Path], env: dict[str, str], log: Path,
            *, timeout: int = 600) -> subprocess.CompletedProcess[str]:
    started = time.monotonic()
    command = list(map(str, argv))
    try:
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True,
                                text=True, errors="replace", timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        def decoded(value):
            return value.decode("utf-8", "replace") if isinstance(value, bytes) else (value or "")
        result = subprocess.CompletedProcess(command, 124, decoded(exc.stdout),
            decoded(exc.stderr) + f"\nTIMEOUT {timeout}s")
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(json.dumps({"command": command, "returncode": result.returncode,
        "seconds": time.monotonic() - started, "stdout": result.stdout,
        "stderr": result.stderr}, indent=2) + "\n")
    return result


def json_result(result: subprocess.CompletedProcess[str]) -> dict:
    for line in reversed(result.stdout.splitlines()):
        if line.startswith("{"):
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                return value
    return {"status": "IR3_ERR", "error": "driver produced no complete JSON result"}


def build_driver(mesa: Path, cache: Path, jobs: int, env: dict[str, str]) -> tuple[Path, dict]:
    helper = module("a840_corpus_build", ROOT / "adreno_compile/tools/build_adreno_compile_ir3.py")
    ir3_cache = checked_output(cache / "ir3")
    ir3_cache.mkdir(parents=True, exist_ok=True)
    reuse = PINNED_CACHE
    marker = reuse / f"mesa-{helper.MESA_COMMIT[:12]}/.pinned-commit"
    manifest = json.loads((ROOT / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json").read_text())
    mesa_head = subprocess.check_output(["git", "-C", str(mesa), "rev-parse", "HEAD"], text=True).strip()
    if mesa_head != helper.MESA_COMMIT or manifest["mesa_commit"] != mesa_head:
        raise ValueError("Mesa source/manifest pin mismatch")
    reused = marker.is_file() and marker.read_text().strip() == mesa_head and (reuse / "meson-targets.json").is_file()
    if reused:
        source, build = reuse / f"mesa-{mesa_head[:12]}", reuse / f"build-{mesa_head[:12]}"
        inventory = json.loads((reuse / "meson-targets.json").read_text())
        targets = {row["name"]: Path(row["filename"][0]) for row in inventory if row["type"] == "static library"}
        names = [*manifest["libraries"], "mesa_util", *helper.MESA_UTIL_SUBLIBS]
        mesa_archives = [targets[name] for name in names if name in targets]
        reused = len(mesa_archives) == len(names) and all(p.is_file() for p in mesa_archives)
    if not reused:
        source, build, _, mesa_archives = helper.build_mesa_archives(
            mesa, ROOT / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json", ir3_cache, jobs, env)
    else:
        shutil.copy2(reuse / "meson-targets.json", ir3_cache / "meson-targets.json")
    # Compile the CURRENT protected IR3 sources into this task's output; retained
    # Mesa sources/archives are read-only input, never rebuilt in someone else's cache.
    ir3_archive, _ = helper.build_ir3_archive(source, build, ir3_cache, env)
    translator = reuse / "translate-build/cargo-target/release/libnative_metal_translate.a"
    cargo_source = reuse / "translate-build/cargo-source"
    translation_inputs = [p for p in (ROOT / "translate").rglob("*") if p.is_file()
        and p.suffix in {".rs", ".h", ".toml"} and not any(x in helper.IGNORE_DIRS for x in p.parts)]
    translator_matches = translator.is_file() and translation_inputs and all(
        (cargo_source / p.relative_to(ROOT / "translate")).is_file() and
        (cargo_source / p.relative_to(ROOT / "translate")).read_bytes() == p.read_bytes()
        for p in translation_inputs)
    if translator_matches:
        destination = ir3_cache / "libnative_metal_translate.a"
        shutil.copy2(translator, destination)
        translator = destination
    else:
        translator = helper.ensure_translate_archive(ir3_cache, jobs, env)
    # Every new archive/object/executable is inside this run cache. Copy only the retained
    # static input archives; no Apple library/firmware/device bytes are copied.
    libs = cache / "lib"
    libs.mkdir(exist_ok=True)
    copied_libs = []
    for archive in mesa_archives:
        destination = libs / archive.name
        receipt = libs / (archive.name + ".source-sha256")
        source_digest = digest_file(archive)
        if not destination.is_file() or not receipt.is_file() or receipt.read_text().strip() != source_digest:
            if destination.exists():
                destination.unlink()
            if archive.read_bytes().startswith(b"!<thin>\n"):
                # ADDLIB resolves the original thin-member paths and writes a
                # self-contained ordinary archive, entirely in this cache.
                result = subprocess.run(["ar", "-M"], input=f"CREATE {destination}\nADDLIB {archive}\nSAVE\nEND\n",
                    cwd=ROOT, env=env, text=True, capture_output=True, check=False)
                if result.returncode:
                    raise RuntimeError(result.stderr)
            else:
                shutil.copy2(archive, destination)
            receipt.write_text(source_digest + "\n")
        copied_libs.append(destination)
    common = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"]
    record_object = cache / "record_io.o"
    result = execute(["gcc", *common, "-I", ROOT / "contracts", "-I", ROOT / "adreno_compile/cache",
        "-c", ROOT / "adreno_compile/cache/a840_record_io.c", "-o", record_object], env, cache / "record-build.json")
    if result.returncode:
        raise RuntimeError(result.stderr)
    plan_object = cache / "binding_plan.o"
    result = execute(["gcc", *common, "-I", ROOT / "contracts", "-I", ROOT / "translate/include",
        "-I", ROOT / "adreno_compile/layout", "-c", ROOT / "adreno_compile/layout/a840_binding_plan.c",
        "-o", plan_object], env, cache / "binding-plan-build.json")
    if result.returncode:
        raise RuntimeError(result.stderr)
    driver_object = cache / "corpus_driver.o"
    result = execute(["gcc", *common, "-DA840_BUILD_CORPUS_DRIVER=1", *helper.mesa_compile_defines(ir3_cache),
        *helper.mesa_include_flags(source, build), "-I", ROOT / "translate/include",
        "-I", ROOT / "adreno_compile/cache", "-I", ROOT / "adreno_compile/layout",
        "-c", ROOT / "adreno_compile/cache/corpus_driver.c",
        "-o", driver_object], env, cache / "driver-build.json")
    if result.returncode:
        raise RuntimeError(result.stderr)
    driver = cache / "corpus_driver"
    result = execute(["g++", "-g", "-fsanitize=address,undefined", "-no-pie", driver_object, record_object, plan_object,
        "-Wl,--start-group", ir3_archive, *copied_libs, translator, "-Wl,--end-group",
        "-lz", "-lpthread", "-ldl", "-lm", "-o", driver], env, cache / "driver-link.json")
    if result.returncode:
        raise RuntimeError(result.stderr)
    return driver, {"mesa_reused": bool(reused), "translator_source_bytes_matched": bool(translator_matches),
        "mesa_commit": mesa_head, "ir3_archive": str(ir3_archive), "ir3_archive_sha256": digest_file(ir3_archive),
        "translator_archive": str(translator), "translator_archive_sha256": digest_file(translator),
        "mesa_archives": [{"path": str(p), "sha256": digest_file(p)} for p in copied_libs],
        "driver": str(driver), "driver_sha256": digest_file(driver)}


def mtlb_functions(data: bytes) -> list[dict]:
    if len(data) < 88 or data[:4] != b"MTLB":
        raise ValueError("not an MTLB record")
    at, length = struct.unpack_from("<QQ", data, 24)
    if at > len(data) - 4 or length > len(data) - at - 4:
        raise ValueError("function table bounds")
    count = struct.unpack_from("<I", data, at)[0]
    at += 4
    end = at + length
    functions = []
    alias_member = False
    for _ in range(count):
        if at + 4 > end:
            raise ValueError("truncated function record")
        size = struct.unpack_from("<I", data, at)[0]
        if size < 8 or size > end - at:
            raise ValueError("bad function record size")
        pos, finish = at + 4, at + size
        name, stage, tags = None, None, set()
        while pos < finish:
            tag = data[pos:pos + 4]
            if tag == b"ENDT":
                pos += 4
                break
            if pos + 6 > finish:
                raise ValueError("truncated metadata tag")
            tag_size = struct.unpack_from("<H", data, pos + 4)[0]
            if tag_size > finish - pos - 6:
                raise ValueError("tag bounds")
            payload = data[pos + 6:pos + 6 + tag_size]
            tags.add(tag)
            if tag == b"NAME":
                if not payload.endswith(b"\0"):
                    raise ValueError("unterminated function name")
                name = payload[:-1].decode("utf-8")
            elif tag == b"TYPE":
                if len(payload) != 1:
                    raise ValueError("invalid TYPE tag")
                stage = payload[0]
            pos += 6 + tag_size
        if pos != finish:
            raise ValueError("metadata trailing bytes")
        if name is None and {b"ALIS", b"SPCI"}.issubset(tags):
            alias_member = True  # NativeMetalTranslate skips this WHOLE archive member.
        elif name is None or stage is None:
            raise ValueError("missing function NAME/TYPE")
        else:
            functions.append({"name": name, "type": stage})
        at = finish
    if at != end:
        raise ValueError("function table length disagreement")
    return [] if alias_member else functions


def container_functions(data: bytes) -> list[dict]:
    if data[:4] == b"MTLB":
        return mtlb_functions(data)
    if data[:4] != bytes.fromhex("cffaedfe") or len(data) < 32:
        raise ValueError("unsupported sample container")
    commands, command_bytes = struct.unpack_from("<II", data, 16)
    at, end = 32, 32 + command_bytes
    if end > len(data):
        raise ValueError("Mach-O load-command bounds")
    section = None
    for _ in range(commands):
        if at + 8 > end:
            raise ValueError("truncated Mach-O command")
        command, size = struct.unpack_from("<II", data, at)
        if size < 8 or size > end - at:
            raise ValueError("bad Mach-O command length")
        if command == 25:
            if size < 72:
                raise ValueError("truncated LC_SEGMENT_64")
            count = struct.unpack_from("<I", data, at + 64)[0]
            if count * 80 > size - 72:
                raise ValueError("Mach-O section table bounds")
            for index in range(count):
                pos = at + 72 + index * 80
                if data[pos:pos + 16].split(b"\0", 1)[0] == b"__metallib" and \
                        data[pos + 16:pos + 32].split(b"\0", 1)[0] == b"__TEXT":
                    length = struct.unpack_from("<Q", data, pos + 40)[0]
                    offset = struct.unpack_from("<I", data, pos + 48)[0]
                    if offset > len(data) or length > len(data) - offset:
                        raise ValueError("Mach-O metallib section bounds")
                    section = data[offset:offset + length]
        at += size
    if section is None:
        raise ValueError("no __TEXT,__metallib")
    functions, at = [], 0
    while at < len(section):
        if at + 24 > len(section) or section[at:at + 4] != b"MTLB":
            raise ValueError("bad embedded metallib")
        length = struct.unpack_from("<Q", section, at + 16)[0]
        if length < 88 or length > len(section) - at:
            raise ValueError("bad embedded library size")
        functions.extend(mtlb_functions(section[at:at + length]))
        at += length
        padding = min((-length) % 16, len(section) - at)
        if any(section[at:at + padding]):
            raise ValueError("nonzero library alignment padding")
        at += padding
    return functions


def summarize(rows: list[dict]) -> dict:
    compiled = [r for r in rows if r.get("compiled_ok") is True]
    stages = [s for r in compiled for s in r.get("stages", [])]
    statistics_report = {}
    for key in ("isa_dwords", "instructions", "max_full_reg", "max_half_reg", "spills", "fills",
                "constlen_vec4", "shared_mem_bytes", "private_mem_bytes"):
        values = [s[key] for s in stages]
        statistics_report[key] = {"count": len(values), "sum": sum(values), "min": min(values) if values else None,
            "max": max(values) if values else None, "mean": statistics.mean(values) if values else None}
    return {"total_attempted": len(rows), "compiled_ok": len(compiled),
        "record_roundtrip_ok": sum(r.get("record_roundtrip_ok") is True for r in rows),
        "binding_plan_ok": sum(r.get("binding_plan_ok") is True for r in rows),
        "failed_by_category": dict(sorted(Counter(r["status"] for r in rows if r["status"] != "PASS").items())),
        "compile_success_rate": len(compiled) / len(rows) if rows else 0,
        "compiled_stage_count": len(stages), "stage_statistics": statistics_report,
        "distinct_pipeline_records": len({r["pipeline_key_sha256"] for r in compiled if r.get("record_roundtrip_ok")})}


def case_key(row: dict) -> tuple:
    return tuple(row[key] for key in ("metallib_sha256", "function", "function_type", "mode", "entry", "other"))


def compare_wave3(path: Path, rows: list[dict], libraries: list[dict]) -> dict:
    data = path.read_bytes()
    previous = json.loads(data)
    previous_rows = previous["functions"]
    if (previous["status"], previous["corpus_build"], previous["total_attempted"],
            previous["compiled_ok"]) != ("PASS", "24A446", WAVE3_FUNCTION_COUNT, 22):
        raise ValueError("baseline is not the retained Wave 3 24A446 22/66 report")
    fingerprints = [(Path(lib["path"]).name, lib["sha256"]) for lib in libraries]
    if fingerprints != [(Path(lib["path"]).name, lib["sha256"]) for lib in previous["libraries"]]:
        raise ValueError("corpus names or SHA-256 differ from Wave 3")
    old = {case_key(row): row for row in previous_rows}
    new = {case_key(row): row for row in rows}
    if len(old) != WAVE3_FUNCTION_COUNT or (old.keys() - new.keys()):
        raise ValueError("selected functions or stage pairings differ from Wave 3")
    if sum(row["status"] == "PASS" for row in old.values()) != previous["compiled_ok"]:
        raise ValueError("Wave 3 row outcomes contradict its summary")
    regressions = [f'{Path(row["metallib"]).name}:{row["function"]}'
                   for key, row in old.items()
                   if row["status"] == "PASS" and new[key].get("compiled_ok") is not True]
    per_library = []
    for name, digest in fingerprints:
        keys = [key for key in old if key[0] == digest]
        before = sum(old[key]["status"] == "PASS" for key in keys)
        after = sum(new[key].get("compiled_ok") is True for key in keys)
        per_library.append({"metallib": name, "matched_functions": len(keys),
            "compiled_before": before, "compiled_after": after, "compile_gain": after - before,
            "binding_plan_ok": sum(new[key].get("binding_plan_ok") is True for key in keys),
            "failed_after": dict(sorted(Counter(new[key]["status"] for key in keys
                                                   if new[key]["status"] != "PASS").items()))})
    compiled_after = sum(row["compiled_after"] for row in per_library)
    return {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
        "matched_functions": len(old), "compiled_before": previous["compiled_ok"],
        "compiled_after": compiled_after, "compile_gain": compiled_after - previous["compiled_ok"],
        "rate_before": previous["compiled_ok"] / len(old), "rate_after": compiled_after / len(old),
        "regressions": regressions, "libraries": per_library}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=32)
    parser.add_argument("--min-functions", type=int, default=50)
    parser.add_argument("--report-out", type=Path, required=True)
    parser.add_argument("--baseline-report", type=Path, default=DEFAULT_BASELINE)
    args = parser.parse_args(argv)
    report = {"status": "FAIL", "record_magic": "A840PR01", "corpus_build": "24A446", "functions": []}
    output = None
    try:
        cache = checked_output(args.cache_dir)
        output = checked_output(args.report_out)
        if not 1 <= args.jobs <= 32 or not 50 <= args.min_functions <= 4096:
            raise ValueError("jobs must be 1..32, min-functions must be 50..4096")
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "tmp").mkdir(exist_ok=True)
        env = os.environ.copy()
        env.update(TMPDIR=str(cache / "tmp"), TEMP=str(cache / "tmp"), TMP=str(cache / "tmp"),
            PYTHONDONTWRITEBYTECODE="1", PYTHONPYCACHEPREFIX=str(cache / "pycache"), CCACHE_DISABLE="1",
            ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        sys.path.insert(0, str(ROOT / "tools/verify"))
        stack_module = module("a840_corpus_stack_key", ROOT / "tools/verify/stack_key.py")
        identity = stack_module.compute_stack_key(ROOT)
        (cache / "stack_key.json").write_text(json.dumps(identity, indent=2) + "\n")
        driver, build = build_driver(args.mesa.resolve(), cache, args.jobs, env)
        report.update(stack_key=identity["stack_key_hex"], stack_digest_sha256=identity["stack_digest_sha256"], build=build)
        cases, libraries = [], []
        per_library = max(32, math.ceil(args.min_functions / len(SAMPLES)))
        for sample_number, path in enumerate(SAMPLES):
            digest = digest_file(path)
            metadata = container_functions(path.read_bytes())
            result = execute([driver, "list", path], env, cache / "logs" / f"library-{sample_number}.json")
            listed = json_result(result)
            if result.returncode or listed.get("status") != "PASS":
                raise RuntimeError(f"enumeration failed for {path.name}: {listed}")
            names = listed["names"]
            if names != [f["name"] for f in metadata] or listed["stage_kinds"] != [f["type"] for f in metadata]:
                raise ValueError(f"NativeMetalTranslate function names or kinds differ from container metadata: {path.name}")
            unique = {f["name"]: f for f in metadata}
            selected = sorted(unique.values(), key=lambda f: (f["type"] not in (0, 1, 2), f["name"]))[:per_library]
            vertex = next((f["name"] for f in metadata if f["type"] == 0), None)
            fragment = next((f["name"] for f in metadata if f["type"] == 1), None)
            libraries.append({"path": str(path), "sha256": digest, "function_count": len(names),
                "unique_function_count": len(unique), "selected": len(selected),
                "function_type_histogram": dict(sorted(Counter(str(f["type"]) for f in metadata).items()))})
            for function in selected:
                name, stage = function["name"], function["type"]
                mode, entry, other = "compute", name, "-"
                if stage == 1 and vertex:
                    mode, entry, other = "render", vertex, name
                elif stage == 0 and fragment:
                    mode, entry, other = "render", name, fragment
                cases.append({"metallib": str(path), "metallib_sha256": digest, "function": name,
                    "function_type": stage, "mode": mode, "entry": entry, "other": other})
            print(f"CORPUS {path.name}: enumerated {len(names)}, selected {len(selected)}", flush=True)
        if len(cases) < args.min_functions:
            raise ValueError(f"only {len(cases)} unique selected functions; require {args.min_functions}")
        records = cache / "records"
        records.mkdir(exist_ok=True)

        def attempt(index: int, case: dict) -> tuple[int, dict]:
            started = time.monotonic()
            command = [driver, "compile", case["metallib"], case["mode"], case["entry"],
                       case["other"], records, identity["stack_key_hex"]]
            result = execute(command, env, cache / "logs" / f"function-{index:04d}.json", timeout=120)
            row = {**case, **json_result(result), "exit_code": result.returncode,
                   "seconds": time.monotonic() - started, "log": str(cache / "logs" / f"function-{index:04d}.json")}
            if result.returncode:
                if row.get("status") != "RECORD_ERR":
                    row.update(status="IR3_ERR", compiled_ok=False)
                row.update(record_roundtrip_ok=False, binding_plan_ok=False)
                row["error"] = (row.get("error", "") + "\n" + result.stderr[-4096:]).strip()
            # The current public translator API exposes only Kernel/VS/FS. This
            # real call returned the stage-specific refusal; record its real
            # error alongside the verified container TYPE instead of guessing.
            if case["function_type"] not in (0, 1, 2) and row.get("error", "").startswith("no Kernel function"):
                row.update(status="UNSUPPORTED", failure_detail="function TYPE not exposed by compute/render API")
            return index, row

        rows = [None] * len(cases)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(attempt, i, case) for i, case in enumerate(cases)]
            for completed, future in enumerate(as_completed(futures), 1):
                index, row = future.result()
                rows[index] = row
                if completed % 10 == 0 or completed == len(cases):
                    print(f"CORPUS progress {completed}/{len(cases)}", flush=True)
        report.update(summarize(rows), functions=rows, libraries=libraries,
            real_function_count=len({(r["metallib_sha256"], r["function"]) for r in rows}),
            error_histogram=dict(sorted(Counter(r.get("error", "") for r in rows if r["status"] != "PASS").items())))
        if stack_module.compute_stack_key(ROOT) != identity:
            raise RuntimeError("source bytes changed during corpus execution")
        if any(digest_file(Path(library["path"])) != library["sha256"] for library in libraries):
            raise RuntimeError("read-only corpus changed during execution")
        comparison = compare_wave3(args.baseline_report.resolve(), rows, libraries)
        report["wave3_comparison"] = comparison
        if report["real_function_count"] < max(args.min_functions, WAVE3_FUNCTION_COUNT):
            raise RuntimeError("24A446 function coverage gate failed")
        if comparison["compile_gain"] <= 0 or comparison["regressions"]:
            raise RuntimeError("compile gain or Wave 3 regression gate failed")
        if report["record_roundtrip_ok"] != report["compiled_ok"] or "RECORD_ERR" in report["failed_by_category"]:
            raise RuntimeError("A840PR01 roundtrip gate failed")
        if report["binding_plan_ok"] != report["compiled_ok"]:
            raise RuntimeError("binding plan gate failed")
        report["status"] = "PASS"
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix(output.suffix + ".tmp")
        temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        temporary.replace(output)
    summary = {k: report[k] for k in ("status", "total_attempted", "compiled_ok", "record_roundtrip_ok",
        "binding_plan_ok", "failed_by_category", "real_function_count", "error") if k in report}
    if "wave3_comparison" in report:
        summary["wave3_comparison"] = report["wave3_comparison"]
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
