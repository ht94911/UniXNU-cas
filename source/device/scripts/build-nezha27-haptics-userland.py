#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the ordinary UIKit haptics app and reused LaunchServices helper."""
import argparse
import hashlib
import json
import plistlib
import subprocess
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    logs = args.out / 'logs'
    logs.mkdir()
    commands = []

    def run(name, command):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, timeout=180)
        (logs / (name + '.stdout')).write_bytes(result.stdout)
        (logs / (name + '.stderr')).write_bytes(result.stderr)
        commands.append(dict(name=name, command=command, exit_code=result.returncode))
        (args.out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(name + ' failed; inspect the saved original logs')
        return result.stdout

    sdk = run('sdk', ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).decode().strip()
    flags = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios27.0',
             '-isysroot', sdk, '-fobjc-arc', '-fblocks', '-O2', '-g0', '-Wall', '-Wextra', '-Werror']
    info = plistlib.loads((args.source / 'Info.plist').read_bytes())
    assert info['CFBundleIdentifier'] == 'org.unixnu.nezha.haptics27.test'
    assert info['CFBundleExecutable'] == 'NezhaHapticsUI'
    bundle = args.out / 'NezhaHapticsUI.app'
    bundle.mkdir()
    shutil.copyfile(args.source / 'Info.plist', bundle / 'Info.plist')
    run('app-compile', [*flags, args.source / 'NezhaHapticsApp.m', '-framework', 'UIKit',
                       '-framework', 'CoreHaptics', '-framework', 'Foundation', '-o', bundle / 'NezhaHapticsUI'])
    run('app-sign', ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--digest-algorithm', 'sha256',
                    '--identifier', info['CFBundleIdentifier'], '--entitlements', args.source / 'app.entitlements.plist',
                    '--generate-entitlement-der', bundle])
    run('app-verify', ['codesign', '--verify', '--strict', bundle])
    launcher = args.out / 'nezha-haptics-launch'
    run('launcher-compile', [*flags, args.source / 'NezhaHapticsLaunch.m', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-o', launcher])
    run('launcher-sign', ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--digest-algorithm', 'sha256',
                         '--identifier', 'org.unixnu.nezha.haptics-launch', '--entitlements',
                         args.source / 'launch.entitlements.plist', '--generate-entitlement-der', launcher])
    run('launcher-verify', ['codesign', '--verify', '--strict', launcher])
    for label, path in [('app', bundle / 'NezhaHapticsUI'), ('launcher', launcher)]:
        run(label + '-signature', ['codesign', '-d', '--verbose=4', path])
        run(label + '-imports', ['xcrun', 'nm', '-u', path])
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = dict(passed=True, target='arm64e-apple-ios27.0', code_page_bytes=4096,
                  source_files={p.name: sha(p) for p in args.source.iterdir() if p.is_file()},
                  outputs={p.relative_to(args.out).as_posix(): sha(p) for p in args.out.rglob('*') if p.is_file()},
                  normal_UIKit_API=True, normal_CoreHaptics_API=True, actual_hardware_verified=False,
                  LaunchServices_reuse='Sensors002/Camera original registration and foreground launch', phone_operations=False)
    (args.out / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS ordinary UIKit/CoreHaptics app and original LaunchServices helper target-built and signed')


if __name__ == '__main__':
    main()
