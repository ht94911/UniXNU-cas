"""Seal explicitly listed files for their owner; never upload plaintext outputs.

Python standard library plus OpenSSL CMS (AES-256/RSA). Encryption is not a
license grant or a signature. Verify the downloaded envelope against its exact
GitHub run/artifact identity before using it. Keep the private key locally.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(part)
    return h.hexdigest()


def safe_name(name):
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise ValueError('Invalid artifact name')
    p = PurePosixPath(name)
    if not p.parts or p.is_absolute() or '..' in p.parts or str(p) != name or name == 'manifest.json':
        raise ValueError('Unsafe/reserved artifact name: ' + name)
    return name


def openssl(args):
    result = subprocess.run(['openssl', *map(str, args)], capture_output=True, timeout=300)
    if result.returncode:
        raise ValueError('OpenSSL operation failed: ' + result.stderr.decode('utf-8', 'replace')[:1000])


def seal(root, names, recipient, output):
    root, recipient, output = Path(root).resolve(), Path(recipient), Path(output)
    if output.exists():
        raise FileExistsError(output)
    entries = []
    if not names or len(names) != len(set(names)):
        raise ValueError('List distinct files explicitly')
    for name in names:
        safe_name(name)
        path = root / name
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError('Artifact must be a regular file inside root: ' + name)
        entries.append({'path': name, 'bytes': path.stat().st_size, 'sha256': sha(path)})
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cas-seal-') as tmp:
        tmp = Path(tmp)
        manifest = {'schema_version': 1, 'files': entries,
                    'run': {k: os.environ.get(k) for k in
                            ('GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')}}
        data = (json.dumps(manifest, indent=2) + '\n').encode()
        with tarfile.open(tmp / 'bundle.tar', 'w') as archive:
            member = tarfile.TarInfo('manifest.json'); member.size = len(data); member.mode = 0o600
            archive.addfile(member, io.BytesIO(data))
            for item in entries:
                archive.add(root / item['path'], arcname=item['path'], recursive=False)
        openssl(['cms', '-encrypt', '-binary', '-aes-256-cbc', '-in', tmp / 'bundle.tar',
                 '-out', tmp / 'bundle.cms', '-outform', 'DER', recipient])
        envelope = {'schema_version': 1, 'cipher': 'openssl-cms-aes-256-cbc-rsa',
                    'ciphertext_sha256': sha(tmp / 'bundle.cms'),
                    'ciphertext_bytes': (tmp / 'bundle.cms').stat().st_size,
                    'recipient_certificate_sha256': sha(recipient)}
        output.mkdir(exist_ok=False)
        shutil.copyfile(tmp / 'bundle.cms', output / 'bundle.cms')
        (output / 'envelope.json').write_text(json.dumps(envelope, indent=2) + '\n')
    return envelope


def unpack_verified(archive_path, output, max_bytes=4 * 1024**3):
    """Validate the entire tar and its manifest before writing any output file."""
    output = Path(output)
    if output.exists():
        raise FileExistsError(output)
    with tarfile.open(archive_path, 'r:') as archive:
        members = archive.getmembers()
        names = [m.name for m in members]
        if len(members) > 10000 or len(names) != len(set(names)) or names.count('manifest.json') != 1:
            raise ValueError('Duplicate/missing/excessive archive members')
        if any(not m.isfile() for m in members) or sum(m.size for m in members) > max_bytes:
            raise ValueError('Non-file or oversized archive member')
        manifest_member = archive.getmember('manifest.json')
        if manifest_member.size > 4 * 1024**2:
            raise ValueError('Manifest exceeds limit')
        manifest = json.load(archive.extractfile(manifest_member))
        if manifest.get('schema_version') != 1:
            raise ValueError('Unknown manifest schema')
        refs = manifest['files']
        wanted = [safe_name(item['path']) for item in refs]
        if not wanted or len(wanted) != len(set(wanted)) or set(names) != set(wanted) | {'manifest.json'}:
            raise ValueError('Archive and manifest file sets differ')
        if any(str(parent) in set(names) for name in wanted for parent in PurePosixPath(name).parents
               if str(parent) != '.'):
            raise ValueError('Archive contains a file/directory name conflict')
        for item in refs:
            member = archive.getmember(item['path'])
            if member.size != item['bytes'] or not re.fullmatch('[0-9a-f]{64}', item['sha256']):
                raise ValueError('Invalid manifest file identity')
            h = hashlib.sha256()
            with archive.extractfile(member) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    h.update(chunk)
            if h.hexdigest() != item['sha256']:
                raise ValueError('Plaintext artifact hash mismatch')
        output.mkdir(parents=True, exist_ok=False)
        output.chmod(0o700)
        for member in members:
            target = output / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as dest, archive.extractfile(member) as source:
                shutil.copyfileobj(source, dest)
            target.chmod(0o600)
    return manifest


def unseal(bundle, key, output):
    bundle = Path(bundle)
    envelope = json.loads((bundle / 'envelope.json').read_text())
    cipher = bundle / 'bundle.cms'
    if (envelope.get('schema_version') != 1 or
            envelope.get('cipher') != 'openssl-cms-aes-256-cbc-rsa' or
            cipher.stat().st_size != envelope['ciphertext_bytes'] or
            sha(cipher) != envelope['ciphertext_sha256']):
        raise ValueError('Ciphertext identity mismatch')
    if Path(output).exists():
        raise FileExistsError(output)
    with tempfile.TemporaryDirectory(prefix='cas-unseal-') as tmp:
        raw = Path(tmp) / 'bundle.tar'
        openssl(['cms', '-decrypt', '-binary', '-inform', 'DER', '-in', cipher,
                 '-inkey', key, '-out', raw])
        try:
            return unpack_verified(raw, output)
        except tarfile.TarError:
            # Some OpenSSL builds implicitly reject a wrong RSA key by emitting
            # unusable plaintext with exit status zero. Still reject it before output.
            raise ValueError('Decrypted artifact is not a valid archive') from None


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    pack = commands.add_parser('seal')
    pack.add_argument('--root', type=Path, required=True)
    pack.add_argument('--file', action='append', required=True)
    pack.add_argument('--recipient', type=Path, required=True)
    pack.add_argument('--out', type=Path, required=True)
    unpack = commands.add_parser('unseal')
    unpack.add_argument('--bundle', type=Path, required=True)
    unpack.add_argument('--key', type=Path, required=True)
    unpack.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'seal':
        result = seal(args.root, args.file, args.recipient, args.out)
    else:
        result = unseal(args.bundle, args.key, args.out)
    print(json.dumps({'result': 'pass', 'operation': args.command,
                      'files': len(result.get('files', [])) if args.command == 'unseal' else None}))
