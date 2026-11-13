#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fill the empty native camera slot using the existing resource finalizer.

Run locally before strict prelink and payload HMAC. No phone or network.
"""

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--expected-native-sha256', required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--expected-profile-sha256', required=True)
    parser.add_argument('--prelink-tools', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    helper = runpy.run_path(str(Path(__file__).with_name('finalize-adreno840-firmware.py')))
    require = helper['require']
    profile = args.profile.read_bytes()
    require(hashlib.sha256(profile).hexdigest() == args.expected_profile_sha256,
            'Camera profile SHA256 differs')
    require(len(profile) == 128 * 1024 and profile[:8] in (b'N27CAM01', b'N27CAM02'),
            'Expected a prepared local camera profile')
    require(args.kernel.resolve() != args.out.resolve(), 'Output must be a new native kernel')
    receipt = args.out.with_suffix(args.out.suffix + '.camera-resource.json')
    require(not args.out.exists() and not receipt.exists(), 'Output or receipt already exists')
    sys.path.insert(0, str(args.prelink_tools.resolve()))
    from kc_image import Image
    result, report = helper['finalize_native'](args.kernel.read_bytes(), args.expected_native_sha256,
        profile, Image, capacity=128 * 1024, section_name='__nezcam',
        owner_symbol='_nezha_camera_profile_slot', size_symbol='_nezha_camera_profile_slot_size')
    report.update(profile_sha256=args.expected_profile_sha256, module_hardware_match_verified=False,
                  strict_prelink_and_payload_HMAC_required=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('xb') as stream:
        stream.write(result)
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print('Filled local camera resource; strict prelink, payload HMAC and hardware qualification remain required')


if __name__ == '__main__':
    main()
