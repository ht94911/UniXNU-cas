#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Strict Type4/Type7 PM4 structural and XML symbol checking (no submission).

Input is little-endian binary or whitespace/comma-separated hex dwords, with
optional # and // line comments. The supported header profile is identical
to UniXNU kits/AdrenoPM4/AdrenoPM4.c. XML names do not authorize addresses or
payloads, and indirect buffers are named but never followed.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
import struct

from check_contracts import GateError
from pm4_xml_index import CHIPS, DEFAULT_MESA, MAX_REGISTER, build_index, load_index, symbol_tables


class PM4Error(GateError):
    def __init__(self, offset: int, reason: str):
        self.offset, self.reason = offset, reason
        super().__init__(f"dword[{offset}]: {reason}")


def odd_bit(value: int) -> int:
    return 1 ^ (bin(value).count("1") & 1)


def type4_header(register: int, count: int) -> int:
    if not 0 <= register <= MAX_REGISTER or not 1 <= count <= 127 or register + count - 1 > MAX_REGISTER:
        raise GateError("invalid Type4 register range or count")
    return 0x40000000 | count | (odd_bit(count) << 7) | (register << 8) | (odd_bit(register) << 27)


def type7_header(opcode: int, count: int) -> int:
    if not 0 <= opcode <= 127 or not 0 <= count <= 0x3fff:
        raise GateError("invalid Type7 opcode or count")
    return 0x70000000 | count | (odd_bit(count) << 15) | (opcode << 16) | (odd_bit(opcode) << 23)


def read_words(data: bytes, format: str = "auto") -> list[int]:
    if format not in {"auto", "binary", "hex"}:
        raise GateError(f"unknown input format: {format}")
    if format == "auto":
        try:
            text = data.decode("ascii")
        except UnicodeDecodeError:
            format = "binary"
        else:
            format = "hex" if re.fullmatch(r"[\x09\x0a\x0d\x20-\x7e]*", text) else "binary"
    if format == "binary":
        if len(data) % 4:
            raise GateError(f"binary input length {len(data)} is not a multiple of 4")
        return [word[0] for word in struct.iter_unpack("<I", data)]
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as exc:
        raise GateError("hex input must be ASCII") from exc
    text = re.sub(r"(?://|#)[^\n]*", "", text)
    words = []
    for token in filter(None, re.split(r"[\s,]+", text)):
        if not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]{1,8}", token):
            raise GateError(f"invalid hex dword: {token!r}")
        words.append(int(token, 16))
    return words


def decode_stream(words: list[int], index: dict, chip: str = "A8XX") -> list[dict]:
    opcodes, registers = symbol_tables(index, chip)
    packets, offset = [], 0
    for position, word in enumerate(words):
        if not isinstance(word, int) or isinstance(word, bool) or not 0 <= word <= 0xffffffff:
            raise PM4Error(position, "value is not a uint32 dword")
    while offset < len(words):
        header = words[offset]
        kind = header >> 28
        if kind == 4:
            identifier, count = (header >> 8) & MAX_REGISTER, header & 127
            fields = ((count, (header >> 7) & 1), (identifier, (header >> 27) & 1))
            reserved_mask = (1 << 26)
            try:
                rebuilt = type4_header(identifier, count)
            except GateError as exc:
                raise PM4Error(offset, str(exc)) from exc
        elif kind == 7:
            identifier, count = (header >> 16) & 127, header & 0x3fff
            fields = ((count, (header >> 15) & 1), (identifier, (header >> 23) & 1))
            reserved_mask = (1 << 14) | (0xf << 24)
            rebuilt = type7_header(identifier, count)
        else:
            raise PM4Error(offset, f"unsupported packet type {kind}")
        if any(odd_bit(value) != parity for value, parity in fields):
            raise PM4Error(offset, f"Type{kind} odd-parity error")
        if header & reserved_mask or header != rebuilt:
            raise PM4Error(offset, f"Type{kind} reserved bits or invalid header")
        if count > len(words) - offset - 1:
            raise PM4Error(offset, f"truncated Type{kind}: expected {count} payload dwords, have {len(words) - offset - 1}")
        payload = words[offset + 1:offset + 1 + count]
        packet = {"header_word": offset, "header": header, "type": kind,
                  "identifier": identifier, "payload_words": count, "payload": payload}
        if kind == 4:
            symbols = []
            for relative, value in enumerate(payload):
                register = identifier + relative
                if register not in registers:
                    raise PM4Error(offset, f"unknown {chip} Type4 register 0x{register:x}")
                symbols.append({"register": register, "names": registers[register], "value": value})
            packet["registers"] = symbols
        else:
            if identifier not in opcodes:
                raise PM4Error(offset, f"unknown/illegal {chip} Type7 opcode 0x{identifier:02x}")
            packet["names"] = opcodes[identifier]
        packets.append(packet)
        offset += 1 + count
    return packets


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="dword stream file, or - for stdin")
    parser.add_argument("--format", choices=("auto", "binary", "hex"), default="auto")
    parser.add_argument("--index", type=Path, help="JSON emitted by pm4_xml_index.py")
    parser.add_argument("--mesa", type=Path, default=DEFAULT_MESA, help="used when --index is omitted")
    parser.add_argument("--chip", choices=CHIPS, default="A8XX")
    args = parser.parse_args(argv)
    try:
        index = load_index(args.index) if args.index else build_index(args.mesa)
        data = sys.stdin.buffer.read() if str(args.input) == "-" else args.input.read_bytes()
        words = read_words(data, args.format)
        packets = decode_stream(words, index, args.chip)
        print(json.dumps({"status": "PASS", "chip": args.chip, "dwords": len(words),
                          "packet_count": len(packets), "packets": packets}, indent=2, sort_keys=True))
    except (GateError, OSError, ValueError) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
