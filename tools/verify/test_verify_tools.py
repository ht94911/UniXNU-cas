#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real contract/XML/C, no-fallback and PM4 oracle tests in approved caches."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

import check_contracts as contracts
import driver_constants_test as constants
import no_fallback_test as no_fallback
import pm4_check as pm4
import pm4_diff as oracle
import pm4_xml_index as xml_index
import verify_wave as wave

TEST_CACHE = contracts.CACHE_PARENT / "w01-l4-test"
REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
REAL_MESA = Path(os.environ.get("A840_VERIFY_MESA", str(xml_index.DEFAULT_MESA)))


class CachedTest(unittest.TestCase):
    def setUp(self) -> None:
        contracts.cache_directory(TEST_CACHE)
        self.temporary = tempfile.TemporaryDirectory(prefix=self.__class__.__name__ + "-", dir=TEST_CACHE)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.env = contracts.cache_environment(self.directory)

    def run_cli(self, script: str, *args: object) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(TOOLS / script), *map(str, args)],
                              cwd=self.directory, env=self.env, text=True,
                              capture_output=True, timeout=120, check=False)


class CacheTests(CachedTest):
    def test_output_boundary_rejects_unapproved_paths(self) -> None:
        for path in ("/mnt/c/blocked", "C:\\Temp\\blocked", "/tmp/blocked", REPO / "build",
                     contracts.CACHE_PARENT, contracts.CACHE_PARENT.parent / "outside"):
            with self.subTest(path=path), self.assertRaises(contracts.GateError):
                contracts.checked_cache_path(path)
        self.assertEqual(contracts.checked_cache_path(self.directory), self.directory)

    def test_output_symlink_escape_is_rejected(self) -> None:
        link = self.directory / "escape"
        link.symlink_to(REPO, target_is_directory=True)
        with self.assertRaises(contracts.GateError):
            contracts.write_json(link / "should-not-exist.json", {})


class ContractTests(CachedTest):
    def setUp(self) -> None:
        super().setUp()
        self.repo, self.docs = self.directory / "repo", self.directory / "docs"
        self.repo.mkdir()
        shutil.copytree(REPO / "contracts", self.repo / "contracts")
        shutil.copytree(REPO / "contracts", self.docs)
        self.cache = self.directory / "compiler"

    def verify(self, **kwargs: object) -> dict:
        return contracts.verify_contracts(self.repo, self.docs, self.cache, **kwargs)

    def git(self, *args: str) -> str:
        env = self.env.copy()
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
        command = ["git", "-c", "user.name=Verify Fixture", "-c", "user.email=fixture@invalid.local",
                   "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgSign=false", "-C", str(self.repo), *args]
        result = subprocess.run(command, env=env, text=True, capture_output=True, check=False, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def baseline(self) -> str:
        self.git("init", "-q")
        self.git("add", "contracts")
        self.git("commit", "-qm", "fixture base")
        return self.git("rev-parse", "HEAD")

    def test_equal_contracts_cli_compiles_all_headers_in_c_and_cpp(self) -> None:
        result = self.run_cli("check_contracts.py", "--repo", self.repo, "--docs-contracts", self.docs,
                              "--cache-dir", self.cache)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["contracts"]), 5)
        self.assertEqual(len(report["compiles"]), 6)
        self.assertEqual({row["command"][0] for row in report["compiles"]}, {"gcc", "g++"})
        for row in report["compiles"]:
            self.assertIn("-Werror", row["command"])
            self.assertIn("-pedantic", row["command"])
            self.assertEqual(row["returncode"], 0)
            self.assertTrue(Path(row["command"][-1]).is_relative_to(self.cache))

    def test_one_byte_contract_tamper_rejected(self) -> None:
        path = self.repo / "contracts/Adreno840EmitABI.h"
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        report = self.verify()
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any("CONTRACT_MISMATCH Adreno840EmitABI.h" in error for error in report["errors"]))

    def test_file_set_mismatch_rejected(self) -> None:
        (self.repo / "contracts/extra.h").write_text("#pragma once\n", encoding="ascii")
        (self.repo / "contracts/Adreno840EmitABI.h").unlink()
        report = self.verify()
        self.assertIn("CONTRACT_EXTRA extra.h", report["errors"])
        self.assertIn("CONTRACT_MISSING Adreno840EmitABI.h", report["errors"])

    def test_symlinked_contract_rejected(self) -> None:
        path = self.repo / "contracts/Adreno840EmitABI.h"
        path.unlink()
        path.symlink_to(self.docs / path.name)
        report = self.verify()
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("symlinked contract", report["errors"][0])

    def test_matching_bad_header_still_fails_compiler(self) -> None:
        for directory in (self.repo / "contracts", self.docs):
            (directory / "bad.h").write_text("#error contract compiler refusal\n", encoding="ascii")
        report = self.verify()
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(sum(row["returncode"] != 0 for row in report["compiles"]), 2)

    def test_allowed_untracked_file_passes(self) -> None:
        base = self.baseline()
        allowed = self.repo / "tools/verify/good.py"
        allowed.parent.mkdir(parents=True)
        allowed.write_text("# fixture\n", encoding="ascii")
        report = self.verify(base=base, allowed_prefixes=["tools/verify"])
        self.assertEqual(report["status"], "PASS", report["errors"])
        self.assertEqual(report["changed_paths"], ["tools/verify/good.py"])

    def test_untracked_outside_whitelist_cli_rejected_with_space(self) -> None:
        base = self.baseline()
        (self.repo / "outside file.c").write_text("/* fixture */\n", encoding="ascii")
        result = self.run_cli("check_contracts.py", "--repo", self.repo, "--docs-contracts", self.docs,
                              "--base", base, "--allowed-prefix", "tools/verify", "--cache-dir", self.cache)
        self.assertEqual(result.returncode, 1)
        self.assertIn("PATH_VIOLATION outside file.c", result.stdout)

    def test_committed_outside_whitelist_rejected(self) -> None:
        base = self.baseline()
        (self.repo / "outside.c").write_text("/* fixture */\n", encoding="ascii")
        self.git("add", "outside.c")
        self.git("commit", "-qm", "fixture out of bounds")
        report = self.verify(base=base, allowed_prefixes=["tools/verify"])
        self.assertEqual(report["violations"], ["outside.c"])

    def test_staged_rename_and_unstaged_edit_are_audited(self) -> None:
        (self.repo / "outside.c").write_text("/* fixture */\n", encoding="ascii")
        (self.repo / "existing.c").write_text("/* fixture */\n", encoding="ascii")
        self.baseline()
        self.git("add", "outside.c", "existing.c")
        self.git("commit", "-qm", "fixture sources")
        base = self.git("rev-parse", "HEAD")
        (self.repo / "tools/verify").mkdir(parents=True)
        self.git("mv", "outside.c", "tools/verify/moved.c")
        (self.repo / "existing.c").write_text("/* changed */\n", encoding="ascii")
        report = self.verify(base=base, allowed_prefixes=["tools/verify"])
        self.assertEqual(report["violations"], ["existing.c", "outside.c"])

    def test_sibling_prefix_does_not_bypass_whitelist(self) -> None:
        base = self.baseline()
        path = self.repo / "tools/verify-evil/new.py"
        path.parent.mkdir(parents=True)
        path.write_text("# fixture\n", encoding="ascii")
        report = self.verify(base=base, allowed_prefixes=["tools/verify/"])
        self.assertEqual(report["violations"], ["tools/verify-evil/new.py"])

    def test_base_and_whitelist_must_be_paired(self) -> None:
        report = self.verify(base="HEAD")
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("--base and --allowed-prefix", report["errors"][0])


