#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run independent Phase 2 host gates; never certify or operate a device.

Available suites run in both modes; absent lanes are PENDING in dry-run and
FAIL in full. C/C++ suites use -Werror and ASan/UBSan, plus real GCC TSan
--tsan-stress 20 for the KMD. All outputs use the shared .cache/<subdir>
guard. No source writes, package installation, uploads, or device commands.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil

from check_contracts import (GateError, cache_directory, cache_environment,
                             checked_cache_path, run_command, write_json)
from driver_constants_test import LITERALS, strip_literals
from no_fallback_test import findings
from pm4_check import decode_stream, read_words
from pm4_xml_index import build_index
from stack_key import compute_stack_key, header_bytes, check_header
from verify_wave import clang_configuration

HOOK = "a840_hwtest_path_for_command_buffer"
HOOK_SIGNATURE = re.compile(r"\bconst\s+char\s*\*\s*" + HOOK + r"\s*\(\s*void\s*\*\s*[A-Za-z_0-9]*\s*\)")
STRICT_FLAGS = ["-Wall", "-Wextra", "-Werror", "-pedantic", "-fno-omit-frame-pointer", "-fno-pie", "-pthread"]
INCLUDES = ("contracts", "kits/Adreno840", "kits/Adreno840/gen", "kits/Adreno840Emit",
            "drivers/Adreno840Metal/core", "drivers/Adreno840Metal/transport", "drivers/Adreno840Kernel",
            "adreno_compile/cache", "apps/NezhaGpuTest/ref")
SUITE_TESTS = {
    "runtime_core_e2e": ("drivers/Adreno840Metal/core/test/test_runtime_core.cpp", "PENDING_W03_L3"),
    "iokit_wire_e2e": ("drivers/Adreno840Metal/transport/test/test_iokit_wire.cpp", "PENDING_W04_L3"),
    "kmd_v3_e2e": ("drivers/Adreno840Kernel/test/test_kmd_v3.cpp", "PENDING_W03_L1"),
    "record_cache_e2e": ("adreno_compile/cache/test_record_io.cpp", "PENDING_W03_L2"),
    "nezha_gputest_host": ("apps/NezhaGpuTest/test/test_cpu_reference.cpp", "PENDING_W03_L4"),
}


def source_file(repo: Path, relative: str) -> Path:
    path = repo
    for part in Path(relative).parts:
        path /= part
        if path.is_symlink():
            raise GateError(f"SYMLINK_SOURCE: {relative}")
    if not path.is_file():
        raise GateError(f"MISSING_SOURCE: {relative}")
    return path


def c_sources(repo: Path, relative: str) -> list[str]:
    root = repo / relative
    if root.is_symlink():
        raise GateError(f"SYMLINK_SOURCE: {relative}")
    return [path.relative_to(repo).as_posix() for path in sorted(root.glob("*.c"))
            if not path.name.startswith("test_")]


def execute(row: dict, command: list[str], cwd: Path, env: dict[str, str], timeout: int = 300) -> dict:
    result = run_command(command, cwd=cwd, env=env, timeout=timeout)
    row["commands"].append(result)
    if result["returncode"]:
        raise GateError(f"COMMAND_FAILED exit={result['returncode']}: " + (result["stderr"].strip() or result["stdout"].strip()))
    return result


def json_pass(result: dict) -> dict:
    try:
        report = json.loads(result["stdout"])
    except ValueError as exc:
        raise GateError("INVALID_GATE_JSON") from exc
    if not isinstance(report, dict) or report.get("status") != "PASS":
        raise GateError("SUBGATE_JSON_FAILED: " + result["stdout"].strip())
    return report


def fresh_output(path: Path) -> Path:
    if path.is_symlink():
        raise GateError(f"SYMLINK_OUTPUT: {path}")
    path = checked_cache_path(path)
    cache_directory(path.parent)
    if path.exists():
        path.unlink()
    return path


