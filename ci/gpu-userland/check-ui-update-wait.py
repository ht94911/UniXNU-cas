"""Exercise the actual UI queue reader and compile its iPhone production header."""
from pathlib import Path
import argparse
import hashlib
import json
import platform
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', required=True, type=Path)
p.add_argument('--out', required=True, type=Path)
p.add_argument('--cxx', default='clang++')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
folder = a.source.parent
header = folder / 'NativeUIUpdateWaitState.h'
fixture = folder / 'bridge/ui_update_wait_host.cpp'
observer = folder / 'NativeFrameObservers.inc'
binary = a.out / 'ui-update-wait'
wire = a.out / 'records.bin'
command = [a.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(fixture), '-o', str(binary)]
subprocess.run(command, check=True)
result = subprocess.run([str(binary), str(wire)], check=True, capture_output=True, text=True)
actual = json.loads(result.stdout)
assert actual['passed'] and actual['cases'] == 10 and actual['wire_bytes'] == 240 and actual['records'] == 7
assert wire.stat().st_size == 7 * 240
report = dict(passed=True, cases=10, actual_Cpp_reader_executed=True, ASan=True, UBSan=True,
              source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
              header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
              fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
              observer_include_sha256=hashlib.sha256(observer.read_bytes()).hexdigest(),
              wire_sha256=hashlib.sha256(wire.read_bytes()).hexdigest(), wire_bytes=wire.stat().st_size,
              arm64e_iPhone_production_header_compiled=False, phone_operations=False)
if platform.system() == 'Darwin':
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    target = a.out / 'iphone-ui-wait.cpp'
    target.write_text('#include "NativeUIUpdateWaitState.h"\n'
        'void check() { nbuiwait::State s; nbuiwait::Binding b; nbuiwait::Layout l;\n'
        ' nbuiwait::capture(s,b,l,[](uint64_t,void *,unsigned){return 1;},\n'
        ' []{return uint64_t(1);},[]{return uint64_t(1);}); }\n', encoding='utf8')
    subprocess.run([a.cxx, '-target', 'arm64e-apple-ios17.0', '-isysroot', sdk, '-std=c++17',
                    '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-I', str(folder), str(target)], check=True)
    report['arm64e_iPhone_production_header_compiled'] = True
(a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
print(json.dumps(report, indent=2))
