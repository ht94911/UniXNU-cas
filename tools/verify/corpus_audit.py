#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit anonymous pipeline telemetry against a read-only A840PK01 pack.

Each JSONL or whitespace-separated key=value record contains pipeline_key_sha256,
hit_tier (0, 1, or miss), compile_us, and a coarse process_kind token. The two
hit rates use all records as the denominator. Cold compile time share uses all
logged compile_us. Directory coverage describes the supplied pack's tier and
may differ from the packs installed when telemetry was captured.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile

from check_contracts import GateError, cache_directory, checked_cache_path, write_json

# a840_pack_v1.h: packed little-endian header and 128-byte directory entries.
HEADER = struct.Struct("<4sBBHQQII32s")
ENTRY = struct.Struct("<32s9I60s")
ALIGNMENT = 128
STAGE_BYTES = 640
PLAN_BYTES = 23632
EMPTY_BUCKET = 0xFFFFFFFF
CHIP_ID = 0x44050A31
HBB = 16
MAX_PROBES = 16
KEY = re.compile(r"[a-fA-F0-9]{64}")
KIND = re.compile(r"[A-Za-z0-9_-]{1,32}")
DURATION = re.compile(r"[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")
FIELDS = {"pipeline_key_sha256", "hit_tier", "compile_us", "process_kind"}


@dataclass(frozen=True)
class PackDirectory:
    tier: int
    sha256: str
    keys: frozenset[str]


def aligned(value: int, alignment: int = ALIGNMENT) -> int:
    return (value + alignment - 1) & -alignment


def bucket_start(key: bytes, mask: int) -> int:
    # Only the low log2(bucket_count) bits of the wire FNV-1a hash matter.
    value = 14695981039346656037 & mask
    for byte in key:
        value = ((value ^ byte) * 1099511628211) & mask
    return value


def directory_keys(index: bytes, buckets: int, directory: int, entries: int, size: int) -> frozenset[str]:
    directory -= HEADER.size
    bucket_end = buckets * 4
    if any(index[bucket_end:directory]):
        raise GateError("PACK_INDEX_PADDING")
    slots = struct.unpack_from(f"<{buckets}I", index)
    if sorted(slot for slot in slots if slot != EMPTY_BUCKET) != list(range(entries)):
        raise GateError("PACK_BUCKET_DIRECTORY_IDS")

    keys = set()
    previous = bytes(32)
    payload = HEADER.size + len(index)
    for entry_id in range(entries):
        offset = directory + entry_id * ENTRY.size
        key, _, stages, meta, bindings, binding_count, plan, plan_bytes, start, count, reserved = ENTRY.unpack_from(index, offset)
        if key <= previous or any(reserved):
            raise GateError("PACK_DIRECTORY_KEY_OR_RESERVED")
        bindings_start = start + STAGE_BYTES * stages
        if (not 1 <= stages <= 2 or start != payload or meta != start or
                bindings != (bindings_start if binding_count else 0) or
                plan != aligned(bindings_start + 32 * binding_count, 64) or plan_bytes != PLAN_BYTES or
                count % ALIGNMENT or not aligned(plan + plan_bytes) < start + count <= size):
            raise GateError("PACK_DIRECTORY_PAYLOAD")
        payload += count
        previous = key
        keys.add(key.hex())

        slot = bucket_start(key, buckets - 1)
        for _ in range(MAX_PROBES):
            value = slots[slot]
            if value == entry_id:
                break
            if value == EMPTY_BUCKET:
                raise GateError("PACK_UNREACHABLE_DIRECTORY_KEY")
            slot = (slot + 1) & (buckets - 1)
        else:
            raise GateError("PACK_DIRECTORY_PROBE_LIMIT")
    if payload != size:
        raise GateError("PACK_UNCLAIMED_PAYLOAD")
    return frozenset(keys)


def read_pack(path: Path) -> PackDirectory:
    """Authenticate the whole file and validate its directory without decoding ISA."""
    try:
        with path.open("rb") as source:
            size = os.fstat(source.fileno()).st_size
            wire = source.read(HEADER.size)
            if not ALIGNMENT <= size <= 0xFFFFFFFF or len(wire) != HEADER.size:
                raise GateError("PACK_LENGTH")
            magic, control, log2, entries, abi, stack, chip, payload_bytes, expected_hash = HEADER.unpack(wire)
            if magic != b"8PK1":
                raise GateError("PACK_MAGIC")
            if control & 3 != 1:
                raise GateError("PACK_VERSION")
            if control >> 3 != HBB:
                raise GateError("PACK_HBB")
            if not abi or not stack or chip != CHIP_ID:
                raise GateError("PACK_IDENTITY")
            if not 1 <= log2 <= 16 or entries > (1 << log2) // 2:
                raise GateError("PACK_INDEX_SIZE")
            buckets = 1 << log2
            directory = aligned(HEADER.size + buckets * 4)
            payload = directory + entries * ENTRY.size
            if payload + payload_bytes != size:
                raise GateError("PACK_PAYLOAD_LENGTH")

            index = source.read(payload - HEADER.size)
            if len(index) != payload - HEADER.size:
                raise GateError("PACK_INDEX_LENGTH")
            digest = hashlib.sha256(wire[:32] + bytes(32))
            digest.update(index)
            for block in iter(lambda: source.read(1 << 20), b""):
                digest.update(block)
            if source.tell() != size or digest.digest() != expected_hash:
                raise GateError("PACK_SHA256_MISMATCH")
    except OSError as exc:
        raise GateError("PACK_READ_FAILED") from exc
    keys = directory_keys(index, buckets, directory, entries, size)
    return PackDirectory((control >> 2) & 1, expected_hash.hex(), keys)


