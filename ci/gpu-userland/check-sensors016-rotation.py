#!/usr/bin/env python3
"""Check the exact prepared runtime's rotated surface layout on the Mac host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
root = a.unixnu.resolve()
runtime = root / '.cache/gpu-reference/metal2vulkan'
expected = json.loads((Path(__file__).parent / 'fixtures/multi-level-output-sources.json').read_text())
for name, digest in expected['source_hashes'].items():
    assert hashlib.sha256((runtime / name).read_bytes()).hexdigest() == digest, name
out = a.logs / 'sensors016-rotation'
out.mkdir(exist_ok=False)
env = dict(os.environ, CARGO_BUILD_JOBS='3', RUSTFLAGS='-Dwarnings',
           CARGO_TARGET_DIR=str(root / 'build/multi-level-output-host'))
command = ['cargo', '+1.98.0', 'test', '--release', '--manifest-path', str(runtime / 'Cargo.toml'),
           '-p', 'metal2vulkan-runtime', '--all-features', '--lib', 'tests::buffer_texture::',
           '--', '--nocapture']
with (out / 'pitch.log').open('w') as log:
    result = subprocess.run(command, cwd=root, env=env, stdout=log,
                            stderr=subprocess.STDOUT, timeout=1500)
text = (out / 'pitch.log').read_text()
assert result.returncode == 0, text[-4000:]
for case in ['ios_rotated_surface_pitch_covers_the_stored_extent',
             'an_iosurface_pitch_below_the_textures_own_row_is_refused_at_every_rotation',
             'no_rotated_plane_s_gpu_rows_share_bytes']:
    assert case + ' ... ok' in text, case
room = (runtime / 'crates/metal2vulkan-runtime/src/device/room.rs').read_text()
assert 'let Some(_busy) = exit_guard::enter() else {\n                return;\n            };\n            let parts = device.shared();' in room
assert 'pub const RUNTIME_VERSION: u32 = 1776;' in (runtime / 'crates/metal2vulkan/src/cache/mod.rs').read_text()
report = dict(passed=True, runtime_tree=expected['patched_tree'],
              current_Mac_pitch_tests=True, current_Mac_Vulkan_execution=False,
              iOS_pitch_rule_selected_by_target=True, idle_room_exit_section_present=True,
              phone_UI_rotation_verified=False, runtime_version=1776,
              pitch_log_sha256=hashlib.sha256((out / 'pitch.log').read_bytes()).hexdigest())
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
