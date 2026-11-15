# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inspect the new T1 lines emitted by the real-runtime host fixture."""
from pathlib import Path
import sys, zlib
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from log_format import inspect_log

rows=Path(sys.argv[1]).read_bytes()
assert b'T1 ' in rows and b'pack_tbat_raw=29000' in rows
prefix=b'NEZHA_POWER_LOG_V1 scoped telemetry; no authentication data\n'+rows
count=len(rows.splitlines())
footer=f'END_NEZHA_POWER_LOG_V1 records={count} dropped=0 crc32={zlib.crc32(prefix):08x} capped=0\n'.encode()
meta,decoded=inspect_log(prefix+footer)
assert meta['complete'] and meta['crc_verified'] and meta['record_types']['T1']>0
assert any(line.startswith('T1 ') for line in decoded) and not meta['unknown_record_types']
bad=prefix.replace(b'pack_tbat_raw=29000',b'pack_tbat_raw=29001',1)
meta,_=inspect_log(bad+footer)
assert not meta['complete'] and not meta['crc_verified']
unknown=prefix.replace(b'T1 ',b'T2 ',1)
footer=f'END_NEZHA_POWER_LOG_V1 records={count} dropped=0 crc32={zlib.crc32(unknown):08x} capped=0\n'.encode()
meta,_=inspect_log(unknown+footer)
assert meta['crc_verified'] and not meta['complete'] and meta['unknown_record_types']==['T2']
print('PASS new T1 record admission, changed-raw CRC rejection and T2 rejection; existing log tests reused')
