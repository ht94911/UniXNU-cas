#!/usr/bin/env python3
"""Fetch one stored member of a remote IPSW with HTTP range requests (curl), without downloading the whole archive.

The BuildManifest must name the member as the OS image of the iPad7,1 / j120ap Erase identity of iPadOS 17.7.11 (21H461),
and the result must match the expected size and SHA-256. Python standard library plus curl.
"""
import argparse
import concurrent.futures
import hashlib
import io
import os
import plistlib
import re
import struct
import subprocess
import sys
import time
import zipfile

CHUNK = 32 * 1024**2


def fetch(url, start, end):
    """Bytes start..end (inclusive) of url."""
    error = ''
    for attempt in range(8):
        result = subprocess.run(['curl', '-fsS', '--connect-timeout', '30', '--max-time', '600', '-r', f'{start}-{end}', url],
                                capture_output=True)
        if result.returncode == 0 and len(result.stdout) == end - start + 1:
            return result.stdout
        error = f'curl {result.returncode}, {len(result.stdout)} bytes, {result.stderr.decode("utf-8", "replace")[-300:]}'
        time.sleep(min(60, 4 * (attempt + 1)))
    raise SystemExit(f'range {start}-{end} failed: {error}')


def remote_size(url):
    result = subprocess.run(['curl', '-fsS', '--connect-timeout', '30', '-r', '0-0', '-D', '-', '-o', '/dev/null', url],
                            capture_output=True, text=True, check=True)
    statuses = re.findall(r'^HTTP/\S+ (\d+)', result.stdout, re.M)
    match = re.search(r'^content-range:\s*bytes 0-0/(\d+)\s*$', result.stdout, re.M | re.I)
    if not statuses or statuses[-1] != '206' or not match:
        raise SystemExit('The server does not answer range requests')
    return int(match.group(1))


class RangeReader(io.RawIOBase):
    def __init__(self, url, size):
        self.url, self.size, self.pos = url, size, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        position = {io.SEEK_SET: 0, io.SEEK_CUR: self.pos, io.SEEK_END: self.size}[whence] + offset
        if position < 0:
            raise ValueError('negative seek')
        self.pos = position
        return position

    def readinto(self, buffer):
        count = min(len(buffer), self.size - self.pos)
        if count <= 0:
            return 0
        buffer[:count] = fetch(self.url, self.pos, self.pos + count - 1)
        self.pos += count
        return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--member', required=True)
    parser.add_argument('--bytes', type=int, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('output')
    args = parser.parse_args()
    if os.path.exists(args.output):
        raise SystemExit('Refuse to overwrite ' + args.output)
    total = remote_size(args.url)
    stream = io.BufferedReader(RangeReader(args.url, total), buffer_size=1024**2)
    with zipfile.ZipFile(stream) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        if (manifest['ProductVersion'] != '17.7.11' or manifest['ProductBuildVersion'] != '21H461'
                or 'iPad7,1' not in manifest['SupportedProductTypes']):
            raise SystemExit('Not the iPad7,1 iPadOS 17.7.11 / 21H461 restore image')
        identity = next(item for item in manifest['BuildIdentities']
                        if item['Info']['DeviceClass'] == 'j120ap' and item['Info']['RestoreBehavior'] == 'Erase'
                        and int(item['ApChipID'], 16) == 0x8011 and int(item['ApBoardID'], 16) == 12)
        if identity['Manifest']['OS']['Info']['Path'] != args.member:
            raise SystemExit('The manifest names another OS image: ' + identity['Manifest']['OS']['Info']['Path'])
        info = archive.getinfo(args.member)
    if info.compress_type != zipfile.ZIP_STORED or info.file_size != args.bytes or info.compress_size != args.bytes:
        raise SystemExit('The member is not stored uncompressed with the expected size')
    stream.seek(info.header_offset)
    header = stream.read(30)
    signature, _, _, method, _, _, _, _, _, name_length, extra_length = struct.unpack('<IHHHHHIIIHH', header)
    if signature != 0x04034b50 or method != 0 or stream.read(name_length).decode() != args.member:
        raise SystemExit('Unexpected local file header')
    start = info.header_offset + 30 + name_length + extra_length
    if start + args.bytes > total:
        raise SystemExit('The member extends past the archive')
    print(f'archive {total} bytes; {args.member} at {start}, {args.bytes} bytes', flush=True)
    began = time.time()
    partial = args.output + '.partial'
    fd = os.open(partial, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.ftruncate(fd, args.bytes)
        offsets = list(range(0, args.bytes, CHUNK))
        done = 0

        def one(offset):
            data = fetch(args.url, start + offset, start + min(offset + CHUNK, args.bytes) - 1)
            os.pwrite(fd, data, offset)
            return len(data)

        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for index, count in enumerate(pool.map(one, offsets), 1):
                done += count
                if index % 20 == 0 or index == len(offsets):
                    print(f'  {done * 100 // args.bytes}% ({done >> 20} MiB, {time.time() - began:.0f} s)', flush=True)
        os.fsync(fd)
    finally:
        os.close(fd)
    digest = hashlib.sha256()
    with open(partial, 'rb') as handle:
        for part in iter(lambda: handle.read(8 * 1024**2), b''):
            digest.update(part)
    if os.path.getsize(partial) != args.bytes or digest.hexdigest() != args.sha256:
        os.unlink(partial)
        raise SystemExit(f'SHA-256 mismatch: {digest.hexdigest()}')
    os.rename(partial, args.output)
    print(f'{args.member}: sha256 {digest.hexdigest()} verified ({time.time() - began:.0f} s)')


if __name__ == '__main__':
    sys.exit(main())
