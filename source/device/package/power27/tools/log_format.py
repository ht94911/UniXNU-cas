# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inspect the existing bounded power log without claiming a missing CRC."""
import collections
import hashlib
import re
import zlib

HEADER = b"NEZHA_POWER_LOG_V1 "
FOOTER = b"END_NEZHA_POWER_LOG_V1 "
RECORD_TYPES = {"P1", "CPREG", "U1", "A1", "S1"}


def inspect_log(data):
    if not 0 < len(data) <= 262144 or not data.startswith(HEADER) or b"\n" not in data:
        raise ValueError("Invalid bounded power log/header")
    meta = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                recovered=True, complete=False, footer_present=False,
                crc_verified=False, crc_valid=False, record_count_valid=False,
                declared_records=None, dropped=None, partial_tail_bytes=0,
                capped=None, integrity_errors=[])
    prefix = data
    footer = None
    offset = data.rfind(FOOTER)
    if offset >= 0:
        if not offset or data[offset - 1:offset] != b"\n":
            raise ValueError("Integrity marker is not a separate record")
        prefix = data[:offset]
        meta["footer_present"] = True
        footer = re.fullmatch(rb"records=(\d+) dropped=(\d+) crc32=([0-9a-f]{8})(?: capped=([01]))?\n",
                              data[offset + len(FOOTER):])
        if footer:
            meta.update(declared_records=int(footer[1]), dropped=int(footer[2]),
                        crc_valid=zlib.crc32(prefix) == int(footer[3], 16))
            meta["crc_verified"] = meta["crc_valid"]
            if footer[4] is not None:
                meta["capped"] = bool(int(footer[4]))
            if not meta["crc_valid"]:
                meta["integrity_errors"].append("CRC mismatch")
        else:
            meta["integrity_errors"].append("Incomplete or malformed integrity footer")
    else:
        meta["integrity_errors"].append("Missing integrity footer")
    if not prefix.endswith(b"\n"):
        last = prefix.rfind(b"\n") + 1
        meta["partial_tail_bytes"] = len(prefix) - last
        prefix = prefix[:last]
    records = prefix.splitlines()[1:]
    decoded = [line.decode("ascii") for line in records]
    counts = collections.Counter(line.partition(" ")[0] for line in decoded)
    unknown = sorted(set(counts) - RECORD_TYPES)
    meta.update(actual_records=len(records), records=len(records), record_types=dict(counts),
                sample_count=counts["P1"], pump_config_records=counts["CPREG"],
                usb_records=counts["U1"], aux_records=counts["A1"], shutdown_records=counts["S1"],
                unknown_record_types=unknown)
    meta["record_count_valid"] = bool(footer and len(records) == meta["declared_records"])
    if footer and not meta["record_count_valid"]:
        meta["integrity_errors"].append("Record count mismatch")
    if unknown:
        meta["integrity_errors"].append("Unknown record types")
    if meta["partial_tail_bytes"]:
        meta["integrity_errors"].append("Trailing partial record excluded")
    meta["complete"] = bool(footer and meta["crc_valid"] and meta["record_count_valid"]
                            and not unknown and not meta["partial_tail_bytes"])
    return meta, decoded
