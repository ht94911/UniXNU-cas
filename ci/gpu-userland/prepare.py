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
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

HERE = Path(__file__).resolve().parent
EXPECT = {
    'unixnu': os.environ.get('UNIXNU_COMMIT', 'a3faa4a43cadcd288fbe9cee3794d5a120629742'),
    'adreno740': os.environ.get('ADRENO740_COMMIT', '1030485546ef7ebb9abbfb7c17fbca93cca40971'),
    'abi_sha256': os.environ.get('ABI_SHA256', 'd1c8a27d6e61787073c2c56a779ff4d5c43d86f2d23ac0c0fba0368af8dcdeae'),
}
SUBMODULES = ['drivers/Adreno740', 'drivers/AdrenoMetal', 'drivers/GopFramebuffer', 'kits/AdrenoPM4', 'kits/ArmSmmu4K']
MESA_MIRRORS = ['https://gitlab.freedesktop.org/mesa/mesa.git',
                'https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa']
BISON_MIRRORS = ['https://ftpmirror.gnu.org/gnu/bison/', 'https://mirrors.kernel.org/gnu/bison/']


def run(cmd, cwd=None, retries=1, quiet=False):
    for attempt in range(retries):
        result = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=subprocess.DEVNULL if quiet else None)
        if result.returncode == 0:
            return True
        if attempt + 1 < retries:
            time.sleep(15 * (attempt + 1))
    return False


