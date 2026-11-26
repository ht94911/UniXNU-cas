#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wave 2 key and exit-gate fixtures, confined to .cache/w02-l4-test.

Full-mode fixtures use clearly marked mock builders/host runners, with real
GCC archives, nm checks, Mesa XML and PM4 decoding. The real current-tree
dry-run is exercised by the task's CLI gate; its unit test isolates the
host runner to prevent recursive verify_wave -> unittest -> verify_wave.
Legacy tests are unchanged; their in-memory TEST_CACHE is redirected here.
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

import phase1_e2e_gate as e2e
import stack_key as stack
from check_contracts import CACHE_PARENT, GateError, cache_environment
from pm4_check import type4_header, type7_header
from pm4_xml_index import DEFAULT_MESA, MESA_COMMIT
import test_verify_tools as legacy

TEST_CACHE = CACHE_PARENT / "w02-l4-test"
legacy.TEST_CACHE = TEST_CACHE
REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
MESA = Path(os.environ.get("A840_VERIFY_MESA", str(DEFAULT_MESA)))


class CachedFixture(unittest.TestCase):
    def setUp(self) -> None:
        stack.wave2_cache_directory(TEST_CACHE)
        temporary = tempfile.TemporaryDirectory(prefix=self.__class__.__name__ + "-", dir=TEST_CACHE)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = cache_environment(self.directory)
        self.repo = self.directory / "repo"
        for relative in stack.SOURCE_ROOTS:
            (self.repo / relative).mkdir(parents=True)
        self.put("contracts/AdrenoCompileABI.h", b"/* ABI */\n")
        self.put("kits/Adreno840/chip_facts_a840.c", b"/* chip */\n")
        self.put("kits/Adreno840Emit/a840_cs.c", b"/* PM4 */\n")
        self.put("translate/src/lib.rs", b"// translate\n")
        self.put("translate/Cargo.toml", b'[package]\nname="fixture"\nversion="0.1.0"\n')
        self.put(stack.MESA_MANIFEST, (json.dumps({"mesa_commit": MESA_COMMIT}) + "\n").encode())

    def put(self, relative: str, data: bytes | str) -> Path:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode("utf-8") if isinstance(data, str) else data)
        return path

    def cli(self, script: str, *args: object) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(TOOLS / script), *map(str, args)],
                              cwd=self.directory, env=self.env, capture_output=True, text=True,
                              timeout=120, check=False)

    def key(self) -> dict:
        return stack.compute_stack_key(self.repo)

    def changed_byte_changes_key(self, relative: str) -> None:
        before = self.key()["stack_key_u64"]
        path = self.repo / relative
        data = path.read_bytes()
        # Use whitespace for the JSON mutation, keeping revision metadata valid.
        if relative == stack.MESA_MANIFEST:
            path.write_bytes(data[:-1] + b" ")
        else:
            path.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        self.assertNotEqual(before, self.key()["stack_key_u64"])

    def ignored_change(self, relative: str) -> None:
        before = self.key()
        path = self.put(relative, "ignored fixture\n")
        self.assertEqual(before, self.key())
        path.write_text("changed ignored fixture\n", encoding="utf-8")
        self.assertEqual(before, self.key())


