#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
from pathlib import Path
import json,os,subprocess
ROOT=Path(__file__).resolve().parents[2]
LINUX=Path(os.environ.get('BB_LINUX_SOURCE',str(ROOT.parents[2]/'ref/linux-sparse')))
pin=json.loads((ROOT/'package/baseband27/integration/ipa-source.json').read_text())
data=subprocess.check_output(['python3',str(ROOT/pin['generator']),'--linux',str(LINUX),'--rev',pin['revision']])
assert data==(ROOT/pin['generated']).read_bytes()
print('PASS IPA v5.5 generator: exact reproduction from fixed local source')