def build_test(row: dict, repo: Path, cache: Path, env: dict[str, str], sources: list[str],
               test: str, compiler: str, *, sanitizer: str = "address,undefined", archive: str | None = None,
               archive_sources: list[str] | None = None) -> Path:
    output = cache_directory(cache / compiler)
    cc, cxx, extra = clang_configuration() if compiler == "clang18" else ("gcc", "g++", [])
    flags = [*STRICT_FLAGS, "-fsanitize=" + sanitizer, *extra]
    if sanitizer == "thread":
        flags += ["-O1", "-g"]
    flags += [arg for relative in INCLUDES if (repo / relative).is_dir() for arg in ("-I", str(repo / relative))]
    objects = {}
    for index, relative in enumerate(sorted(set(sources))):
        source = source_file(repo, relative)
        target = fresh_output(output / f"source-{index}.o")
        program, standard = (cc, "c11") if source.suffix == ".c" else (cxx, "c++17")
        execute(row, [program, "-std=" + standard, *flags, "-c", str(source), "-o", str(target)], output, env)
        objects[relative] = str(target)
    if archive:
        selected = archive_sources if archive_sources is not None else list(objects)
        if not selected or any(relative not in objects for relative in selected):
            raise GateError("ARCHIVE_SOURCE_SET_MISSING")
        target = fresh_output(output / archive)
        execute(row, ["ar", "rcs", str(target), *(objects[relative] for relative in selected)], output, env)
        row.setdefault("artifacts", []).append(str(target))
        checked = json_pass(execute(row, [sys.executable, "-B", str(source_file(repo, "tools/verify/no_fallback_test.py")),
                                         "--profile", "final", "--artifact", str(target)], output, env))
        if not checked.get("symbols_scanned"):
            raise GateError("EMPTY_NATIVE_ARCHIVE")
    binary = fresh_output(output / "test")
    execute(row, [cxx, "-std=c++17", *flags, str(source_file(repo, test)), *objects.values(),
                  "-ldl", "-lm", "-no-pie", "-o", str(binary)], output, env)
    return binary


def context_rows(repo: Path) -> list[dict]:
    with source_file(repo, "kits/Adreno840/caps/context_init_a840.tsv").open(encoding="utf-8") as source:
        rows = list(csv.DictReader((line for line in source if not line.startswith("#")), delimiter="\t"))
    required = {"reg_offset_hex", "value_hex", "scope", "note"}
    if not rows or any(not required <= row.keys() for row in rows):
        raise GateError("INVALID_CONTEXT_INIT_TABLE")
    return rows


