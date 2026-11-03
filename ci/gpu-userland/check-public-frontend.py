#!/usr/bin/env python3
"""Run the unchanged public-Vulkan phase of the pinned upstream frontend test.

The later Turnip software-CP phase requires another build and original Apple
QuartzCore data which may not be uploaded. Record those omissions explicitly;
do not turn any executed assertion, process failure or pixel check into success.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
a = p.parse_args()
root = a.unixnu.resolve()
source = root / 'scripts/check-native-metal-frontend-host.py'
raw = source.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
if digest != '218aac4214e78e3ec89dbf14d9b739e445ed35a857c3efe6686ef59706fcd176':
    raise SystemExit('review the changed upstream frontend phase before selecting it')
tree = ast.parse(raw, str(source))
boundary = [n for n in tree.body if isinstance(n, ast.Assign) and
            any(isinstance(t, ast.Name) and t.id == 'HOSTSIM' for t in n.targets)]
if len(boundary) != 1:
    raise SystemExit('the upstream public-Vulkan/Turnip-peer boundary changed')
last_line = boundary[0].lineno - 1
selected = ast.Module(body=[n for n in tree.body if n.lineno <= last_line], type_ignores=[])
record = {'source_sha256': digest, 'last_selected_line': last_line,
          'upstream_test_body_unmodified': True, 'public_vulkan_phase_completed': False,
          'phone_operations': False, 'Turnip_or_A840_pixels_qualified': False,
          'not_covered': ['original QuartzCore shader and selector fixtures',
                          'Turnip software-CP and original driver-cache phase',
                          'the subsequent CoreImage host phase', 'real iPhone/A840 execution']}
out = root / 'build/adreno-metal-frontend-host'
out.mkdir(parents=True, exist_ok=True)
try:
    exec(compile(selected, str(source), 'exec'), {'__file__': str(source), '__name__': '__main__'})
    record['public_vulkan_phase_completed'] = True
finally:
    (out / 'public-phase.json').write_text(json.dumps(record, indent=2) + '\n')
print('PASS: unchanged public-Vulkan frontend phase; later/private-fixture phases explicitly untested')
