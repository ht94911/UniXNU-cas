#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the separately pinned WCN7860 source after a test package is selected."""
from pathlib import Path
import hashlib
import json

root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'tools/source-lock-wcn7860.json').read_text())
# Validate the complete pair before changing a staging directory.
data={}
for name,digest in lock['files'].items():
    source=root/'vendor/linux-wcn7860'/Path(name).name
    raw=source.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=digest:
        raise SystemExit('WCN7860 Linux source identity mismatch: '+name)
    data[Path(name).name]=raw
out=root/'out/linux-wcn7860'
out.mkdir(parents=True,exist_ok=True)
for name,raw in data.items():
    (out/name).write_bytes(raw)
print('Verified WCN7860 Linux source: '+lock['commit'])
