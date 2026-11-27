#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find unregistered register/opcode literals in emitter and driver sources.

The heuristic covers values 0x100..0xfffff, including padded spellings, and
short hex opcodes on lines using opcode/type7/pkt7 names. Contiguous low-bit
masks and the explicit 4 KiB/64 KiB alignment values are exempt. Other values
need a TSV row. TSV accepts a value/hex/offset/opcode column, optional path,
symbol and reason columns, or headerless rows with exactly one hex value.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import csv
from dataclasses import dataclass
import fnmatch
import json
from pathlib import Path
import re

from check_contracts import GateError

LITERALS = re.compile(
    r'(?:(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\"]{0,16})\(.*?\)(?P=delimiter)")'
    r'|//(?:\\\r?\n|[^\n])*'
    r'|/\*.*?(?:\*/|\Z)'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?<!\w)(?:u8|u|U|L)?'(?:\\.|[^'\\\r\n])+'",
    re.DOTALL,
)
HEX_TOKEN = re.compile(r"(?<![\w'])0[xX][0-9a-fA-F]+(?:'[0-9a-fA-F]+)*(?:[uUlL]+)?(?![\w'])")
VALUE_COLUMNS = {"value", "hex", "offset", "opcode", "token", "literal", "constant", "number"}
IGNORED_DIRECTORIES = {"gen", "test", "tests", ".git", "__pycache__"}
ALIGNMENT_VALUES = {0x1000, 0x10000}


@dataclass(frozen=True)
class AllowEntry:
    value: int
    path: str = "*"

    def matches(self, value: int, path: str) -> bool:
        return self.value == value and (fnmatch.fnmatchcase(path, self.path) or
                                        path.startswith(self.path.rstrip("/") + "/"))


def strip_literals(source: str) -> str:
    return LITERALS.sub(lambda match: re.sub(r"[^\r\n]", " ", match.group()), source)


def token_value(token: str) -> int:
    return int(re.sub(r"[uUlL]+$", "", token).replace("'", ""), 16)


def read_allow_table(path: Path | None) -> list[AllowEntry]:
    if path is None:
        return []
    rows = [(number, row) for number, row in enumerate(csv.reader(path.read_text(encoding="utf-8-sig").splitlines(),
                                                                 delimiter="\t"), 1)
            if row and any(cell.strip() for cell in row) and not row[0].lstrip().startswith("#")]
    if not rows:
        return []
    headers = [cell.strip().lower() for cell in rows[0][1]]
    value_columns = [index for index, name in enumerate(headers) if name in VALUE_COLUMNS]
    value_column = value_columns[0] if len(value_columns) == 1 else None
    path_column = next((index for index, name in enumerate(headers) if name in {"path", "file"}), None)
    if value_column is not None:
        rows = rows[1:]
    entries = []
    for line, row in rows:
        if value_column is None:
            tokens = [cell.strip() for cell in row if HEX_TOKEN.fullmatch(cell.strip())]
            if len(tokens) != 1:
                raise GateError(f"{path}:{line}: expected exactly one hex constant or a value-column header")
            literal, scope = tokens[0], "*"
        else:
            if value_column >= len(row) or (path_column is not None and path_column >= len(row)):
                raise GateError(f"{path}:{line}: incomplete TSV row")
            literal = row[value_column].strip()
            scope = row[path_column].strip() if path_column is not None else "*"
        try:
            value = token_value(literal) if HEX_TOKEN.fullmatch(literal) else int(literal, 0)
        except ValueError as exc:
            raise GateError(f"{path}:{line}: invalid constant {literal!r}") from exc
        if not 0 <= value <= 0xffffffff or not scope:
            raise GateError(f"{path}:{line}: invalid value or empty path scope")
        entries.append(AllowEntry(value, scope))
    return entries


def suspicious(value: int, line: str) -> bool:
    if value in ALIGNMENT_VALUES or (value >= 0xff and value & (value + 1) == 0):
        return False
    if 0x100 <= value <= 0xfffff:
        return True
    return value < 0x100 and bool(re.search(r"\b\w*(?:opcode|type7|pkt7)\w*\b", line, re.IGNORECASE))


def scan_constants(root: str | Path, allow_table: str | Path | None = None) -> dict:
    root = Path(root).resolve()
    if not root.is_dir():
        raise GateError(f"missing source root: {root}")
    table = Path(allow_table).resolve() if allow_table is not None else next(
        (p for p in (root / "kits/Adreno840/test/driver_constants.tsv", root / "driver_constants.tsv", root / "kits/Adreno840Emit/driver_constants.tsv",
                     root / "drivers/driver_constants.tsv") if p.is_file()), None)
    entries = read_allow_table(table)
    report = {"status": "PASS", "root": str(root), "allow_table": str(table) if table else None,
              "registered_values": len(entries), "files_scanned": 0, "violations": []}
    paths = set()
    for directory in (root / "kits/Adreno840Emit", root / "drivers"):
        if directory.is_dir():
            paths.update(p for p in directory.rglob("*") if p.suffix.lower() in {".c", ".cpp", ".h", ".mm"})
    for path in sorted(paths):
        relative = path.relative_to(root).as_posix()
        if any(part in IGNORED_DIRECTORIES for part in path.relative_to(root).parts[:-1]):
            continue
        if path.is_symlink():
            raise GateError(f"symlinked source: {relative}")
        report["files_scanned"] += 1
        for number, line in enumerate(strip_literals(path.read_text(encoding="utf-8")).splitlines(), 1):
            for match in HEX_TOKEN.finditer(line):
                token, value = match.group(), token_value(match.group())
                if suspicious(value, line) and not any(entry.matches(value, relative) for entry in entries):
                    report["violations"].append({"file": relative, "line": number, "token": token})
    if report["violations"]:
        report["status"] = "FAIL"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--allow-table", type=Path, help="driver_constants.tsv; missing explicit files are errors")
    args = parser.parse_args(argv)
    try:
        report = scan_constants(args.root, args.allow_table)
    except (GateError, OSError, UnicodeError) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    for violation in report["violations"]:
        print("{file}:{line}:{token}".format(**violation))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
