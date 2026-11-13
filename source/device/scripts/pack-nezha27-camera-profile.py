#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack one local stock OVX10500U module and its actual CSIPHY0 tables.

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


def make_pack(module_path, phy_path, kernel_path, alternate_module_path=None):
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
    initialization = [g['settings'] for g in module.initialization() if g['settings'] and
                      all(s['operation'] in ('WRITE', 'WRITE_BURST') for s in g['settings'])]
    require(len(initialization) == 1 and initialization[0][0]['register'] == 0x103 and
            initialization[0][0]['values'] == [1], 'Ambiguous sensor initialization')
    sequences = [initialization[0], list(module.settings(mode['settings_symbol']))]
    selection = []
    if alternate_module_path is not None:
        alternate = StockModule(alternate_module_path)
        alternate_initialization = [g['settings'] for g in alternate.initialization()
                                    if all(s['operation'] in ('WRITE', 'WRITE_BURST') for s in g['settings'])]
        require(len(alternate_initialization) == 1 and alternate.board() == board and
                alternate.modes()[1]['streams'] == mode['streams'] and
                list(alternate.settings(alternate.modes()[1]['settings_symbol'])) == sequences[1],
                'The two stock revisions differ outside their initialization')
        for source in (module, alternate):
            conditions = {v['field']: v['predicate'] for v in source.ovx_module_selection()[0]['conditions']}
            selection.append((int(conditions['vendorId'], 16), conditions['sensorVersion']))
        require(selection[0][0] in (3, 7) and selection[1][0] == selection[0][0] and
                {v[1] for v in selection} == {'0x41', 'gt 0x41'}, 'Expected one ordinary and one AS1 profile')
        if selection[0][1] == '0x41':
            module, alternate = alternate, module
            sequences[0], alternate_initialization[0] = alternate_initialization[0], sequences[0]
        sequences.append(alternate_initialization[0])
    phy = json.loads(phy_path.read_text())
    require(phy['source_sha256'] == sha(kernel_path.read_bytes()) and
            phy['control'] == 'ctrl_reg_2_4_0_1' and
            phy['requested_data_rate'] == mode['data_rate_bps'] and
            phy['selected']['max_data_rate'] >= mode['data_rate_bps'],
            'PHY source or selected data-rate profile differs')

    pack = bytearray(HEADER + (48 if selection else 0))
    version = 2 if selection else 1
    struct.pack_into('<8s8IQ', pack, 0, b'N27CAM02' if selection else b'N27CAM01', version, 0, 0x5058,
                     image['width'], image['height'], image['bits'], board['csi_lane_assign'],
                     mode['settle_ns'], mode['data_rate_bps'])
    struct.pack_into('<5I', pack, 104, 1, 1, *[phy['lane_offsets'][i] for i in (1, 3, 5)])
    struct.pack_into('<I', pack, 124, image['data_type'])
    pack[128:160] = bytes.fromhex(module.sha256)
    pack[160:192] = bytes.fromhex(phy['source_sha256'])
    if selection:
        struct.pack_into('<II', pack, 200, selection[0][0], 0x41)
        pack[208:240] = bytes.fromhex(alternate.sha256)
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
        module_hardware_match_verified=False, hardware_verified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--alternate-module', type=Path, help='the other stock revision for this module vendor')
    parser.add_argument('--phy', type=Path, required=True)
    parser.add_argument('--camera-kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    pack, report = make_pack(args.module, args.phy, args.camera_kernel, args.alternate_module)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('xb') as stream:
        stream.write(pack)
    args.out.with_suffix(args.out.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Packed local', report['module'], 'initialization', report['initialization_count'],
          'mode', report['mode_count'], 'used', report['used_bytes'], 'of', CAPACITY)


if __name__ == '__main__':
    main()