def validate_pm4(words: list[int], index: dict, rows: list[dict], require_context: bool = False) -> dict:
    packets = decode_stream(words, index, "A8XX")
    if not packets:
        raise GateError("EMPTY_PM4_STREAM")
    blocked = {int(row["reg_offset_hex"], 0) for row in rows if row["scope"] == "umd_blocked_a8xx"}
    registers = [(reg["register"], reg["value"]) for packet in packets if packet["type"] == 4 for reg in packet["registers"]]
    bad = sorted({reg for reg, _ in registers} & blocked)
    if bad:
        raise GateError("UMD_BLOCKED_REGISTER: " + ",".join(f"0x{reg:x}" for reg in bad))
    if require_context:
        packet_cursor = 0

        def expect7(name: str, payload: list[int] | None = None) -> dict:
            nonlocal packet_cursor
            if packet_cursor >= len(packets):
                raise GateError(f"CONTEXT_INIT_PREFIX_MISSING: {name}")
            packet = packets[packet_cursor]
            if packet["type"] != 7 or name not in packet["names"] or (payload is not None and packet["payload"] != payload):
                raise GateError(f"CONTEXT_INIT_PREFIX_MISMATCH: {name}")
            packet_cursor += 1
            return packet

        expect7("CP_THREAD_CONTROL", [3])
        for event in (25, 24, 40, 51):
            expect7("CP_EVENT_WRITE7", [event])
        expect7("CP_WAIT_FOR_IDLE", [])
        if packet_cursor >= len(packets) or packets[packet_cursor]["type"] != 4 or \
                packets[packet_cursor]["payload"] != [63] or not any(
                    "SP_UPDATE_CNTL" in name for name in packets[packet_cursor]["registers"][0]["names"]):
            raise GateError("CONTEXT_INIT_PREFIX_MISSING: SP_UPDATE")
        packet_cursor += 1
        expect7("CP_WAIT_FOR_IDLE", [])
        required_regs = [row for row in rows if row["scope"] in {"static_ctx", "rp_init"} and
                         "skipped by context_init" not in row["note"]]
        border_address = None
        for row in required_regs:
            reg, expected = int(row["reg_offset_hex"], 0), int(row["value_hex"], 0)
            optional = "conditionally enabled reset" in row["note"]
            packet = packets[packet_cursor] if packet_cursor < len(packets) else None
            if optional and (packet is None or packet["type"] != 4 or packet["identifier"] != reg):
                continue
            if packet is None or packet["type"] != 4 or packet["identifier"] != reg:
                raise GateError(f"CONTEXT_INIT_REGISTER_MISSING: 0x{reg:x}")
            value = packet["payload"][0]
            if "runtime border_color_gpu_va" in row["note"]:
                if len(packet["payload"]) != 2:
                    raise GateError("CONTEXT_INIT_BORDER_VA_HIGH_MISSING")
                address = value | (packet["payload"][1] << 32)
                if address == 0 or address >= (1 << 48) or address % 128 or (border_address is not None and address != border_address):
                    raise GateError("CONTEXT_INIT_BORDER_VA_INVALID")
                border_address = address
            else:
                allowed = {expected, expected | 4} if "use_tex_coord_nearest_even" in row["note"] else {expected}
                if len(packet["payload"]) != 1 or value not in allowed:
                    raise GateError(f"CONTEXT_INIT_VALUE_MISMATCH: 0x{reg:x}")
            packet_cursor += 1
        expect7("CP_SET_DRAW_STATE", [1 << 18, 0, 0])
        counts = expect7("CP_BV_BR_COUNT_OPS")["payload"]
        if len(counts) != 2 or counts[0] != 2 or not 1 <= counts[1] <= 65535:
            raise GateError("CONTEXT_INIT_PIPE_COUNTS_MISSING")
        for thread in (1 | (1 << 27), 2):
            expect7("CP_THREAD_CONTROL", [thread])
            payload = expect7("CP_SET_AMBLE")["payload"]
            if len(payload) != 3 or payload[2] >> 20 != 1 or not (payload[2] & ((1 << 20) - 1)) or \
                    not 0 < (payload[0] | (payload[1] << 32)) < (1 << 48) or payload[0] % 4:
                raise GateError("CONTEXT_INIT_BIN_AMBLE_INVALID")
        expect7("CP_THREAD_CONTROL", [3])
        expect7("CP_REG_TEST", [(1 << 31) | (1 << 25), (1 << 3) | (1 << 4), 1 << 4])
        for kind in (0, 2):
            payload = expect7("CP_SET_AMBLE")["payload"]
            if len(payload) != 3 or payload[2] >> 20 != kind:
                raise GateError("CONTEXT_INIT_PREEMPT_AMBLE_INVALID")
            count = payload[2] & ((1 << 20) - 1)
            address = payload[0] | (payload[1] << 32)
            if (count == 0 and address != 0) or (count > 0 and (not 0 < address < (1 << 48) or address % 4)):
                raise GateError("CONTEXT_INIT_PREEMPT_AMBLE_INVALID")
        expect7("CP_THREAD_CONTROL", [1 | (1 << 27)])
        if packet_cursor >= len(packets):
            raise GateError("CONTEXT_INIT_FRAME_BODY_MISSING")
    return {"packets": len(packets), "dwords": len(words), "context_checked": require_context,
            "blocked_registers": []}


def check_stream(row: dict, repo: Path, mesa: Path, sample: Path, env: dict[str, str], context: bool) -> dict:
    sample = checked_cache_path(sample)
    if not sample.is_file() or sample.is_symlink():
        raise GateError("PM4_OUTPUT_MISSING")
    checked = json_pass(execute(row, [sys.executable, "-B", str(source_file(repo, "tools/verify/pm4_check.py")),
                                     "--mesa", str(mesa), "--chip", "A8XX", "--format", "binary", str(sample)], sample.parent, env))
    if not checked.get("packet_count"):
        raise GateError("EMPTY_PM4_STREAM")
    result = validate_pm4(read_words(sample.read_bytes(), "binary"), build_index(mesa), context_rows(repo), context)
    result.update(path=str(sample), sha256=hashlib.sha256(sample.read_bytes()).hexdigest())
    return result


