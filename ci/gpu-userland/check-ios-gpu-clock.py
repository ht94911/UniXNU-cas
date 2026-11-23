"""Compile the actual GPU clock selection for iOS, including its old failing form."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', required=True, type=Path)
p.add_argument('--logs', required=True, type=Path)
a = p.parse_args()
r = a.unixnu.resolve()
source = r / '.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/gpu_time.rs'
text = source.read_text()
begin = text.index('/// The host\'s clock.\n')
end = text.index('/// `clock` now, in nanoseconds.', begin)
current = text[begin:end]
assert 'target_os = "ios"' in current
previous = current.replace('any(target_os = "macos", target_os = "ios")', 'target_os = "macos"')
assert current != previous
out = a.logs / 'ios-gpu-clock'
out.mkdir(exist_ok=False)
manifest = out / 'Cargo.toml'
manifest.write_text('[package]\nname="ios-gpu-clock-contract"\nversion="0.0.0"\nedition="2024"\n'
    '[workspace]\n[lib]\npath="lib.rs"\ncrate-type=["rlib"]\n'
    '[dependencies]\nlibc="0.2"\n'
    '[profile.release]\npanic="unwind"\ndebug=1\nlto="fat"\ncodegen-units=1\n')
env = dict(os.environ, RUSTC_BOOTSTRAP='1', IPHONEOS_DEPLOYMENT_TARGET='27.0')
target = 'arm64e-apple-ios'
cfg = subprocess.check_output(['rustup', 'run', '1.98.0', 'rustc', '--print', 'cfg', '--target', target],
    env=env, text=True)
assert 'target_os="ios"' in cfg and 'target_vendor="apple"' in cfg
(out / 'actual-target-cfg.txt').write_text(cfg)
command = ['cargo', '+1.98.0', '-Z', 'build-std=std,panic_unwind', 'build', '--offline', '--release',
    '--manifest-path', str(manifest), '--target', target, '--target-dir', str(r / 'build/m2v-target')]
results = []
for name, body in [('previous', previous), ('current', current)]:
    (out / 'lib.rs').write_text('#![no_std]\n' + body + '\n'
        'const _: () = assert!(BASE == libc::CLOCK_UPTIME_RAW);\n')
    result = subprocess.run(command, env=env, cwd=r, capture_output=True, text=True, timeout=600)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if name == 'previous':
        assert result.returncode != 0 and 'E0080' in result.stderr, result.stderr[-2000:]
    else:
        assert result.returncode == 0, result.stderr[-2000:]
    results.append(dict(source=name, exit=result.returncode))
report = dict(passed=True, actual_Rust_target=target, actual_target_os='ios',
    actual_production_cfg_extracted=True, old_iOS_clock_rejected=True, corrected_iOS_clock_compiled=True,
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), tests=results,
    existing_compiled_iOS_standard_library_reused=True, iOS_hardware_execution=False, phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