class ConstantTests(CachedTest):
    def setUp(self) -> None:
        super().setUp()
        self.source = self.directory / "drivers/Adreno840/check.mm"
        self.source.parent.mkdir(parents=True)

    def test_clean_comments_strings_masks_and_alignment_pass(self) -> None:
        source = ('// 0x8e09\n/* 0x084c\n0x8e09 */\n'
                  'const char *s = "0x8e09 // text";\n'
                  'auto raw = R"tag(0x8e09 " text\n0x084c)tag";\n'
                  "auto ch = '\\x41';\n"
                  'unsigned masks[] = {0xfff, 0xffff, 0xfffff, 0x1000, 0x10000};\n')
        self.source.write_text(source, encoding="ascii")
        report = constants.scan_constants(self.directory)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(constants.strip_literals(source).count("\n"), source.count("\n"))

    def test_bare_register_cli_reports_exact_file_line_and_token(self) -> None:
        self.source.write_text("/* header */\nunsigned value =\n    0x8e09;\n", encoding="ascii")
        result = self.run_cli("driver_constants_test.py", "--root", self.directory)
        self.assertEqual(result.returncode, 1)
        self.assertIn("drivers/Adreno840/check.mm:3:0x8e09\n", result.stdout)

    def test_registered_table_values_and_scopes(self) -> None:
        self.source.write_text("unsigned value = 0x8E09u;\n", encoding="ascii")
        table = self.directory / "driver_constants.tsv"
        table.write_text("symbol\tvalue\tpath\treason\nRB_GC_GMEM_PROTECT\t0x8e09\tdrivers/Adreno840/*\tMesa A8XX\n",
                         encoding="ascii")
        self.assertEqual(constants.scan_constants(self.directory)["status"], "PASS")
        other = self.directory / "drivers/Other/check.c"
        other.parent.mkdir()
        other.write_text("unsigned value = 0x8e09;\n", encoding="ascii")
        self.assertEqual(constants.scan_constants(self.directory)["violations"][0]["file"], "drivers/Other/check.c")

    def test_generated_and_test_directories_skipped(self) -> None:
        for directory in ("kits/Adreno840Emit/gen", "kits/Adreno840Emit/test", "drivers/Adreno840/tests"):
            path = self.directory / directory / "fixture.c"
            path.parent.mkdir(parents=True)
            path.write_text("unsigned value = 0x8e09;\n", encoding="ascii")
        self.source.write_text("unsigned value = 0;\n", encoding="ascii")
        report = constants.scan_constants(self.directory)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["files_scanned"], 1)

    def test_padded_separator_literals_and_opcode_context_detected(self) -> None:
        self.source.write_text("unsigned reg = 0X00008E09ULL;\nunsigned other = 0x8'ee'09;\n"
                               "unsigned opcode = 0x43;\n", encoding="ascii")
        report = constants.scan_constants(self.directory)
        self.assertEqual([(r["line"], r["token"]) for r in report["violations"]],
                         [(1, "0X00008E09ULL"), (2, "0x8'ee'09"), (3, "0x43")])

    def test_invalid_allow_table_is_an_error(self) -> None:
        table = self.directory / "bad.tsv"
        table.write_text("value\tsymbol\nnot-a-number\tbad\n", encoding="ascii")
        with self.assertRaises(contracts.GateError):
            constants.scan_constants(self.directory, table)


