#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deferred local input preparation, AFTER the human selects a test package.

Uses the existing pandora PSPKGR01 container, not another graph format. This
tool is source-only at delivery; it must not be invoked to choose a baseline.
No compiler, subprocess, network, firmware writes or phone access. Output is
private ACDB data and must remain outside repositories and upload workflows.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from inspect_acdb import ROOT, graph_parts


def container(path, selection, channels):
    if channels not in (1, 2):
        raise ValueError('only the reviewed mono/stereo paths are available')
    a, ids, tagged, blobs = graph_parts(path, selection)
    if not 1 <= len(tagged) <= 32:
        raise ValueError('invalid module count')
    tags = [int(selection['module_tags'][name], 0) for name in tagged]
    instances = [int(value, 0) for value in tagged.values()]
    if len(set(tags)) != len(tags) or len(set(instances)) != len(instances):
        raise ValueError('duplicate tag or instance')
    sections = [blobs['open'], blobs['calibration'], blobs['hardware'],
                struct.pack('<I', len(ids))+struct.pack('<'+'I'*len(ids), *ids),
                struct.pack('<II', channels, len(tags))+
                b''.join(struct.pack('<II', tag, iid) for tag, iid in zip(tags, instances))]
    result = b'PSPKGR01'+struct.pack('<I', len(sections))
    for kind, data in enumerate(sections, 1):
        result += struct.pack('<II', kind, len(data))+data+b'\0'*((-len(data)) % 8)
    if len(result) > 0x100000:
        raise ValueError('graph exceeds runtime parser limit')
    return result, hashlib.sha256(a.d).hexdigest()


def private_destination(path):
    path = path.resolve()
    if path.exists() or path.is_relative_to(ROOT):
        raise ValueError('output must be a new private file outside the source tree')
    if any((parent/'.git').exists() for parent in path.parents):
        raise ValueError('private graph output cannot be under a repository')
    return path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('acdb', type=Path)
    parser.add_argument('selection', type=Path)
    parser.add_argument('--channels', required=True, type=int, choices=[1, 2])
    parser.add_argument('--selected-package', required=True, type=Path,
                        help='Human-selected package identity manifest; never use the disabled draft')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.selected_package.read_text())
    if not manifest.get('selected_test_package'):
        parser.error('human-selected package identity is required; source-only draft is disabled')
    output = private_destination(args.output)
    data, source = container(args.acdb, json.loads(args.selection.read_text()), args.channels)
    with output.open('xb') as stream:
        stream.write(data)
    print(json.dumps({'kind': 'private-graph-input', 'bytes': len(data),
                      'sha256': hashlib.sha256(data).hexdigest(), 'source_sha256': source,
                      'hardware_validated': False, 'phone_accessed': False}))
