#!/usr/bin/env python3
"""Record host GPU availability and symbolized failures of our own frontend test."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
root, out = a.unixnu.resolve(), a.logs.resolve() / 'host-diagnostics'
out.mkdir(parents=True, exist_ok=True)
source = out / 'metal-devices.mm'
source.write_text('''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(){@autoreleasepool{NSMutableArray *rows=[NSMutableArray array];
for(id<MTLDevice> d in MTLCopyAllDevices())[rows addObject:@{@"name":d.name,@"registryID":@(d.registryID),@"headless":@(d.headless)}];
NSData *data=[NSJSONSerialization dataWithJSONObject:rows options:0 error:nil];fwrite(data.bytes,1,data.length,stdout);fputc('\\n',stdout);return 0;}}
''')
records = []
def run(label, command, env=None, timeout=120):
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
        text = result.stdout + result.stderr
        code = result.returncode
    except subprocess.TimeoutExpired as error:
        text = 'bounded host diagnostic timed out\n' + str(error.stdout or '') + str(error.stderr or '')
        code = 'timeout'
    (out / (label + '.log')).write_text(text)
    records.append({'label': label, 'command': list(map(str, command)), 'result': code})
    return code
devices = out / 'metal-devices'
if run('compile-devices', ['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', str(source),
                          '-framework', 'Foundation', '-framework', 'Metal', '-o', str(devices)]) == 0:
    run('metal-devices', [str(devices)])
test = root / 'build/adreno-metal-frontend-host/frontend_host'
loader = os.environ.get('NMB_HOST_LOADER', '')
if test.is_file() and loader:
    for label, use_nsobject in [('host-private-base', False), ('host-nsobject-base', True)]:
        env = dict(os.environ, RUST_BACKTRACE='full', NSUnbufferedIO='YES', NATIVE_METAL_ABORT_PROBE='1')
        env.pop('NMB_DEVICE_BASE_NSOBJECT', None)
        if use_nsobject:
            env['NMB_DEVICE_BASE_NSOBJECT'] = '1'
        cache = out / (label + '-cache')
        cache.mkdir(exist_ok=True)
        run(label, ['xcrun', 'lldb', '-b', '-o', 'run', '-k', 'thread backtrace all',
                    '-k', 'register read',
                    '-o', 'thread backtrace all', '-o', 'register read', '-o', 'quit', '--', str(test), loader, str(cache), ''], env, timeout=180)
    saved = root / 'build/host-debug'
    saved.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(test, saved / 'frontend_host')
    bridge = root / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
    if bridge.is_file() and os.environ.get("NMB_SAVE_BRIDGE") == "1":
        shutil.copyfile(bridge, saved / bridge.name)
reports = Path.home() / 'Library/Logs/DiagnosticReports'
if reports.is_dir():
    for path in reports.glob('frontend_host*'):
        if path.is_file() and path.stat().st_size <= 8 * 1024 * 1024:
            shutil.copyfile(path, out / path.name)
(out / 'diagnostics.json').write_text(json.dumps({'records': records, 'phone_operations': False,
    'scope': 'host process and host Metal only; no original iPhone fixtures'}, indent=2) + '\n')
print(json.dumps(records, indent=2))
