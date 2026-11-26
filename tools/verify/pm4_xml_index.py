#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Index pinned Mesa PM4 opcodes and variant-aware dword register offsets.

Arrays are expanded using offset + index * stride + child offset. A reg64
occupies two dwords. Auxiliary domains remain in the index, but only AXXX /
A6XX / A7XX / A8XX registers can be resolved as Type4 writes by pm4_check.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from check_contracts import GateError, write_json

MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
DEFAULT_MESA = Path(
    "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work"
)
CHIPS = ("A6XX", "A7XX", "A8XX")
PM4_DOMAINS = {"AXXX", *CHIPS}
SCHEMA_VERSION = 1
MAX_REGISTER = (1 << 18) - 1


def tag(element: ET.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def variant_chips(expression: str | None) -> set[str]:
    if not expression:
        return set(CHIPS)
    accepted = set()
    for variant in re.split(r"[,\s]+", expression.strip()):
        match = re.fullmatch(r"A(\d+)XX(?:(-)(?:A(\d+)XX)?)?", variant)
        if not match:
            raise GateError(f"unsupported chip variant expression: {expression!r}")
        low = int(match[1])
        high = int(match[3]) if match[3] else (99 if match[2] else low)
        if high < low:
            raise GateError(f"reversed chip variant range: {expression!r}")
        accepted.update(chip for chip in CHIPS if low <= int(chip[1]) <= high)
    return accepted


def number(element: ET.Element, attribute: str, default: int | None = None) -> int:
    text = element.get(attribute)
    if text is None:
        if default is None:
            raise GateError(f"missing {attribute} on {tag(element)} {element.get('name')}")
        return default
    try:
        return int(text, 0)
    except ValueError as exc:
        raise GateError(f"invalid XML {attribute}={text!r}") from exc


def extract_opcodes(root: ET.Element) -> tuple[dict[str, int], list[dict]]:
    enums = [element for element in root.iter() if tag(element) == "enum" and
             element.get("name") == "adreno_pm4_type3_packets"]
    if len(enums) != 1:
        raise GateError("expected exactly one adreno_pm4_type3_packets enum")
    opcodes, records, next_value = {}, [], 0
    for element in enums[0]:
        if tag(element) != "value":
            continue
        name, value = element.get("name"), number(element, "value", next_value)
        next_value = value + 1
        if not name or not 0 <= value < (1 << 7):
            raise GateError(f"invalid 7-bit opcode: {name}={value}")
        if name in opcodes and opcodes[name] != value:
            raise GateError(f"conflicting opcode values: {name}")
        opcodes[name] = value
        records.append({"name": name, "value": value,
                        "chips": sorted(variant_chips(element.get("variants"))),
                        "variants": element.get("variants", "")})
    if not records:
        raise GateError("empty PM4 opcode enum")
    return opcodes, records


def extract_registers(root: ET.Element, source: str) -> list[dict]:
    registers = []

    def add(name: str, offset: int, width: int, chips: set[str], domain: str, lane: str = "") -> None:
        if not chips:
            return
        if not name or not 0 <= offset <= MAX_REGISTER:
            raise GateError(f"invalid 18-bit register: {domain}_{name}={offset}")
        for chip in sorted(chips):
            prefix = chip if domain in PM4_DOMAINS else domain
            registers.append({"name": prefix + "_" + name, "offset": offset,
                              "chip": chip, "domain": domain, "width": width,
                              "lane": lane, "source": source})

    def walk(element: ET.Element, offset: int, names: tuple[str, ...], chips: set[str], domain: str) -> None:
        kind = tag(element)
        chips = chips & variant_chips(element.get("variants"))
        if kind == "array":
            start, stride, length = number(element, "offset"), number(element, "stride"), number(element, "length")
            name = element.get("name", "")
            if not name or start < 0 or stride <= 0 or not 1 <= length <= MAX_REGISTER + 1:
                raise GateError(f"invalid array: {element.attrib}")
            children = [child for child in element if tag(child) in {"array", "reg32", "reg64", "stripe"}]
            if offset + start + (length - 1) * stride > MAX_REGISTER:
                raise GateError(f"array exceeds 18-bit register space: {name}")
            for index in range(length):
                base, nested = offset + start + index * stride, names + (f"{name}[{index}]",)
                if children:
                    for child in children:
                        walk(child, base, nested, chips, domain)
                else:
                    add("_".join(nested), base, 32, chips, domain)
            return
        if kind in {"reg32", "reg64"}:
            base, name = offset + number(element, "offset"), element.get("name", "")
            stride, length = number(element, "stride", 1), number(element, "length", 1)
            if not name or base < 0 or stride <= 0 or not 1 <= length <= MAX_REGISTER + 1:
                raise GateError(f"invalid register: {element.attrib}")
            width = 64 if kind == "reg64" else 32
            if base + (length - 1) * stride + width // 32 - 1 > MAX_REGISTER:
                raise GateError(f"register exceeds 18-bit space: {name}")
            for index in range(length):
                full_name = "_".join(names + ((name + f"[{index}]") if length > 1 else name,))
                position = base + index * stride
                add(full_name, position, width, chips, domain, "lo" if width == 64 else "")
                if width == 64:
                    add(full_name + "_HI", position + 1, width, chips, domain, "hi")
            return
        if kind in {"domain", "stripe"}:
            for child in element:
                if tag(child) in {"array", "reg32", "reg64", "stripe"}:
                    walk(child, offset, names, chips, domain)

    for domain in root:
        name = domain.get("name", "")
        if tag(domain) == "domain" and (name.startswith(CHIPS) or name == "AXXX"):
            walk(domain, 0, (), set(CHIPS), name)
    return registers


def mesa_revision(mesa: Path) -> str:
    result = subprocess.run(["git", "-C", str(mesa), "rev-parse", "HEAD"],
                            capture_output=True, text=True, timeout=30, check=False)
    if result.returncode or result.stdout.strip() != MESA_COMMIT:
        raise GateError(f"Mesa must be pinned to {MESA_COMMIT}; got {result.stdout.strip() or result.stderr.strip()}")
    return result.stdout.strip()


def build_index(mesa: str | Path = DEFAULT_MESA) -> dict:
    mesa = Path(mesa).resolve()
    revision = mesa_revision(mesa)
    sources, roots = {}, {}
    for filename in ("adreno_pm4.xml", "a6xx.xml", "adreno_common.xml"):
        relative = "src/freedreno/registers/adreno/" + filename
        data = (mesa / relative).read_bytes()
        committed = subprocess.run(["git", "-C", str(mesa), "show", revision + ":" + relative],
                                   capture_output=True, timeout=30, check=False)
        if committed.returncode or data != committed.stdout:
            raise GateError(f"XML differs from pinned Mesa commit: {relative}")
        sources[relative] = hashlib.sha256(data).hexdigest()
        try:
            roots[filename] = ET.fromstring(data)
        except ET.ParseError as exc:
            raise GateError(f"malformed {relative}: {exc}") from exc
    opcodes, opcode_records = extract_opcodes(roots["adreno_pm4.xml"])
    registers = []
    for filename in ("a6xx.xml", "adreno_common.xml"):
        registers.extend(extract_registers(roots[filename], filename))
    if opcodes.get("CP_SET_DRAW_STATE") != 0x43 or len(registers) <= 500:
        raise GateError("pinned XML sanity check failed: CP_SET_DRAW_STATE or register count")
    return {"schema_version": SCHEMA_VERSION, "mesa_commit": revision, "mesa": str(mesa),
            "sources_sha256": sources, "chips": list(CHIPS), "opcodes": opcodes,
            "opcode_records": opcode_records, "registers": registers,
            "statistics": {"opcode_names": len(opcodes), "register_entries": len(registers),
                           "registers_by_chip": {chip: sum(r["chip"] == chip for r in registers) for chip in CHIPS}}}


def load_index(path: str | Path) -> dict:
    try:
        result = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise GateError(f"cannot read PM4 index: {exc}") from exc
    if not isinstance(result, dict) or result.get("schema_version") != SCHEMA_VERSION or \
            result.get("mesa_commit") != MESA_COMMIT:
        raise GateError("PM4 index schema or Mesa revision mismatch")
    if not isinstance(result.get("opcodes"), dict) or not isinstance(result.get("opcode_records"), list) or \
            not isinstance(result.get("registers"), list):
        raise GateError("PM4 index missing symbol tables")
    for record in result["opcode_records"]:
        if not isinstance(record, dict) or not isinstance(record.get("value"), int) or \
                not 0 <= record["value"] <= 127 or not isinstance(record.get("name"), str) or \
                not isinstance(record.get("chips"), list) or not set(record["chips"]).issubset(CHIPS):
            raise GateError("invalid opcode index record")
    for record in result["registers"]:
        if not isinstance(record, dict) or not isinstance(record.get("offset"), int) or \
                not 0 <= record["offset"] <= MAX_REGISTER or record.get("chip") not in CHIPS or \
                not isinstance(record.get("name"), str) or not isinstance(record.get("domain"), str):
            raise GateError("invalid register index record")
    return result


def symbol_tables(index: dict, chip: str = "A8XX") -> tuple[dict[int, list[str]], dict[int, list[str]]]:
    if chip not in CHIPS:
        raise GateError(f"unknown chip: {chip}")
    opcodes, registers = {}, {}
    for record in index["opcode_records"]:
        if chip in record["chips"]:
            opcodes.setdefault(record["value"], set()).add(record["name"])
    for record in index["registers"]:
        if record["chip"] == chip and record["domain"] in PM4_DOMAINS:
            registers.setdefault(record["offset"], set()).add(record["name"])
    return ({key: sorted(value) for key, value in opcodes.items()},
            {key: sorted(value) for key, value in registers.items()})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, default=DEFAULT_MESA)
    parser.add_argument("--out", type=Path, help="JSON output below .cache/w00-l4*; otherwise print JSON")
    args = parser.parse_args(argv)
    try:
        index = build_index(args.mesa)
        if args.out:
            output = write_json(args.out, index)
            print(json.dumps({"status": "PASS", "out": str(output), "mesa_commit": index["mesa_commit"],
                              "CP_SET_DRAW_STATE": index["opcodes"]["CP_SET_DRAW_STATE"],
                              **index["statistics"]}, indent=2, sort_keys=True))
        else:
            print(json.dumps(index, indent=2, sort_keys=True))
    except (GateError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
