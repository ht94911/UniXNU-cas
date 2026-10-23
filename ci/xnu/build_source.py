"""Build one new source-only XNU candidate; preserve private results and symbols.

There is no call to prelink, IPSW extraction, payload, firmware or device tools.
Each output directory is new. Source and crypto inputs stay outside the repository.
"""
import argparse
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
OBJ = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(part)
    return h.hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args])


def acquire(path, url, commit, log):
    path.mkdir(parents=True, exist_ok=False)
    for command in (['git', '-C', path, 'init', '-q'],
                    ['git', '-C', path, 'remote', 'add', 'origin', url],
                    ['git', '-C', path, 'fetch', '--depth', '1', 'origin', commit],
                    ['git', '-C', path, 'checkout', '--detach', 'FETCH_HEAD']):
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    if git(path, 'rev-parse', 'HEAD').decode().strip() != commit or git(path, 'status', '--porcelain'):
        raise ValueError('Source identity or cleanliness mismatch: ' + str(path))
    return path


def export(path, destination):
    destination.mkdir(parents=True, exist_ok=True)
    archive = git(path, 'archive', 'HEAD')
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(destination, filter='data')


def tracked_manifest(path):
    files = []
    for raw in git(path, 'ls-files', '-s', '-z').split(b'\0'):
        if not raw:
            continue
        metadata, name = raw.split(b'\t', 1)
        mode, blob, stage = metadata.decode().split()
        if mode not in ('100644', '100755') or stage != '0':
            continue
        name = os.fsdecode(name)
        item = path / name
        data = item.read_bytes()
        actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if actual != blob:
            raise ValueError('Source blob mismatch: ' + name)
        files.append({'path': name, 'git_blob': blob, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)})
    return files


def select_kernel_patches(tree, output, spec):
    """Verify the complete baseline before excluding explicitly disabled features."""
    ordered = sorted(tree.glob('patches/xnu/*.patch')) + sorted(tree.glob('kits/NativePrelink/patches/*.patch'))
    actual = [(str(p.relative_to(tree)), digest(p)) for p in ordered]
    expected = [(i['path'], i['sha256']) for i in spec['kernel_patches']]
    if actual != expected:
        raise ValueError('Kernel patch list/order/hash differs from the fixed baseline')
    excluded = spec.get('excluded_kernel_patches', [])
    names = [i['path'] for i in excluded]
    if len(set(names)) != len(names) or not set(names) <= {p for p, _ in expected}:
        raise ValueError('Patch exclusions must be unique members of the verified baseline')
    if any(not i.get('reason', '').strip() for i in excluded):
        raise ValueError('A disabled feature needs an explicit reason')
    for item in excluded:
        source = tree / item['path']
        destination = output / 'excluded-kernel-patches' / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        source.rename(destination)
    return {'baseline': spec['kernel_patches'], 'excluded': excluded,
            'effective': [i for i in spec['kernel_patches'] if i['path'] not in names]}


