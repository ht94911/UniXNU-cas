"""Record the actual macOS executor and compile one ARM64 object, not XNU."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def command(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    return {'argv': list(map(str, argv)), 'exit_code': result.returncode,
            'stdout': result.stdout[:24000], 'stderr': result.stderr[:8000]}


def identity(path):
    path = Path(path)
    return {'path': str(path), 'resolved': str(path.resolve()),
            'bytes': path.stat().st_size, 'sha256': digest(path)}


def inspect_object(path):
    data = Path(path).read_bytes()
    if len(data) < 32:
        raise ValueError('Truncated Mach-O object')
    magic, cpu, subtype, filetype, ncmds, size, flags, reserved = struct.unpack_from('<8I', data)
    if (magic, cpu, filetype) != (0xfeedfacf, 0x100000c, 1):
        raise ValueError('Expected an ARM64 Mach-O object')
    if 32 + size > len(data):
        raise ValueError('Truncated load commands')
    offset, builds = 32, []
    for _ in range(ncmds):
        if offset + 8 > 32 + size:
            raise ValueError('Load command outside declared region')
        cmd, length = struct.unpack_from('<II', data, offset)
        if length < 8 or offset + length > 32 + size:
            raise ValueError('Invalid load command size')
        if cmd == 0x32:
            if length < 24:
                raise ValueError('Truncated build version')
            builds.append(dict(zip(('platform', 'minimum_os', 'sdk', 'tools'),
                                   struct.unpack_from('<4I', data, offset + 8))))
        offset += length
    if offset != 32 + size or len(builds) != 1:
        raise ValueError('Expected exactly one build version')
    if builds[0]['platform'] != 2 or builds[0]['minimum_os'] != 17 << 16:
        raise ValueError('Expected iOS 17 deployment target')
    return {'cpu_type': cpu, 'cpu_subtype': subtype, 'file_type': filetype,
            'build_version': builds[0], 'sha256': digest(path)}


def run(profile, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    errors, probes, tools, sdks = [], {}, {}, {}
    def probe(name, argv):
        try:
            record = command(argv)
        except (OSError, subprocess.TimeoutExpired) as error:
            record = {'argv': list(map(str, argv)), 'error': str(error), 'exit_code': None}
        probes[name] = record
        return record.get('stdout', '').strip() if record.get('exit_code') == 0 else None
    if platform.system() != profile['system']:
        errors.append('Host must be Darwin')
    if platform.machine() != profile['machine']:
        errors.append('Host must be arm64')
    if sys.version_info[:3] < tuple(profile['python_minimum']):
        errors.append('Python version below selected minimum')
    if os.environ.get('DEVELOPER_DIR') != profile['developer_dir']:
        errors.append('DEVELOPER_DIR differs from selected Xcode')
    xcode = probe('xcode', ['xcodebuild', '-version'])
    if xcode != 'Xcode ' + profile['xcode_version'] + '\nBuild version ' + profile['xcode_build']:
        errors.append('Xcode version or build mismatch')
    probe('sw_vers', ['sw_vers'])
    probe('xcode_select', ['xcode-select', '-p'])
    for name, argv in {
        'git': ['git', '--version'], 'bash': ['bash', '--version'],
        'make': ['make', '--version'], 'cmake': ['cmake', '--version'],
        'ninja': ['ninja', '--version'], 'unifdef': ['unifdef', '-V'],
        'openssl': ['openssl', 'version'],
    }.items():
        path = shutil.which(argv[0])
        if not path:
            errors.append('Missing tool: ' + name)
            continue
        tools[name] = identity(path)
        if probe(name, argv) is None:
            errors.append('Tool probe failed: ' + name)
    tools['python'] = identity(sys.executable)
    for sdk in ('iphoneos', 'macosx'):
        version = probe(sdk + '_version', ['xcrun', '--sdk', sdk, '--show-sdk-version'])
        path = probe(sdk + '_path', ['xcrun', '--sdk', sdk, '--show-sdk-path'])
        build = probe(sdk + '_build', ['xcrun', '--sdk', sdk, '--show-sdk-build-version'])
        if version != profile[sdk + '_sdk'] or not path or not build:
            errors.append('SDK version/path/build mismatch: ' + sdk)
            continue
        files = []
        for rel in ('SDKSettings.json', 'usr/include/mach/mach.h', 'usr/include/sys/types.h'):
            item = Path(path) / rel
            if item.is_file():
                files.append(identity(item))
            else:
                errors.append('Missing SDK input: ' + sdk + '/' + rel)
        sdks[sdk] = {'version': version, 'build': build, 'path': path, 'files': files}
    for name in ('clang', 'clang++', 'ld', 'ar', 'nm', 'otool', 'libtool', 'mig', 'dsymutil'):
        path = probe('find_' + name, ['xcrun', '--sdk', 'iphoneos', '--find', name])
        if path and Path(path).is_file():
            tools['xcrun_' + name] = identity(path)
        else:
            errors.append('Missing selected Xcode tool: ' + name)
    probe('clang_version', ['xcrun', '--sdk', 'iphoneos', 'clang', '--version'])
    obj = None
    # The negative Linux probe records every missing input but never tries to fake target compilation.
    if not errors:
        source = output / 'target-probe.c'
        source.write_text('#include <mach/mach.h>\n#include <sys/types.h>\n'
                          'unsigned long cas_probe(void) { return sizeof(vm_address_t); }\n')
        destination = output / 'target-probe.o'
        argv = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', profile['target'],
                '-isysroot', sdks['iphoneos']['path'], '-Wall', '-Werror', '-c',
                str(source), '-o', str(destination)]
        if probe('target_compile', argv) is None:
            errors.append('Target object compilation failed')
        else:
            try:
                obj = inspect_object(destination)
            except ValueError as error:
                errors.append(str(error))
        probe('target_otool', ['xcrun', 'otool', '-hv', str(destination)])
    report = {'schema_version': 1, 'kind': 'cas_xnu_executor_preflight',
              'observed_at': dt.datetime.now(dt.timezone.utc).isoformat(),
              'result': 'fail' if errors else 'pass', 'errors': errors, 'profile': profile,
              'host': {'system': platform.system(), 'machine': platform.machine(),
                       'python': platform.python_version()},
              'runner': {key: os.environ.get(key) for key in
                         ('ImageOS', 'ImageVersion', 'RUNNER_OS', 'RUNNER_ARCH',
                          'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')},
              'tools': tools, 'sdks': sdks, 'probes': probes, 'target_object': obj,
              'xnu_build': 'not_run', 'xnu_runtime': 'not_run', 'cas_device': 'not_run'}
    (output / 'toolchain.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = run(json.loads(args.profile.read_text()), args.out)
    print(json.dumps({'result': report['result'], 'errors': report['errors'],
                      'report': str(args.out / 'toolchain.json')}))
    sys.exit(report['result'] != 'pass')