class StackKeyTests(CachedFixture):
    def test_repeated_computation_is_identical(self) -> None:
        self.assertEqual(self.key(), self.key())

    def test_contract_one_byte_changes_key(self) -> None:
        self.changed_byte_changes_key("contracts/AdrenoCompileABI.h")

    def test_chip_one_byte_changes_key(self) -> None:
        self.changed_byte_changes_key("kits/Adreno840/chip_facts_a840.c")

    def test_emitter_one_byte_changes_key(self) -> None:
        self.changed_byte_changes_key("kits/Adreno840Emit/a840_cs.c")

    def test_translator_one_byte_changes_key(self) -> None:
        self.changed_byte_changes_key("translate/src/lib.rs")

    def test_mesa_manifest_one_byte_changes_key(self) -> None:
        self.changed_byte_changes_key(stack.MESA_MANIFEST)

    def test_apps_changes_do_not_change_key(self) -> None:
        self.ignored_change("apps/NezhaGpuTest/app.c")

    def test_verifier_changes_do_not_change_key(self) -> None:
        self.ignored_change("tools/verify/fixture.py")

    def test_chip_test_changes_do_not_change_key(self) -> None:
        self.ignored_change("kits/Adreno840/test/test_fixture.cpp")

    def test_readme_changes_do_not_change_key(self) -> None:
        self.ignored_change("README.md")

    def test_caps_tsv_and_generated_headers_are_included(self) -> None:
        original = self.key()["stack_key_u64"]
        self.put("kits/Adreno840/caps/formats.tsv", "id\tformat\n")
        after_caps = self.key()["stack_key_u64"]
        self.assertNotEqual(original, after_caps)
        self.put("kits/Adreno840/gen/Adreno840Regs.h", "/* regs */\n")
        self.assertNotEqual(after_caps, self.key()["stack_key_u64"])

    def test_all_contract_file_extensions_are_included(self) -> None:
        self.put("contracts/contract.md", "contract prose\n")
        self.put("contracts/predicates.tsv", "predicate\n")
        paths = {record["path"] for record in self.key()["files"]}
        self.assertIn("contracts/contract.md", paths)
        self.assertIn("contracts/predicates.tsv", paths)

    def test_excluded_segments_and_gitkeep_do_not_change_key(self) -> None:
        before = self.key()
        for excluded in sorted(stack.EXCLUDED):
            self.put(f"kits/Adreno840/{excluded}/ignored.c", "ignored\n")
        self.put("translate/.gitkeep", "ignored\n")
        self.put("adreno_compile/docs/ignored.h", "ignored\n")
        self.assertEqual(before, self.key())

    def test_input_paths_are_posix_and_sorted(self) -> None:
        self.put("kits/Adreno840/z.c", "z\n")
        self.put("kits/Adreno840/a.c", "a\n")
        paths = [record["path"] for record in self.key()["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertTrue(all(not path.startswith("/") and "\\" not in path for path in paths))

    def test_host_checkout_path_does_not_change_digest(self) -> None:
        copied = self.directory / "another-repo"
        shutil.copytree(self.repo, copied)
        self.assertEqual(self.key(), stack.compute_stack_key(copied))

    def test_hash_framing_and_big_endian_u64_match_independent_oracle(self) -> None:
        digest = hashlib.sha256()
        for relative, data in stack.source_inputs(self.repo):
            digest.update(("FILE " + relative + "\0" + str(len(data)) + "\0").encode())
            digest.update(data)
        digest.update(b"MESA_COMMIT\0" + MESA_COMMIT.encode() + b"\0")
        digest.update(b"M2V_COMMIT\0" + stack.DEFAULT_M2V_COMMIT.encode() + b"\0")
        report = self.key()
        self.assertEqual(report["stack_digest_sha256"], digest.hexdigest())
        self.assertEqual(report["stack_key_u64"], int.from_bytes(digest.digest()[:8], "big"))
        self.assertEqual(len(report["stack_key_hex"]), 18)
        self.assertEqual(len(report["stack_digest_sha256"]), 64)

    def test_mesa_and_translator_salts_change_key(self) -> None:
        original = self.key()["stack_key_u64"]
        self.assertNotEqual(original, stack.compute_stack_key(self.repo, mesa_commit="1" * 40)["stack_key_u64"])
        self.assertNotEqual(original, stack.compute_stack_key(self.repo, m2v_commit="2" * 40)["stack_key_u64"])

    def test_invalid_revision_and_zero_key_are_rejected(self) -> None:
        with self.assertRaisesRegex(GateError, "40-character"):
            stack.compute_stack_key(self.repo, mesa_commit="not-a-sha")
        with self.assertRaisesRegex(GateError, "zero stack_key"):
            stack.key_from_digest(b"\0" * 32)

    def test_symlinked_source_file_rejected(self) -> None:
        path = self.repo / "translate/src/lib.rs"
        path.unlink()
        path.symlink_to(self.repo / "contracts/AdrenoCompileABI.h")
        with self.assertRaisesRegex(GateError, "symlinked source"):
            self.key()

    def test_symlinked_source_directory_rejected(self) -> None:
        (self.repo / "kits/Adreno840/link").symlink_to(self.repo / "translate", target_is_directory=True)
        with self.assertRaisesRegex(GateError, "symlinked source"):
            self.key()

    def test_output_to_c_and_other_wave_cache_rejected_without_writes(self) -> None:
        for output in ("/mnt/c/blocked-A840StackKey.h", "C:\\Temp\\blocked.h", CACHE_PARENT, "/tmp/blocked-A840StackKey.h"):
            result = self.cli("stack_key.py", "--repo", self.repo, "--header-out", output)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["status"], "FAIL")

    def test_output_symlink_escape_rejected(self) -> None:
        (self.directory / "outside").symlink_to(REPO, target_is_directory=True)
        with self.assertRaises(GateError):
            stack.write_bytes(self.directory / "outside/header.h", b"no write")

    def test_json_and_header_cli_roundtrip_and_one_character_tamper(self) -> None:
        output, header = self.directory / "stack.json", self.directory / "A840StackKey.h"
        result = self.cli("stack_key.py", "--repo", self.repo, "--json-out", output, "--header-out", header)
        self.assertEqual(result.returncode, 0, result.stdout)
        report = json.loads(output.read_text())
        self.assertEqual(report, self.key())
        result = self.cli("stack_key.py", "--repo", self.repo, "--check-header", header)
        self.assertEqual(result.returncode, 0, result.stdout)
        data = header.read_bytes()
        header.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        result = self.cli("stack_key.py", "--repo", self.repo, "--check-header", header)
        self.assertEqual(result.returncode, 1)
        self.assertIn("STACK_HEADER_MISMATCH", result.stdout)

    def test_check_header_is_not_overwritten_before_validation(self) -> None:
        header = self.directory / "A840StackKey.h"
        header.write_bytes(b"stale header")
        result = self.cli("stack_key.py", "--repo", self.repo, "--check-header", header, "--header-out", header)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(header.read_bytes(), b"stale header")

    def test_generated_header_compiles_c11_and_cpp17_with_werror(self) -> None:
        header = self.directory / "A840StackKey.h"
        stack.write_bytes(header, stack.header_bytes(self.key()))
        source = '#include "A840StackKey.h"\nint main(void) { return A840_STACK_KEY_U64 == 0; }\n'
        for compiler, standard, language in (("gcc", "c11", "c"), ("g++", "c++17", "c++")):
            command = [compiler, "-std=" + standard, "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(self.directory), "-x", language, "-c", "-", "-o", str(self.directory / (compiler + ".o"))]
            result = subprocess.run(command, input=source, cwd=self.directory, env=self.env,
                                    capture_output=True, text=True, timeout=60, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_source_root_or_manifest_rejected(self) -> None:
        (self.repo / stack.MESA_MANIFEST).unlink()
        with self.assertRaisesRegex(GateError, "missing pinned Mesa manifest"):
            self.key()


MOCK_RUNNER = '''import argparse, json
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--repo'); p.add_argument('--mesa'); p.add_argument('--cache-dir'); p.add_argument('--with-clang',action='store_true')
a=p.parse_args()
d=Path(a.cache_dir); d.mkdir(parents=True,exist_ok=True)
report={'status':'PASS','repo':a.repo,'cache_dir':a.cache_dir,'with_clang':a.with_clang,
        'mock':True,'results':[{'name':name,'status':'PASS','commands':[]}
                              for name in ('kits/Adreno840','kits/Adreno840Emit','adreno_compile','translate')]}
(d/'verify_wave_summary.json').write_text(json.dumps(report))
print('MOCK host runner PASS')
'''
MOCK_BUILDER = '''import argparse, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--mesa'); p.add_argument('--cache-dir'); p.add_argument('--jobs'); p.add_argument('--run-e2e',action='store_true'); p.add_argument('--dump-disasm')
a=p.parse_args(); assert a.run_e2e
d=Path(a.cache_dir); d.mkdir(parents=True,exist_ok=True)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-x','c','-','-c','-o',str(d/'fixture.o')],
               input='void a840_fixture_backend(void) {}\\n',text=True,check=True)
subprocess.run(['ar','rcs',str(d/'libadreno_compile_ir3.a'),str(d/'fixture.o')],check=True)
(d/'mock-e2e-ran').write_text('explicit mock e2e')
print('MOCK ir3 E2E PASS')
'''
MOCK_REGGEN = '''import argparse
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--mesa'); p.add_argument('--out-dir'); p.add_argument('--check',action='store_true'); p.add_argument('--tmp-dir')
a=p.parse_args(); assert a.check
expected=b'/* deterministic mock registers */\\n'
d=Path(a.tmp_dir); d.mkdir(parents=True,exist_ok=True); (d/'Adreno840Regs.h').write_bytes(expected)
assert (Path(a.out_dir)/'Adreno840Regs.h').read_bytes()==expected, 'mock register regeneration mismatch'
print('MOCK reggen CHECK PASS')
'''


class Phase1GateTests(CachedFixture):
    def tools(self) -> None:
        for name in ("check_contracts.py", "no_fallback_test.py", "pm4_check.py", "pm4_xml_index.py", "pm4_diff.py"):
            self.put("tools/verify/" + name, (TOOLS / name).read_bytes())
        self.put("tools/verify/verify_wave.py", MOCK_RUNNER)

    def complete_fixture(self) -> Path:
        self.tools()
        self.put("adreno_compile/tools/build_adreno_compile_ir3.py", MOCK_BUILDER)
        self.put("kits/Adreno840/gen/Adreno840Regs.h", "/* deterministic mock registers */\n")
        self.put("kits/Adreno840/tools/gen_caps_and_regs.py", MOCK_REGGEN)
        self.put("kits/Adreno840Emit/a840_emit_ops.h", "/* mock emitter API */\n")
        words = [type4_header(0x84c, 1), 0, type7_header(0x43, 3), 0, 0, 0]
        return self.put("kits/Adreno840Emit/sample.pm4", struct.pack("<6I", *words))

    def gate_report(self, mode: str = "full", with_clang: bool = False) -> dict:
        return e2e.run_gate(self.repo, MESA, self.directory / "output", mode, with_clang)

    def result(self, report: dict, name: str) -> dict:
        return next(row for row in report["results"] if row["name"] == name)

    def test_dry_run_mock_runner_reports_pending_without_claiming_complete(self) -> None:
        self.tools()
        report = self.gate_report("dry-run")
        self.assertEqual(report["status"], "PASS", report)
        self.assertFalse(report["phase1_complete"])
        self.assertEqual(self.result(report, "adreno_ir3_e2e")["status"], "PENDING_W02_L2")
        self.assertEqual(self.result(report, "a840_regs_and_emit")["status"], "PENDING_W01_L1_L3")

    def test_current_repo_dry_run_readiness_with_isolated_host_runner(self) -> None:
        # The CLI hard gate independently runs the real host runner; this unit
        # test isolates only that invocation to avoid recursion through unittest.
        original = e2e.run_command
        def isolated(command, **kwargs):
            if any(Path(word).name == "verify_wave.py" for word in command):
                output = Path(command[command.index("--cache-dir") + 1])
                stack.wave2_cache_directory(output)
                stack.write_report(output / "verify_wave_summary.json", {"status": "PASS", "results": []})
                return {"command": command, "returncode": 0, "stdout": "MOCK host runner", "stderr": "", "seconds": 0}
            return original(command, **kwargs)
        with patch.object(e2e, "run_command", isolated):
            report = e2e.run_gate(REPO, MESA, self.directory / "current-dry", "dry-run")
        self.assertEqual(report["status"], "PASS", report)
        self.assertFalse(report["phase1_complete"])

    def test_full_missing_ir3_and_generated_modules_fails(self) -> None:
        self.tools()
        report = self.gate_report()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("IR3_BACKEND_MISSING", self.result(report, "adreno_ir3_e2e")["error"])
        self.assertIn("REGS_EMIT_MISSING", self.result(report, "a840_regs_and_emit")["error"])

    def test_complete_mock_fixture_full_passes_real_nm_and_pm4(self) -> None:
        self.complete_fixture()
        report = self.gate_report()
        self.assertEqual(report["status"], "PASS", report)
        self.assertTrue(report["phase1_complete"])
        self.assertTrue((self.directory / "output/ir3/mock-e2e-ran").is_file())
        sample = self.result(report, "a840_regs_and_emit")["samples"][0]
        self.assertEqual(sample["packets"], 2)
        self.assertEqual(sample["oracle"], "SELF_CONSISTENCY")

    def test_bad_pm4_parity_precisely_fails(self) -> None:
        sample = self.complete_fixture()
        words = list(struct.unpack("<6I", sample.read_bytes()))
        words[0] ^= 1 << 7
        sample.write_bytes(struct.pack("<6I", *words))
        report = self.gate_report()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("odd-parity", self.result(report, "a840_regs_and_emit")["error"])

    def test_register_regeneration_tamper_fails(self) -> None:
        self.complete_fixture()
        self.put("kits/Adreno840/gen/Adreno840Regs.h", "/* stale generated header */\n")
        report = self.gate_report()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("mock register regeneration mismatch", self.result(report, "a840_regs_and_emit")["error"])

    def test_golden_stream_mismatch_is_detected(self) -> None:
        sample = self.complete_fixture()
        golden = list(struct.unpack("<6I", sample.read_bytes()))
        golden[1] ^= 1
        self.put("kits/Adreno840Emit/sample.golden.pm4", struct.pack("<6I", *golden))
        report = self.gate_report()
        self.assertEqual(report["status"], "FAIL")
        commands = self.result(report, "a840_regs_and_emit")["commands"]
        diff = json.loads(commands[-1]["stdout"])
        self.assertEqual(diff["first_difference"]["index"], 0)
        self.assertEqual(diff["first_difference"]["actual"]["name"], "A8XX_CP_MISC_CNTL")

    def test_matching_golden_stream_is_recorded_separately(self) -> None:
        sample = self.complete_fixture()
        self.put("kits/Adreno840Emit/sample.golden.pm4", sample.read_bytes())
        report = self.gate_report()
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(self.result(report, "a840_regs_and_emit")["samples"][0]["oracle"], "GOLDEN")

    def test_empty_and_missing_pm4_streams_fail(self) -> None:
        sample = self.complete_fixture()
        sample.write_bytes(b"")
        report = self.gate_report()
        self.assertIn("PM4_SAMPLE_EMPTY", self.result(report, "a840_regs_and_emit")["error"])
        sample.unlink()
        report = self.gate_report()
        self.assertIn("PM4_SAMPLE_MISSING", self.result(report, "a840_regs_and_emit")["error"])

    def test_builder_missing_archive_fails(self) -> None:
        self.complete_fixture()
        self.put("adreno_compile/tools/build_adreno_compile_ir3.py", "print('MOCK no archive')\n")
        report = self.gate_report()
        self.assertIn("IR3_ARCHIVE_MISSING", self.result(report, "adreno_ir3_e2e")["error"])

    def test_stale_backend_archive_cannot_replace_current_builder_output(self) -> None:
        self.complete_fixture()
        report = self.gate_report()
        self.assertEqual(report["status"], "PASS", report)
        self.assertTrue((self.directory / "output/ir3/libadreno_compile_ir3.a").is_file())
        self.put("adreno_compile/tools/build_adreno_compile_ir3.py", "print('MOCK no current archive')\n")
        report = self.gate_report()
        self.assertIn("IR3_ARCHIVE_MISSING", self.result(report, "adreno_ir3_e2e")["error"])

    def test_full_rejects_pass_report_without_required_suites(self) -> None:
        self.complete_fixture()
        self.put("tools/verify/verify_wave.py", MOCK_RUNNER.replace(
            "('kits/Adreno840','kits/Adreno840Emit','adreno_compile','translate')", "('MOCK_ONLY',)"))
        report = self.gate_report()
        self.assertIn("VERIFY_WAVE_FAILED", self.result(report, "verify_wave")["error"])

    def test_builder_vulkan_archive_is_rejected_by_real_symbol_guard(self) -> None:
        self.complete_fixture()
        self.put("adreno_compile/tools/build_adreno_compile_ir3.py", MOCK_BUILDER.replace("a840_fixture_backend", "vkCreateDevice"))
        report = self.gate_report()
        row = self.result(report, "adreno_ir3_e2e")
        self.assertEqual(row["status"], "FAIL")
        checked = json.loads(row["commands"][-1]["stdout"])
        self.assertEqual(checked["violations"][0]["value"], "vkCreateDevice")

    def test_failed_host_runner_blocks_full_completion(self) -> None:
        self.complete_fixture()
        self.put("tools/verify/verify_wave.py", "raise SystemExit(7)\n")
        report = self.gate_report()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("exit 7", self.result(report, "verify_wave")["error"])
        self.assertEqual(self.result(report, "adreno_ir3_e2e")["commands"], [])

    def test_stale_host_report_cannot_replace_current_invocation(self) -> None:
        self.tools()
        first = self.gate_report("dry-run")
        self.assertEqual(first["status"], "PASS", first)
        self.put("tools/verify/verify_wave.py", "print('MOCK omitted current summary')\n")
        second = self.gate_report("dry-run")
        self.assertEqual(second["status"], "FAIL")
        self.assertEqual(self.result(second, "verify_wave")["status"], "FAIL")

    def test_clang_option_forwarded_to_host_runner(self) -> None:
        self.complete_fixture()
        report = self.gate_report(with_clang=True)
        self.assertEqual(report["status"], "PASS", report)
        self.assertIn("--with-clang", self.result(report, "verify_wave")["commands"][0]["command"])
        wave = json.loads((self.directory / "output/wave/verify_wave_summary.json").read_text())
        self.assertTrue(wave["with_clang"])

    def test_cli_dry_run_writes_structured_summary(self) -> None:
        self.tools()
        result = self.cli("phase1_e2e_gate.py", "--repo", self.repo, "--mesa", MESA,
                          "--cache-dir", self.directory / "cli-dry", "--mode", "dry-run")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        summary = json.loads((self.directory / "cli-dry/phase1_e2e_summary.json").read_text())
        self.assertEqual(summary["status"], "PASS")
        self.assertIn("PENDING_W02_L2", result.stdout)

    def test_source_change_during_subordinate_execution_is_rejected(self) -> None:
        self.tools()
        self.put("tools/verify/verify_wave.py", MOCK_RUNNER +
                 "(Path(a.repo)/'contracts/AdrenoCompileABI.h').write_text('changed by MOCK runner')\n")
        report = self.gate_report("dry-run")
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("SOURCE_IDENTITY_CHANGED", self.result(report, "source_identity_after_gates")["error"])

    def test_gate_rejects_unapproved_output_and_scripts_have_help(self) -> None:
        with self.assertRaises(GateError):
            e2e.run_gate(self.repo, MESA, "/mnt/c/blocked-phase1-cache")
        for script in ("stack_key.py", "phase1_e2e_gate.py", "test_phase1_gates.py"):
            result = self.cli(script, "--help")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2 if args.verbose else 1)
