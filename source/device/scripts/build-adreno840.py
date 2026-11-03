#!/usr/bin/env python3
"""Build A840 board code and the pinned shared core using real XNU records.

CI produces an empty private-resource slot, never firmware bytes. The local
finalizer and a fresh strict prelink are mandatory before candidate assembly.
"""
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/adreno840'
BOARD = ROOT / 'drivers/Adreno840'
COMMON = ROOT / 'refer/Adreno740'
METAL = ROOT / 'refer/AdrenoMetal'
OBJ = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
CORE = ['Memory', 'Cpu', 'SmmuMemory', 'SmmuContext', 'Vm', 'GpuContext',
        'Ring', 'Preempt', 'Submit', 'UserClient', 'Timer', 'Publish',
        'Client', 'CpInit', 'Irq', 'Sha256']
TRACE_ID = None


def require(value, why):
    if not value:
        raise RuntimeError(why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def final_link_providers():
    # Use the existing final-link inventory before spending time compiling the
    # GPU. A selected but not-yet-built archive is an ordering error, not an
    # absent optional observer that may be silently replaced with NULL.
    sys.path.insert(0, str(ROOT / 'scripts'))
    spec = importlib.util.spec_from_file_location('actual_boot_progress', ROOT / 'scripts/build-boot-progress.py')
    observer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(observer)
    providers = [p for p in observer.linked_inputs() if p != OUT / 'libAdreno840.a']
    missing = [str(p) for p in providers if not p.is_file()]
    require(not missing, 'actual final-link providers missing before A840: ' + ', '.join(missing))
    return providers


def object_layout(path):
    # Reuse build-adreno740-accelerator.py's layout-only adapter: Mach-O
    # relocatable objects have no fileoff-zero executable segment. Validate
    # the real arm64e object first; mask subtype only in the parser's copy.
    # The original bytes, PAC relocations and emitted object are unchanged.
    from native_prelink import macho as ordinary_macho
    raw = Path(path).read_bytes()
    check_object(raw, str(path))
    require(struct.unpack_from('<I', raw, 12)[0] == 1, 'late staging expects MH_OBJECT')
    return ordinary_macho(raw[:8] + bytes(4) + raw[12:])


def record(path):
    data = json.loads(path.read_text().strip().rstrip(','))
    arguments = data['arguments']
    require(any('arm64e' in str(a) for a in arguments), 'compile record is not arm64e')
    return data


def compile_one(source, recipe, includes):
    obj = OUT / (source.stem + '.o')
    args, cmd, i = recipe['arguments'], [], 0
    while i < len(args):
        v = args[i]
        if v == recipe['file']:
            cmd.append(str(source))
        elif v in ('-o', '-MF', '-MT', '-MJ'):
            if v == '-o':
                cmd += ['-o', str(obj)]
            i += 1
        elif v == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not v.startswith('-W') and v not in ('-MD', '-MP'):
            cmd.append(v)
        i += 1
    cmd += ['-mgeneral-regs-only', '-DNEZHA_GPU_SHARED_BACKEND=1',
            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
            '-Wno-unused-variable', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
            '-Wno-overloaded-virtual', '-Wno-incompatible-sysroot']
    if TRACE_ID:
        cmd += ['-DNATIVE_GPU_BUILD_ID=0x' + TRACE_ID + 'ULL']
    for inc in includes:
        cmd += ['-I', str(inc)]
    result = subprocess.run(cmd, cwd=recipe['directory'], capture_output=True, text=True, timeout=180)
    (OUT / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    item = {'source': str(source), 'source_sha256': sha(source), 'command': cmd,
            'directory': recipe['directory'], 'exit_code': result.returncode}
    if result.returncode:
        item['error'] = result.stderr[-6000:]
    else:
        check_object(obj.read_bytes(), str(source))
        item.update(object=str(obj), object_sha256=sha(obj))
    return item


def main():
    global TRACE_ID
    require(ARCH == 'arm64e' and IOS_VERSION.startswith('27.'), 'iPhone27 A840 recipe requires arm64e/iOS27')
    OUT.mkdir(parents=True, exist_ok=True)
    providers = final_link_providers()
    c = record(OBJ / 'bsd/DEVELOPMENT/bsd_init.o.json')
    cpp = record(OBJ / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    compiler = subprocess.check_output([cpp['arguments'][0], '--version'], text=True)
    require('Apple clang' in compiler, 'use the authorized Apple compiler; Linux host peers are a different test')
    profile = json.loads((ROOT / 'scripts/data/ioaccelerator-base-iphone-24A446.json').read_text())
    require(profile['product'] == 'iPhone14,4' and profile['build'] == '24A446' and
            profile['instance_bytes'] == 136 and profile['virtual_entries'] == 168 and
            not profile['operational_overrides'], 'unexpected original accelerator contract')
    # Only the include path changes. Use the maintained accelerator source and
    # the actual iPhone imports; never seed this build from the iPad ABI JSON.
    accelerator = OUT / 'NativeAdrenoAccelerator.cpp'
    body = (COMMON / 'NativeAdrenoAccelerator.cpp').read_text()
    old = '#include "../AdrenoMetal/OriginalAcceleratorABI.h"'
    require(body.count(old) == 1, 'accelerator include changed; review upstream')
    accelerator.write_text(body.replace(old, '#include "OriginalAcceleratorABI.h"'))
    includes = [BOARD, COMMON, METAL, ROOT / 'kits/ArmSmmu4K', ROOT / 'kits/AdrenoPM4',
                ROOT / '.cache/xnu-work/osfmk']
    sources = [BOARD / 'NezhaGpu.c', BOARD / 'NezhaScm.c', BOARD / 'NezhaGpuFirmware.cpp',
               BOARD / 'NezhaAdreno840.cpp', accelerator]
    sources += [COMMON / ('NativeAdreno' + name + '.cpp') for name in CORE]
    sources += sorted((ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
    sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
    digest = hashlib.sha256()
    for path in sorted(set(sources) | set(COMMON.glob('Native*.h')) | set(BOARD.glob('*.h'))):
        digest.update(path.name.encode()); digest.update(path.read_bytes())
    TRACE_ID = digest.hexdigest()[:16]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(int(os.environ.get('JOBS', '3')), 32))) as pool:
        for item in pool.map(lambda p: compile_one(p, c if p.suffix == '.c' else cpp, includes), sources):
            results.append(item)
    report = {'target': TRIPLE, 'compiler': compiler, 'records': results,
              'firmware_present': False, 'resource_finalization_required': True,
              'original_abi': profile, 'hardware_verified': False,
              'trace_source_id': TRACE_ID, 'source_sha256': digest.hexdigest()}
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    failed = [r for r in results if r['exit_code']]
    if failed:
        raise RuntimeError('\n'.join(r['source'] + '\n' + r['error'] for r in failed))
    # Reuse the diagnostic builder's inventory of this actual final link. An
    # absent optional observer is a typed null, not a fabricated provider.
    sys.path.insert(0, str(ROOT / 'scripts'))
    import xnu_symbols
    compiled = [Path(r['object']) for r in results]
    defined = xnu_symbols.kernel_names(ROOT) | xnu_symbols.defined_names(providers + compiled, True)
    weak = set()
    for obj in compiled:
        nm = subprocess.check_output(['xcrun', 'nm', '-m', str(obj)], text=True)
        weak.update(re.findall(r'\(undefined\) weak external (_\w+)', nm))
    missing = weak - defined
    optional = {
        '_boot_log_export_put': ('NativeAdrenoPublish.cpp',
            r'extern "C" int boot_log_export_put\((.*?)\) __attribute__\(\(weak_import\)\);',
            r'static int (*const boot_log_export_put)(\1)=nullptr;'),
        '_native_pmap_user_mappings': ('NativeAdrenoMemory.cpp',
            r'extern "C" unsigned int native_pmap_user_mappings\((.*?)\) __attribute__\(\(weak_import\)\);',
            r'static unsigned int (*const native_pmap_user_mappings)(\1)=nullptr;'),
        '_native_gpu_dcvs': ('NativeAdrenoSubmit.cpp',
            r'extern "C" volatile uint32_t native_gpu_dcvs\[15\] __attribute__\(\(weak_import\)\);',
            r'static volatile uint32_t *const native_gpu_dcvs=nullptr;'),
    }
    require(not missing - optional.keys(), 'unreviewed optional imports: ' + str(sorted(missing - optional.keys())))
    report['absent_optional_observers'] = sorted(missing)
    report['optional_compilations'] = []
    for name in sorted(missing):
        filename, pattern, replacement = optional[name]
        source = OUT / filename
        text = source.read_text() if source.exists() else (COMMON / filename).read_text()
        text, count = re.subn(pattern, replacement, text)
        require(count == 1, 'optional declaration changed: ' + name)
        source.write_text(text)
        result = compile_one(source, cpp, includes)
        report['optional_compilations'].append(result)
        (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        require(not result['exit_code'], result.get('error', 'optional source compile failed'))
    objects = [Path(r['object']) for r in results if Path(r['source']) != accelerator]
    llvm = Path(os.environ['UNIXNU_LLVM_OBJDUMP']).parent if os.environ.get('UNIXNU_LLVM_OBJDUMP') else None
    objcopy = str(llvm / 'llvm-objcopy') if llvm else shutil.which('llvm-objcopy')
    require(objcopy, 'llvm-objcopy is needed for the existing late initializer protocol')
    late = OUT / 'NativeAdrenoAccelerator.late.o'
    subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                    '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp',
                    str(OUT / 'NativeAdrenoAccelerator.o'), str(late)], check=True)
    sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
    def text_bytes(path):
        raw = path.read_bytes()
        section = next(s for segment in object_layout(path)['segments']
                       for s in segment['sections'] if s['name'] == '__text')
        return raw[section['fileoff']:section['fileoff'] + section['size']]
    require(text_bytes(late) == text_bytes(OUT / 'NativeAdrenoAccelerator.o'), 'late staging changed executable code')
    require(not any(s['name'] == '__mod_init_func' for segment in object_layout(late)['segments']
                    for s in segment['sections']), 'accelerator still has an early initializer')
    bindings = {
        '__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(profile['protected_compatible_constructor_aliases'][0], 16),
        '__ZN15IOAcceleratorESD2Ev': int(profile['nondeleting_destructor'], 16),
        '__ZN15IOAcceleratorES10gMetaClassE': int(profile['metaclass'], 16),
    }
    stub, addresses = '.text\n.p2align 2\n', ''
    for name, address in bindings.items():
        if name.endswith('gMetaClassE'):
            addresses += f'.globl {name}\n.set {name}, {address:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
            stub += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    (OUT / 'original-bindings.S').write_text(stub)
    (OUT / 'original-addresses.S').write_text(addresses)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    for source in [OUT / 'original-bindings.S', OUT / 'original-addresses.S', BOARD / 'NezhaGpuFirmwareSlot.S']:
        obj = OUT / (source.stem + '.o')
        subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', str(source), '-o', str(obj)], check=True)
        check_object(obj.read_bytes(), str(source))
        objects.append(obj)
    objects.append(late)
    archive = OUT / 'libAdreno840.a'
    # Owned output only. A new archive cannot retain a member from a previous source list.
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True,
                   env=dict(os.environ, ZERO_AR_DATE='1'))
    report.update(archive=str(archive), archive_sha256=sha(archive),
                  objects={p.name: sha(p) for p in objects}, bindings={k: hex(v) for k, v in bindings.items()},
                  late_code_equal=True, strict_prelink_required=True)
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: arm64e A840 archive; private firmware slot empty; native link, ABI and strict prelink still required')


if __name__ == '__main__':
    main()