class XMLTests(CachedTest):
    @classmethod
    def setUpClass(cls) -> None:
        cls.index = xml_index.build_index(REAL_MESA)

    def test_real_pinned_xml_opcode_and_a8xx_registers(self) -> None:
        self.assertEqual(self.index["mesa_commit"], xml_index.MESA_COMMIT)
        self.assertEqual(self.index["opcodes"]["CP_SET_DRAW_STATE"], 0x43)
        for name in ("CP_NOP", "CP_MEM_WRITE", "CP_INDIRECT_BUFFER"):
            self.assertIn(name, self.index["opcodes"])
        self.assertGreater(len(self.index["registers"]), 500)
        opcodes, registers = xml_index.symbol_tables(self.index)
        self.assertIn("CP_SET_DRAW_STATE", opcodes[0x43])
        self.assertEqual(registers[0x8e09], ["A8XX_RB_GC_GMEM_PROTECT"])
        self.assertEqual(registers[0x84c], ["A8XX_CP_MISC_CNTL"])

    def test_real_array_expansion_and_reg64_high_dwords(self) -> None:
        _, registers = xml_index.symbol_tables(self.index)
        for index in range(8):
            self.assertIn(f"A8XX_RB_MRT[{index}]_CONTROL", registers[0x8820 + 8 * index])
            self.assertIn(f"A8XX_RB_MRT[{index}]_BASE_HI", registers[0x8826 + 8 * index])
        self.assertIn("A8XX_RBBM_PERFCTR_CP[13]", registers[0x1b0 + 2 * 13])
        self.assertIn("A8XX_CP_RB_BASE_HI", registers[0x801])

    def test_generation_aliases_are_not_conflated(self) -> None:
        _, older = xml_index.symbol_tables(self.index, "A7XX")
        _, current = xml_index.symbol_tables(self.index, "A8XX")
        self.assertEqual(older[0x8e09], ["A7XX_RB_UNKNOWN_8E09"])
        self.assertNotIn("A7XX_RB_UNKNOWN_8E09", current[0x8e09])

    def test_nested_array_relative_offsets_and_variant_intersection(self) -> None:
        root = ET.fromstring('''<database xmlns="http://nouveau.freedesktop.org/">
          <domain name="A6XX"><array name="OUT" offset="0x100" stride="16" length="2" variants="A7XX-">
            <reg64 name="ADDR" offset="2"/>
            <array name="IN" offset="4" stride="2" length="2"><reg32 name="VALUE" offset="1" variants="A8XX"/></array>
          </array></domain></database>''')
        records = xml_index.extract_registers(root, "fixture.xml")
        a8 = {r["name"]: r["offset"] for r in records if r["chip"] == "A8XX"}
        self.assertEqual(a8["A8XX_OUT[1]_ADDR_HI"], 0x113)
        self.assertEqual(a8["A8XX_OUT[1]_IN[1]_VALUE"], 0x117)
        self.assertFalse(any(r["chip"] == "A6XX" for r in records))
        self.assertFalse(any(r["chip"] == "A7XX" and "VALUE" in r["name"] for r in records))

    def test_array_bounds_and_invalid_variants_fail(self) -> None:
        root = ET.fromstring('<database><domain name="A6XX"><array name="BAD" offset="0x3ffff" stride="2" length="2"/></domain></database>')
        with self.assertRaises(contracts.GateError):
            xml_index.extract_registers(root, "fixture.xml")
        with self.assertRaises(contracts.GateError):
            xml_index.variant_chips("A8XX-A6XX")

    def test_json_export_and_load_roundtrip(self) -> None:
        output = self.directory / "index.json"
        contracts.write_json(output, self.index)
        self.assertEqual(xml_index.load_index(output), self.index)
        self.assertEqual(xml_index.symbol_tables(xml_index.load_index(output)), xml_index.symbol_tables(self.index))

    def test_bad_index_schema_rejected(self) -> None:
        output = self.directory / "index.json"
        contracts.write_json(output, {"schema_version": 0})
        with self.assertRaises(contracts.GateError):
            xml_index.load_index(output)

    def test_mesa_revision_mismatch_rejected(self) -> None:
        with self.assertRaises(contracts.GateError):
            xml_index.build_index(REPO)


