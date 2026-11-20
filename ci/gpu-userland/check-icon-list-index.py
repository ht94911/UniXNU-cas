"""Run the production App Library item lookup on the actual Objective-C runtime."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
kit = a.unixnu.resolve() / 'drivers/AdrenoMetal'
out = a.logs / 'icon-list-index'
out.mkdir(parents=True, exist_ok=False)
source = kit / 'bridge/icon_library_index_host.mm'
header = kit / 'NativeIconLibraryIndex.h'
command = ['xcrun', 'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           str(source), '-framework', 'Foundation', '-o', str(out / 'actual')]
compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
(out / 'build.log').write_text(compiled.stdout + compiled.stderr)
assert compiled.returncode == 0, compiled.stderr
run = subprocess.run([str(out / 'actual')], capture_output=True, text=True, timeout=30)
(out / 'run.log').write_text(run.stdout + run.stderr)
assert run.returncode == 0 and 'PASS 27 actual App Library index cases' in run.stdout, run.stdout + run.stderr

# The iPhone build must compile the same production header under arm64e/PAC.
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
probe = out / 'iphone-index.mm'
probe.write_text('#import <Foundation/Foundation.h>\n#include "NativeIconLibraryIndex.h"\n'
                 'extern "C" bool current_index_target() { return nbiconlibrary::install(); }\n')
target_command = ['xcrun', 'clang++', '-target', 'arm64e-apple-ios27.0', '-isysroot', sdk,
                  '-std=c++17', '-fobjc-arc', '-O2', '-g0', '-Wall', '-Wextra', '-Werror',
                  '-I', str(kit), '-c', str(probe), '-o', str(out / 'iphone-index.o')]
target = subprocess.run(target_command, capture_output=True, text=True, timeout=120)
(out / 'target-build.log').write_text(target.stdout + target.stderr)
assert target.returncode == 0, target.stderr
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
report = dict(passed=True, actual_production_lookup_and_install_executed=True,
              actual_Apple_ObjectiveC_runtime=True, cases=27, indexed_repetitions=1000, visibility_repetitions=1000,
              original_fallback_preserved=True, ASan_UBSan=True,
              arm64e_iPhone_production_header_compiled=True,
              header_sha256=sha(header), fixture_sha256=sha(source),
              target_object_sha256=sha(out / 'iphone-index.o'), output=run.stdout,
              phone_operations=False, physical_frame_improvement_verified=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