def must(cmd, cwd=None, retries=1, quiet=False):
    if not run(cmd, cwd, retries, quiet):
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
    """Shallow fetch of exactly `commit` into a fresh repository at dest (first URL that serves it), then a detached checkout."""
    dest.mkdir(parents=True)
    must(['git', 'init', '-q', dest])
    if sparse:
        must(['git', '-C', dest, 'sparse-checkout', 'init', '--cone'])
        must(['git', '-C', dest, 'sparse-checkout', 'set', *sparse])
    for url in urls:
        run(['git', '-C', dest, 'remote', 'remove', 'origin'], quiet=True)
        must(['git', '-C', dest, 'remote', 'add', 'origin', url])
        if run(['git', '-C', dest, 'fetch', '-q', '--depth', '1', *(['--filter=blob:none'] if sparse else []), 'origin', commit], retries=3):
            must(['git', '-C', dest, 'checkout', '-q', 'FETCH_HEAD'])
            if out(['git', '-C', dest, 'rev-parse', 'HEAD']) != commit:
                sys.exit(f'{dest}: HEAD is not {commit}')
            return url
    sys.exit(f'{dest}: no mirror served {commit}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--unixnu', type=Path)
    parser.add_argument('--record', type=Path)
    parser.add_argument('--check-sources-only', action='store_true')
    args = parser.parse_args()
    ci_root = HERE.parents[1]
    declared = verify_ci_sources(ci_root)
    if args.check_sources_only:
        print(json.dumps({'passed': True, 'tracked_inputs': len(declared),
                          'network_access': False, 'phone_operations': False}))
        return
    if args.unixnu is None or args.record is None:
        parser.error('--unixnu and --record are required unless --check-sources-only')
    root = args.unixnu.resolve()
    record = dict(expect=EXPECT)
    record['ci_source_sha256'] = declared

    # 1. UniXNU, its submodules and the user-client ABI header.
    head = out(['git', '-C', root, 'rev-parse', 'HEAD'])
    if head != EXPECT['unixnu']:
        sys.exit(f'UniXNU checkout is {head}, expected {EXPECT["unixnu"]}')
    must(['git', '-C', root, 'submodule', 'update', '--init', '--depth', '1', *SUBMODULES], retries=3)
    modules = {}
    for path in SUBMODULES:
        pinned = out(['git', '-C', root, 'ls-tree', 'HEAD', path]).split()[2]
        actual = out(['git', '-C', root / path, 'rev-parse', 'HEAD'])
        if pinned != actual:
            sys.exit(f'{path} is {actual}, UniXNU pins {pinned}')
        modules[path] = actual
    if modules['drivers/Adreno740'] != EXPECT['adreno740']:
        sys.exit(f'drivers/Adreno740 is {modules["drivers/Adreno740"]}, expected {EXPECT["adreno740"]}')
    abi = root / 'drivers/Adreno740/NativeAdrenoABI.h'
    if sha(abi) != EXPECT['abi_sha256']:
        sys.exit(f'NativeAdrenoABI.h sha256 {sha(abi)} differs from {EXPECT["abi_sha256"]}')
    record.update(unixnu=head, submodules=modules,
                  abi=dict(path='drivers/Adreno740/NativeAdrenoABI.h', sha256=sha(abi), adreno740_commit=modules['drivers/Adreno740']))
    record['metal_source_policy'] = 'Current pinned upstream; no old17 frontend patch applied.'
    host_context = HERE / 'patches/intel-signal-context.patch'
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', '--check', host_context])
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', host_context])
    record['host_signal_context_patch'] = sha(host_context)
    record['metal_source_policy'] = 'Pinned upstream plus Intel signal-context diagnostics; actual ARM signal-handler equality and both iOS target compilations are required. ENCODE source-line constants are not claimed byte-identical.'
    alias = root / 'drivers/NativeMobileDisplay'
    if not alias.exists():
        alias.symlink_to('GopFramebuffer', target_is_directory=True)
    wire = root / 'drivers/GopFramebuffer/NativeGOPWire.h'
    record['display_wire'] = dict(path='drivers/GopFramebuffer/NativeGOPWire.h', sha256=sha(wire),
                                  gop_framebuffer_commit=modules['drivers/GopFramebuffer'], alias='drivers/NativeMobileDisplay')

    # 2. Mesa at the pin with the recorded patches.
    # Explicit new-file metadata makes this upstream patch portable to the
    # Intel runner's git. The added header and all executable changes are
    # unchanged; the existing full reverse reconstruction stays mandatory.
    metadata_patch = HERE / 'patches/mesa-new-file-mode.patch'
    must(['git', '-C', root, 'apply', '--check', metadata_patch])
    must(['git', '-C', root, 'apply', metadata_patch])
    record['mesa_patch_metadata_fix'] = sha(metadata_patch)
    context_recipe = HERE / 'patches/device-context-recipes.patch'
    must(['git', '-C', root, 'apply', '--check', context_recipe])
    must(['git', '-C', root, 'apply', context_recipe])
    record['device_context_recipe_patch'] = sha(context_recipe)
    upstream_recipe = HERE / 'patches/upstream-gpu-recipes.patch'
    must(['git', '-C', root, 'apply', '--check', upstream_recipe])
    must(['git', '-C', root, 'apply', upstream_recipe])
    record['reviewed_upstream_GPU_recipe_patch'] = sha(upstream_recipe)
    for repository, name in [(root, 'atomic-submit-recipes.patch'),
                             (root / 'drivers/Adreno740', 'atomic-submit-driver.patch')]:
        selected = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', selected])
        must(['git', '-C', repository, 'apply', selected])
        record.setdefault('atomic_submit_patches', {})[name] = sha(selected)
    assert sha(abi) == '24b6d71df96a4849137ad6869c0bceea7e87602066df37b12c57f0cfa17c87a7', 'Atomic submission wire header differs'
    driver = root / 'drivers/Adreno740'
    with tempfile.TemporaryDirectory(prefix='atomic-driver-') as temporary:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / 'index'))
        def index(*words):
            return subprocess.check_output(['git', '-C', str(driver), *words], env=env, text=True).strip()
        index('read-tree', 'HEAD')
        index('add', '-A')
        assert index('write-tree') == '0e46e88101d3c24bd91abcc479a3eb8eee775af8', 'Atomic driver tree differs'
    record['abi']['upstream_sha256'] = record['abi']['sha256']
    record['abi']['sha256'] = sha(abi)
    record['atomic_submit'] = dict(driver_revision='e0e559acdaaaf97d46903ea80904c6ea6eb541d8', driver_tree='0e46e88101d3c24bd91abcc479a3eb8eee775af8',
        max_buffers=1024, wire_bytes=3104, actual_KMD_sha256='04591f5a3711d4ea68568ea3f7e993f2f57f2070de14e40d63d223c8c570a65e', hardware_verified=False)
    pins = json.loads((root / 'scripts/data/adreno-compiler-inputs.json').read_text())
    mesa = root / '.cache/mesa-work'
    url = fetch_commit(mesa, MESA_MIRRORS, pins['mesa']['commit'], pins['mesa']['sparse_directories'])
    tree = out(['git', '-C', mesa, 'rev-parse', 'HEAD^{tree}'])
    if tree != pins['mesa']['tree']:
        sys.exit(f'Mesa tree {tree} differs from the pin {pins["mesa"]["tree"]}')
    for patch in pins['patches']:
        must(['git', '-C', mesa, 'apply', '--whitespace=nowarn', root / patch])
    sys.path.insert(0, str(root / 'scripts'))
    from mesa_patch_identity import validate_recorded_patches
    validate_recorded_patches(root, mesa, pins)
    assert sha(mesa / 'src/freedreno/vulkan/tu_knl_darwin.cc') == '04591f5a3711d4ea68568ea3f7e993f2f57f2070de14e40d63d223c8c570a65e', 'Compiled KMD source differs'
    record['mesa'] = dict(url=url, commit=pins['mesa']['commit'], tree=tree,
                          patches={p: sha(root / p) for p in pins['patches']})

    # 3. metal2vulkan at the pin.
    translator = json.loads((root / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
    source = root / '.cache/gpu-reference/metal2vulkan'
    fetch_commit(source, [translator['repository']], translator['commit'])
    for name, digest in translator['files'].items():
        if sha(source / name) != digest:
            sys.exit(f'metal2vulkan {name} differs from its pinned hash')
    bridge_patches = sorted((root / 'drivers/AdrenoMetal/bridge/patches').glob('*.patch'))
    record['metal2vulkan'] = dict(repository=translator['repository'], commit=translator['commit'],
                                  bridge_patches={p.name: sha(p) for p in bridge_patches})

    # 4. bison and the Python environment of build-turnip-darwin.py.
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

    # One reviewed host-architecture adaptation; iOS27 remains arm64e.
    patch = HERE / 'patches/intel-host-bridge.patch'
    must(['git', '-C', root, 'apply', '--check', patch])
    must(['git', '-C', root, 'apply', patch])
    record['recipe_patches'] = {patch.name: sha(patch)}
    for repository, name in [(root, 'full-iphone-recipes.patch'),
                             (root / 'drivers/AdrenoMetal', 'full-iphone-plugin.patch'),
                             (root / 'drivers/AdrenoMetal', 'device-context-plugin.patch')]:
        selected = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', selected])
        must(['git', '-C', repository, 'apply', selected])
        record.setdefault('full_chain_patches', {})[name] = sha(selected)
    selected = HERE / 'patches/upstream-gpu-plugin.patch'
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', '--check', selected])
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', selected])
    record['reviewed_upstream_GPU_plugin_patch'] = sha(selected)
    for repository, name in [(root, 'buffer-reserve-recipes.patch'),
                             (root / 'drivers/AdrenoMetal', 'buffer-reserve-plugin.patch')]:
        selected = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', selected])
        must(['git', '-C', repository, 'apply', selected])
        record.setdefault('buffer_reserve_patches', {})[name] = sha(selected)
    for repository, name in [(root, 'vulkan-operation-recipes.patch'),
                             (root / 'drivers/AdrenoMetal', 'vulkan-operation-plugin.patch')]:
        selected = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', selected])
        must(['git', '-C', repository, 'apply', selected])
        record.setdefault('vulkan_operation_patches', {})[name] = sha(selected)
    for repository, name in [(root, 'tiled-words-recipes.patch'),
                             (root / 'drivers/AdrenoMetal', 'tiled-words-plugin.patch')]:
        selected = HERE / 'patches' / name
        must(['git', '-C', repository, 'apply', '--check', selected])
        must(['git', '-C', repository, 'apply', selected])
        record.setdefault('tiled_word_patches', {})[name] = sha(selected)
    # The final bridge recipe applies these at plugin-build time. Record the
    # complete patch set after all frontend patches have been installed.
    bridge_patches = sorted((root / 'drivers/AdrenoMetal/bridge/patches').glob('*.patch'))
    record['metal2vulkan']['bridge_patches'] = {p.name: sha(p) for p in bridge_patches}
    record['buffer_reserve_policy'] = 'Reuse same-pool/type unused buffers; separate 64 MiB device budget; preserve address objects, pressure and teardown'
    record['iphone_private_layout_policy'] = 'Explicit --iphone-24a446 excludes all iPad raw QuartzCore layout access; public policy only'
    record['integrated_fulltest'] = 'One optional backboardd worker runs existing raw/Turnip/Metal checks alongside normal publication,without duplicate scanout'
    record['device_context_policy'] = 'Each VkDevice has its own connection,VA/cache/fence state;enumeration does not touch live ports;CS startup failures return before writes'
    record['fulltest_cache_policy'] = 'Reuse the writable published runtime directory and release the test frontend device owner reference'
    record['GMEM_policy'] = 'Reviewed upstream memory-side colour/lazy-transient fixes;thread-local colour checks and race-free cached boot mode;device and imported host pixels verified only on hardware'
    record['VA_teardown_policy'] = 'Remove both actual Mesa RB indices before freeing;actual-source real-RB and fake-Vulkan regression fixtures are mandatory'
    record['quartzcore_fixture'] = 'not supplied: real iPhone metallibs remain local'
    record['replay'] = 'disabled by the current upstream --no-replay option'
    record['target'] = 'arm64e-apple-ios27.0'

    args.record.parent.mkdir(parents=True, exist_ok=True)
    args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