def unique_fields(pairs) -> dict:
    fields = {}
    for key, value in pairs:
        if key in fields:
            raise ValueError("duplicate field")
        fields[key] = value
    return fields


def corpus_record(line: str) -> tuple[str, str, Decimal]:
    if line.startswith("{"):
        fields = json.loads(line, object_pairs_hook=unique_fields, parse_float=Decimal)
    else:
        fields = unique_fields(token.split("=", 1) for token in line.split())
    if not isinstance(fields, dict) or fields.keys() != FIELDS:
        raise ValueError("expected four anonymous fields")

    key, tier, microseconds, kind = (fields[field] for field in
        ("pipeline_key_sha256", "hit_tier", "compile_us", "process_kind"))
    if not isinstance(key, str) or not KEY.fullmatch(key) or key == "0" * 64:
        raise ValueError("invalid pipeline key SHA-256")
    if not isinstance(kind, str) or not KIND.fullmatch(kind):
        raise ValueError("invalid anonymous process_kind")
    if tier is None:
        tier = "miss"
    tiers = {"0": "tier0", "tier0": "tier0", "tier-0": "tier0",
             "1": "tier1", "tier1": "tier1", "tier-1": "tier1", "-1": "miss", "miss": "miss"}
    hit = tiers.get(str(tier).lower())
    if hit is None:
        raise ValueError("invalid hit_tier")
    duration = str(microseconds)
    if len(duration) > 64 or not DURATION.fullmatch(duration):
        raise ValueError("invalid compile_us")
    value = Decimal(duration)
    if value > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("compile_us exceeds uint64 range")
    return key.lower(), hit, value


def json_duration(duration: Decimal) -> int | float:
    return int(duration) if duration == duration.to_integral_value() else float(duration)


def audit_corpus(pack: PackDirectory, path: Path, top_n: int = 10) -> dict:
    if type(top_n) is not int or top_n < 1:
        raise GateError("TOP_N_MUST_BE_POSITIVE")
    hits = Counter()
    miss_counts = Counter()
    miss_times = defaultdict(Decimal)
    compile_us = cold_us = Decimal(0)
    covered = covered_misses = hits_absent = 0

    try:
        with path.open(encoding="utf-8") as corpus:
            for number, text in enumerate(corpus, 1):
                line = text.strip()
                if not line:
                    continue
                try:
                    key, tier, duration = corpus_record(line)
                except (ValueError, InvalidOperation, RecursionError) as exc:
                    # Include the line number without reflecting private values.
                    raise GateError(f"CORPUS_INVALID_RECORD line={number}") from exc
                hits[tier] += 1
                compile_us += duration
                in_pack = key in pack.keys
                covered += in_pack
                hits_absent += tier == f"tier{pack.tier}" and not in_pack
                if tier == "miss":
                    cold_us += duration
                    covered_misses += in_pack
                    miss_counts[key] += 1
                    miss_times[key] += duration
    except (OSError, UnicodeError) as exc:
        raise GateError("CORPUS_READ_FAILED") from exc

    count = hits.total()
    if not count:
        raise GateError("CORPUS_EMPTY")

    def top(order) -> list[dict]:
        return [{"pipeline_key_sha256": key, "miss_count": miss_counts[key],
                 "compile_us": json_duration(miss_times[key])}
                for key in sorted(miss_counts, key=order)[:top_n]]

    return {
        "status": "PASS",
        "pack": {"format": "8PK1", "version": 1, "highest_bank_bit": HBB, "tier": pack.tier,
                 "file_sha256": pack.sha256, "entry_count": len(pack.keys)},
        "record_count": count,
        "tier0_hits": hits["tier0"], "tier1_hits": hits["tier1"],
        "tier0_hit_rate": hits["tier0"] / count, "tier1_hit_rate": hits["tier1"] / count,
        "cold_compile_misses": hits["miss"],
        "compile_us": json_duration(compile_us),
        "cold_compile_us": json_duration(cold_us),
        "cold_compile_time_share": float(cold_us / compile_us) if compile_us else 0.0,
        "pack_coverage": {"records_in_directory": covered, "miss_records_in_directory": covered_misses,
                          "reported_hits_absent_from_supplied_tier": hits_absent},
        "top_misses_by_count": top(lambda key: (-miss_counts[key], -miss_times[key], key)),
        "top_misses_by_compile_us": top(lambda key: (-miss_times[key], -miss_counts[key], key)),
    }


def run_audit(pack: Path, corpus: Path, output: Path, top_n: int = 10) -> dict:
    try:
        target = checked_cache_path(output)
        checked_cache_path(target.parent)
    except (GateError, OSError, RuntimeError) as exc:
        raise GateError("OUTPUT_OUTSIDE_PROJECT_CACHE") from exc
    if target in {pack.resolve(), corpus.resolve()}:
        raise GateError("OUTPUT_ALIASES_INPUT")
    report = audit_corpus(read_pack(pack), corpus, top_n)
    try:
        with tempfile.TemporaryDirectory(prefix=".corpus-audit-", dir=cache_directory(target.parent)) as temporary:
            write_json(Path(temporary) / "report.json", report).replace(target)
    except OSError as exc:
        raise GateError("REPORT_WRITE_FAILED") from exc
    return report


def main(argv: list[str] | None = None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--pack", required=True, type=Path)
    cli.add_argument("--corpus", required=True, type=Path)
    cli.add_argument("--json-out", required=True, type=Path)
    cli.add_argument("--top-n", type=int, default=10)
    args = cli.parse_args(argv)
    try:
        report = run_audit(args.pack, args.corpus, args.json_out, args.top_n)
    except GateError as exc:
        report = {"status": "FAIL", "error": str(exc)}
    except (OSError, ValueError, RuntimeError):
        report = {"status": "FAIL", "error": "AUDIT_INPUT_ERROR"}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