def apple_lexical_text(raw: str, relative: str) -> tuple[str, str]:
    strings = []
    for match in LITERALS.finditer(raw):
        token = match.group()
        if token.startswith("/*") and not token.endswith("*/"):
            raise GateError(f"UNTERMINATED_APPLE_SOURCE: {relative}")
        if token.startswith(("//", "/*")):
            continue
        if not re.match(r'(?:u8|u|U|L)?R"', token) and re.search(
                r"[\r\n]", re.sub(r"\\\r?\n", "", token)):
            raise GateError(f"INVALID_APPLE_LITERAL: {relative}")
        strings.append(token)
    clean = strip_literals(raw)
    # C++ digit separators survive strip_literals; they are not character quotes.
    remaining = re.sub(r"(?<=[0-9A-Fa-f])'(?=[0-9A-Fa-f])", "", clean)
    if '"' in remaining or "'" in remaining or "/*" in remaining:
        raise GateError(f"UNTERMINATED_APPLE_SOURCE: {relative}")
    return clean, "\n".join(strings)


def apple_source_checks(repo: Path, mode: str) -> dict:
    required = ("apps/NezhaGpuTest/shaders/NezhaGpuTest.metal", "apps/NezhaGpuTest/ios/NezhaGpuTestRunner.mm",
                "apps/NezhaGpuTest/ios/main.mm")
    paths = [source_file(repo, relative) for relative in required]
    for directory in ("drivers/Adreno840Metal/transport", "drivers/Adreno840Metal/objc"):
        paths.extend(source_file(repo, path.relative_to(repo).as_posix()) for path in sorted((repo / directory).glob("*.mm")))
    for path in paths:
        raw = path.read_text(encoding="utf-8")
        clean, strings = apple_lexical_text(raw, path.relative_to(repo).as_posix())
        violations = findings(clean + "\n" + strings, "final")
        if re.search(r"\b(?:Vk[A-Z]\w*|vk[A-Z]\w*|Turnip)\b", clean):
            violations.append(("VULKAN_SYMBOL", "Vulkan/Turnip identifier"))
        if violations:
            raise GateError(f"FORBIDDEN_APPLE_SYMBOL: {path.relative_to(repo)} {violations}")
        stack = []
        for character in clean:
            if character in "{[(":
                stack.append(character)
            elif character in "}])" and (not stack or stack.pop() != {"}": "{", "]": "[", ")": "("}[character]):
                raise GateError(f"UNBALANCED_APPLE_SOURCE: {path.relative_to(repo)}")
        if stack:
            raise GateError(f"UNBALANCED_APPLE_SOURCE: {path.relative_to(repo)}")
    runner = strip_literals(paths[1].read_text(encoding="utf-8"))
    if not HOOK_SIGNATURE.search(runner) or len(re.findall(r"\b" + HOOK + r"\s*\(", runner)) < 2:
        raise GateError("HWTEST_HOOK_CALLER_SIGNATURE_MISMATCH")
    hook_path = repo / "drivers/Adreno840Metal/transport/a840_hwtest_hook.c"
    hook_state = "PENDING_W04_L3"
    if hook_path.exists() or hook_path.is_symlink():
        hook = strip_literals(source_file(repo, "drivers/Adreno840Metal/transport/a840_hwtest_hook.c").read_text(encoding="utf-8"))
        if not re.search(HOOK_SIGNATURE.pattern + r"\s*\{", hook):
            raise GateError("HWTEST_HOOK_IMPLEMENTATION_SIGNATURE_MISMATCH")
        header = repo / "drivers/Adreno840Metal/transport/a840_hwtest_hook.h"
        if header.exists() or header.is_symlink():
            declaration = strip_literals(source_file(repo, "drivers/Adreno840Metal/transport/a840_hwtest_hook.h").read_text(encoding="utf-8"))
            if not HOOK_SIGNATURE.search(declaration):
                raise GateError("HWTEST_HOOK_HEADER_SIGNATURE_MISMATCH")
        hook_state = "MATCHED"
    elif mode == "full":
        raise GateError("HWTEST_HOOK_IMPLEMENTATION_MISSING")
    return {"files": [path.relative_to(repo).as_posix() for path in paths], "lexical_status": "PASS",
            "forbidden_symbols": [], "hook": hook_state, "apple_sdk_compile": "PENDING_MAC_CI"}


