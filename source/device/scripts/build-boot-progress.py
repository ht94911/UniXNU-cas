#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the provider-aware boot-progress diagnostic (boot-progress=2) for cas.

Port of UniXNU-umi scripts/build-boot-progress.py (3eb72578597d2ca16471b172d45eddc096a34a04), the complete
sheng diagnostic with absent providers made explicit: build-xnu-4k-baseline.sh runs it when
CONFIG_XNU_BOOT_PROGRESS=y, after the [kernel] archives, and force-loads build/boot-progress/libBootProgress.a.
Sources are kits/nezha/boot-progress (umi's package/boot-progress; the build tag argument is cas-build-tag).

Changes against umi, for this Linux host and the Format 2 tree:

  * llvm-objdump comes from PATH (the distribution's LLVM), not /opt/homebrew;
  * no UMI_FULL_BOOT_PROGRESS/UMI_EARLY_TRACE profile: the umi RAM-console trace does not exist on cas, so
    UMI_BOOT_PROGRESS_PHONE=0, UMI_RUNTIME_TRACE=0 and UMI_FTS_TOUCH=0 (touch arrives in M3);
  * the provider inventory is what this kernel actually links: the XNU objects of link.filelist
    (scripts/xnu_symbols.py) and the archives build-xnu-4k-baseline.sh force-loads for the current XNU_*
    selection, instead of umi's earlier kernel.macho and archive paths;
  * a weak import without a provider becomes a typed NULL address in generated copies under build/
    (sources and their local headers), as umi does; besides native_* telemetry, the armcpu_*/umi_armcpu_*
    counters of umi's ArmCpu observer patch may be absent.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'kits/nezha/boot-progress'
OUT = ROOT / 'build/boot-progress'
OBJ = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
UNITS = ('BootProgress', 'BootFaultProbe', 'UmiUserspaceProbe')
OPTIONAL_PREFIXES = ('_native_', '_armcpu_', '_umi_armcpu_')
sys.path.insert(0, str(ROOT / 'scripts'))
import xnu_symbols  # noqa: E402


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def on(name):
    return os.environ.get(name) == '1'


def linked_inputs():
    """The archives and objects build-xnu-4k-baseline.sh links for the current XNU_* selection."""
    paths = [ROOT / 'build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a']
    selected = [('XNU_ARM_DT_PLATFORM', ROOT / 'build/armdt-platform/libArmDTPlatform.a'),
                ('XNU_ARMCPU', ROOT / 'build/armcpu/libArmCpu.a'),
                ('XNU_NATIVE_UFS', ROOT / 'build/sheng-ufs/libShengUfs.a'),
                ('XNU_NATIVE_LEGACY_DISPLAY', ROOT / 'build/native-legacy-display/libNativeLegacyDisplay.late.a'),
                ('XNU_NATIVE_AES', Path(os.environ.get('XNU_NATIVE_AES_ARCHIVE') or ROOT / 'build/software-aes/libSoftwareAES.a')),
                ('XNU_NATIVE_VIRTIO_BLOCK', Path(os.environ.get('XNU_NATIVE_VIRTIO_BLOCK_ARCHIVE') or
                                                 ROOT / 'build/virtio-mmio-block/libVirtioMmioBlock.a')),
                ('XNU_NATIVE_RAM_BLOCK', ROOT / ('build/ram-block/NativeRamBlock.o' if on('XNU_NATIVE_VIRTIO_BLOCK')
                                                 else 'build/ram-block/libRamBlock.a')),
                ('XNU_SOURCE_PTHREAD', ROOT / 'build/libpthread-kernel/libpthread_kernel.a'),
                ('XNU_VERIFIED_ROOT_PROBE', Path(os.environ.get('XNU_VERIFIED_ROOT_ARCHIVE') or
                                                 ROOT / 'build/verified-root-xnu/libVerifiedRootProbe.a')),
                ('XNU_SOURCE_MOCKFS', ROOT / 'build/mockfs-fixture/libmockfs_fixture.a'),
                ('XNU_SHENG_PMGR', ROOT / 'build/sheng-platform-power/libShengPlatformPower.a')]
    paths += [path for flag, path in selected if on(flag)]
    paths += [Path(p.rsplit(',', 1)[-1]) for p in os.environ.get('XNU_EXTRA_LD_ARCHIVES', '').split() if p.endswith(('.a', '.o'))]
    return paths


def record(name):
    path = OBJ / ('bsd/DEVELOPMENT/kern_exec.o.json' if name in ('BootWatchdogPolicy', 'UmiUserspaceProbe')
                  else 'osfmk/DEVELOPMENT/startup.o.json')
    return json.loads(path.read_text().strip().rstrip(','))


def compile_command(name, source, obj):
    r = record(name)
    args, command, i = r['arguments'], [], 0
    while i < len(args):
        arg = args[i]
        if arg == r['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif arg == '-D' and args[i + 1].startswith('XNU_BOOT_PROGRESS='):
            i += 1
        elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
            command.append(arg)
        i += 1
    command += ['-DUMI_FTS_TOUCH=0', '-DXNU_BOOT_PROGRESS=1', '-DUMI_BOOT_PROGRESS_PHONE=0', '-DUMI_RUNTIME_TRACE=0',
                '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    return command, r['directory']


def inspect(obj, required=()):
    asm = subprocess.check_output([os.environ.get('UNIXNU_LLVM_OBJDUMP') or 'llvm-objdump', '-d', str(obj)], text=True)
    operands = '\n'.join(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+(?:[0-9a-f]{8}|(?:[0-9a-f]{2} ){3}[0-9a-f]{2})[ \t]+\w+[ \t]+([^\n]+)', asm, re.M))
    if re.search(r'\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b', operands):
        raise SystemExit('FP/SIMD found in ' + obj.name)
    if re.search(r'\bmsr\s+(?:TCR_EL1|TTBR[01]_EL1|SCTLR_EL1)', asm, re.I):
        raise SystemExit('Translation write found in observer ' + obj.name)
    for pattern in required:
        if not re.search(pattern, asm, re.I):
            raise SystemExit('Missing actual ARM64 instruction: ' + pattern)
    (OUT / (obj.stem + '.asm')).write_text(asm)
    return operands


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    objects, commands, compile_errors = [], {}, []
    for name in UNITS:
        obj = OUT / (name + '.o')
        command, directory = compile_command(name, SOURCE / (name + '.c'), obj)
        compiled = subprocess.run(command, cwd=directory, timeout=90)
        if compiled.returncode:
            compile_errors.append(name)
        commands[name] = (command, directory)
        objects.append(obj)
    if compile_errors:
        raise SystemExit('Target diagnostic units failed: ' + ', '.join(compile_errors))
    # Mach-O static kernel links reject absent weak imports: an optional address without a provider becomes a
    # compile-time NULL in a generated copy. No driver symbol or backing counter is defined here.
    providers = linked_inputs()
    for path in providers:
        if not path.exists():
            raise SystemExit('Missing actual provider inventory: ' + str(path))
    defined = xnu_symbols.kernel_names(ROOT) | xnu_symbols.defined_names(providers + objects, True)
    if on('XNU_OWNER_ROOT_AUDIT'):
        # Defined in this build's bsd_init translation unit, rebuilt after this observer.
        text = (ROOT / '.cache/xnu-work/bsd/kern/bsd_init.c').read_text()
        defined |= {'_' + n for n in re.findall(r'\b(native_owner_root_\w+)\s*=', text)}
    weak = set()
    for obj in objects:
        symbols = subprocess.check_output(['xcrun', 'nm', '-m', str(obj)], text=True)
        weak.update(re.findall(r'\(undefined\) weak external (_\w+)', symbols))
    missing = sorted(weak - defined)
    unexpected = [n for n in missing if not n.startswith(OPTIONAL_PREFIXES)]
    if unexpected:
        raise SystemExit('Unexpected missing kernel observer import: ' + str(unexpected))
    found = set()

    def optional(match):
        declaration = match.group()
        names = [s[1:] for s in missing if re.search(r'\b' + re.escape(s[1:]) + r'\b', declaration)]
        if not names:
            return declaration
        repeated = [n for n in names if '_' + n in found]
        found.update('_' + n for n in names)
        return ('\n'.join('#undef ' + n for n in repeated) + '\n' + declaration +
                '\n// No linked provider: typed unavailable addresses, not fake objects.\n' +
                '\n'.join(f'#define {n} (*(__typeof__(&{n}))0)' for n in names) + '\n')

    generated = {}
    weak_declaration = re.compile(r'extern\b[^;]*?__attribute__\(\(weak_import\)\)[^;]*;', re.S)
    # Local headers first (their declarations precede the units' uses), then every unit, all compiled from OUT so
    # that quoted includes resolve to the generated headers.
    for header in sorted(SOURCE.glob('*.h')):
        text = weak_declaration.sub(optional, header.read_text())
        (OUT / header.name).write_text(text)
        if text != header.read_text():
            generated[header.name] = sha(OUT / header.name)
    for name in UNITS:
        text = weak_declaration.sub(optional, (SOURCE / (name + '.c')).read_text())
        path = OUT / (name + '.providers.c')
        path.write_text(text)
        generated[path.name] = sha(path)
        command, directory = commands[name]
        command = [str(path) if v == str(SOURCE / (name + '.c')) else v for v in command] + ['-I', str(SOURCE)]
        subprocess.run(command, cwd=directory, check=True, timeout=90)
        commands[name] = (command, directory)
        undefined = subprocess.check_output(['xcrun', 'nm', '-u', str(OUT / (name + '.o'))], text=True)
        for symbol in found:
            if re.search(r'^' + re.escape(symbol) + r'$', undefined, re.M):
                raise SystemExit('Unavailable import survived compilation: ' + symbol)
    if set(missing) - found:
        raise SystemExit('Absent imports without a declaration to replace: ' + str(sorted(set(missing) - found)))
    for obj in objects:
        inspect(obj, (r'mrs\s+x\d+,\s*CNTPCT_EL0', r'mrs\s+x\d+,\s*CNTFRQ_EL0', r'msr\s+DAIFSet,\s*#(?:0x)?3\b',
                      r'msr\s+DAIF,\s*x\d+') if obj.stem == 'BootProgress' else ())
    archive = OUT / 'libBootProgress.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    (OUT / 'compile-commands.json').write_text(json.dumps({k: v[0] for k, v in commands.items()}, indent=2) + '\n')
    (OUT / 'build-report.json').write_text(json.dumps({
        'implementation': 'complete-sheng-diagnostics-with-umi-provider-adaptation (cas port)',
        'upstream': json.loads((SOURCE / 'upstream.json').read_text()),
        'umi_revision': '3eb72578597d2ca16471b172d45eddc096a34a04',
        'phone_trace': False,
        'sources': {p.name: sha(p) for p in sorted(SOURCE.iterdir()) if p.is_file()},
        'objects': {p.name: sha(p) for p in objects}, 'archive_sha256': sha(archive),
        'generated_sources': generated, 'unavailable_optional_imports': missing,
        'provider_inventory': {str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p): sha(p) for p in providers},
        'actual_arm64_instructions_checked': True, 'no_fp_simd': True, 'hardware_execution': False,
        'storage_provider_required': False, 'absent_providers_reported': True,
    }, indent=2) + '\n')
    print(f'PASS: complete diagnostic, fault and process observers built for ARM64; {len(missing)} absent optional imports')


if __name__ == '__main__':
    main()