class PM4Tests(CachedTest):
    @classmethod
    def setUpClass(cls) -> None:
        cls.index = xml_index.build_index(REAL_MESA)

    def stream(self) -> list[int]:
        return [pm4.type4_header(0x84c, 1), 0,
                pm4.type7_header(self.index["opcodes"]["CP_SET_DRAW_STATE"], 3), 0, 0, 0]

    def test_valid_type4_type7_stream(self) -> None:
        result = pm4.decode_stream(self.stream(), self.index)
        self.assertEqual([r["type"] for r in result], [4, 7])
        self.assertEqual(result[0]["registers"][0]["names"], ["A8XX_CP_MISC_CNTL"])
        self.assertIn("CP_SET_DRAW_STATE", result[1]["names"])
        self.assertEqual(result[1]["header_word"], 2)

    def test_cli_accepts_little_endian_binary_and_hex(self) -> None:
        index = self.directory / "index.json"
        contracts.write_json(index, self.index)
        binary, text = self.directory / "stream.bin", self.directory / "stream.hex"
        binary.write_bytes(struct.pack("<6I", *self.stream()))
        text.write_text(", ".join(f"0x{word:08x}" for word in self.stream()) + " # fixture\n", encoding="ascii")
        for path, format in ((binary, "binary"), (text, "hex")):
            with self.subTest(format=format):
                result = self.run_cli("pm4_check.py", path, "--format", format, "--index", index)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads(result.stdout)["packet_count"], 2)

    def test_each_parity_field_bit_flip_rejected(self) -> None:
        for position, bit in ((0, 7), (0, 27), (2, 15), (2, 23)):
            words = self.stream()
            words[position] ^= 1 << bit
            with self.subTest(position=position, bit=bit), self.assertRaisesRegex(pm4.PM4Error, "odd-parity"):
                pm4.decode_stream(words, self.index)

    def test_all_reserved_header_bits_rejected(self) -> None:
        for position, bit in ((0, 26), (2, 14), (2, 24), (2, 25), (2, 26), (2, 27)):
            words = self.stream()
            words[position] ^= 1 << bit
            with self.subTest(position=position, bit=bit), self.assertRaisesRegex(pm4.PM4Error, "reserved bits"):
                pm4.decode_stream(words, self.index)

    def test_truncated_payloads_rejected(self) -> None:
        for words in (self.stream()[:1], self.stream()[:-1]):
            with self.subTest(words=words), self.assertRaisesRegex(pm4.PM4Error, "truncated"):
                pm4.decode_stream(words, self.index)

    def test_unknown_and_chip_illegal_opcode_rejected(self) -> None:
        opcodes, _ = xml_index.symbol_tables(self.index)
        unknown = next(value for value in range(128) if value not in opcodes)
        with self.assertRaisesRegex(pm4.PM4Error, "unknown/illegal"):
            pm4.decode_stream([pm4.type7_header(unknown, 0)], self.index)
        legacy = next(r["value"] for r in self.index["opcode_records"] if r["value"] not in opcodes and r["chips"])
        with self.assertRaisesRegex(pm4.PM4Error, "unknown/illegal"):
            pm4.decode_stream([pm4.type7_header(legacy, 0)], self.index)

    def test_unknown_register_and_later_register_in_sequence_rejected(self) -> None:
        _, registers = xml_index.symbol_tables(self.index)
        before_hole = next(register for register in sorted(registers) if register + 1 not in registers)
        for words in ([pm4.type4_header(0x3ffff, 1), 0], [pm4.type4_header(before_hole, 2), 0, 0]):
            with self.subTest(words=words), self.assertRaisesRegex(pm4.PM4Error, "unknown.*register"):
                pm4.decode_stream(words, self.index)

    def test_zero_type4_count_and_register_range_overflow_rejected(self) -> None:
        with self.assertRaisesRegex(pm4.PM4Error, "Type4 register range or count"):
            pm4.decode_stream([0x48000080], self.index)
        register, count = 0x3ffff, 2
        malformed = 0x40000000 | count | pm4.odd_bit(count) << 7 | register << 8 | pm4.odd_bit(register) << 27
        with self.assertRaisesRegex(pm4.PM4Error, "Type4 register range or count"):
            pm4.decode_stream([malformed, 0, 0], self.index)

    def test_header_builders_refuse_out_of_range_values(self) -> None:
        for arguments in ((-1, 1), (0x40000, 1), (0, 0), (0, 128), (0x3ffff, 2)):
            with self.subTest(arguments=arguments), self.assertRaises(contracts.GateError):
                pm4.type4_header(*arguments)
        for arguments in ((-1, 0), (128, 1), (0, -1), (0, 0x4000)):
            with self.subTest(arguments=arguments), self.assertRaises(contracts.GateError):
                pm4.type7_header(*arguments)

    def test_empty_zero_count_nop_and_64bit_register_sequence(self) -> None:
        self.assertEqual(pm4.decode_stream([], self.index), [])
        self.assertEqual(pm4.decode_stream([pm4.type7_header(0x10, 0)], self.index)[0]["payload_words"], 0)
        result = pm4.decode_stream([pm4.type4_header(0x800, 2), 0, 0], self.index)
        self.assertEqual(result[0]["registers"][1]["names"], ["A8XX_CP_RB_BASE_HI"])

    def test_invalid_type_and_non_dword_input_rejected(self) -> None:
        for words in ([0], [0x30000000], [-1], [0x100000000], [True]):
            with self.subTest(words=words), self.assertRaises(pm4.PM4Error):
                pm4.decode_stream(words, self.index)

    def test_binary_alignment_and_hex_syntax_rejected(self) -> None:
        with self.assertRaisesRegex(contracts.GateError, "multiple of 4"):
            pm4.read_words(b"\x00\x00\x00", "binary")
        for text in (b"0x100000000", b"0x43 garbage", b"-1"):
            with self.subTest(text=text), self.assertRaises(contracts.GateError):
                pm4.read_words(text, "hex")
        self.assertEqual(pm4.read_words(b"00000001, 0x00000002 // comment\n", "auto"), [1, 2])

    def test_cli_parity_truncation_unknown_opcode_exit_nonzero(self) -> None:
        index, stream = self.directory / "index.json", self.directory / "bad.hex"
        contracts.write_json(index, self.index)
        opcodes, _ = xml_index.symbol_tables(self.index)
        unknown = next(value for value in range(128) if value not in opcodes)
        parity = self.stream()
        parity[0] ^= 1 << 7
        for words, message in ((parity, "odd-parity"), (self.stream()[:-1], "truncated"),
                               ([pm4.type7_header(unknown, 0)], "unknown/illegal")):
            stream.write_text(" ".join(f"{word:08x}" for word in words), encoding="ascii")
            result = self.run_cli("pm4_check.py", stream, "--format", "hex", "--index", index)
            self.assertEqual(result.returncode, 1)
            self.assertIn(message, result.stderr)

    def test_python_headers_match_real_adreno_pm4_c_with_sanitizers(self) -> None:
        # Execute the existing library; it is never copied or edited.
        source = r'''#include "AdrenoPM4.h"
#include <stdio.h>
int main(void) {
    const uint32_t regs[] = {0, 1, 0x84c, 0x8e09, 0x1ffff, 0x3ff80, 0x3ffff};
    const uint32_t counts[] = {0, 1, 2, 3, 127, 128, 0x3ffe, 0x3fff};
    uint32_t header = 0;
    for (size_t r = 0; r < sizeof(regs)/sizeof(regs[0]); ++r) {
        for (uint32_t count = 1; count <= 127; ++count) {
            AdrenoPM4Status status = AdrenoPM4Type4(regs[r], count, &header);
            printf("4 %u %u %u %u\n", regs[r], count, (unsigned)status, header);
        }
    }
    for (uint32_t opcode = 0; opcode < 128; ++opcode) {
        for (size_t c = 0; c < sizeof(counts)/sizeof(counts[0]); ++c) {
            AdrenoPM4Status status = AdrenoPM4Type7(opcode, counts[c], &header);
            printf("7 %u %u %u %u\n", opcode, counts[c], (unsigned)status, header);
        }
    }
    return 0;
}'''
        binary = self.directory / "real-pm4"
        reference = wave.REFERENCE_PM4
        command = ["gcc", "-std=c11", *wave.NATIVE_FLAGS, "-I", str(reference), "-x", "c", "-",
                   str(reference / "AdrenoPM4.c"), "-no-pie", "-o", str(binary)]
        result = subprocess.run(command, input=source, text=True, capture_output=True,
                                cwd=self.directory, env=self.env, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        executed = subprocess.run([str(binary)], cwd=self.directory, env=self.env, text=True,
                                  capture_output=True, timeout=60, check=False)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        rows = executed.stdout.splitlines()
        self.assertEqual(len(rows), 1913)
        for line in rows:
            kind, identifier, count, status, header = map(int, line.split())
            builder = pm4.type4_header if kind == 4 else pm4.type7_header
            if status == 0:
                self.assertEqual(builder(identifier, count), header, line)
            else:
                with self.assertRaises(contracts.GateError):
                    builder(identifier, count)


class NoFallbackTests(CachedTest):
    def compile(self, source: str, name: str = "fixture") -> Path:
        target = self.directory / (name + ".o")
        command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-x", "c", "-", "-c", "-o", str(target)]
        result = subprocess.run(command, input=source, cwd=self.directory, env=self.env,
                                capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return target

    def test_clean_archive_cargo_json_pass(self) -> None:
        obj = self.compile("int fixture(void) { return 1; }\n")
        archive = self.directory / "libfixture.a"
        result = subprocess.run(["ar", "rcs", str(archive), str(obj)], capture_output=True, env=self.env, check=False)
        self.assertEqual(result.returncode, 0)
        cargo, manifest = self.directory / "Cargo.toml", self.directory / "source.json"
        cargo.write_text('[package]\nname="clean"\nversion="0.1.0"\n[dependencies]\n', encoding="ascii")
        manifest.write_text('{"files": ["src/freedreno/ir3/ir3_shader.c"]}', encoding="ascii")
        result = self.run_cli("no_fallback_test.py", "--artifact", archive, "--cargo-manifest", cargo,
                              "--json-manifest", manifest, "--profile", "compiler")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["scanned_items"], 3)
        self.assertEqual(report["symbols_scanned"], 1)
        self.assertEqual(report["violations"], [])

    def test_defined_vk_create_device_object_exact_symbol_rejected(self) -> None:
        obj = self.compile("void vkCreateDevice(void) {}\n")
        result = self.run_cli("no_fallback_test.py", "--artifact", obj)
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertEqual(report["violations"][0]["location"], "symbol:vkCreateDevice")
        self.assertEqual(report["violations"][0]["path"], str(obj))

    def test_undefined_vulkan_and_runtime_symbols_rejected(self) -> None:
        obj = self.compile("extern void vkCreateGraphicsPipelines(void);\n"
                           "void metal2vulkan_runtime_entry(void) { vkCreateGraphicsPipelines(); }\n")
        report = no_fallback.scan_no_fallback(artifacts=[obj], env=self.env)
        self.assertEqual({r["value"] for r in report["violations"]},
                         {"vkCreateGraphicsPipelines", "metal2vulkan_runtime_entry"})

    def test_cargo_dependency_key_and_renamed_package_rejected(self) -> None:
        path = self.directory / "Cargo.toml"
        path.write_text('[dependencies]\nmetal2vulkan-runtime="0.2.0"\n'
                        'renamed={package="metal2vulkan_runtime",version="0.2.0"}\n', encoding="ascii")
        result = self.run_cli("no_fallback_test.py", "--cargo-manifest", path)
        self.assertEqual(result.returncode, 1)
        values = {r["value"] for r in json.loads(result.stdout)["violations"]}
        self.assertEqual(values, {"metal2vulkan-runtime", "metal2vulkan_runtime"})

    def test_optional_cargo_lock_is_scanned(self) -> None:
        path = self.directory / "Cargo.toml"
        path.write_text('[package]\nname="clean"\nversion="0.1.0"\n', encoding="ascii")
        lock = self.directory / "Cargo.lock"
        lock.write_text('version=4\n[[package]]\nname="metal2vulkan-runtime"\nversion="0.2.0"\n', encoding="ascii")
        report = no_fallback.scan_no_fallback(cargo_manifests=[path])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["scanned_items"], 2)
        self.assertEqual(report["violations"][0]["path"], str(lock))

    def test_json_recursive_vulkan_source_exact_location_rejected(self) -> None:
        path = self.directory / "manifest.json"
        path.write_text('{"components":{"driver":{"files":["src/freedreno/vulkan/tu_device.cc"]}}}', encoding="ascii")
        result = self.run_cli("no_fallback_test.py", "--json-manifest", path)
        self.assertEqual(result.returncode, 1)
        violation = json.loads(result.stdout)["violations"][0]
        self.assertEqual(violation["location"], "$/components/driver/files/0")
        self.assertEqual(violation["value"], "src/freedreno/vulkan/tu_device.cc")

    def test_json_exclusion_declaration_does_not_exempt_real_sources(self) -> None:
        path = self.directory / "manifest.json"
        path.write_text('{"excluded_source_prefixes":["src/freedreno/vulkan"],'
                        '"files":["src/freedreno/vulkan/tu_device.cc"]}', encoding="ascii")
        report = no_fallback.scan_no_fallback(json_manifests=[path])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(len(report["exclusion_declarations"]), 1)
        self.assertEqual(len(report["violations"]), 1)
        self.assertEqual(report["violations"][0]["location"], "$/files/0")

    def test_real_mesa_manifest_passes_with_explicit_exclusion(self) -> None:
        path = REPO / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
        report = no_fallback.scan_no_fallback(json_manifests=[path])
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["exclusion_declarations"][0]["value"], "src/freedreno/vulkan")

    def test_final_native_bridge_symbol_is_rejected(self) -> None:
        obj = self.compile("void native_bridge_metal_init(void) {}\n")
        self.assertEqual(no_fallback.scan_no_fallback(artifacts=[obj], env=self.env)["status"], "PASS")
        result = self.run_cli("no_fallback_test.py", "--artifact", obj, "--profile", "final")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["violations"][0]["value"], "native_bridge_metal_init")

    def test_final_bridge_names_and_paths_and_windows_paths_rejected(self) -> None:
        path = self.directory / "manifest.json"
        path.write_text(json.dumps({"files": ["drivers/NativeBridgeMetalFrontend.mm", "NBMetalDevice", "NBCommand"],
                                    "nested": ["src\\freedreno\\vulkan\\tu_device.cc"]}), encoding="ascii")
        report = no_fallback.scan_no_fallback(json_manifests=[path], profile="final")
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual({r["value"] for r in report["violations"]},
                         {"drivers/NativeBridgeMetalFrontend.mm", "NBMetalDevice", "NBCommand",
                          "src\\freedreno\\vulkan\\tu_device.cc"})

    def test_invalid_missing_inputs_fail_closed(self) -> None:
        for report in (no_fallback.scan_no_fallback(),
                       no_fallback.scan_no_fallback(artifacts=[self.directory / "missing.a"])):
            self.assertEqual(report["status"], "FAIL")
            self.assertTrue(report["errors"])
        invalid = self.directory / "not-object.o"
        invalid.write_text("not an object", encoding="ascii")
        self.assertEqual(no_fallback.scan_no_fallback(artifacts=[invalid])["status"], "FAIL")

    def test_reports_are_deterministic_and_duplicate_inputs_deduplicated(self) -> None:
        path = self.directory / "manifest.json"
        path.write_text('{"z":"vkCreateDevice", "a":"libvulkan_freedreno.a"}', encoding="ascii")
        first = no_fallback.scan_no_fallback(json_manifests=[path, path])
        second = no_fallback.scan_no_fallback(json_manifests=[path])
        self.assertEqual(first, second)
        self.assertEqual(first["scanned_items"], 1)


