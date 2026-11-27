#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 2 gate and public-manifest positive/negative tests in w04-l4-test.

Full fixtures are clearly mock workloads but really compile C/C++, run nm,
decode pinned Mesa PM4 and execute a TSan stress binary. Current-tree dry-run
unit coverage isolates only verify_wave to avoid recursive unittest; the
task CLI independently runs the real current-tree host gate.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import phase2_host_gate as host
import phase2_mac_ci_manifest as public
from check_contracts import CACHE_PARENT, GateError, cache_directory, cache_environment, write_json
from pm4_check import type4_header, type7_header
from pm4_xml_index import DEFAULT_MESA, MESA_COMMIT, build_index
from stack_key import SOURCE_ROOTS
import test_hwtest_parser as hwtests
import test_phase1_gates as phase1
import test_verify_tools as legacy

TEST_CACHE = CACHE_PARENT / "w04-l4-test"
hwtests.TEST_CACHE = phase1.TEST_CACHE = legacy.TEST_CACHE = TEST_CACHE
REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
MESA = Path(os.environ.get("A840_VERIFY_MESA", str(DEFAULT_MESA)))


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        cache_directory(TEST_CACHE)
        temp = tempfile.TemporaryDirectory(prefix=self.__class__.__name__ + "-", dir=TEST_CACHE)
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)
        self.env = cache_environment(self.directory)
        self.repo = self.directory / "repo"
        for root in SOURCE_ROOTS:
            (self.repo / root).mkdir(parents=True)
        self.put("contracts/ABI.h", "/* fixture contract */\n")
        self.put("kits/Adreno840/chip_facts_a840.c", "void fixture_chip(void) {}\n")
        self.put("kits/Adreno840/a840_abi_v3_host.c", "void fixture_abi(void) {}\n")
        self.put("kits/Adreno840/a840_context_init.c", "void fixture_context(void) {}\n")
        self.put("kits/Adreno840Emit/fixture.c", "void fixture_emit(void) {}\n")
        self.put("translate/src/lib.rs", "// public fixture\n")
        self.put("translate/Cargo.toml", '[package]\nname="fixture"\nversion="0.1.0"\n')
        self.put("adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json", json.dumps({"mesa_commit": MESA_COMMIT}))
        self.put("adreno_compile/src/adreno_compile_stub.c", "void fixture_compiler(void) {}\n")

    def put(self, relative: str, data: str | bytes) -> Path:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode("utf-8") if isinstance(data, str) else data)
        return path

    def cli(self, script: str, *args: object) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(TOOLS / script), *map(str, args)],
                              cwd=self.directory, env=self.env, capture_output=True, text=True, timeout=120, check=False)


