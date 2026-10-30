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
    work.mkdir(parents=True, exist_ok=False)
    for name in ('device', 'soc'):
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
    run(['git', '-C', usb, 'remote', 'add', 'origin', manifest['usb']['repository']])
    run(['git', '-C', usb, 'remote', 'set-url', '--push', 'origin', 'DISABLED-no-push'])
    (work / 'device/local.toml').write_text(
        '[[source]]\nname = "sm8850"\nkind = "soc"\nlocal = "../soc"\n'
        '\n[[source]]\nname = "QcomXnuUsbDebug"\nkind = "driver"\nlocal = "../usb"\n')
    jobs = max(1, min(os.cpu_count() or 1, 3))  # hosted M1 memory/CPU budget; local builds retain 32.
    config = {'device': str(work / 'device'), 'out': str(work / 'out'), 'jobs': jobs}
    text = '[build]\n' + ''.join(k + ' = ' + json.dumps(v) + '\n' for k, v in config.items())
    (work / 'build.toml').write_text(text)
    (work / 'source-report.json').write_text(json.dumps({
        'source_inputs': manifest, 'actual_framework_base': run(['git', 'rev-parse', 'HEAD'], cwd=framework),
        'actual_framework_tree': tree, 'source_tree_verified': True, 'jobs': jobs,
        'device_executed': False, 'proprietary_inputs_uploaded': False,
    }, indent=2) + '\n')


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
        os.environ.update(UNIXNU_XNU_ARCH='arm64e', UNIXNU_IOS_VERSION='27.0')
        from kernel_target import check_object
        check_object((work / 'authenticated-null.o').read_bytes(), 'authenticated-null probe')
        report['object_target_validated'] = True
        if run([*command, '-S', source, '-o', work / 'authenticated-null.s'], log=logs / 'compiler-assembly.log'):
            raise ValueError('probe assembly generation failed')
    (work / 'compiler-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if probe:
        return probe
    config = work / 'build.toml'
    commands = [
        ['python3', framework / 'unixnu', 'defconfig', 'nezha_ios27_defconfig', '--config', config],
        ['python3', framework / 'unixnu', 'kernel', '--skip-prelink', '--config', config],
    ]
    for index, command in enumerate(commands):
        code = run(command, cwd=framework, log=logs / ('step-' + str(index) + '.log'))
        if code:
            return code
    return 0


def reference(framework, work):
    targets = list((work / 'out/nezha/ios-27.0.1-24A446-iphone14-4').glob('*'))
    targets = [p for p in targets if (p / 'tree/.cache/xnu-work').is_dir()]
    if len(targets) != 1:
        raise ValueError('select exactly one prepared source target for the comparison reference')
    return run(['python3', framework / 'scripts/build-fileset-release-reference.py',
                '--tree', targets[0] / 'tree', '--ios-version', '27.0', '--jobs', '3', '--lto'],
               cwd=framework, log=work / 'logs/reference-step.log')


def collect(framework, work, sealed):
    collected = work.parent / 'collected'
    collected.mkdir(exist_ok=False)
    names = []

    def add(source, name):
        if not source.is_file() or source.is_symlink():
            return
        target = collected / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        names.append(name)

    add(ROOT / 'inputs.json', 'inputs.json')
    for name in ('source-report.json', 'compiler-report.json', 'authenticated-null.c',
                 'authenticated-null.o', 'authenticated-null.s'):
        add(work / name, name)
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
        add(tree / 'build/qcom-usb-debug/build-report.json', 'archives/qcom-usb-debug/build-report.json')
        # A failed standalone compile retains its exact flags and diagnostics.
        for attempt in sorted((tree / '.cache/qcom-usb-debug').glob('archive-*/module')):
            for path in sorted(attempt.iterdir()):
                if path.suffix in ('.log', '.json', '.o'):
                    add(path, 'usb-attempts/' + attempt.parent.name + '/' + path.name)
        objects = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
        for name in ('link.filelist', 'all-alias.exp'):
            add(objects / name, 'objects/' + name)
        reference = tree / 'build/xnu/fileset-reference'
        for path in sorted(reference.glob('*.json')):
            add(path, 'reference/' + path.name)
        for path in sorted(reference.glob('*.log')):
            add(path, 'reference/' + path.name)
        add(reference / 'lto-obj/RELEASE_ARM64_BCM2837/mach.release.bcm2837.unstripped.noctf',
            'reference/kernel.macho')
        for stem in ('start', 'pe_kprintf', 'debug', 'printf', 'serial_console', 'serial_general',
                     'bsd_init', 'subr_prf', 'arm_init', 'startup'):
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
