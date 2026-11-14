#!/usr/bin/env python3
"""Thin Actions adapter around the existing UniXNU driver and CMS artifact tool."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, *, cwd=None, env=None, log=None):
    if log:
        with Path(log).open('wb') as stream:
            return subprocess.run([str(a) for a in argv], cwd=cwd, env=env,
                                  stdout=stream, stderr=subprocess.STDOUT).returncode
    return subprocess.check_output([str(a) for a in argv], cwd=cwd, env=env, text=True).strip()


def check_keystore_source(framework, work):
    """Bind the existing explicit module override before any target compilation."""
    manifest = json.loads((ROOT / 'inputs.json').read_text())
    selected = (work / 'device/components/AppleKeyStoreState').resolve()
    sys.path.insert(0, str(framework / 'scripts'))
    from unixnu.cli import parser
    from unixnu.config import Context
    from unixnu.feeds import resolve
    override = 'driver:AppleKeyStoreState=' + str(selected)
    args = parser().parse_args(['status', '--config', str(work / 'build.toml'),
                               '--device', str(work / 'device'), '--out', str(work / 'out'),
                               '--local-module', override])
    module = resolve(Context(args))[('driver', 'AppleKeyStoreState')]
    if module.path.resolve() != selected or module.origin != 'local (--local-module)':
        raise ValueError('The effective AppleKeyStoreState source is not the explicit snapshot')
    actual = {name: sha(selected / name) for name in manifest['keystore']['source_files']}
    if actual != manifest['keystore']['source_files']:
        raise ValueError('Effective AppleKeyStoreState source SHA differs')
    lock = work / 'device/package/keystore013-source-lock.json'
    if sha(lock) != manifest['keystore']['source_lock_sha256']:
        raise ValueError('Effective AppleKeyStoreState source lock differs')
    source_lock = json.loads(lock.read_text())
    if (source_lock['source_revision'] != manifest['keystore']['source_revision'] or
            source_lock['tree'] != manifest['keystore']['source_tree'] or
            actual != {name: item['sha256'] for name, item in source_lock['source_files'].items()} or
            sha(work / 'device/package/KeyStoreProbe.cpp') != source_lock['device_callback_source_sha256']):
        raise ValueError('Effective KeyStore component/getter provenance differs')
    result = dict(passed=True, module='AppleKeyStoreState', origin=module.origin,
                  source=str(selected), source_files=actual,
                  device_revision=manifest['device_revision'], override=override,
                  source_lock_sha256=sha(lock), callback_source_sha256=source_lock['device_callback_source_sha256'],
                  phone_operations=False, target_compiled=False)
    (work / 'keystore-source-selection.json').write_text(json.dumps(result, indent=2) + '\n')
    return override

def prepare(framework, work):
    manifest = json.loads((ROOT / 'inputs.json').read_text())
    for name, digest in manifest['files'].items():
        if sha(ROOT / name) != digest:
            raise ValueError('source input hash mismatch: ' + name)
    for name, key in [('ci/artifacts.py', 'artifacts_sha256'),
                      ('ci/artifact-recipient.pem', 'certificate_sha256')]:
        if sha(ROOT / name) != manifest[key]:
            raise ValueError('artifact helper identity mismatch')
    if run(['git', 'rev-parse', 'HEAD'], cwd=framework) != manifest['framework']['upstream_revision']:
        raise ValueError('framework upstream commit mismatch')
    # Stage the exact tracked overlay; compare Git tree objects rather than
    # claiming the checkout's base commit is the local integration commit.
    run(['git', 'apply', '--check', '--index', ROOT / 'source/framework.patch'], cwd=framework)
    run(['git', 'apply', '--index', ROOT / 'source/framework.patch'], cwd=framework)
    tree = run(['git', 'write-tree'], cwd=framework)
    if tree != manifest['framework']['source_tree']:
        raise ValueError('reconstructed framework tree differs from integration tree')
    base_tree = tree
    userlog = manifest['userlog_target']
    overlay = ROOT / userlog['source_overlay']
    selected = ['--include=' + name for name in userlog['framework_overlay_files']]
    run(['git', 'apply', '--check', '--index', *selected, overlay], cwd=framework)
    run(['git', 'apply', '--index', *selected, overlay], cwd=framework)
    tree = run(['git', 'write-tree'], cwd=framework)
    if tree != userlog['framework_tree']:
        raise ValueError('Accepted UserLog framework adapter tree differs')
    work.mkdir(parents=True, exist_ok=False)
    for name in ('device', 'soc'):
        shutil.copytree(ROOT / 'source' / name, work / name)
    for name in ('adreno', 'metal', 'software-aes'):
        shutil.copytree(ROOT / 'source' / name, work / name)
    usb = work / 'usb'
    run(['git', 'init', '-q', usb])
    run(['git', '-C', usb, 'bundle', 'unbundle', ROOT / 'source/usb.bundle'])
    # Git bundles do not transport shallow boundary metadata. This bundle is
    # deliberately a single audited source tree, not the source repo history.
    (usb / '.git/shallow').write_text(manifest['usb']['source_revision'] + '\n')
    run(['git', '-C', usb, 'checkout', '--quiet', '--detach', manifest['usb']['source_revision']])
    if run(['git', '-C', usb, 'rev-parse', 'HEAD^{tree}']) != manifest['usb']['source_tree']:
        raise ValueError('USB source tree mismatch')
    if run(['git', '-C', usb, 'status', '--porcelain']):
        raise ValueError('USB source is not clean')
    actual_userlog = {name: sha(usb / name) for name in userlog['source_files']}
    if actual_userlog != userlog['source_files']:
        raise ValueError('Composed USB/UserLog source identity differs')
    (work / 'userlog-source-selection.json').write_text(json.dumps(dict(
        passed=True, actual_usb_tree=manifest['usb']['source_tree'],
        base_framework_tree=base_tree, actual_framework_tree=tree,
        source_files=actual_userlog, source_only=True), indent=2) + '\n')
    run(['git', '-C', usb, 'remote', 'add', 'origin', manifest['usb']['repository']])
    run(['git', '-C', usb, 'remote', 'set-url', '--push', 'origin', 'DISABLED-no-push'])
    ufs = work / 'ufs'
    run(['git', 'init', '-q', ufs])
    run(['git', '-C', ufs, 'bundle', 'unbundle', ROOT / 'source/ufs.bundle'])
    (ufs / '.git/shallow').write_text(manifest['ufs']['source_revision'] + '\n')
    run(['git', '-C', ufs, 'checkout', '--quiet', '--detach', manifest['ufs']['source_revision']])
    if run(['git', '-C', ufs, 'rev-parse', 'HEAD^{tree}']) != manifest['ufs']['source_tree']:
        raise ValueError('UFS source tree mismatch')
    run(['git', '-C', ufs, 'remote', 'add', 'origin', manifest['ufs']['repository']])
    run(['git', '-C', ufs, 'remote', 'set-url', '--push', 'origin', 'DISABLED-no-push'])
    ramblock = work / 'ramblock'
    run(['git', 'init', '-q', ramblock])
    run(['git', '-C', ramblock, 'bundle', 'unbundle', ROOT / 'source/ramblock.bundle'])
    (ramblock / '.git/shallow').write_text(manifest['ramblock']['source_revision'] + '\n')
    run(['git', '-C', ramblock, 'checkout', '--quiet', '--detach', manifest['ramblock']['source_revision']])
    if run(['git', '-C', ramblock, 'rev-parse', 'HEAD^{tree}']) != manifest['ramblock']['source_tree']:
        raise ValueError('RamBlock source tree mismatch')
    if run(['git', '-C', ramblock, 'status', '--porcelain']):
        raise ValueError('RamBlock source is not clean')
    run(['git', '-C', ramblock, 'remote', 'add', 'origin', manifest['ramblock']['repository']])
    run(['git', '-C', ramblock, 'remote', 'set-url', '--push', 'origin', 'DISABLED-no-push'])
    gop = work / 'gop'
    run(['git', 'init', '-q', gop])
    run(['git', '-C', gop, 'bundle', 'unbundle', ROOT / 'source/gop.bundle'])
    (gop / '.git/shallow').write_text(manifest['gop']['source_revision'] + '\n')
    run(['git', '-C', gop, 'checkout', '--quiet', '--detach', manifest['gop']['source_revision']])
    if run(['git', '-C', gop, 'rev-parse', 'HEAD^{tree}']) != manifest['gop']['source_tree']:
        raise ValueError('GOP source tree mismatch')
    if run(['git', '-C', gop, 'status', '--porcelain']):
        raise ValueError('GOP source is not clean')
    run(['git', '-C', gop, 'remote', 'add', 'origin', manifest['gop']['repository']])
    run(['git', '-C', gop, 'remote', 'set-url', '--push', 'origin', 'DISABLED-no-push'])
    (work / 'device/local.toml').write_text(
        '[[source]]\nname = "sm8850"\nkind = "soc"\nlocal = "../soc"\n'
        '\n[[source]]\nname = "QcomXnuUsbDebug"\nkind = "driver"\nlocal = "../usb"\n')
    jobs = max(1, min(os.cpu_count() or 1, 3))  # hosted M1 memory/CPU budget; local builds retain 32.
    with (work / 'device/local.toml').open('a') as output:
        output.write('\n[[source]]\nname = "ShengUfs"\nkind = "driver"\nlocal = "../ufs"\n')
    with (work / 'device/local.toml').open('a') as output:
        output.write('\n[[source]]\nname = "RamBlock"\nkind = "driver"\nlocal = "../ramblock"\n')
    with (work / 'device/local.toml').open('a') as output:
        output.write('\n[[source]]\nname = "Adreno740"\nkind = "refer"\nlocal = "../adreno"\n'
                     '\n[[source]]\nname = "AdrenoMetal"\nkind = "refer"\nlocal = "../metal"\n')
    with (work / 'device/local.toml').open('a') as output:
        output.write('\n[[source]]\nname = "GopFramebuffer"\nkind = "driver"\nlocal = "../gop"\n')
    with (work / 'device/local.toml').open('a') as output:
        output.write('\n[[source]]\nname = "SoftwareAES"\nkind = "driver"\nlocal = "../software-aes"\n')
    config = {'device': str(work / 'device'), 'out': str(work / 'out'), 'jobs': jobs}
    text = '[build]\n' + ''.join(k + ' = ' + json.dumps(v) + '\n' for k, v in config.items())
    (work / 'build.toml').write_text(text)
    run([sys.executable, work / 'device/scripts/check-adreno840-integration.py',
         '--framework', framework, '--device', work / 'device',
         '--out', work / 'integration-source-qualified.json'])
    (work / 'source-report.json').write_text(json.dumps({
        'source_inputs': manifest, 'actual_framework_base': run(['git', 'rev-parse', 'HEAD'], cwd=framework),
        'actual_framework_tree': tree, 'base_framework_tree': base_tree, 'source_tree_verified': True, 'jobs': jobs,
        'device_executed': False, 'proprietary_inputs_uploaded': False,
    }, indent=2) + '\n')
    check_keystore_source(framework, work)


def build(framework, work):
    logs = work / 'logs'
    logs.mkdir(exist_ok=True)
    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    source = work / 'authenticated-null.c'
    source.write_text('#include <ptrauth.h>\n'
        'struct S { int *__ptrauth(2, 1, 123, "authenticates-null-values") p; };\n'
        'void clear(struct S *s) { s->p = 0; }\n'
        'int *read(struct S *s) { return s->p; }\n')
    command = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios27.0',
               '-isysroot', sdk, '-mkernel', '-ffreestanding', '-O2']
    probe = run([*command, '-c', source, '-o', work / 'authenticated-null.o'], log=logs / 'compiler-probe.log')
    report = {'compiler_version': run(['xcrun', '--sdk', 'iphoneos', 'clang', '--version']),
              'xcode': run(['xcodebuild', '-version']), 'sdk': sdk, 'probe_compiled': probe == 0,
              'null_codegen_review_required': True, 'target_code_executed': False}
    if not probe:
        sys.path.insert(0, str(framework / 'scripts'))
        manifest = json.loads((ROOT / 'inputs.json').read_text())
        os.environ.update(UNIXNU_XNU_ARCH='arm64e', UNIXNU_IOS_VERSION='27.0',
                          UNIXNU_QCOM_USB_USERLOG='1',
                          UNIXNU_QCOM_USB_USERLOG_TREE=manifest['usb']['source_tree'])
        from kernel_target import check_object
        check_object((work / 'authenticated-null.o').read_bytes(), 'authenticated-null probe')
        report['object_target_validated'] = True
        if run([*command, '-S', source, '-o', work / 'authenticated-null.s'], log=logs / 'compiler-assembly.log'):
            raise ValueError('probe assembly generation failed')
    (work / 'compiler-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if probe:
        return probe
    config = work / 'build.toml'
    keystore_override = check_keystore_source(framework, work)
    commands = [
        ['python3', framework / 'unixnu', 'defconfig', 'nezha_ios27_defconfig', '--config', config, '--local-module', keystore_override],
        ['python3', framework / 'unixnu', 'kernel', '--skip-prelink', '--config', config, '--local-module', keystore_override],
    ]
    for index, command in enumerate(commands):
        code = run(command, cwd=framework, log=logs / ('step-' + str(index) + '.log'))
        if code:
            return code
    sources = list((work / 'out/nezha/ios-27.0.1-24A446-iphone14-4').glob(
        '*/tree/.cache/xnu-work/bsd/kern/kern_memorystatus.c'))
    if len(sources) != 1:
        raise ValueError('select the actual compiled memorystatus source')
    code = run([sys.executable, work / 'device/scripts/check-iconservice-memory.py',
                '--source', sources[0], '--out', work / 'icon-memory-host'],
               log=logs / 'icon-memory-host.log')
    if code: return code
    kernels = {path.resolve() for path in
        (work / 'out/nezha/ios-27.0.1-24A446-iphone14-4').glob('*/kernel/kernel.macho')}
    if len(kernels) != 1: raise ValueError('Select the actual joint Native043')
    kernel = next(iter(kernels)); tree = kernel.parent.parent / 'tree'
    llvm = Path(run(['brew', '--prefix', 'llvm'])) / 'bin'
    code = run([sys.executable, tree / 'scripts/check-keystore-source.py',
                '--source', tree / 'drivers/AppleKeyStoreState',
                '--probe', tree / 'kits/nezha/KeyStoreProbe.cpp',
                '--lock', tree / 'kits/nezha/keystore013-source-lock.json',
                '--tree', tree, '--llvm', llvm, '--kernel', kernel,
                '--out', work / 'keystore-target-qualification.json'],
               log=logs / 'keystore-target-qualification.log')
    if code: return code
    return run([sys.executable, HERE / 'check-native-userlog.py',
                '--tree', tree, '--component', work / 'usb', '--llvm', llvm,
                '--kernel', kernel, '--out', work / 'userlog-target-qualification.json'],
               log=logs / 'userlog-target-qualification.log')


def reference(framework, work):
    targets = list((work / 'out/nezha/ios-27.0.1-24A446-iphone14-4').glob('*'))
    targets = [p for p in targets if (p / 'tree/.cache/xnu-work').is_dir()]
    if len(targets) != 1:
        raise ValueError('select exactly one prepared source target for the comparison reference')
    env = dict(os.environ, UNIXNU_IOS_VERSION='27.0', UNIXNU_XNU_DARWIN='25.5.0',
               UNIXNU_XNU_VERSION='12377.121.6', XNU_TOPOLOGY_V3_COMPAT='1', XNU_IOS27_PLATFORM_FUNCTION_CLIENT='1', XNU_IOS27_IMAGE4_V2_NOMONITOR='1', XNU_SHENG_PMGR='1', XNU_SOURCE_STORAGE='1', XNU_OWNER_ROOT_AUDIT='1', XNU_OWNER_ROOT_TRUST='1')
    return run(['python3', framework / 'scripts/build-fileset-release-reference.py',
                '--tree', targets[0] / 'tree', '--jobs', '3', '--lto'],
               cwd=framework, env=env, log=work / 'logs/reference-step.log')


def collect(framework, work, sealed):
    collected = work.parent / 'collected'
    collected.mkdir(exist_ok=False)
    names = []

    def add(source, name):
        if not source.is_file() or source.is_symlink():
            return
        target = collected / name
        # Success cache and last-attempt can name the same build. Coalesce
        # identical content, while refusing real output-name collisions.
        if target.exists():
            if sha(target) != sha(source):
                raise ValueError('conflicting collected artifact: ' + name)
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        names.append(name)

    add(ROOT / 'inputs.json', 'inputs.json')
    manifest = json.loads((ROOT / 'inputs.json').read_text())
    for name in ('keystore-source-selection.json', 'keystore-target-qualification.json',
                 'userlog-source-selection.json', 'userlog-target-qualification.json'):
        add(work / name, name)
    for name in manifest['userlog_target']['source_files']:
        add(work / 'usb' / name, 'userlog-source/' + name)
    add(framework / 'scripts/build-qcom-usb-debug.py', 'userlog-source/build-qcom-usb-debug.py')
    for path in sorted((work / 'userlog-producer-host').glob('*')):
        if path.is_file() and path.suffix in ('.json', '.log', '.c'):
            add(path, 'userlog-producer-host/' + path.name)

    for name in ('source-report.json', 'integration-source-qualified.json', 'compiler-report.json', 'authenticated-null.c',
                 'authenticated-null.o', 'authenticated-null.s'):
        add(work / name, name)
    for name in ('report.json', 'actual-memory-limit.c', 'execution.txt'):
        add(work / 'icon-memory-host' / name, 'icon-memory-host/' + name)
    for path in sorted((work / 'logs').glob('*.log')):
        add(path, 'logs/' + path.name)
    for path in sorted((work.parent / 'setup-logs').glob('*.log')):
        add(path, 'setup/' + path.name)
    builds = list((work / 'out/nezha/ios-27.0.1-24A446-iphone14-4').glob('*'))
    for target in builds:
        if not target.is_dir():
            continue
        for path in sorted((target / 'logs').glob('*.log')):
            add(path, 'kernel-logs/' + path.name)
        for name in ('.config', 'xnu-inputs.json', 'xnu-patches.list', 'kernel/kernel.macho', 'kernel/kernel.json'):
            add(target / name, 'build/' + name)
        tree = target / 'tree'
        for path in sorted((tree / 'build').glob('*/lib*.a')):
            add(path, 'archives/' + path.parent.name + '/' + path.name)
        for path in sorted((tree / 'build/armcpu').glob('*')):
            if path.is_file() and path.suffix in ('.json', '.log', '.o', '.asm'):
                add(path, 'archives/armcpu/' + path.name)
        for path in sorted((tree / 'build/sheng-platform-power').glob('*')):
            if path.suffix in ('.json', '.log', '.o'):
                add(path, 'archives/sheng-platform-power/' + path.name)
        for path in sorted((tree / 'build/sheng-source-storage').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o'):
                add(path, 'archives/sheng-source-storage/' + path.name)
        for path in sorted((tree / 'build/native-legacy-display').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o','.S','.h','.cpp'):
                add(path, 'archives/native-legacy-display/' + path.name)
        for path in sorted((tree / 'build/ram-block').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o','.cpp','.h'):
                add(path, 'archives/ram-block/' + path.name)
        for path in sorted((tree / 'build/nezha27-usb-control').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o','.c'):
                add(path, 'archives/nezha27-usb-control/' + path.name)
        for path in sorted((tree / 'build/nezha27-input').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o','.S','.h','.cpp','.c'):
                add(path, 'archives/nezha27-input/' + path.name)
        for path in sorted((tree / 'build/adreno840').glob('*')):
            if path.is_file() and path.suffix in ('.json','.log','.o','.S','.h','.cpp'):
                add(path, 'archives/adreno840/' + path.name)
        for path in sorted((tree / 'build/boot-progress').glob('*')):
            if path.is_file() and path.suffix in ('.json','.asm','.o','.c','.h'):
                add(path, 'archives/boot-progress/' + path.name)
        add(tree / 'build/qcom-usb-debug/build-report.json', 'archives/qcom-usb-debug/build-report.json')
        # A failed standalone compile retains its exact flags and diagnostics.
        for attempt in sorted((tree / '.cache/qcom-usb-debug').glob('archive-*/module')):
            for path in sorted(attempt.iterdir()):
                if path.suffix in ('.log', '.json', '.o'):
                    add(path, 'usb-attempts/' + attempt.parent.name + '/' + path.name)
        for name in ('nezha27-wifi', 'nezha27-bluetooth', 'software-aes',
                     'nezha27-als', 'nezha27-als-owner', 'apple-key-store-state'):
            for path in sorted((tree / 'build' / name).glob('*')):
                if path.is_file() and path.suffix in ('.a', '.o', '.json', '.log', '.d', '.S', '.h', '.cpp', '.c', '.txt'):
                    add(path, 'archives/' + name + '/' + path.name)
        for name in manifest['keystore']['source_files']:
            add(tree / 'drivers/AppleKeyStoreState' / name, 'keystore-source/' + name)
        add(tree / 'kits/nezha/KeyStoreProbe.cpp', 'keystore-source/KeyStoreProbe.cpp')
        add(tree / 'kits/nezha/keystore013-source-lock.json', 'keystore-source/source-lock.json')
        add(tree / '.cache/xnu-work/bsd/kern/ubc_subr.c', 'text-fixup-source/ubc_subr.c')
        objects = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
        for name in ('link.filelist', 'all-alias.exp'):
            add(objects / name, 'objects/' + name)
        add(tree / 'build/xnu/owner-data-volume.h', 'build/owner-data-volume.h')
        for name in ('kern_memorystatus.c', 'kern_exec.c'):
            add(tree / '.cache/xnu-work/bsd/kern' / name, 'compiled-source/' + name)
        reference = tree / 'build/xnu/fileset-reference'
        for path in sorted(reference.glob('*.json')):
            add(path, 'reference/' + path.name)
        for path in sorted(reference.glob('*.log')):
            add(path, 'reference/' + path.name)
        add(reference / 'mach.release.bcm2837.unstripped.noctf', 'reference/kernel.macho')
        record = reference / 'mach.release.bcm2837.unstripped.json'
        # The canonical executable's .with_suffix('.json') cache record is
        # adjacent to it and carries the exact isolated build directory.
        records = list(reference.glob('*.json'))
        for candidate in records:
            data = json.loads(candidate.read_text())
            if 'inputs' not in data or 'build' not in data:
                continue
            selected = Path(data['build']).resolve()
            if not selected.is_relative_to(reference.resolve()):
                raise ValueError('comparison build directory escaped its output scope')
            for name in ('inputs.json', 'source-sha256.json', 'lto-command.json', 'lto-build.log'):
                add(selected / name, 'reference/' + name)
        for stem in ('start', 'pe_kprintf', 'debug', 'printf', 'serial_console', 'serial_general',
                     'bsd_init', 'subr_prf', 'arm_init', 'startup', 'machine_routines', 'IOUserServer', 'kern_memorystatus', 'ubc_subr'):
            for path in sorted(objects.glob('**/' + stem + '.o*')):
                if path.suffix in ('.o', '.json'):
                    add(path, 'objects/' + str(path.relative_to(objects)))
    spec = importlib.util.spec_from_file_location('existing_artifacts', HERE / 'artifacts.py')
    artifacts = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(artifacts)
    artifacts.seal(collected, names, HERE / 'artifact-recipient.pem', sealed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'build', 'reference', 'collect'))
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--sealed', type=Path)
    args = parser.parse_args()
    framework, work = args.framework.resolve(), args.work.resolve()
    if args.action == 'prepare':
        prepare(framework, work)
    elif args.action == 'build':
        return build(framework, work)
    elif args.action == 'reference':
        return reference(framework, work)
    elif args.action == 'collect':
        if args.sealed is None:
            parser.error('--sealed required')
        collect(framework, work, args.sealed.resolve())
    return 0


if __name__ == '__main__':
    sys.exit(main())