def prepare_headers(tree, xnu, spec):
    """Restore/hash all fixed header blobs, including files already present."""
    records = []
    cache = tree / '.cache/sources'; cache.mkdir(parents=True, exist_ok=True)
    for entry in spec:
        commit = entry['commit']
        subprocess.run(['git', '-C', xnu, 'fetch', '--depth', '1', 'origin', commit], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
        for relative in entry['paths']:
            listing = git(xnu, 'ls-tree', '-r', '-z', commit, '--', relative)
            entries = []
            for raw in listing.split(b'\0'):
                if not raw:
                    continue
                metadata, name = raw.split(b'\t', 1)
                mode, kind, blob = metadata.decode().split()
                name = os.fsdecode(name)
                if kind != 'blob' or not name.endswith('.h'):
                    continue
                data = git(xnu, 'show', commit + ':' + name)
                target = tree / '.cache/xnu-work' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
                if actual != blob:
                    raise ValueError('Supplemental header blob mismatch')
                relative_name = name.removeprefix(relative + '/')
                if '/' in relative_name:
                    # Each of the three fixed paths has its own contents cache.
                    continue
                entries.append({'type': 'file', 'name': relative_name, 'sha': blob,
                                'download_url': target.as_uri()})
                records.append({'path': name, 'source_commit': commit, 'git_blob': blob,
                                'sha256': hashlib.sha256(data).hexdigest()})
            if relative.endswith('DriverKit/crypto'):
                cache_name = 'xnu-driverkit-crypto-' + commit
            elif relative.endswith('DriverKit'):
                cache_name = 'xnu-driverkit-' + commit
            else:
                cache_name = 'xnu-trustcache-' + commit
            (cache / (cache_name + '.json')).write_text(json.dumps(entries) + '\n')
    return records


def kernel_identity(path):
    data = path.read_bytes()
    if len(data) < 32 or struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 2):
        raise ValueError('Expected ordinary ARM64 MH_EXECUTE source kernel')
    offset, uuid, symbols, segments = 32, None, None, []
    end = 32 + struct.unpack_from('<I', data, 20)[0]
    if end > len(data):
        raise ValueError('Truncated kernel load commands')
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        if offset + 8 > end:
            raise ValueError('Load command outside declared command region')
        cmd, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > end:
            raise ValueError('Invalid kernel load command')
        if cmd == 0x1b:
            if size < 24:
                raise ValueError('Truncated kernel UUID')
            uuid = data[offset + 8:offset + 24].hex()
        elif cmd == 2:
            if size < 24 or symbols is not None:
                raise ValueError('Invalid or duplicate kernel symbol table')
            symbols = struct.unpack_from('<4I', data, offset + 8)
            symoff, count, stroff, strsize = symbols
            if symoff + count * 16 > len(data) or stroff + strsize > len(data):
                raise ValueError('Kernel symbol table extends beyond the file')
        elif cmd == 0x19:
            if size < 72:
                raise ValueError('Truncated kernel segment')
            name, va, length, fileoff, filesize = struct.unpack_from('<16s4Q', data, offset + 8)
            if fileoff + filesize > len(data):
                raise ValueError('Kernel segment extends beyond the file')
            segments.append({'name': name.rstrip(b'\0').decode(), 'vmaddr': hex(va),
                             'vmsize': length, 'fileoff': fileoff, 'filesize': filesize})
        offset += size
    if offset != end or not symbols or not symbols[1]:
        raise ValueError('Unstripped kernel must retain a real symbol table')
    return {'sha256': digest(path), 'bytes': len(data), 'cpu_type': 0x100000c,
            'cpu_subtype': 0, 'file_type': 2, 'uuid': uuid, 'symbol_count': symbols[1],
            'segments': segments}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--framework', type=Path)
    parser.add_argument('--crypto', type=Path)
    parser.add_argument('--toolchain-report', type=Path)
    parser.add_argument('--platform', choices=('cas', 'qemu'), default='cas')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    if not args.prepare_only and platform.system() != 'Darwin':
        parser.error('A real build requires the selected macOS executor')
    if not args.prepare_only and not args.toolchain_report:
        parser.error('A real build requires a fresh toolchain report')
    output = args.out.resolve(); output.mkdir(parents=True, exist_ok=False)
    exports = output / 'exports'; exports.mkdir()
    logs = output / 'logs'; logs.mkdir()
    spec = json.loads((HERE / 'inputs.json').read_text())
    tree = output / 'tree'
    report = {'schema_version': 1, 'result': 'in_progress', 'profile': args.platform,
              'started_at': dt.datetime.now(dt.timezone.utc).isoformat(), 'steps': [],
              'source_only': True, 'prelink': False, 'phone_operations': [],
              'crypto_scope': 'Controlled internal correctness/integration verification; no public raw/derived input distribution.',
              'xnu_runtime': 'not_run', 'cas_device': 'not_run'}
    environment = dict(os.environ, LC_ALL='C', LANG='C', JOBS=str(args.jobs))

    def run(name, command, overrides=None, required=True):
        print('step ' + name, flush=True)
        with (logs / (name + '.log')).open('w') as log:
            result = subprocess.run(command, cwd=tree, env={**environment, **(overrides or {})},
                                    stdout=log, stderr=subprocess.STDOUT)
        report['steps'].append({'name': name, 'argv': list(map(str, command)), 'exit_code': result.returncode})
        (exports / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        if required and result.returncode:
            raise RuntimeError(name + ' failed; inspect the encrypted log')
        return result.returncode

    try:
        if args.toolchain_report:
            actual = json.loads(args.toolchain_report.read_text())
            if actual['result'] != 'pass' or actual['errors'] or actual['profile'] != json.loads((HERE / 'toolchain.json').read_text()):
                raise ValueError('Toolchain qualification does not match the fixed profile')
            shutil.copyfile(args.toolchain_report, exports / 'toolchain.json')
        with (logs / 'acquire.log').open('w') as log:
            framework = args.framework.resolve() if args.framework else acquire(
                output / 'inputs/framework', spec['framework']['git'], spec['framework']['commit'], log)
            if git(framework, 'rev-parse', 'HEAD').decode().strip() != spec['framework']['commit'] or git(framework, 'status', '--porcelain'):
                raise ValueError('Framework must be clean at the fixed commit')
            export(framework, tree)
            for module in spec['modules']:
                path = framework / module['name']
                # An uninitialized submodule path is not a separate worktree.
                if path.exists() and git(path, 'rev-parse', '--show-toplevel').decode().strip() == str(path.resolve()):
                    if git(path, 'rev-parse', 'HEAD').decode().strip() != module['commit'] or git(path, 'status', '--porcelain'):
                        raise ValueError('Module mismatch: ' + module['name'])
                else:
                    path = acquire(output / 'inputs' / module['name'], module['git'], module['commit'], log)
                export(path, tree / module['name'])
            source_inputs = []
            for feed in spec['feeds']:
                target = tree / 'refer' / feed['name']
                if feed['name'] == 'corecrypto-2023' and args.crypto:
                    original = args.crypto.resolve()
                    if git(original, 'rev-parse', 'HEAD').decode().strip() != feed['commit'] or git(original, 'status', '--porcelain'):
                        raise ValueError('Explicit crypto input mismatch')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    subprocess.run(['git', 'clone', '--shared', original, target], stdout=log, stderr=subprocess.STDOUT, check=True)
                else:
                    acquire(target, feed['git'], feed['commit'], log)
                files = tracked_manifest(target) if feed['name'] == 'corecrypto-2023' else None
                source_inputs.append({'name': feed['name'], 'url': feed['git'], 'commit': feed['commit'],
                                      'tree': git(target, 'rev-parse', 'HEAD^{tree}').decode().strip(), 'files': files})
        selection = select_kernel_patches(tree, output, spec)
        (exports / 'patch-selection.json').write_text(json.dumps(selection, indent=2) + '\n')
        adaptations = []
        for patch in spec['adaptations']:
            source = HERE / patch['file']
            if digest(source) != patch['sha256']:
                raise ValueError('Candidate adaptation hash mismatch')
            destination = tree / patch['target']
            names = [line[6:] for line in source.read_text().splitlines() if line.startswith('+++ b/')]
            if not names or any(Path(name).is_absolute() or '..' in Path(name).parts for name in names):
                raise ValueError('Invalid adaptation target')
            before = {name: digest(destination / name) for name in names}
            subprocess.run(['git', 'apply', '--check', source], cwd=destination, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['git', 'apply', source], cwd=destination, check=True)
            after = {name: digest(destination / name) for name in names}
            adaptations.append({'patch': patch, 'changed': [{'path': p, 'before': before.get(p), 'after': after.get(p)}
                                                            for p in sorted(set(before) | set(after)) if before.get(p) != after.get(p)]})
        (exports / 'adaptations.json').write_text(json.dumps(adaptations, indent=2) + '\n')
        work = tree / '.cache/xnu-work'; work.parent.mkdir(exist_ok=True)
        subprocess.run(['git', 'clone', '--shared', tree / 'refer/xnu', work],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        headers = prepare_headers(tree, tree / 'refer/xnu', spec['supplemental_headers'])
        (exports / 'headers.json').write_text(json.dumps(headers, indent=2) + '\n')
        (exports / 'source-inputs.json').write_text(json.dumps(source_inputs, indent=2) + '\n')
        shutil.copyfile(HERE / 'build_arm_driver.py', tree / 'scripts/build-cas-arm-driver.py')
        enabled = ['SOURCE_CORECRYPTO14', 'SOURCE_PTHREAD', 'ARMCPU', 'NATIVE_ARCH_TIMER',
                   'DT_PANIC_REGION', 'NATIVE_PAN', 'ARM_DT_PLATFORM' if args.platform == 'cas' else 'ARMVIRT_PLATFORM']
        settings = {'XNU_' + name: '1' for name in enabled}
        settings.update(XNU_CRYPTO_BOOTSTRAP='source', UNIXNU_DRIVERKIT_TAG=spec['supplemental_headers'][0]['commit'],
                        UNIXNU_TRUSTCACHE_TAG=spec['supplemental_headers'][1]['commit'])
        environment.update(settings)
        (exports / 'configuration.json').write_text(json.dumps(settings, indent=2) + '\n')
        report['input_lock_sha256'] = digest(HERE / 'inputs.json')
        if args.prepare_only:
            report['result'] = 'prepared_only'; report['xnu_build'] = 'not_run'
            return 0
        run('bootstrap', ['bash', 'scripts/build-xnu-4k-baseline.sh'], {'XNU_BOOTSTRAP': '1'}, required=False)
        for path in ('osfmk/DEVELOPMENT/startup.o.json', 'osfmk/DEVELOPMENT/prng_random.o.json',
                     'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', 'link.filelist'):
            if not (tree / OBJ / path).is_file():
                raise ValueError('Bootstrap did not create required compile records: ' + path)
        run('crypto-plain', [sys.executable, 'scripts/build-corecrypto-14.py'])
        run('crypto-namespaced', [sys.executable, 'scripts/build-corecrypto-14.py', '--namespace'])
        run('pthread', [sys.executable, 'scripts/build-libpthread-kernel.py'])
        selected_platform = 'ArmDTPlatform' if args.platform == 'cas' else 'ArmVirtPlatform'
        for component in (selected_platform, 'ArmCpu'):
            run(component, [sys.executable, 'scripts/build-cas-arm-driver.py', component])
        run('kernel', ['bash', 'scripts/build-xnu-4k-baseline.sh'])
        kernel = tree / OBJ / 'mach.development.bcm2837.unstripped.noctf'
        report['kernel'] = kernel_identity(kernel)
        shutil.copyfile(kernel, exports / 'kernel.unstripped.macho')
        with (exports / 'kernel.symbols.txt').open('w') as symbols:
            subprocess.run(['xcrun', 'nm', '-nm', kernel], stdout=symbols, check=True)
        report['symbols'] = {'file': 'kernel.symbols.txt', 'sha256': digest(exports / 'kernel.symbols.txt'),
                             'source_kernel_sha256': report['kernel']['sha256'],
                             'unstripped_symbol_table_retained': True}
        for item in headers:
            if digest(work / item['path']) != item['sha256']:
                raise ValueError('Restored header changed during build')
        report['result'] = 'pass'
        report['xnu_build'] = 'source_kernel_linked'
        return 0
    except Exception as error:
        report['result'] = 'fail'; report['error'] = {'type': type(error).__name__, 'detail': str(error)}
        print('Build failed; the full log and report are retained for encrypted retrieval.', flush=True)
        return 1
    finally:
        report['finished_at'] = dt.datetime.now(dt.timezone.utc).isoformat()
        (exports / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        with tarfile.open(exports / 'logs.tar.gz', 'w:gz') as tar:
            tar.add(logs, arcname='logs')
            if tree.exists():
                for relative in ('osfmk/DEVELOPMENT/startup.o.json', 'osfmk/DEVELOPMENT/prng_random.o.json',
                                 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', 'link.filelist'):
                    path = tree / OBJ / relative
                    if path.is_file():
                        tar.add(path, arcname='details/xnu/' + relative)
                for pattern in ('build/*/compile-command*.json', 'build/*/report.json', 'build/*/build-report*.json',
                                'build/*/link-survey*.json', 'build/*/compile.log'):
                    for path in tree.glob(pattern):
                        tar.add(path, arcname='details/' + str(path.relative_to(tree)))
        print(json.dumps({'result': report['result'], 'report': str(exports / 'build-report.json')}), flush=True)


if __name__ == '__main__':
    sys.exit(main())
