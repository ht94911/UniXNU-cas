#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack local stock OVX10500U module tables and their actual CSIPHY0 tables.

Private output only. A pack records the selected module; selection is not a
hardware match. Never add its data or generated receipt to source-only CI.
"""

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct


CAPACITY = 128 * 1024
HEADER = 192
ROOT = Path(__file__).resolve().parent
StockModule = runpy.run_path(str(ROOT / 'inspect-nezha-camera-stock-module.py'))['StockModule']


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_initialization(module):
    sequences = [group['settings'] for group in module.initialization() if group['settings'] and
                 all(entry['operation'] in ('WRITE', 'WRITE_BURST') for entry in group['settings'])]
    require(len(sequences) == 1 and sequences[0][0]['register'] == 0x103 and
            sequences[0][0]['values'] == [1], 'Ambiguous sensor initialization')
    return sequences[0]


def selector(module):
    conditions = {entry['field']: entry['predicate'] for entry in module.ovx_module_selection()[0]['conditions']}
    return int(conditions['vendorId'], 16), conditions['sensorVersion']


def same_capture_contract(module, board, mode, mode_settings):
    actual = module.modes()[1]
    fields = ('c_phy', 'lanes', 'data_rate_bps', 'fps', 'settle_ns', 'streams')
    return (module.board() == board and all(actual[k] == mode[k] for k in fields) and
            list(module.settings(actual['settings_symbol'])) == mode_settings)


def make_pack(module_path, phy_path, kernel_path, alternate_module_path=None, other_vendor_paths=None):
    module = StockModule(module_path)
    board = module.board()
    require('ovx10500u_wide' in module.name and board['slave_7bit'] == 0x10 and
            board['id_register'] == 0x300b and board['expected_id'] == 0x5058 and
            board['csi_lane_assign'] == 0x210, 'Not a stock nezha main sensor module')
    mode = module.modes()[1]
    image = mode['streams'][0]
    require(mode['c_phy'] and mode['lanes'] == 3 and mode['data_rate_bps'] == 8000000000 and
            29.9 <= mode['fps'] <= 30.1 and
            (image['width'], image['height'], image['bits'], image['vc'], image['data_type']) ==
            (4096, 3072, 10, 0, 0x2b), 'Unsupported mode1 image stream')
    # The old schema does not identify the current header_value field. Select
    # the one actual write array and retain the other read groups in evidence.
    sequences = [write_initialization(module), list(module.settings(mode['settings_symbol']))]
    selection = []
    other_modules = []
    if alternate_module_path is not None:
        alternate = StockModule(alternate_module_path)
        alternate_initialization = write_initialization(alternate)
        require(same_capture_contract(alternate, board, mode, sequences[1]),
                'The two stock revisions differ outside their initialization')
        selection = [selector(module), selector(alternate)]
        require(selection[0][0] in (3, 7) and selection[1][0] == selection[0][0] and
                {v[1] for v in selection} == {'0x41', 'gt 0x41'}, 'Expected one ordinary and one AS1 profile')
        if selection[0][1] == '0x41':
            module, alternate = alternate, module
            sequences[0], alternate_initialization = alternate_initialization, sequences[0]
        sequences.append(alternate_initialization)
    if other_vendor_paths:
        require(selection and selection[0][0] == 3, 'A combined pack starts with the SEMCO pair')
        other_modules = [StockModule(path) for path in other_vendor_paths]
        other_modules.sort(key=lambda entry: selector(entry)[1] == '0x41')
        require([selector(entry) for entry in other_modules] == [(7, 'gt 0x41'), (7, '0x41')],
                'Expected the two OFILM revisions')
        for entry, table in zip(other_modules, (sequences[0], sequences[2])):
            require(same_capture_contract(entry, board, mode, sequences[1]) and
                    write_initialization(entry) == table,
                    'The other vendor requires different sensor writes or a different mode')
    phy = json.loads(phy_path.read_text())
    require(phy['source_sha256'] == sha(kernel_path.read_bytes()) and
            phy['control'] == 'ctrl_reg_2_4_0_1' and
            phy['requested_data_rate'] == mode['data_rate_bps'] and
            phy['selected']['max_data_rate'] >= mode['data_rate_bps'],
            'PHY source or selected data-rate profile differs')

    version = 3 if other_modules else 2 if selection else 1
    pack = bytearray(HEADER + (48 if selection else 0) + (64 if other_modules else 0))
    struct.pack_into('<8s8IQ', pack, 0, f'N27CAM0{version}'.encode(), version, 0, 0x5058,
                     image['width'], image['height'], image['bits'], board['csi_lane_assign'],
                     mode['settle_ns'], mode['data_rate_bps'])
    struct.pack_into('<5I', pack, 104, 1, 1, *[phy['lane_offsets'][i] for i in (1, 3, 5)])
    struct.pack_into('<I', pack, 124, image['data_type'])
    pack[128:160] = bytes.fromhex(module.sha256)
    pack[160:192] = bytes.fromhex(phy['source_sha256'])
    if selection:
        struct.pack_into('<II', pack, 200, selection[0][0], 0x41)
        pack[208:240] = bytes.fromhex(alternate.sha256)
    if other_modules:
        pack[240:304] = b''.join(bytes.fromhex(entry.sha256) for entry in other_modules)
    pending_bursts = []
    for table, sequence in enumerate(sequences):
        struct.pack_into('<II', pack, 192 if table == 2 else 48 + table * 8, len(pack), len(sequence))
        for entry in sequence:
            values = entry['values']
            require(entry['address_bytes'] == 2 and entry['data_bytes'] == 1 and
                    entry['register'] != 0x100 and 0 <= entry['delay_us'] <= 65535000,
                    'Unsupported pre-stream sensor operation')
            delay_ms = (entry['delay_us'] + 999) // 1000
            if entry['operation'] == 'WRITE':
                require(len(values) == 1, 'Scalar write has multiple data bytes')
                pack += struct.pack('<HBBIII', entry['register'], values[0], 0, delay_ms, 0, 0)
            else:
                require(entry['operation'] == 'WRITE_BURST' and 0 < len(values) <= 65535,
                        'Unsupported burst operation')
                pending_bursts.append((len(pack), bytes(values)))
                pack += struct.pack('<HBBIII', entry['register'], 0, 1, delay_ms, 0, len(values))

    for table, name in enumerate(('common', 'reset_enter', 'reset_exit', 'cphy', 'selected')):
        require('common' not in phy['selected'], 'Additional rate-specific common settings need explicit integration')
        source = phy[name]
        records = b''.join(struct.pack('<4I', *[row[k] for k in ('offset', 'value', 'delay_us', 'kind')])
                           for row in source['registers'])
        require(records and sha(records) == source['sha256'], 'PHY table hash differs')
        struct.pack_into('<II', pack, 64 + table * 8, len(pack), len(source['registers']))
        pack += records
    for at, data in pending_bursts:
        struct.pack_into('<I', pack, at + 8, len(pack))
        pack += data
        pack += bytes(-len(pack) % 4)
    used = len(pack)
    require(used <= CAPACITY, 'Camera profile exceeds its native resource slot')
    struct.pack_into('<I', pack, 12, used)
    pack += bytes(CAPACITY - used)
    return bytes(pack), dict(module=module.name, module_sha256=module.sha256,
        camera_kernel_sha256=phy['source_sha256'], mode=1,
        initialization_count=len(sequences[0]), mode_count=len(sequences[1]),
        burst_bytes=[len(data) for _, data in pending_bursts],
        phy_control=phy['control'], rate_profile=phy['selected']['symbol'],
        used_bytes=used, capacity_bytes=CAPACITY, sha256=sha(pack), profile_version=version,
        alternate_module_sha256=alternate.sha256 if selection else None,
        alternate_initialization_count=len(sequences[2]) if selection else None,
        other_vendor_sources=[dict(module=entry.name, sha256=entry.sha256) for entry in other_modules],
        module_hardware_match_verified=False, hardware_verified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--alternate-module', type=Path, help='the other stock revision for this module vendor')
    parser.add_argument('--other-vendor', type=Path, nargs=2, metavar=('OFILM', 'OFILM_AS1'),
                        help='both OFILM revisions, qualified against the primary SEMCO pair')
    parser.add_argument('--phy', type=Path, required=True)
    parser.add_argument('--camera-kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    pack, report = make_pack(args.module, args.phy, args.camera_kernel, args.alternate_module, args.other_vendor)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('xb') as stream:
        stream.write(pack)
    args.out.with_suffix(args.out.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Packed local', report['module'], 'initialization', report['initialization_count'],
          'mode', report['mode_count'], 'used', report['used_bytes'], 'of', CAPACITY)


if __name__ == '__main__':
    main()
