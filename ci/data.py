"""Build a fresh iPhone Data seed on macOS from Apple's pinned public OS.

Local Apple inputs, phone data and private keys are never build inputs.
Only the packed seed and explicit diagnostics enter the existing CMS envelope.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
from types import SimpleNamespace

import artifacts
import packed_ramdisk
from unixnu.ipsw import Ipsw

ROOT = Path(__file__).resolve().parent
PINS = json.loads((ROOT / 'inputs.json').read_text())
TOOL_URL = ('https://github.com/blacktop/ipsw/releases/download/v3.1.741/'
            'ipsw_3.1.741_macOS_universal.tar.gz')
TOOL_SHA = 'c9db538d44babab6873cb0e2a590ca5b497bf791623ad20c48de2942e3cae789'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(*args):
    return subprocess.check_output(list(map(str, args)))


def install_tool(work):
    target = work / 'tool'
    target.mkdir()
    archive = target / 'ipsw.tar.gz'
    with urllib.request.urlopen(TOOL_URL, timeout=120) as source, archive.open('xb') as out:
        shutil.copyfileobj(source, out)
    assert sha(archive) == TOOL_SHA
    with tarfile.open(archive, 'r:gz') as tar:
        members = [m for m in tar.getmembers() if Path(m.name).name == 'ipsw' and m.isfile()]
        assert len(members) == 1
        with tar.extractfile(members[0]) as source, (target / 'ipsw').open('xb') as out:
            shutil.copyfileobj(source, out)
    (target / 'ipsw').chmod(0o755)
    os.environ['PATH'] = str(target) + os.pathsep + os.environ['PATH']
    version = run(target / 'ipsw', 'version').decode().strip()
    assert '3.1.741' in version
    return dict(url=TOOL_URL, archive_sha256=TOOL_SHA, version=version)


def attach(image, mountpoint, readonly):
    mountpoint.mkdir()
    command = ['hdiutil', 'attach', '-nobrowse', '-owners', 'on', '-mountpoint',
               str(mountpoint), '-plist']
    if readonly:
        command.append('-readonly')
    command.append(str(image))
    result = plistlib.loads(run(*command))
    entities = result['system-entities']
    devices = [e['dev-entry'] for e in entities if 'dev-entry' in e]
    mounted = [e for e in entities if e.get('mount-point') == str(mountpoint)]
    assert devices and len(mounted) == 1
    info = plistlib.loads(run('diskutil', 'info', '-plist', mounted[0]['dev-entry']))
    return devices[0], info


def metadata(path, st, symlink=False):
    os.chown(path, st.st_uid, st.st_gid, follow_symlinks=not symlink)
    if not symlink:
        os.chmod(path, stat.S_IMODE(st.st_mode))
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=not symlink)


def populate(source, target):
    records, dirs_to_finish = [], []
    counts = dict(directories=0, files=0, symlinks=0, regular_bytes=0)
    for current, dirs, files in os.walk(source, followlinks=False):
        if Path(current) == source:
            dirs[:] = [n for n in dirs if n != 'staged_system_apps']
        relative = Path(current).relative_to(source)
        dest = target / relative
        dest.mkdir(exist_ok=True)
        st = Path(current).stat()
        dirs_to_finish.append((dest, st))
        counts['directories'] += 1
        records.append(dict(path=str(relative), kind='directory', mode=stat.S_IMODE(st.st_mode),
                            uid=st.st_uid, gid=st.st_gid))
        for name in list(dirs) + files:
            src, dst = Path(current) / name, dest / name
            st = src.lstat()
            if stat.S_ISLNK(st.st_mode):
                os.symlink(os.readlink(src), dst)
                metadata(dst, st, True)
                if name in dirs:
                    dirs.remove(name)
                counts['symlinks'] += 1
                records.append(dict(path=str(dst.relative_to(target)), kind='symlink',
                                    target=os.readlink(src), uid=st.st_uid, gid=st.st_gid))
            elif stat.S_ISREG(st.st_mode):
                # Read decompressed contents; do not attach an old decmpfs xattr
                # to an ordinary newly written mutable file.
                shutil.copyfile(src, dst)
                metadata(dst, st)
                digest = sha(src)
                assert sha(dst) == digest
                counts['files'] += 1
                counts['regular_bytes'] += st.st_size
                records.append(dict(path=str(dst.relative_to(target)), kind='file', bytes=st.st_size,
                                    sha256=digest, mode=stat.S_IMODE(st.st_mode), uid=st.st_uid, gid=st.st_gid))
            elif not stat.S_ISDIR(st.st_mode):
                raise ValueError('Unexpected original var entry type')
    assert counts == dict(directories=PINS['original_var_directories'], files=PINS['original_var_files'],
                          symlinks=PINS['original_var_symlinks'], regular_bytes=PINS['original_var_bytes']), counts
    for dest, st in reversed(dirs_to_finish):
        metadata(dest, st)
    for name, mode, uid, gid in [('keybags', 0o755, 0, 0), ('mobile/tmp', 0o1777, 501, 501),
                                 ('tmp', 0o1777, 0, 0), ('run', 0o775, 0, 1)]:
        dest = target / name
        if not dest.exists():
            dest.mkdir(parents=True)
            os.chown(dest, uid, gid)
            os.chmod(dest, mode)
    os.chmod(target / 'tmp', 0o1777)
    return counts, records


def build(work):
    assert sys.platform == 'darwin' and os.geteuid() == 0
    work.mkdir(parents=True, exist_ok=True)
    assert shutil.disk_usage(work).free > 21 * 1024**3, 'Insufficient runner space for pinned public OS'
    report = dict(status='running', platform=platform.platform(), local_Apple_inputs_uploaded=False,
                  phone_data_uploaded=False, private_key_uploaded=False, public_Apple_source=PINS['apple_url'])
    state = work / 'build-state.json'
    state.write_text(json.dumps(report, indent=2))
    try:
        report['tool'] = install_tool(work)
        config = dict(product=PINS['product'], version=PINS['version'], build=PINS['build'],
                      device_class=PINS['device_class'], chip_id=PINS['chip_id'], board_id=PINS['board_id'],
                      sha256={PINS['OS_member']: PINS['OS_aea_sha256']})
        context = SimpleNamespace(board={'ipsw': config}, out_root=work)
        source = Ipsw(context, PINS['apple_url'])
        manifest = source.zip.read('BuildManifest.plist')
        assert hashlib.sha256(manifest).hexdigest() == PINS['manifest_sha256']
        assert source.member('OS') == PINS['OS_member']
        image = source.disk_image('OS')
        assert sha(image) == PINS['OS_image_sha256']
        os_device = data_device = None
        try:
            os_device, os_info = attach(image, work / 'original-OS', True)
            assert os_info['VolumeUUID'].upper() == '28E4CF35-9C12-417E-B8FF-906618C6FD22'
            assert sha(work / 'original-OS/sbin/launchd') == PINS['launchd_sha256']
            seed = work / 'data4096.cdr'
            run('hdiutil', 'create', '-size', str(PINS['new_Data_MiB']) + 'm', '-layout', 'NONE',
                '-fs', 'APFS', '-volname', 'Nezha27Data', '-type', 'UDTO', seed)
            data_device, info = attach(seed, work / 'new-Data', False)
            uuid = info['VolumeUUID'].upper()
            assert uuid not in ('28E4CF35-9C12-417E-B8FF-906618C6FD22', 'FFADDA0D-7102-4121-8AB8-48818B912D06')
            counts, records = populate(work / 'original-OS/private/var', work / 'new-Data')
            (work / 'original-var-inventory.json').write_text(json.dumps(records, indent=2))
            import setup_policy
            setup=setup_policy.apply_to_mount(ROOT/'seed-data-skip-setup.py',work/'new-Data',PINS['build'],owner=(501,501))
            (work/'setup-policy.json').write_text(json.dumps(setup,indent=2))
            report['setup_policy']=setup
            run('sync')
        finally:
            if data_device:
                run('hdiutil', 'detach', data_device)
            if os_device:
                run('hdiutil', 'detach', os_device)
        assert seed.stat().st_size == PINS['new_Data_MiB'] * 1024**2
        with seed.open('rb') as stream:
            assert stream.read(4096)[32:36] == b'NXSB'
        packed = work / 'data4096.dbr'
        packed_ramdisk.pack(seed, packed)
        identity, header = packed_ramdisk.inspect(packed)
        assert identity['sha256'] == packed_ramdisk.seed_identity(*packed_ramdisk.regions(packed.read_bytes())) and header[32:36] == b'NXSB'
        report.update(status='complete', volume_uuid=uuid, counts=counts, seed=identity,
                      original_OS_sha256=PINS['OS_image_sha256'], manifest_sha256=PINS['manifest_sha256'],
                      fresh_independent_Data=True, allocation_bytes=seed.stat().st_size, sparse_seed_identity_scheme='SHNGSEED2 (>2GiB); not the plain full-file SHA', original_plain_image_sha256=sha(seed), owners_and_modes_preserved=True, initial_files_decompressed=True,
                      runtime_writable=True, hardware_qualified=False, phone_operations=False)
        (work / 'seed.json').write_text(json.dumps(report, indent=2))
        print('Fresh independent iPhone27 Data seed complete; runtime qualification remains separate', flush=True)
    except BaseException as exc:
        report.update(status='failed', error=str(exc))
        raise
    finally:
        state.write_text(json.dumps(report, indent=2))


def seal(work, output):
    names = ['build.log']
    for name in ['build-state.json', 'seed.json', 'original-var-inventory.json', 'setup-policy.json']:
        if (work / name).is_file():
            names.append(name)
    if (work / 'seed.json').is_file() and json.loads((work / 'seed.json').read_text())['status'] == 'complete':
        names.append('data4096.dbr')
    artifacts.seal(work, names, ROOT / 'artifact-recipient.pem', output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['build', 'seal'])
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--sealed', type=Path)
    args = parser.parse_args()
    if args.action == 'build':
        build(args.work)
    else:
        seal(args.work, args.sealed)
