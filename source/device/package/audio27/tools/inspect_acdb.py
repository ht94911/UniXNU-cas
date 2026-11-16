#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only ACDB graph audit using pandora's parser. Does not compile, package,
write firmware, connect to phones, choose a test baseline or invoke any shell.
Private reports must stay outside source repos. No raw calibration is printed.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('PandoraAcdb', ROOT/'upstream/PandoraAcdb.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CheckedAcdb(module.Acdb):
    def __init__(self, path):
        path = Path(path)
        if not 12 <= path.stat().st_size <= 128*1024*1024:
            raise ValueError('ACDB size outside audit bounds')
        super().__init__(path)
        offset = 12
        seen = set()
        while offset < len(self.d):
            if len(self.d)-offset < 8:
                raise ValueError('short chunk header')
            tag, size = struct.unpack_from('<II', self.d, offset)
            if tag in seen or size > len(self.d)-offset-8:
                raise ValueError('duplicate or overflowing ACDB chunk')
            seen.add(tag)
            offset += 8+size
        self.checked_reads = 0

    def u32(self, offset, n=1):
        self.checked_reads += 1
        if self.checked_reads > 2_000_000 or not 1 <= n <= 65536 or offset < 0 or n*4 > len(self.d)-offset:
            raise ValueError('ACDB read outside file/budget')
        return super().u32(offset, n)

    def pool_param(self, offset):
        base, size = self.chunk('POOL')
        if not 0 <= offset <= size-4:
            raise ValueError('POOL offset outside chunk')
        n = self.u32(base+offset)
        if n > size-offset-4:
            raise ValueError('POOL data outside chunk')
        return self.d[base+offset+4:base+offset+4+n]

    def sg_connections(self, edges):
        base, size = self.chunk('SCLU')
        if size < 4 or (size-4) % 16:
            raise ValueError('bad connection table')
        found = set()
        for offset in range(base+4, base+size, 16):
            pair = tuple(self.u32(offset, 2))
            if pair in found:
                raise ValueError('duplicate connection')
            found.add(pair)
        if not set(edges) <= found:
            raise ValueError('graph connection missing; cannot skip it')
        return super().sg_connections(edges)


def spf(data):
    offset = 0
    parameters = []
    while offset < len(data):
        if len(data)-offset < 16:
            raise ValueError('short SPF header')
        iid, pid, size, error = struct.unpack_from('<IIII', data, offset)
        padded = (size+7) & ~7
        if not iid or not pid or error or padded > len(data)-offset-16:
            raise ValueError('invalid SPF parameter')
        if any(data[offset+16+size:offset+16+padded]):
            raise ValueError('nonzero SPF padding')
        parameters.append({'iid': hex(iid), 'pid': hex(pid), 'bytes': size})
        offset += 16+padded
    if not parameters:
        raise ValueError('empty SPF data')
    return parameters


def integer_map(value, allow_empty=False):
    if not isinstance(value, dict) or not (0 if allow_empty else 1) <= len(value) <= 64:
        raise ValueError('explicit nonempty key vector required')
    result = {int(k, 0): int(v, 0) if isinstance(v, str) else int(v) for k, v in value.items()}
    if len(result) != len(value) or any(not 0 <= x <= 0xffffffff for pair in result.items() for x in pair):
        raise ValueError('invalid key vector')
    return result


def graph_parts(path, selection):
    a = CheckedAcdb(path)
    gkv, ckv, tkv = [integer_map(selection[k], k == 'ckv') for k in ('gkv', 'ckv', 'tkv')]
    sg_list, sg_prop = a.find_graph(gkv)
    graphs = a.sg_list(sg_list)
    ids = [n for n, _ in graphs]
    if not 1 <= len(ids) <= 16 or len(set(ids)) != len(ids) or any(not n for n in ids):
        raise ValueError('invalid subgraph identity list')
    edges = [(src, dst) for src, targets in graphs for dst in targets]
    if any(dst not in ids for _, dst in edges):
        raise ValueError('graph edge escapes selected subgraphs')
    with contextlib.redirect_stdout(io.StringIO()):
        opened = b''.join(a.sg_spf_blob(sg_prop, sg) for sg in ids)+a.sg_connections(edges)
        calibration, _ = a.calibration(ids, ckv)
        hardware = a.tag_data(ids, int(selection['hardware_tag'], 0), tkv)
    tagged = {}
    for name, tag in selection['module_tags'].items():
        found = a.tagged(ids, int(tag, 0))
        if len(found) != 1:
            raise ValueError(f'{name}: expected one exact tagged module, found {len(found)}')
        tagged[name] = hex(found[0])
    blobs = {'open': opened, 'calibration': calibration, 'hardware': hardware}
    for data in blobs.values():
        spf(data)
    return a, ids, tagged, blobs


def inspect(path, selection):
    a, ids, tagged, blobs = graph_parts(path, selection)
    sections = {}
    for name, data in blobs.items():
        params = spf(data)
        sections[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'parameters': params}
    return {'kind': 'read-only-acdb-audit', 'source_sha256': hashlib.sha256(a.d).hexdigest(),
            'selection': selection, 'subgraphs': [hex(x) for x in ids], 'modules': tagged,
            'sections': sections, 'android_live_selection_proven': False,
            'driver_or_image_built': False, 'phone_accessed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('acdb', type=Path)
    parser.add_argument('selection', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    report = args.report.resolve()
    if report.is_relative_to(ROOT) or '.git' in report.parts:
        parser.error('Private graph reports belong outside the source tree')
    result = inspect(args.acdb, json.loads(args.selection.read_text()))
    report.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('selection', 'sections')}, indent=2))
