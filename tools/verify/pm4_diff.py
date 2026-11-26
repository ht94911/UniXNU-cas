#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare ordered XML-named PM4 writes and opcode payloads.

Type4 grouping is ignored by default; --strict-packets preserves it.
Ignore options mask values only: register/opcode identity, occurrence,
order and payload length remain checked. XML reg64 low words also accept
the conventional NAME_LO spelling in --ignore-reg globs; output retains
the original XML name. Structural errors are checked before any mask.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import fnmatch
from itertools import zip_longest
import json
from pathlib import Path

from check_contracts import GateError, write_json
from pm4_check import decode_stream, read_words
from pm4_xml_index import CHIPS, DEFAULT_MESA, PM4_DOMAINS, build_index, load_index


def flatten_packets(packets: list[dict]) -> list[tuple]:
    operations = []
    for packet in packets:
        if packet["type"] == 4:
            operations.extend(("REG", min(reg["names"]), reg["register"], reg["value"])
                              for reg in packet["registers"])
        else:
            operations.append(("PKT7", min(packet["names"]), packet["identifier"], tuple(packet["payload"])))
    return operations


def describe(operation: tuple | None) -> dict | None:
    if operation is None:
        return None
    kind, name, identifier, value = operation
    if kind == "REG":
        return {"kind": kind, "name": name, "offset": f"0x{identifier:x}", "value": f"0x{value:08x}"}
    return {"kind": kind, "name": name, "opcode": f"0x{identifier:02x}",
            "payload": [f"0x{word:08x}" for word in value]}


def compare_streams(expected: list[int], actual: list[int], index: dict, chip: str = "A8XX",
                    ignore_regs=(), ignore_opcodes=(), strict_packets: bool = False) -> dict:
    decoded = []
    for label, words in (("expected", expected), ("actual", actual)):
        try:
            decoded.append(decode_stream(words, index, chip))
        except GateError as exc:
            raise GateError(f"{label}: {exc}") from exc
    register_aliases, opcode_aliases = {}, {}
    for record in index["registers"]:
        if record["chip"] == chip and record["domain"] in PM4_DOMAINS:
            names = register_aliases.setdefault(record["offset"], set())
            names.add(record["name"])
            if record.get("width") == 64 and record.get("lane") == "lo":
                names.add(record["name"] + "_LO")
    for record in index["opcode_records"]:
        if chip in record["chips"]:
            opcode_aliases.setdefault(record["value"], set()).add(record["name"])
    masked_registers, masked_opcodes = set(), set()

    def masked(operation: tuple) -> tuple:
        kind, name, identifier, value = operation
        if kind == "REG" and any(fnmatch.fnmatchcase(alias, pattern) for alias in register_aliases[identifier]
                                  for pattern in ignore_regs):
            masked_registers.add(name)
            value = None
        elif kind == "PKT7" and any(alias in ignore_opcodes for alias in opcode_aliases[identifier]):
            masked_opcodes.add(name)
            value = (None,) * len(value)
        return kind, name, identifier, value

    flattened = [flatten_packets(packets) for packets in decoded]
    normalized = [[masked(operation) for operation in operations] for operations in flattened]
    described = [[describe(operation) for operation in operations] for operations in flattened]
    if strict_packets:
        grouped, grouped_descriptions = [], []
        for packets, operations, details in zip(decoded, normalized, described):
            cursor, signatures, groups = 0, [], []
            for packet in packets:
                size = packet["payload_words"] if packet["type"] == 4 else 1
                signatures.append((packet["type"], packet["identifier"], packet["payload_words"],
                                   tuple(operations[cursor:cursor + size])))
                groups.append({"type": packet["type"], "header": f"0x{packet['header']:08x}",
                               "operations": details[cursor:cursor + size]})
                cursor += size
            grouped.append(signatures)
            grouped_descriptions.append(groups)
        comparisons, displays = grouped, grouped_descriptions
    else:
        comparisons, displays = normalized, described
    difference = None
    for position, (left, right) in enumerate(zip_longest(*comparisons)):
        if left != right:
            difference = {"index": position,
                          "expected": displays[0][position] if position < len(displays[0]) else None,
                          "actual": displays[1][position] if position < len(displays[1]) else None}
            break
    return {"status": "PASS" if difference is None else "FAIL", "chip": chip,
            "comparison": "packets" if strict_packets else "operations",
            "expected_packets": len(decoded[0]), "actual_packets": len(decoded[1]),
            "expected_operations": len(flattened[0]), "actual_operations": len(flattened[1]),
            "ignored_register_values": sorted(masked_registers), "ignored_opcode_payloads": sorted(masked_opcodes),
            "first_difference": difference}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--mesa", type=Path, default=DEFAULT_MESA)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--chip", choices=CHIPS, default="A8XX")
    parser.add_argument("--ignore-reg", action="append", default=[])
    parser.add_argument("--ignore-opcode", action="append", default=[])
    parser.add_argument("--strict-packets", action="store_true")
    parser.add_argument("--format", choices=("auto", "binary", "hex"), default="auto")
    parser.add_argument("--json-out", type=Path, help="report path inside adreno840-native/.cache")
    args = parser.parse_args(argv)
    try:
        index = load_index(args.index) if args.index else build_index(args.mesa)
        report = compare_streams(read_words(args.expected.read_bytes(), args.format),
                                 read_words(args.actual.read_bytes(), args.format), index, args.chip,
                                 args.ignore_reg, args.ignore_opcode, args.strict_packets)
    except (GateError, OSError, ValueError) as exc:
        report = {"status": "FAIL", "error": str(exc)}
    if args.json_out:
        try:
            write_json(args.json_out, report)
        except (GateError, OSError) as exc:
            report = {"status": "FAIL", "error": str(exc)}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