class OracleTests(CachedTest):
    @classmethod
    def setUpClass(cls) -> None:
        cls.index = xml_index.build_index(REAL_MESA)

    def combined(self, low: int = 1, high: int = 2) -> list[int]:
        return [pm4.type4_header(0x800, 2), low, high, pm4.type7_header(0x43, 3), 0, 0, 0]

    def split(self, low: int = 1, high: int = 2) -> list[int]:
        return [pm4.type4_header(0x800, 1), low, pm4.type4_header(0x801, 1), high,
                pm4.type7_header(0x43, 3), 0, 0, 0]

    def test_identical_streams_pass(self) -> None:
        report = oracle.compare_streams(self.combined(), self.combined(), self.index)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["expected_operations"], 3)

    def test_merged_vs_split_type4_semantics_pass(self) -> None:
        report = oracle.compare_streams(self.combined(), self.split(), self.index)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual((report["expected_packets"], report["actual_packets"]), (2, 3))

    def test_strict_packet_mode_rejects_split(self) -> None:
        report = oracle.compare_streams(self.combined(), self.split(), self.index, strict_packets=True)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["comparison"], "packets")
        self.assertEqual(report["first_difference"]["index"], 0)

    def test_one_bit_register_difference_exact_index_and_values(self) -> None:
        report = oracle.compare_streams(self.combined(), self.combined(high=3), self.index)
        difference = report["first_difference"]
        self.assertEqual(difference["index"], 1)
        self.assertEqual(difference["expected"]["name"], "A8XX_CP_RB_BASE_HI")
        self.assertEqual(difference["expected"]["value"], "0x00000002")
        self.assertEqual(difference["actual"]["value"], "0x00000003")

    def test_ignore_low_va_alias_masks_value_but_preserves_presence(self) -> None:
        report = oracle.compare_streams(self.combined(), self.combined(low=42), self.index, ignore_regs=["*_LO"])
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["ignored_register_values"], ["A8XX_CP_RB_BASE"])
        missing = [pm4.type4_header(0x801, 1), 2, pm4.type7_header(0x43, 3), 0, 0, 0]
        report = oracle.compare_streams(self.combined(), missing, self.index, ignore_regs=["*_LO"])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["first_difference"]["index"], 0)

    def test_ignore_register_glob_still_preserves_identity_and_order(self) -> None:
        actual = [pm4.type4_header(0x801, 1), 2, pm4.type4_header(0x800, 1), 1,
                  pm4.type7_header(0x43, 3), 0, 0, 0]
        report = oracle.compare_streams(self.combined(), actual, self.index, ignore_regs=["*"])
        self.assertEqual(report["status"], "FAIL")

    def test_ignore_opcode_masks_payload_only(self) -> None:
        actual = self.combined()
        actual[-1] = 42
        self.assertEqual(oracle.compare_streams(self.combined(), actual, self.index)["status"], "FAIL")
        self.assertEqual(oracle.compare_streams(self.combined(), actual, self.index,
                                               ignore_opcodes=["CP_SET_DRAW_STATE"])["status"], "PASS")
        actual[3] = pm4.type7_header(0x10, 3)
        self.assertEqual(oracle.compare_streams(self.combined(), actual, self.index,
                                               ignore_opcodes=["CP_SET_DRAW_STATE", "CP_NOP"])["status"], "FAIL")

    def test_ignored_opcode_length_and_missing_opcode_are_rejected(self) -> None:
        for actual in ([pm4.type7_header(0x10, 2), 0, 0], []):
            report = oracle.compare_streams([pm4.type7_header(0x10, 1), 0], actual, self.index, ignore_opcodes=["CP_NOP"])
            self.assertEqual(report["status"], "FAIL")

    def test_parity_is_checked_before_ignore_masks(self) -> None:
        actual = self.combined()
        actual[0] ^= 1 << 7
        with self.assertRaisesRegex(contracts.GateError, "actual:.*odd-parity"):
            oracle.compare_streams(self.combined(), actual, self.index, ignore_regs=["*"])

    def test_cli_binary_hex_and_json_report(self) -> None:
        expected, actual, index, report = (self.directory / name for name in ("expected.bin", "actual.hex", "index.json", "report.json"))
        expected.write_bytes(struct.pack("<7I", *self.combined()))
        actual.write_text(" ".join(f"{word:08x}" for word in self.split()), encoding="ascii")
        contracts.write_json(index, self.index)
        result = self.run_cli("pm4_diff.py", "--expected", expected, "--actual", actual, "--index", index, "--json-out", report)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), json.loads(report.read_text()))
        strict = self.run_cli("pm4_diff.py", "--expected", expected, "--actual", actual, "--index", index, "--strict-packets")
        self.assertEqual(strict.returncode, 1)

    def test_empty_streams_and_extra_operations(self) -> None:
        self.assertEqual(oracle.compare_streams([], [], self.index)["status"], "PASS")
        report = oracle.compare_streams([], [pm4.type7_header(0x10, 0)], self.index)
        self.assertEqual(report["first_difference"]["expected"], None)
        self.assertEqual(report["first_difference"]["actual"]["name"], "CP_NOP")