def run_native_suite(row: dict, repo: Path, mesa: Path, cache: Path, env: dict[str, str], name: str,
                     mode: str, with_clang: bool) -> None:
    test = SUITE_TESTS[name][0]
    chip = ["kits/Adreno840/chip_facts_a840.c"]
    emit = c_sources(repo, "kits/Adreno840Emit")
    core = c_sources(repo, "drivers/Adreno840Metal/core")
    archive = None
    selected = None
    if name == "runtime_core_e2e":
        sources = chip + emit + core
        archive = "liba840_runtime_core.a"
        selected = [path for path in core if Path(path).name != "a840_mock_transport.c"]
    elif name == "iokit_wire_e2e":
        transport = c_sources(repo, "drivers/Adreno840Metal/transport")
        sources = chip + ["kits/Adreno840/a840_context_init.c", "kits/Adreno840/a840_abi_v3_host.c"] + emit + core + transport
        archive, selected = "liba840_iokit_wire.a", transport
    elif name == "kmd_v3_e2e":
        kmd = c_sources(repo, "drivers/Adreno840Kernel")
        sources = chip + ["kits/Adreno840/a840_abi_v3_host.c", "kits/Adreno840/a840_context_init.c"] + emit + kmd
        archive, selected = "liba840_kmd_v3.a", kmd
    elif name == "record_cache_e2e":
        sources = ["adreno_compile/cache/a840_record_io.c", "adreno_compile/src/adreno_compile_stub.c"]
    else:
        sources = ["apps/NezhaGpuTest/ref/cpu_reference.cpp"]
    samples = []
    for compiler in (["gcc", "clang18"] if with_clang else ["gcc"]):
        binary = build_test(row, repo, cache, env, sources, test, compiler, archive=archive, archive_sources=selected)
        arguments = []
        sample = None
        if name in {"runtime_core_e2e", "iokit_wire_e2e"}:
            sample = fresh_output(binary.parent / "submitted.pm4")
            arguments = ["--dump-submitted-pm4" if name == "runtime_core_e2e" else "--dump-full-frame-pm4", str(sample)]
        elif name == "record_cache_e2e":
            arguments = [str(cache_directory(binary.parent / "disk-cache"))]
        elif name == "nezha_gputest_host":
            sample = fresh_output(binary.parent / "sample.hwtest")
            arguments = ["--emit-sample-hwtest", str(sample)]
        execute(row, [str(binary), *arguments], binary.parent, env)
        if sample and name != "nezha_gputest_host":
            samples.append(check_stream(row, repo, mesa, sample, env, name == "iokit_wire_e2e"))
        elif sample:
            checked = json_pass(execute(row, [sys.executable, "-B", str(source_file(repo, "tools/verify/hwtest_parser.py")),
                                             "--predicates", str(source_file(repo, "contracts/DualPathPredicates.tsv")),
                                             "--policy", "phase2", "--input", str(sample)], sample.parent, env))
            if checked.get("fallback_count") or len(checked.get("records", [])) != 8:
                raise GateError("HWTEST_SAMPLE_FAILED")
    if samples:
        row["samples"] = samples
        compared = json_pass(execute(row, [sys.executable, "-B", str(source_file(repo, "tools/verify/pm4_diff.py")),
                                           "--mesa", str(mesa), "--chip", "A8XX", "--format", "binary", "--strict-packets",
                                           "--expected", samples[0]["path"], "--actual", samples[-1]["path"]], cache, env))
        row["pm4_comparison"] = "GCC_VS_CLANG" if len(samples) == 2 else "SELF_CONSISTENCY"
        row["pm4_operations"] = compared["actual_operations"]
    if name == "kmd_v3_e2e":
        binary = build_test(row, repo, cache / "tsan", env, sources, test, "gcc", sanitizer="thread")
        prefix = ["setarch", "x86_64", "-R"] if shutil.which("setarch") else []
        execute(row, [*prefix, str(binary), "--tsan-stress", "20"], binary.parent, env, timeout=600)
        row["tsan_rounds"] = 20
    if name == "nezha_gputest_host":
        row["apple_sources"] = apple_source_checks(repo, mode)