class PublicManifestTests(Fixture):
    def test_valid_manifest_is_sorted_and_deterministic(self) -> None:
        self.put("drivers/Example/example.mm", "// public ObjC++\n")
        a, b = public.generate_manifest(self.repo), public.generate_manifest(self.repo)
        self.assertEqual(a, b)
        paths = [row["path"] for row in a["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(len(a["manifest_sha256"]), 64)
        self.assertIn("stack_key_u64", a["stack_key"])

    def test_manifest_hash_matches_canonical_encoding(self) -> None:
        report = public.generate_manifest(self.repo)
        digest = report.pop("manifest_sha256")
        canonical = json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
        self.assertEqual(digest, hashlib.sha256(canonical).hexdigest())

    def test_verify_manifest_roundtrip_and_single_byte_change(self) -> None:
        output = self.directory / "manifest.json"
        report = public.generate_manifest(self.repo)
        write_json(output, report)
        self.assertEqual(public.verify_manifest(self.repo, output), report)
        path = self.repo / "contracts/ABI.h"
        path.write_bytes(path.read_bytes().replace(b"fixture", b"Fixture", 1))
        with self.assertRaisesRegex(GateError, "contracts/ABI.h"):
            public.verify_manifest(self.repo, output)

    def test_verify_detects_deleted_and_added_source(self) -> None:
        output = self.directory / "manifest.json"
        write_json(output, public.generate_manifest(self.repo))
        self.put("translate/src/new.rs", "// new public\n")
        (self.repo / "contracts/ABI.h").unlink()
        with self.assertRaises(GateError):
            public.verify_manifest(self.repo, output)

    def test_metallib_is_rejected(self) -> None:
        self.put("translate/sample" + ".metallib", "private binary")
        with self.assertRaisesRegex(GateError, "FORBIDDEN_INPUT_SUFFIX"):
            public.generate_manifest(self.repo)

    def test_key_file_is_rejected(self) -> None:
        self.put("drivers/device" + ".key", "secret")
        with self.assertRaisesRegex(GateError, "FORBIDDEN_INPUT_SUFFIX"):
            public.generate_manifest(self.repo)

    def test_all_forbidden_suffixes_including_double_extensions(self) -> None:
        for suffix in public.FORBIDDEN_SUFFIXES:
            with self.subTest(suffix=suffix), self.assertRaises(GateError):
                public.validate_public_path("translate/input" + suffix + ".txt")

    def test_private_path_is_rejected(self) -> None:
        self.put("translate/" + "private" + "/source.rs", "// private")
        with self.assertRaisesRegex(GateError, "FORBIDDEN_INPUT_PATH"):
            public.generate_manifest(self.repo)

    def test_top_level_evidence_runs_and_receipts_are_rejected(self) -> None:
        for segment in ("evidence", "runs", "receipts", ".cache"):
            path = self.put(segment + "/fixture.txt", "forbidden")
            with self.assertRaisesRegex(GateError, "FORBIDDEN_INPUT_PATH"):
                public.generate_manifest(self.repo)
            path.unlink(); path.parent.rmdir()

    def test_private_key_headers_are_rejected(self) -> None:
        for header in public.PRIVATE_HEADERS:
            self.put("translate/src/secret.rs", b"-----" + header + b"-----\n")
            with self.assertRaisesRegex(GateError, "PRIVATE_KEY_IN_SOURCE"):
                public.generate_manifest(self.repo)

    def test_device_identifier_content_is_rejected(self) -> None:
        self.put("contracts/device.txt", public.DEVICE_IDENTIFIER.upper())
        with self.assertRaisesRegex(GateError, "DEVICE_IDENTIFIER_IN_SOURCE"):
            public.generate_manifest(self.repo)

    def test_symlink_file_is_rejected(self) -> None:
        (self.repo / "translate/src/link.rs").symlink_to(self.repo / "contracts/ABI.h")
        with self.assertRaisesRegex(GateError, "SYMLINK_INPUT"):
            public.generate_manifest(self.repo)

    def test_symlink_ancestor_directory_is_rejected(self) -> None:
        (self.repo / "apps").symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(GateError, "SYMLINK_INPUT"):
            public.generate_manifest(self.repo)

    def test_symlink_cannot_hide_behind_build_exclusion(self) -> None:
        (self.repo / "build").symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(GateError, "SYMLINK_INPUT: build"):
            public.generate_manifest(self.repo)

    def test_unlisted_binary_and_non_utf8_content_are_rejected(self) -> None:
        path = self.put("translate/src/bad.rs", b"\0\0binary")
        with self.assertRaisesRegex(GateError, "BINARY_CONTENT"):
            public.generate_manifest(self.repo)
        path.write_bytes(b"\xff")
        with self.assertRaisesRegex(GateError, "NON_UTF8"):
            public.generate_manifest(self.repo)

    def test_unlisted_extension_is_rejected(self) -> None:
        self.put("translate/blob.dat", "unknown")
        with self.assertRaisesRegex(GateError, "UNLISTED_SOURCE_TYPE"):
            public.generate_manifest(self.repo)

    def test_readme_outside_whitelist_does_not_enter_manifest(self) -> None:
        before = public.generate_manifest(self.repo)
        self.put("README.md", "public root readme")
        self.assertEqual(before, public.generate_manifest(self.repo))

    def test_manifest_metadata_tamper_is_rejected(self) -> None:
        output = self.directory / "manifest.json"
        report = public.generate_manifest(self.repo); report["manifest_sha256"] = "0" * 64
        write_json(output, report)
        with self.assertRaisesRegex(GateError, "PUBLIC_MANIFEST_MISMATCH"):
            public.verify_manifest(self.repo, output)

    def test_malformed_manifest_schema_is_rejected(self) -> None:
        output = self.directory / "manifest.json"
        for value in ([], {"files": None}, {"files": [1]}, {"files": [{"path": None}]}):
            write_json(output, value)
            with self.assertRaisesRegex(GateError, "INVALID_MANIFEST_SCHEMA"):
                public.verify_manifest(self.repo, output)

    def test_cli_generation_verification_and_output_boundary(self) -> None:
        output = self.directory / "manifest.json"
        result = self.cli("phase2_mac_ci_manifest.py", "--repo", self.repo, "--json-out", output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verified = self.cli("phase2_mac_ci_manifest.py", "--repo", self.repo, "--verify-manifest", output)
        self.assertEqual(verified.returncode, 0, verified.stdout)
        blocked = self.cli("phase2_mac_ci_manifest.py", "--repo", self.repo, "--json-out", "/mnt/c/forbidden-manifest.json")
        self.assertEqual(blocked.returncode, 1)

    def test_existing_build_bytecode_is_not_packaged(self) -> None:
        self.put("tools/verify/__pycache__/old.pyc", b"\0bytecode")
        report = public.generate_manifest(self.repo)
        self.assertFalse(any("__pycache__" in row["path"] for row in report["files"]))


MOCK_RUNNER = '''import argparse,json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--repo');p.add_argument('--mesa');p.add_argument('--cache-dir');p.add_argument('--with-clang',action='store_true')
a=p.parse_args();d=Path(a.cache_dir);d.mkdir(parents=True,exist_ok=True)
report={'status':'PASS','mock':True,'results':[{'name':name,'status':'PASS'} for name in ('kits/Adreno840','kits/Adreno840Emit','adreno_compile','translate')]}
(d/'verify_wave_summary.json').write_text(json.dumps(report));print('MOCK host runner PASS')
'''


class HostGateTests(Fixture):
    def setUp(self) -> None:
        super().setUp()
        self.index = build_index(MESA)

    def setup_gate(self, complete: bool = False) -> None:
        for name in ("check_contracts.py", "no_fallback_test.py", "pm4_check.py", "pm4_xml_index.py", "pm4_diff.py", "hwtest_parser.py"):
            self.put("tools/verify/" + name, (TOOLS / name).read_bytes())
        self.put("tools/verify/verify_wave.py", MOCK_RUNNER)
        self.put("contracts/DualPathPredicates.tsv", (REPO / "contracts/DualPathPredicates.tsv").read_bytes())
        self.put("kits/Adreno840/caps/context_init_a840.tsv", (REPO / "kits/Adreno840/caps/context_init_a840.tsv").read_bytes())
        # The CPU suite and Metal/ObjC++ source checks remain actual shared source.
        for relative in ("apps/NezhaGpuTest/ref/cpu_reference.h", "apps/NezhaGpuTest/ref/cpu_reference.cpp",
                         "apps/NezhaGpuTest/test/test_cpu_reference.cpp", "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal",
                         "apps/NezhaGpuTest/ios/NezhaGpuTestRunner.mm", "apps/NezhaGpuTest/ios/main.mm"):
            self.put(relative, (REPO / relative).read_bytes())
        self.put("drivers/Adreno840Metal/core/fixture.c", "void fixture_runtime(void) {}\n")
        words = [type4_header(0x84c, 1), 0, type7_header(0x43, 3), 0, 0, 0]
        self.put(host.SUITE_TESTS["runtime_core_e2e"][0], self.dump_test("--dump-submitted-pm4", words))
        if complete:
            self.put("drivers/Adreno840Metal/transport/a840_hwtest_hook.c",
                     'const char *a840_hwtest_path_for_command_buffer(void *cb) { (void)cb; return "raw"; }\n')
            self.put(host.SUITE_TESTS["iokit_wire_e2e"][0], self.dump_test("--dump-full-frame-pm4", self.context_stream()))
            self.put("drivers/Adreno840Kernel/fixture.c", "void fixture_kmd(void) {}\n")
            self.put(host.SUITE_TESTS["kmd_v3_e2e"][0], '''#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
int main(int argc,char **argv) {
    unsigned rounds=1;
    if(argc==3 && std::strcmp(argv[1],"--tsan-stress")==0) rounds=20;
    for(unsigned i=0;i<rounds;++i) {std::atomic<unsigned> n{0};std::thread t([&] {n.fetch_add(1);});t.join();if(n.load()!=1)return 1;}
    std::puts("MOCK KMD stress PASS");return 0;
}
''')
            self.put("adreno_compile/cache/a840_record_io.c", "void fixture_record(void) {}\n")
            self.put(host.SUITE_TESTS["record_cache_e2e"][0], "int main(int argc,char **) {return argc==2 ? 0 : 1;}\n")

    def dump_test(self, option: str, words: list[int]) -> str:
        data = ",".join(str(value) for value in words)
        return '''#include <cstdio>
#include <cstdint>
#include <cstring>
int main(int argc,char **argv) {
    if(argc!=3 || std::strcmp(argv[1],"''' + option + '''")!=0) return 2;
    const std::uint32_t words[]={''' + data + '''};
    FILE *f=std::fopen(argv[2],"wb");if(!f)return 3;
    const auto n=std::fwrite(words,sizeof(words),1,f);const int closed=std::fclose(f);
    std::puts("MOCK PM4 stream emitted");return n==1 && closed==0 ? 0 : 1;
}
'''

    def context_stream(self) -> list[int]:
        # Construct a protocol fixture from the real TSV, not a device frame.
        words = [type7_header(self.index["opcodes"]["CP_THREAD_CONTROL"], 1), 3]
        for event in (25, 24, 40, 51):
            words += [type7_header(self.index["opcodes"]["CP_EVENT_WRITE7"], 1), event]
        words += [type7_header(self.index["opcodes"]["CP_WAIT_FOR_IDLE"], 0)]
        regs = {record["name"]: record["offset"] for record in self.index["registers"] if record["chip"] == "A8XX"}
        words += [type4_header(regs["A8XX_SP_UPDATE_CNTL"], 1), 63]
        words += [type7_header(self.index["opcodes"]["CP_WAIT_FOR_IDLE"], 0)]
        for row in host.context_rows(self.repo):
            if row["scope"] not in {"static_ctx", "rp_init"} or "skipped by context_init" in row["note"]:
                continue
            reg, value = int(row["reg_offset_hex"], 0), int(row["value_hex"], 0)
            if "runtime border_color_gpu_va" in row["note"]:
                words += [type4_header(reg, 2), 128, 0]
            else:
                words += [type4_header(reg, 1), value]
        words += [type7_header(0x43, 3), 1 << 18, 0, 0]
        words += [type7_header(self.index["opcodes"]["CP_BV_BR_COUNT_OPS"], 2), 2, 1]
        for thread in (1 | (1 << 27), 2):
            words += [type7_header(self.index["opcodes"]["CP_THREAD_CONTROL"], 1), thread]
            words += [type7_header(self.index["opcodes"]["CP_SET_AMBLE"], 3), 256, 0, (1 << 20) | 1]
        words += [type7_header(self.index["opcodes"]["CP_THREAD_CONTROL"], 1), 3]
        words += [type7_header(self.index["opcodes"]["CP_REG_TEST"], 3), (1 << 31) | (1 << 25), 24, 16]
        for kind in (0, 2):
            words += [type7_header(self.index["opcodes"]["CP_SET_AMBLE"], 3), 0, 0, kind << 20]
        words += [type7_header(self.index["opcodes"]["CP_THREAD_CONTROL"], 1), 1 | (1 << 27)]
        words += [type7_header(self.index["opcodes"]["CP_NOP"], 0)]
        return words

    def run_report(self, mode="dry-run", clang=False):
        return host.run_gate(self.repo, MESA, self.directory / "output", mode, clang)

    def row(self, report: dict, name: str) -> dict:
        return next(row for row in report["results"] if row["name"] == name)

    def test_dry_run_runs_available_suites_and_marks_pending(self) -> None:
        self.setup_gate()
        report = self.run_report()
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(self.row(report, "runtime_core_e2e")["status"], "PASS")
        self.assertEqual(self.row(report, "iokit_wire_e2e")["status"], "PENDING_W04_L3")
        self.assertFalse(report["phase2_host_ready"])
        self.assertFalse(report["device_verified"])

    def test_full_missing_lanes_precisely_fails(self) -> None:
        self.setup_gate()
        report = self.run_report("full")
        self.assertEqual(report["status"], "FAIL")
        for name in ("iokit_wire_e2e", "kmd_v3_e2e", "record_cache_e2e"):
            self.assertEqual(self.row(report, name)["status"], "FAIL")
            self.assertIn(host.SUITE_TESTS[name][0], self.row(report, name)["error"])

    def test_full_mock_compiles_and_runs_all_suites_with_real_tsan(self) -> None:
        self.setup_gate(True)
        report = self.run_report("full", clang=True)
        self.assertEqual(report["status"], "PASS", report)
        self.assertTrue(report["phase2_host_ready"])
        self.assertFalse(report["device_verified"])
        self.assertEqual(self.row(report, "kmd_v3_e2e")["tsan_rounds"], 20)
        for name in host.SUITE_TESTS:
            commands = self.row(report, name)["commands"]
            self.assertTrue(any("clang" in Path(command["command"][0]).name for command in commands), name)

    def test_pm4_parity_corruption_is_rejected(self) -> None:
        self.setup_gate()
        words = [type4_header(0x84c, 1) ^ (1 << 7), 0]
        self.put(host.SUITE_TESTS["runtime_core_e2e"][0], self.dump_test("--dump-submitted-pm4", words))
        report = self.run_report()
        self.assertIn("odd-parity", self.row(report, "runtime_core_e2e")["error"])

    def test_runtime_fallback_symbol_is_rejected(self) -> None:
        self.setup_gate()
        self.put("drivers/Adreno840Metal/core/fixture.c", "void native_bridge_metal_init(void) {}\n")
        report = self.run_report()
        self.assertEqual(self.row(report, "runtime_core_e2e")["status"], "FAIL")
        row = self.row(report, "runtime_core_e2e")
        self.assertIn("COMMAND_FAILED exit=1", row["error"])
        refused = json.loads(row["commands"][-1]["stdout"])
        self.assertEqual(refused["violations"][0]["location"], "symbol:native_bridge_metal_init")

    def test_tsan_failure_is_not_hidden(self) -> None:
        self.setup_gate(True)
        test = self.repo / host.SUITE_TESTS["kmd_v3_e2e"][0]
        test.write_text(test.read_text().replace("rounds=20;", "return 9;"))
        report = self.run_report("full")
        self.assertEqual(self.row(report, "kmd_v3_e2e")["status"], "FAIL")
        self.assertIn("exit=9", self.row(report, "kmd_v3_e2e")["error"])

    def test_context_init_prefix_and_values_are_checked(self) -> None:
        self.setup_gate()
        words = self.context_stream()
        result = host.validate_pm4(words, self.index, host.context_rows(self.repo), True)
        self.assertTrue(result["context_checked"])
        words[1] = 1
        with self.assertRaisesRegex(GateError, "CONTEXT_INIT_PREFIX"):
            host.validate_pm4(words, self.index, host.context_rows(self.repo), True)

    def test_context_init_missing_event_and_bad_va_are_rejected(self) -> None:
        self.setup_gate()
        words = self.context_stream()
        with self.assertRaisesRegex(GateError, "CONTEXT_INIT_PREFIX"):
            host.validate_pm4(words[:2] + words[4:], self.index, host.context_rows(self.repo), True)
        packets = host.decode_stream(words, self.index)
        border = next(packet for packet in packets if packet['type'] == 4 and packet['payload_words'] == 2)
        words[border['header_word'] + 1] = 0
        with self.assertRaisesRegex(GateError, "CONTEXT_INIT_BORDER_VA_INVALID"):
            host.validate_pm4(words, self.index, host.context_rows(self.repo), True)

    def test_blocked_register_and_empty_stream_are_rejected(self) -> None:
        self.setup_gate()
        with self.assertRaisesRegex(GateError, "UMD_BLOCKED_REGISTER"):
            host.validate_pm4([type4_header(0x8e09, 1), 0], self.index, host.context_rows(self.repo))
        with self.assertRaisesRegex(GateError, "EMPTY_PM4_STREAM"):
            host.validate_pm4([], self.index, host.context_rows(self.repo))

    def test_apple_lexical_check_rejects_fallback_symbol(self) -> None:
        self.setup_gate()
        path = self.repo / "apps/NezhaGpuTest/ios/main.mm"
        path.write_text(path.read_text() + "void vkCreateDevice(void);\n")
        with self.assertRaisesRegex(GateError, "FORBIDDEN_APPLE_SYMBOL"):
            host.apple_source_checks(self.repo, "dry-run")

    def test_apple_bracket_corruption_is_rejected(self) -> None:
        self.setup_gate()
        path = self.repo / "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal"
        path.write_text(path.read_text() + "{\n")
        with self.assertRaisesRegex(GateError, "UNBALANCED_APPLE_SOURCE"):
            host.apple_source_checks(self.repo, "dry-run")

    def test_apple_unclosed_comment_is_rejected(self) -> None:
        self.setup_gate()
        path = self.repo / "apps/NezhaGpuTest/ios/main.mm"
        path.write_text(path.read_text() + "/* unfinished\n")
        with self.assertRaisesRegex(GateError, "UNTERMINATED_APPLE_SOURCE"):
            host.apple_source_checks(self.repo, "dry-run")

    def test_apple_unclosed_string_and_literal_newline_are_rejected(self) -> None:
        with self.assertRaisesRegex(GateError, "UNTERMINATED_APPLE_SOURCE"):
            host.apple_lexical_text('const char *s = "unfinished;', "fixture.mm")
        with self.assertRaisesRegex(GateError, "INVALID_APPLE_LITERAL"):
            host.apple_lexical_text('const char *s = "bad\nline";', "fixture.mm")

    def test_apple_valid_raw_strings_and_digit_separators(self) -> None:
        clean, strings = host.apple_lexical_text(
            'const auto n = 1\'024; const char *s = R"tag(line\n{[(})tag"; // ignored quote "\n',
            "fixture.mm")
        self.assertIn("1'024", clean)
        self.assertIn("line\n{[(}", strings)
        self.assertNotIn("ignored", strings)

    def test_hook_signature_mismatch_and_missing_implementation_are_rejected(self) -> None:
        self.setup_gate()
        with self.assertRaisesRegex(GateError, "IMPLEMENTATION_MISSING"):
            host.apple_source_checks(self.repo, "full")
        self.put("drivers/Adreno840Metal/transport/a840_hwtest_hook.c", 'const char *a840_hwtest_path_for_command_buffer(int cb) {return "raw";}')
        with self.assertRaisesRegex(GateError, "IMPLEMENTATION_SIGNATURE_MISMATCH"):
            host.apple_source_checks(self.repo, "full")

    def test_hook_header_mismatch_is_rejected(self) -> None:
        self.setup_gate(True)
        self.put("drivers/Adreno840Metal/transport/a840_hwtest_hook.h", 'int a840_hwtest_path_for_command_buffer(void *cb);\n')
        with self.assertRaisesRegex(GateError, "HEADER_SIGNATURE_MISMATCH"):
            host.apple_source_checks(self.repo, "full")

    def test_host_runner_failure_does_not_pass(self) -> None:
        self.setup_gate()
        self.put("tools/verify/verify_wave.py", "raise SystemExit(7)\n")
        report = self.run_report()
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(self.row(report, "verify_wave")["status"], "FAIL")

    def test_stale_wave_summary_cannot_replace_current_output(self) -> None:
        self.setup_gate()
        output = cache_directory(self.directory / "output/wave")
        write_json(output / "verify_wave_summary.json", {"status": "PASS", "results": []})
        self.put("tools/verify/verify_wave.py", "print('MOCK no current summary')\n")
        report = self.run_report()
        self.assertEqual(self.row(report, "verify_wave")["status"], "FAIL")

    def test_stack_key_drift_is_rejected(self) -> None:
        self.setup_gate()
        self.put("tools/verify/verify_wave.py", MOCK_RUNNER +
                 "(Path(a.repo)/'contracts/ABI.h').write_text('changed by MOCK')\n")
        report = self.run_report()
        self.assertEqual(self.row(report, "stack_key_after_gates")["status"], "FAIL")

    def test_cache_path_is_general_and_c_drive_is_rejected(self) -> None:
        self.assertEqual(host.checked_cache_path(CACHE_PARENT / "future-phase/cache"), CACHE_PARENT / "future-phase/cache")
        with self.assertRaises(GateError):
            host.run_gate(self.repo, MESA, "/mnt/c/blocked")
        with self.assertRaises(GateError):
            host.checked_cache_path(CACHE_PARENT)

    def test_current_repository_dry_run_executes_real_native_workloads(self) -> None:
        original = host.run_command
        def isolated(command, **kwargs):
            if any(Path(word).name == "verify_wave.py" for word in command):
                output = Path(command[command.index("--cache-dir") + 1]); cache_directory(output)
                write_json(output / "verify_wave_summary.json", {"status": "PASS", "results": []})
                return {"command": command, "returncode": 0, "stdout": "MOCK host runner", "stderr": "", "seconds": 0}
            return original(command, **kwargs)
        with patch.object(host, "run_command", isolated):
            report = host.run_gate(REPO, MESA, self.directory / "current-dry", "dry-run")
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(self.row(report, "runtime_core_e2e")["status"], "PASS")
        self.assertFalse(report["device_verified"])

    def test_all_three_new_scripts_have_help(self) -> None:
        for script in ("phase2_host_gate.py", "phase2_mac_ci_manifest.py", "test_phase2_gates.py"):
            result = self.cli(script, "--help")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("-v", "--verbose", action="store_true")
    args = cli.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2 if args.verbose else 1)
