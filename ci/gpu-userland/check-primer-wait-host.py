"""Exercise the actual selected primer wait function with controlled host inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unixnu', type=Path, required=True)
parser.add_argument('--logs', type=Path, required=True)
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
source = args.unixnu / 'drivers/AdrenoMetal/PrimeShaders.h'
raw = source.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
assert digest == 'eeaf7259480eb84d4117481fa0f025fa9120aeaf4b8a79b73ccbee8af3526f68'
text = raw.decode()
begin = text.index('static uint64_t waitForQuiet() {')
end = text.index('\n}\n', begin) + 3
function = text[begin:end].replace('waitForQuiet()', 'fixedWait()')
fixture_path = Path(__file__).with_name('fixtures') / 'primer-wait-test.cpp.in'
fixture = fixture_path.read_text()
assert fixture.count('@SELECTED_WAIT_FUNCTION@') == 1
test = fixture.replace('@SELECTED_WAIT_FUNCTION@', function)
out = args.logs / 'primer-policy'
out.mkdir(parents=True, exist_ok=False)
test_path, executable = out / 'actual-wait.cpp', out / 'actual-wait'
test_path.write_text(test)
compiler = shutil.which(args.compiler)
assert compiler
command = [compiler, '-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', str(test_path), '-o', str(executable)]
with (out / 'build.log').open('x') as log:
    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=120)
with (out / 'test.log').open('x') as log:
    subprocess.run([str(executable)], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=30)
log = (out / 'test.log').read_text()
assert '6 actual-function checks passed' in log
result = dict(passed=True, actual_selected_source=True, source_sha256=digest,
    fixture_sha256=hashlib.sha256(fixture.encode()).hexdigest(),
    generated_test_sha256=hashlib.sha256(test.encode()).hexdigest(),
    command=command, checks=6, ASan_UBSan=True,
    mock_clock_and_commit_schedule=True, hardware_performance_verified=False,
    phone_operations=False)
(args.logs / 'primer-policy-host.json').write_text(json.dumps(result, indent=2) + '\n')
print(log, end='')
print(json.dumps(result, indent=2))
