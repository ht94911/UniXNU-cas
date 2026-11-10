#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Dedicated integer-lux ALS archive using the selected original27 HID ABI.

Run after build-nezha27-input.py, reusing its pinned public headers and actual
kernel compiler record. This recipe is prepared for the normal native build;
host qualification does not execute it or prove the resulting native PAC.
"""
from pathlib import Path
import ast
import hashlib
import importlib.util
import json
import os
import shlex
import struct
import subprocess

from kernel_target import ARCH, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
D = Path(os.environ['UNIXNU_DEVICE']).resolve()
OUT = ROOT/'build/nezha27-als'
OUT.mkdir(parents=True, exist_ok=True)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()

def require(ok, why):
    if not ok:
        raise SystemExit(why)

def run(command, **options):
    return subprocess.check_output([str(item) for item in command], timeout=300, **options)

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

require(ARCH == 'arm64e', 'ALS requires the selected27 arm64e target')
fixture_path = Path(__file__).with_name('als-event-service-iphone-24A446.json')
abi = json.loads(fixture_path.read_text())
require(abi['kernelcache_sha256'] == 'd99b7719ad4b367db264a7682806aecaf6e0621c6761d07325a58337319d4238', 'exact24A446 donor fixture')
require(abi['instance_bytes'] == 200 and abi['virtual_entries'] == 219, 'actual event-service layout')
input_out = ROOT/'build/nezha27-input'
input_report = json.loads((input_out/'build-report.json').read_text())
require(input_report['public_HID_commit'] == abi['IOHIDFamily_revision'], 'same selected IOHIDFamily2238 source')
include = input_out/'headers/include'
require((include/'IOKit/hid/IOHIDEventService.h').is_file(), 'run the input archive header preparation first')

# Reuse only the existing, completed binding algorithm helpers. Importing the
# old executable recipe would compile unrelated input sources a second time.
recipe = ROOT/'scripts/build-nezha27-input.py'
tree = ast.parse(recipe.read_text())
helper_names = {'discriminator', 'macho', 'object_symbols', 'object_vtable'}
definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in helper_names]
require({node.name for node in definitions} == helper_names, 'retained binding helpers')
namespace = {'struct': struct, 'require': require}
exec(compile(ast.Module(body=definitions, type_ignores=[]), str(recipe), 'exec'), namespace)

compiler = load('als_source_compiler', ROOT/'scripts/build-sheng-source-drivers.py')
sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
record = ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
compile_record = json.loads(record.read_text().strip().rstrip(','))
source = D/'package/als/NezhaAlsFrontend.cpp'
object_path = OUT/'NezhaAlsFrontend.o'
includes = ['-I', str(include), '-idirafter', str(sdk/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
command = compiler.compile_command(compile_record, source, object_path, includes,
    ['-I', str(ROOT/'.cache/xnu-work/osfmk'), '-I', str(source.parent),
     '-Wno-unknown-warning-option', '-Wno-unused-variable'])
(OUT/'NezhaAlsFrontend-command.json').write_text(json.dumps(command, indent=2)+'\n')
process = subprocess.run(command, cwd=compile_record['directory'], capture_output=True, text=True)
(OUT/'NezhaAlsFrontend.log').write_text(process.stdout+process.stderr)
require(process.returncode == 0, process.stdout+process.stderr)
raw = object_path.read_bytes()
check_object(raw, 'NezhaAlsFrontend.cpp')
headers = {}
for name in shlex.split(object_path.with_suffix('.d').read_text().replace('\\\n', '').split(':', 1)[1]):
    path = Path(name)
    path = path if path.is_absolute() else Path(compile_record['directory'])/path
    headers[str(path.resolve())] = sha(path)
(OUT/'NezhaAlsFrontend-headers.json').write_text(json.dumps(headers, indent=2)+'\n')

symbols = namespace['object_symbols'](raw)
vtables = [name for name, _, kind in symbols if name.startswith('__ZTV') and kind != 1 and '9MetaClassE' not in name]
require(len(vtables) == 1, 'one owned ALS vtable')
entries = namespace['object_vtable'](raw, symbols, vtables[0], 219)
require(all(entries), 'all219 compiled authenticated ALS slots')
base_slots = {}
for index, (name, word) in enumerate(entries):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and
            (word >> 32) & 0xffff == int(abi['slots'][index]['diversity'], 16),
            f'ALS slot{index} PAC differs from actual original')
    if '17IOHIDEventService' in name:
        base_slots[name] = index
bindings = {name: int(value, 16) for name, value in abi['bindings'].items()}
for name, _, kind in symbols:
    if kind != 1 or '17IOHIDEventService' not in name or name in bindings:
        continue
    if name in base_slots:
        index = base_slots[name]
    else:
        diversity = namespace['discriminator'](name[1:])
        matches = [i for i, slot in enumerate(abi['slots']) if int(slot['diversity'], 16) == diversity]
        require(len(matches) == 1, 'unclassified original ALS service import '+name)
        index = matches[0]
    bindings[name] = int(abi['slots'][index]['target'], 16)
for name, _, kind in symbols:
    if kind == 1 and '23ambientLightSensorEvent' in name:
        require(name in bindings, 'unclassified ambient constructor ABI '+name)

text, addresses = '.text\n.p2align 2\n', ''
data_symbols = {'__ZN17IOHIDEventService10gMetaClassE', '__ZTV17IOHIDEventService'}
for name, address in sorted(bindings.items()):
    if name in data_symbols:
        addresses += f'.globl {name}\n.set {name}, {address:#x}\n'
    else:
        addresses += f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
        text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
iphone_sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
for stem, body in [('als-bindings', text), ('als-addresses', addresses)]:
    path = OUT/(stem+'.S')
    path.write_text(body)
    run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', iphone_sdk, '-c', path, '-o', OUT/(stem+'.o')])
llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip())/'bin'
late = OUT/'NezhaAlsFrontend.late.o'
run([llvm/'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NezhaAlsFrontend.cpp', object_path, late])
archive = OUT/'libNezha27Als.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT/'als-bindings.o', OUT/'als-addresses.o'])
report = dict(source_recipe='retained nezha27-input binding helpers; dedicated ALS archive',
    source_sha256=sha(source), object_sha256=sha(object_path), archive_sha256=sha(archive),
    fixture_sha256=sha(fixture_path), public_HID_commit=abi['IOHIDFamily_revision'],
    kernelcache_sha256=abi['kernelcache_sha256'], original_base_bytes=200,
    compiled_original_PAC_slots=219, owned_class=vtables[0],
    bindings={name: hex(value) for name, value in bindings.items()},
    late_no_mod_init=True, backend_readiness_required=True, actual_hardware=False,
    automatic_brightness_hardware_qualified=False, TrueTone_capability_added=False)
(OUT/'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
print('ALS native archive compiled;219 original PAC slots bound;hardware/backend acceptance pending')
