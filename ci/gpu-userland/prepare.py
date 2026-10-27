#!/usr/bin/env python3
"""Prepare the pinned inputs of the UniXNU GPU userland build in a UniXNU checkout (macOS runner or a Mac).

  prepare.py --unixnu DIR --record FILE

1. Checks the UniXNU commit, the component submodules this build reads (drivers/Adreno740, drivers/AdrenoMetal,
   drivers/GopFramebuffer, kits/AdrenoPM4, kits/ArmSmmu4K) against the gitlinks of that commit, and the sha256 of
   drivers/Adreno740/NativeAdrenoABI.h. Links drivers/NativeMobileDisplay to drivers/GopFramebuffer as the assembled target tree
   does (components.toml legacy_name): the plugin's GpuScanoutClient.h includes ../NativeMobileDisplay/NativeGOPWire.h.
2. .cache/mesa-work: the Mesa commit and sparse directories of scripts/data/adreno-compiler-inputs.json (commit and tree checked),
   with the recorded patches applied in their listed order.
3. .cache/gpu-reference/metal2vulkan: the translator commit of scripts/data/gpu-metal-vulkan-inputs.json (pinned file hashes
   checked). build-adreno-metal-bridge.py applies its own recorded patches.
4. .cache/mesa-tools: bison from the pinned release tarball (size and sha256 checked); .cache/mesa-venv: meson, Mako, PyYAML.
5. The recipe patch of ci/gpu-userland/patches: 0001 on the UniXNU checkout (no A740 replay in the plugin). nezha (Adreno 840)
   needs nothing else here: ae68700's ABI already has 128 IBs (the nezha kernel's layout, 3104 bytes) and Turnip's darwin
   backend already waits in 100 ms slices; Mesa 85fda3b has the A840 (chip 0x44050a31) entry.
6. scripts/check-quartzcore-turnip-cache-host.py: the stand-in of ci/gpu-userland when the checkout has none (that check belongs
   to a device repository and needs the original QuartzCore library).
Writes every identity to FILE (JSON)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

HERE = Path(__file__).resolve().parent
EXPECT = {
    'unixnu': os.environ.get('UNIXNU_COMMIT', 'ae687009348296e97e49ce6e925795044f363ce6'),
    'adreno740': os.environ.get('ADRENO740_COMMIT', '5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1'),
    'abi_sha256': os.environ.get('ABI_SHA256', '20ac9298bb390d86788c1fc58155e1f9581ffa942e222752efc95c67cb5670d2'),
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
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    args = parser.parse_args()
    root = args.unixnu.resolve()
    record = dict(expect=EXPECT)

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
    # Exact delta ce5e19e..3e3fcc4 from the locally reviewed nezha Metal branch.
    patch = HERE / 'patches/0002-nezha-metal-006.patch'
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', '--check', patch])
    must(['git', '-C', root / 'drivers/AdrenoMetal', 'apply', patch])
    record['nezha_metal_patch'] = dict(source_base=modules['drivers/AdrenoMetal'],
                                       local_source_commit='3e3fcc4', sha256=sha(patch))
    alias = root / 'drivers/NativeMobileDisplay'
    if not alias.exists():
        alias.symlink_to('GopFramebuffer', target_is_directory=True)
    wire = root / 'drivers/GopFramebuffer/NativeGOPWire.h'
    record['display_wire'] = dict(path='drivers/GopFramebuffer/NativeGOPWire.h', sha256=sha(wire),
                                  gop_framebuffer_commit=modules['drivers/GopFramebuffer'], alias='drivers/NativeMobileDisplay')

    # 2. Mesa at the pin with the recorded patches.
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

    # 5. The recipe patches.
    applied = {}
    for patch, where in [('0001-plugin-turnip-replay-switch.patch', root)]:
        must(['git', '-C', where, 'apply', HERE / 'patches' / patch])
        applied[patch] = sha(HERE / 'patches' / patch)
    patch = HERE / 'patches/0003-register-runtime-render-patch.patch'
    must(['git', '-C', root, 'apply', patch])
    applied[patch.name] = sha(patch)
    record['recipe_patches'] = applied

    # 6. The device repository check this runner cannot run.
    stand_in = root / 'scripts/check-quartzcore-turnip-cache-host.py'
    record['quartzcore_check'] = 'upstream' if stand_in.exists() else 'stand-in'
    if not stand_in.exists():
        shutil.copyfile(HERE / 'check-quartzcore-turnip-cache-host.py', stand_in)

    args.record.parent.mkdir(parents=True, exist_ok=True)
    args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