class RunnerTests(CachedTest):
    def test_registry_copy_is_independent_and_reused(self) -> None:
        registry = self.directory / "host-registry"
        (registry / "cache").mkdir(parents=True)
        original = registry / "cache/fixture.crate"
        original.write_bytes(b"source registry")
        copied = self.directory / "cargo-home"
        report = wave.prepare_cargo_home(copied, registry)
        self.assertEqual(report["registry_status"], "COPIED")
        target = copied / "registry/cache/fixture.crate"
        self.assertEqual(target.read_bytes(), original.read_bytes())
        target.write_bytes(b"independent copy")
        self.assertEqual(original.read_bytes(), b"source registry")
        self.assertEqual(wave.prepare_cargo_home(copied, registry)["registry_status"], "REUSED")
        self.assertFalse((copied / "registry").is_symlink())

    def test_registry_symlink_to_original_is_rejected(self) -> None:
        cargo_home = self.directory / "cargo-home"
        cargo_home.mkdir()
        (cargo_home / "registry").symlink_to(Path.home() / ".cargo/registry", target_is_directory=True)
        with self.assertRaises(contracts.GateError):
            wave.prepare_cargo_home(cargo_home)

    def test_cargo_static_archive_is_reported_without_a_c_test(self) -> None:
        module = self.directory / "translate"
        (module / "src").mkdir(parents=True)
        (module / "Cargo.toml").write_text('''[package]
name="lane4_archive_fixture"
version="0.1.0"
edition="2021"
[lib]
crate-type=["staticlib", "rlib"]
''', encoding="ascii")
        (module / "src/lib.rs").write_text('''#[no_mangle]
pub extern "C" fn fixture_value() -> u32 { 42 }
''', encoding="ascii")
        report = wave.optional_suite(self.directory, "translate", self.directory / "out", self.env)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(len(report["artifacts"]), 1)
        self.assertEqual(Path(report["artifacts"][0]).name, "liblane4_archive_fixture.a")
        self.assertEqual(len(report["cargo_manifests"]), 1)
        self.assertEqual(no_fallback.scan_no_fallback(artifacts=report["artifacts"], env=self.env)["status"], "PASS")
        self.assertFalse((module / "Cargo.lock").exists())

    def test_real_registry_dependency_builds_offline_without_source_changes(self) -> None:
        module = self.directory / "translate"
        (module / "src").mkdir(parents=True)
        (module / "Cargo.toml").write_text('''[package]
name="lane4_registry_fixture"
version="0.1.0"
edition="2021"
[dependencies]
cfg-if="=1.0.5"
''', encoding="ascii")
        (module / "src/lib.rs").write_text('''cfg_if::cfg_if! {
    if #[cfg(unix)] { pub fn value() -> u32 { 42 } }
    else { pub fn value() -> u32 { 0 } }
}
#[cfg(test)] mod tests {
    #[test] fn offline_dependency() { assert_eq!(super::value(), 42); }
}
''', encoding="ascii")
        report = wave.optional_suite(self.directory, "translate", self.directory / "out", self.env)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["cargo_cache"]["registry_status"], "COPIED")
        self.assertTrue(all("--offline" in command["command"] for command in report["commands"]))
        self.assertFalse((module / "Cargo.lock").exists())
        self.assertFalse((module / "target").exists())

    def test_multiple_c_sources_and_cpp_tests_link_complete_object_set(self) -> None:
        module = self.directory / "kits/Adreno840Emit"
        (module / "test").mkdir(parents=True)
        (module / "first.c").write_text("int first(void) { return 20; }\n", encoding="ascii")
        (module / "second.c").write_text("int second(void) { return 22; }\n", encoding="ascii")
        for name, expression in (("first", "first() + second() == 42"), ("second", "first() == 20 && second() == 22")):
            (module / f"test/test_{name}.cpp").write_text('''#include <cassert>
extern "C" int first(void);
extern "C" int second(void);
int main() { assert(''' + expression + '''); return 0; }
''', encoding="ascii")
        report = wave.optional_suite(self.directory, "kits/Adreno840Emit", self.directory / "out", self.env)
        self.assertEqual(report["status"], "PASS", report)
        compiles = [c for c in report["commands"] if c["command"][0] == "gcc"]
        links = [c for c in report["commands"] if c["command"][0] == "g++"]
        self.assertEqual(len(compiles), 2)
        self.assertEqual(len(links), 2)
        objects = [c["command"][-1] for c in compiles]
        for command in links:
            self.assertTrue(all(obj in command["command"] for obj in objects))
            self.assertIn("-Werror", command["command"])
            self.assertIn("-fsanitize=address,undefined", command["command"])
        self.assertFalse(any("AdrenoPM4.c" in part for c in report["commands"] for part in c["command"]))

    def test_clang18_runs_all_three_real_integrated_native_suites(self) -> None:
        for relative in wave.CLANG_SUITES:
            with self.subTest(relative=relative):
                report = wave.optional_suite(REPO, relative, self.directory / "real-clang", self.env, with_clang=True)
                self.assertEqual(report["status"], "PASS", report)
                native = [c for c in report["commands"] if Path(c["command"][0]).name in {"clang-18", "clang++-18"}]
                self.assertGreaterEqual(len(native), 2)
                for command in native:
                    self.assertIn("-resource-dir", command["command"])
                    self.assertIn(str(wave.CLANG_RUNTIME), command["command"])
                    self.assertIn("-Werror", command["command"])
                    self.assertIn("-fsanitize=address,undefined", command["command"])

    def test_new_scripts_and_clang_flag_help(self) -> None:
        for script in ("no_fallback_test.py", "pm4_diff.py", "verify_wave.py"):
            result = self.run_cli(script, "--help")
            self.assertEqual(result.returncode, 0, result.stderr)
            if script == "verify_wave.py":
                self.assertIn("--with-clang", result.stdout)

    def test_missing_and_placeholder_suites_are_explicit_skips(self) -> None:
        for relative in wave.SUITES:
            (self.directory / relative).mkdir(parents=True)
            (self.directory / relative / ".gitkeep").touch()
            report = wave.optional_suite(self.directory, relative, self.directory / "out", self.env)
            self.assertEqual(report["status"], "SKIP_NOT_YET_MERGED")
            self.assertEqual(report["commands"], [])

    def test_populated_suite_without_tests_fails(self) -> None:
        module = self.directory / "kits/Adreno840"
        module.mkdir(parents=True)
        (module / "driver.c").write_text("int fixture(void) { return 1; }\n", encoding="ascii")
        report = wave.optional_suite(self.directory, "kits/Adreno840", self.directory / "out", self.env)
        self.assertEqual(report["status"], "FAIL")

    def test_discovers_python_and_sanitized_native_suites(self) -> None:
        module = self.directory / "kits/Adreno840"
        tests = module / "test"
        tests.mkdir(parents=True)
        (module / "fixture.c").write_text("int fixture(void) { return 42; }\n", encoding="ascii")
        (tests / "test_fixture.cpp").write_text('''#include <cassert>
extern "C" int fixture(void);
int main() { assert(fixture() == 42); return 0; }
''', encoding="ascii")
        (tests / "test_fixture.py").write_text('''import unittest
class Fixture(unittest.TestCase):
    def test_value(self):
        self.assertEqual(6 * 7, 42)
''', encoding="ascii")
        report = wave.optional_suite(self.directory, "kits/Adreno840", self.directory / "out", self.env)
        self.assertEqual(report["status"], "PASS", report)
        native = [row for row in report["commands"] if row["command"][0] in {"gcc", "g++"}]
        self.assertEqual(len(native), 2)
        for row in native:
            self.assertIn("-Werror", row["command"])
            self.assertIn("-fsanitize=address,undefined", row["command"])
        self.assertTrue(all(row["returncode"] == 0 for row in report["commands"]))

    def test_command_failure_is_not_hidden_by_later_success(self) -> None:
        report = wave.command_gate("fixture", [[sys.executable, "-c", "raise SystemExit(3)"],
                                               [sys.executable, "-c", "print('should not run')"]],
                                   cwd=self.directory, env=self.env)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(len(report["commands"]), 1)
        self.assertEqual(report["commands"][0]["returncode"], 3)

    def test_offline_cargo_and_sanitized_c_abi_keep_checkout_clean(self) -> None:
        module = self.directory / "translate"
        (module / "src").mkdir(parents=True)
        (module / "include").mkdir()
        (module / "tests").mkdir()
        (module / "Cargo.toml").write_text('''[package]
name = "lane4_verify_fixture"
version = "0.1.0"
edition = "2021"
[lib]
crate-type = ["rlib", "staticlib"]
''', encoding="ascii")
        (module / "src/lib.rs").write_text('''#[no_mangle]
pub extern "C" fn fixture_value() -> u32 { 42 }
#[cfg(test)]
mod tests {
    #[test]
    fn c_abi_value() { assert_eq!(super::fixture_value(), 42); }
}
''', encoding="ascii")
        (module / "include/fixture.h").write_text('''#include <stdint.h>
uint32_t fixture_value(void);
''', encoding="ascii")
        (module / "tests/test_abi.c").write_text('''#include "fixture.h"
#include <assert.h>
int main(void) { assert(fixture_value() == 42); return 0; }
''', encoding="ascii")
        before = {path.relative_to(module).as_posix(): path.read_bytes() for path in wave.source_files(module)}
        report = wave.optional_suite(self.directory, "translate", self.directory / "out", self.env)
        self.assertEqual(report["status"], "PASS", report)
        after = {path.relative_to(module).as_posix(): path.read_bytes() for path in wave.source_files(module)}
        self.assertEqual(before, after)
        self.assertFalse((module / "Cargo.lock").exists())
        self.assertFalse((module / "target").exists())
        cargo = [row for row in report["commands"] if Path(row["command"][0]).name == "cargo"]
        self.assertEqual(len(cargo), 2)
        self.assertTrue(all("--offline" in row["command"] for row in cargo))
        c_test = next(row for row in report["commands"] if row["command"][0] == "gcc")
        self.assertIn("-fsanitize=address,undefined", c_test["command"])
        self.assertIn("-Werror", c_test["command"])

    def test_all_six_scripts_support_help(self) -> None:
        scripts = ("check_contracts.py", "driver_constants_test.py", "pm4_xml_index.py",
                   "pm4_check.py", "verify_wave.py", "test_verify_tools.py")
        for script in scripts:
            with self.subTest(script=script):
                result = self.run_cli(script, "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2 if args.verbose else 1)
