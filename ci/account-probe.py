"""Build an ordinary iOS SecItem helper; seal only its code and build diagnostics."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys

from artifacts import seal

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--sealed', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True, exist_ok=False)
logs = a.output / 'logs'
logs.mkdir()
commands = []


def run(name, command):
    value = subprocess.run(list(map(str, command)), capture_output=True, timeout=180)
    (logs / (name + '.stdout')).write_bytes(value.stdout)
    (logs / (name + '.stderr')).write_bytes(value.stderr)
    commands.append(dict(name=name, command=list(map(str, command)), exit_code=value.returncode))
    (a.output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    if value.returncode:
        raise RuntimeError('Command failed: ' + name + '; original diagnostics are in the sealed artifact')
    return value.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


ok = False
try:
    source, entitlements = root / 'source/AccountKeyItemProbe.m', root / 'source/entitlements.plist'
    expected = {'application-identifier': 'org.unixnu.nezha.account27.test',
                'keychain-access-groups': ['org.unixnu.nezha.account27.test']}
    assert plistlib.loads(entitlements.read_bytes()) == expected
    sdk = run('sdk', ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).decode().strip()
    sdk_version = run('sdk-version', ['xcrun', '--sdk', 'iphoneos', '--show-sdk-version']).decode().strip()
    compiler = run('compiler', ['xcrun', '--sdk', 'iphoneos', 'clang', '--version']).decode().strip()
    binary = a.output / 'account27-secitem'
    run('compile', ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios27.0',
                   '-isysroot', sdk, '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-O2',
                   '-framework', 'Foundation', '-framework', 'Security', '-framework', 'CoreFoundation',
                   source, '-o', binary])
    run('sign', ['codesign', '--force', '--sign', '-', '--identifier', expected['application-identifier'],
                 '--entitlements', entitlements, '--generate-entitlement-der', '--pagesize', '4096',
                 '--digest-algorithm', 'sha256', binary])
    run('verify-sign', ['codesign', '--verify', '--strict', binary])
    actual_entitlements = run('entitlements', ['codesign', '-d', '--entitlements', ':-', binary])
    assert plistlib.loads(actual_entitlements) == expected
    imports = run('imports', ['xcrun', 'nm', '-u', binary]).decode()
    required = ['_SecItemAdd', '_SecItemCopyMatching', '_SecItemUpdate', '_SecItemDelete', '_sysctlbyname']
    assert all(name in imports.split() for name in required)
    dependencies = run('dependencies', ['xcrun', 'otool', '-L', binary]).decode()
    build = run('build-platform', ['xcrun', 'vtool', '-show-build', binary]).decode()
    data = binary.read_bytes()
    magic, cpu, subtype, kind, count, length = struct.unpack_from('<6I', data)
    assert magic == 0xfeedfacf and cpu == 0x0100000c and subtype & 0xff == 2 and kind == 2
    cursor = 32
    target = []
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, cursor)
        assert 8 <= size and cursor + size <= 32 + length
        if command == 0x32:
            target.append(struct.unpack_from('<4I', data, cursor + 8))
        cursor += size
    assert cursor == 32 + length and len(target) == 1 and target[0][0:2] == (2, 27 << 16)
    shutil.copyfile(source, a.output / source.name)
    shutil.copyfile(entitlements, a.output / entitlements.name)
    report = dict(passed=True, target='arm64e-apple-ios27.0', sdk_version=sdk_version, compiler=compiler,
                  source_sha256=sha(source), entitlements_sha256=sha(entitlements), binary_sha256=sha(binary),
                  binary_bytes=len(data), declared_entitlements=expected, required_imports=required,
                  dependencies=dependencies, platform_report=build, device_executed=False,
                  SecItem_runtime_verified=False, cross_boot_verified=False, server_verified=False)
    (a.output / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')
    ok = True
finally:
    (a.output / 'build-state.json').write_text(json.dumps(dict(passed=ok, phone_operations=False,
        purpose='Ordinary dedicated-test SecItem helper; no device or account data input')) + '\n')
    names = sorted(str(path.relative_to(a.output)) for path in a.output.rglob('*') if path.is_file())
    seal(a.output, names, root / 'ci/artifact-recipient.pem', a.sealed)

print('AA27 ordinary SecItem helper compiled/signed; runtime and deployment remain unverified')