def run_gate(repo: str | Path, mesa: str | Path, cache_dir: str | Path,
             mode: str = "dry-run", with_clang: bool = False) -> dict:
    if mode not in {"dry-run", "full"}:
        raise GateError("invalid Phase 2 gate mode")
    if Path(repo).is_symlink():
        raise GateError("SYMLINK_REPOSITORY")
    repo, mesa = Path(repo).resolve(), Path(mesa).resolve()
    cache = cache_directory(cache_dir)
    env = cache_environment(cache)
    env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               TSAN_OPTIONS="halt_on_error=1", A840_VERIFY_MESA=str(mesa))
    report = {"status": "PASS", "mode": mode, "with_clang": with_clang, "phase2_host_ready": False,
              "device_verified": False, "repo": str(repo), "cache_dir": str(cache), "results": []}

    def stage(name):
        row = {"name": name, "status": "PASS", "commands": []}
        report["results"].append(row)
        return row

    key_row = stage("stack_key")
    try:
        key = compute_stack_key(repo)
        if key != compute_stack_key(repo):
            raise GateError("STACK_KEY_DRIFT_BEFORE_GATES")
        write_json(cache / "stack_key.json", key)
        header = fresh_output(cache / "A840StackKey.h")
        header.write_bytes(header_bytes(key))
        check_header(header, key)
        key_row.update(stack_key_hex=key["stack_key_hex"], stack_digest_sha256=key["stack_digest_sha256"])
    except (GateError, OSError, ValueError) as exc:
        key_row.update(status="FAIL", error=str(exc))
        report["status"] = "FAIL"
        write_json(cache / "phase2_host_summary.json", report)
        return report
    wave = stage("verify_wave")
    try:
        summary = fresh_output(cache / "wave/verify_wave_summary.json")
        command = [sys.executable, "-B", str(source_file(repo, "tools/verify/verify_wave.py")),
                   "--repo", str(repo), "--mesa", str(mesa), "--cache-dir", str(summary.parent)]
        if with_clang:
            command.append("--with-clang")
        execute(wave, command, repo, env, timeout=1800)
        outcome = json.loads(summary.read_text(encoding="utf-8"))
        if not isinstance(outcome, dict) or outcome.get("status") != "PASS" or not isinstance(outcome.get("results"), list) or \
                any(not isinstance(row, dict) for row in outcome["results"]):
            raise GateError("VERIFY_WAVE_REPORT_FAILED")
        statuses = {row.get("name"): row.get("status") for row in outcome["results"]}
        if mode == "full" and (any(value != "PASS" for value in statuses.values()) or any(
                statuses.get(name) != "PASS" for name in ("kits/Adreno840", "kits/Adreno840Emit", "adreno_compile", "translate"))):
            raise GateError("VERIFY_WAVE_INCOMPLETE")
        wave["summary"] = str(summary)
    except (GateError, OSError, ValueError) as exc:
        wave.update(status="FAIL", error=str(exc))
    for name, (test, pending) in SUITE_TESTS.items():
        row = stage(name)
        path = repo / test
        if not path.exists() and not path.is_symlink():
            row.update(status="FAIL" if mode == "full" else pending, error="MISSING_MODULE: " + test)
            continue
        try:
            run_native_suite(row, repo, mesa, cache_directory(cache / name), env, name, mode, with_clang)
        except (GateError, OSError, ValueError) as exc:
            row.update(status="FAIL", error=str(exc))
    final = stage("stack_key_after_gates")
    try:
        if compute_stack_key(repo) != key:
            raise GateError("STACK_KEY_DRIFT_AFTER_GATES")
    except (GateError, OSError, ValueError) as exc:
        final.update(status="FAIL", error=str(exc))
    report["status"] = "FAIL" if any(row["status"] == "FAIL" for row in report["results"]) else "PASS"
    report["phase2_host_ready"] = all(row["status"] == "PASS" for row in report["results"]) and all(
        row.get("apple_sources", {}).get("hook", "MATCHED") == "MATCHED" for row in report["results"])
    write_json(cache / "phase2_host_summary.json", report)
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
        print(json.dumps({"status": "FAIL", "phase2_host_ready": False, "device_verified": False, "error": str(exc)}))
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    print("\nGATE                          STATUS")
    for row in report["results"]:
        print(f"{row['name']:<30}{row['status']}")
    print(f"Phase2 host: {report['status']} | phase2_host_ready={report['phase2_host_ready']} | device_verified=false")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
