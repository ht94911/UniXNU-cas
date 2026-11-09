# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Integrity checks for actual power log consumers, including partial recovery."""
from pathlib import Path
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from log_format import inspect_log

header = b"NEZHA_POWER_LOG_V1 scoped telemetry; no authentication data\n"
rows = [b"CPREG initial_enable=0 sampled_before_update=0000\n",
        b"P1 id=1 ms=1000\n", b"A1 wire=0 decoded=1 raw=01000000\n",
        b"U1 phase=6 failed=-4\n", b"S1 shutdown=1\n"]
prefix = header + b"".join(rows)
end = f"END_NEZHA_POWER_LOG_V1 records=5 dropped=2 crc32={zlib.crc32(prefix):08x}\n".encode()
meta, parsed = inspect_log(prefix + end)
assert meta["complete"] and meta["crc_verified"] and meta["dropped"] == 2 and len(parsed) == 5
assert meta["usb_records"] == meta["aux_records"] == meta["shutdown_records"] == 1
assert meta["capped"] is None  # legacy file does not declare its stop reason
for capped in (0, 1):
    modern = end[:-1] + f" capped={capped}\n".encode()
    meta, parsed = inspect_log(prefix + modern)
    assert meta["complete"] and meta["crc_verified"] and meta["capped"] is bool(capped)
meta, parsed = inspect_log(prefix)
assert meta["recovered"] and not meta["complete"] and not meta["crc_verified"] and len(parsed) == 5
meta, parsed = inspect_log(prefix + b"P1 id=6 ms=")
assert len(parsed) == 5 and meta["partial_tail_bytes"] == 11 and not meta["complete"]
meta, _ = inspect_log(prefix + end[:-3])
assert meta["footer_present"] and not meta["complete"] and not meta["crc_verified"]
meta, _ = inspect_log(prefix.replace(b"wire=0", b"wire=1") + end)
assert "CRC mismatch" in meta["integrity_errors"] and not meta["complete"]
meta, _ = inspect_log(prefix + end.replace(b"records=5", b"records=4"))
assert meta["crc_verified"] and not meta["record_count_valid"] and not meta["complete"]
unknown = prefix + b"OTHER ignored=0\n"
meta, _ = inspect_log(unknown + f"END_NEZHA_POWER_LOG_V1 records=6 dropped=0 crc32={zlib.crc32(unknown):08x}\n".encode())
assert meta["crc_verified"] and not meta["complete"] and meta["unknown_record_types"] == ["OTHER"]
auth_prefix = prefix + b"A2 ms=1500 id=5 property=21007 write=1 result=0 state=10 error=0 owned=1 verified=0\n"
auth_footer = f"END_NEZHA_POWER_LOG_V1 records=6 dropped=0 crc32={zlib.crc32(auth_prefix):08x}\n".encode()
meta, parsed = inspect_log(auth_prefix + auth_footer)
assert meta["complete"] and meta["auth_records"] == 1 and len(parsed) == 6
assert "raw=" not in parsed[-1]
print("PASS full/mixed, missing/truncated footer, partial record, bad CRC/count and unknown type recovery")
