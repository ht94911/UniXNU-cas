#!/usr/bin/env python3
"""Prepare the pinned inputs of the UniXNU GPU userland build in a UniXNU checkout (macOS runner or a Mac).

  prepare.py --unixnu DIR --record FILE

Reuses the established source/patch/tool bootstrap. All inputs are public source.
No stock OS or GPU firmware is fetched. Uses the current upstream iOS27 recipes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import urllib.parse

HERE = Path(__file__).resolve().parent
EXPECT = {
    'unixnu': os.environ.get('UNIXNU_COMMIT', 'e7bddb9a7eecdcf81f36da9fa38bd9d4d3f155c2'),
    'adreno740': os.environ.get('ADRENO740_COMMIT', '2962e289c9690aeb668072c170679e6fb28bf8de'),
    'abi_sha256': os.environ.get('ABI_SHA256', 'd1c8a27d6e61787073c2c56a779ff4d5c43d86f2d23ac0c0fba0368af8dcdeae'),
}
SUBMODULES = ['drivers/Adreno740', 'drivers/AdrenoMetal', 'drivers/GopFramebuffer', 'kits/AdrenoPM4', 'kits/ArmSmmu4K']
MESA_MIRRORS = ['https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa',
                'https://gitlab.freedesktop.org/mesa/mesa.git']
BISON_MIRRORS = ['https://ftpmirror.gnu.org/gnu/bison/', 'https://mirrors.kernel.org/gnu/bison/']


def progress(label):
    # The wrapper exposes only these source-operation labels. Full output stays
    # in the encrypted build log, including command diagnostics on failure.
    if os.environ.get('UNIXNU_PREPARE_PROGRESS_FD') == '3':
        os.write(3, ('[prepare] ' + label + '\n').encode())


def stop_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)


def run(cmd, cwd=None, retries=1, quiet=False, timeout=None, label=None):
    for attempt in range(retries):
        if label:
            progress(label + ' attempt ' + str(attempt + 1))
        words = [str(c) for c in cmd]
        if timeout is None:
            result = subprocess.run(words, cwd=cwd, stdout=subprocess.DEVNULL if quiet else None)
            code = result.returncode
        else:
            process = subprocess.Popen(words, cwd=cwd, stdout=subprocess.DEVNULL if quiet else None,
                                       start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                stop_group(process)
                progress((label or Path(words[0]).name) + ' timed out after ' + str(timeout) + ' seconds')
                code = 124
            except BaseException:
                stop_group(process)
                raise
        if code == 0:
            if label:
                progress(label + ' complete')
            return True
        if attempt + 1 < retries:
            time.sleep(15 * (attempt + 1))
    return False


def must(cmd, cwd=None, retries=1, quiet=False, timeout=None, label=None):
    if not run(cmd, cwd, retries, quiet, timeout, label):
        sys.exit('failed: ' + ' '.join(str(c) for c in cmd))


def out(cmd, cwd=None):
    return subprocess.check_output([str(c) for c in cmd], cwd=cwd, text=True).strip()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_ci_sources(ci_root):
    """Verify the complete tracked CI input set before downloads or builds."""
    declared = json.loads((ci_root / 'source-sha256.json').read_text())
    tracked = set(subprocess.check_output(
        ['git', '-C', str(ci_root), 'ls-files', '-z'], text=True).split('\0'))
    tracked.discard('')
    tracked.discard('source-sha256.json')
    if not tracked or set(declared) != tracked:
        raise ValueError('CI source manifest coverage differs: unlisted=' +
                         repr(sorted(tracked - set(declared))) + ', untracked=' +
                         repr(sorted(set(declared) - tracked)))
    mismatches = []
    for name, digest in declared.items():
        relative = PurePosixPath(name)
        path = ci_root / name
        if (relative.is_absolute() or '..' in relative.parts or
                '\\' in name or str(relative) != name or path.is_symlink() or
                not path.is_file() or not path.resolve().is_relative_to(ci_root.resolve())):
            raise ValueError('Unsafe or absent CI source: ' + name)
        if sha(path) != digest:
            mismatches.append(name)
    if mismatches:
        raise ValueError('CI source identity differs: ' + ', '.join(mismatches))
    return declared


def fetch_commit(dest, urls, commit, sparse=None):
    """Fetch one exact public source revision; reuse an unchanged existing source checkout."""
    dest = Path(dest)
    if (dest / '.git').exists():
        if out(['git', '-C', dest, 'rev-parse', 'HEAD']) == commit:
            return urls[0]
        raise ValueError('Existing source checkout has a different revision: ' + str(dest))
    dest.mkdir(parents=True, exist_ok=True)
    must(['git', 'init', '-q', dest])
    if sparse:
        must(['git', '-C', dest, 'sparse-checkout', 'init', '--cone'])
        must(['git', '-C', dest, 'sparse-checkout', 'set', *sparse])
    failures = []
    for url in urls:
        try:
            must(['git', '-c', 'http.lowSpeedLimit=1024', '-c', 'http.lowSpeedTime=45',
                  '-C', dest, 'fetch', '-q', '--depth', '1',
                  *(['--filter=blob:none'] if sparse else []), url, commit], timeout=300)
            must(['git', '-C', dest, 'checkout', '-q', '--detach', 'FETCH_HEAD'], timeout=300)
            if out(['git', '-C', dest, 'rev-parse', 'HEAD']) != commit:
                raise ValueError('Source revision mismatch')
            return url
        except Exception as error:
            failures.append(str(error))
    raise ValueError('No public mirror served the pinned source: ' + str(dest) + ' ' + '; '.join(failures))



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path)
    parser.add_argument('--record', type=Path)
    parser.add_argument('--check-sources-only', action='store_true')
    args = parser.parse_args()
    ci_root = HERE.parents[1]
    declared = verify_ci_sources(ci_root)
    if args.check_sources_only:
        print(json.dumps({'passed': True, 'tracked_inputs': len(declared), 'network_access': False, 'phone_operations': False}))
        return
    if args.unixnu is None or args.record is None:
        parser.error('--unixnu and --record are required')
    root = args.unixnu.resolve()
    if out(['git', '-C', root, 'rev-parse', 'HEAD']) != EXPECT['unixnu']:
        raise ValueError('framework base differs from frozen upstream revision')
    modules = {}
    selections = {
        'drivers/Adreno740': ('https://github.com/UniXNU/Adreno740.git', EXPECT['adreno740']),
        'drivers/AdrenoMetal': ('https://github.com/UniXNU/AdrenoMetal.git', 'b25e48e51dfdf4b3889ede6e701ab7dd59929a03'),
        'drivers/GopFramebuffer': ('https://github.com/UniXNU/GopFramebuffer.git', 'b0d812d578efa2acb005b66f73168891719af09a'),
    }
    for path in ['kits/AdrenoPM4', 'kits/ArmSmmu4K']:
        commit = out(['git', '-C', root, 'ls-tree', 'HEAD', path]).split()[2]
        selections[path] = ('https://github.com/UniXNU/' + path.split('/')[-1] + '.git', commit)
    for path, (url, commit) in selections.items():
        fetch_commit(root / path, [url], commit)
        modules[path] = out(['git', '-C', root / path, 'rev-parse', 'HEAD'])
        if modules[path] != commit:
            raise ValueError('component base mismatch: ' + path)
    abi = root / 'drivers/Adreno740/NativeAdrenoABI.h'
    if sha(abi) != EXPECT['abi_sha256']:
        raise ValueError('upstream ABI header mismatch')
    record = dict(expect=EXPECT, unixnu=EXPECT['unixnu'], submodules=modules, ci_source_sha256=declared)
    for repository, name in [(root, 'gpu088-core.patch'),
                             (root / 'drivers/Adreno740', 'gpu088-native-abi.patch'),
                             (root / 'drivers/AdrenoMetal', 'gpu088-metal.patch'),
                             (root / 'drivers/GopFramebuffer', 'gpu088-gop.patch')]:
        patch = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', patch])
        must(['git', '-C', repository, 'apply', patch])
        record.setdefault('cumulative_patches', {})[name] = sha(patch)
    if sha(abi) != '24b6d71df96a4849137ad6869c0bceea7e87602066df37b12c57f0cfa17c87a7':
        raise ValueError('paired local 1024IB ABI differs from Native066')
    record['abi'] = dict(path='drivers/Adreno740/NativeAdrenoABI.h', sha256=sha(abi), upstream_sha256=EXPECT['abi_sha256'], paired_native='Native066')
    alias = root / 'drivers/NativeMobileDisplay'
    if not alias.exists(): alias.symlink_to('GopFramebuffer', target_is_directory=True)
    record['display_wire'] = dict(path='drivers/GopFramebuffer/NativeGOPWire.h', sha256=sha(root / 'drivers/GopFramebuffer/NativeGOPWire.h'))
    record['display_timing_wire'] = dict(header_sha256=sha(root / 'drivers/GopFramebuffer/NativeGOPTiming.h'), hardware_verified=False)
    pins = json.loads((root / 'scripts/data/adreno-compiler-inputs.json').read_text())
    mesa = root / '.cache/mesa-work'
    url = fetch_commit(mesa, MESA_MIRRORS, pins['mesa']['commit'], pins['mesa']['sparse_directories'])
    tree = out(['git', '-C', mesa, 'rev-parse', 'HEAD^{tree}'])
    if tree != pins['mesa']['tree']: raise ValueError('Mesa base tree differs')
    for name in pins['patches']:
        must(['git', '-C', mesa, 'apply', '--whitespace=nowarn', root / name])
    sys.path.insert(0, str(root / 'scripts'))
    from mesa_patch_identity import validate_recorded_patches
    validate_recorded_patches(root, mesa, pins)
    record['mesa'] = dict(url=url, commit=pins['mesa']['commit'], tree=tree, patches={name: sha(root / name) for name in pins['patches']})
    record['driver_cpu_profile'] = dict(actual_KMD=sha(mesa / 'src/freedreno/vulkan/tu_knl_darwin.cc'))
    record['pipeline_cache_sharing'] = dict(source_sha256=sha(mesa / 'src/freedreno/vulkan/tu_pipeline.cc'))
    # 4. bison and the Python environment of build-turnip-darwin.py.
    progress('bison and Python build tools')
    bison = pins['bison']
    tools = root / '.cache/mesa-tools'
    with tempfile.TemporaryDirectory(prefix='bison-') as temporary:
        work = Path(temporary)
        tarball = work / Path(bison['url']).name
        for base in [bison['url'].rsplit('/', 1)[0] + '/', *BISON_MIRRORS]:
            try:
                with urllib.request.urlopen(base + tarball.name, timeout=120) as response:
                    data = response.read()
            except OSError as error:
                print('bison download failed from', base, error)
                continue
            if len(data) == bison['bytes'] and hashlib.sha256(data).hexdigest() == bison['sha256']:
                tarball.write_bytes(data)
                break
            print('bison tarball from', base, 'differs from the pin')
        else:
            sys.exit('No mirror served the pinned bison tarball')
        with tarfile.open(tarball) as archive:
            archive.extractall(work, filter='data')
        build = work / tarball.name.removesuffix('.tar.xz')
        must(['./configure', '--quiet', '--disable-nls', '--prefix=' + str(tools)], cwd=build)
        must(['make', '-s', '-j4'], cwd=build, quiet=True)
        must(['make', '-s', 'install'], cwd=build, quiet=True)
    bison_version = out([tools / 'bin/bison', '--version']).splitlines()[0]
    if bison['version'] not in bison_version:
        sys.exit('Built bison reports ' + bison_version)
    venv = root / '.cache/mesa-venv'
    must([sys.executable, '-m', 'venv', venv])
    must([venv / 'bin/pip', 'install', '--disable-pip-version-check', '-q', 'meson>=1.4,<2', 'mako>=1.3', 'pyyaml', 'packaging'],
         retries=3)
    record['tools'] = dict(bison=bison_version, bison_sha256=bison['sha256'],
                           python_packages=out([venv / 'bin/pip', 'freeze', '--disable-pip-version-check']).splitlines())

    translator = json.loads((root / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
    source = root / '.cache/gpu-reference/metal2vulkan'
    fetch_commit(source, [translator['repository']], translator['commit'])
    for name, digest in translator['files'].items():
        if sha(source / name) != digest: raise ValueError('runtime pin file differs: ' + name)
    bridge_patches = sorted((root / 'drivers/AdrenoMetal/bridge/patches').glob('*.patch'))
    if [p.name for p in bridge_patches] != ['0088-runtime-v2-local-retained.patch']:
        raise ValueError('runtime-v2 active cumulative patch set differs')
    record['metal2vulkan'] = dict(repository=translator['repository'], commit=translator['commit'], bridge_patches={p.name:sha(p) for p in bridge_patches})
    record['runtime_policy'] = 'complete upstream runtime-v2 plus retained resource/heap wait identity, iOS QoS, Mach running-time clock and foreground demand backing'
    record['iphone_private_layout_policy'] = 'iPhone24A446 guards retained; no iPad raw offset access'
    record['device_context_policy'] = 'per-VkDevice native connection and VA; atomic1024IB, outside-lock release and cache-miss retirement retained'
    record['replay'] = 'disabled'; record['target'] = 'arm64e-apple-ios27.0'
    args.record.parent.mkdir(parents=True, exist_ok=True)
    args.record.write_text(json.dumps(record, indent=2) + '\n')
    progress('complete upstream and local source inputs verified')

if __name__ == '__main__':
    main()
