#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate the board's actual stereo/mono VAD graph configuration."""
import argparse
from pathlib import Path
import plistlib
import shutil

PLAYBACK = ('speaker_general', 'speaker_alarm', 'speaker_ringtone')
MICROPHONE = ('front_mic_general', 'beamformed_mic_general',
              'bottom_mic2_general', 'back_mic_general', 'bottom_mic_general')
VAD = '/Library/Audio/Tunings/AID8001/VAD'

def generate(source, output):
    output.mkdir(parents=True, exist_ok=False)
    configurations = {}
    for graph, channels, names in [('nezha_playback', 2, PLAYBACK),
                                   ('nezha_microphone', 1, MICROPHONE)]:
        shutil.copyfile(source / (graph + '.dspg'), output / (graph + '.dspg'))
        (output / (graph + '.austrip')).write_bytes(plistlib.dumps(
            {'strips': [{'effects': []}]}, fmt=plistlib.FMT_BINARY))
        (output / (graph + '.propstrip')).write_bytes(plistlib.dumps(
            {'Boxes': []}, fmt=plistlib.FMT_BINARY))
        for name in names:
            configurations[name] = {'graph': graph, 'austrip': graph,
                                    'propstrip': graph, 'chainType': 'dflt',
                                    'busChannelCounts': [[channels, channels]]}
    document = {'CommonData': {'tuningPath': VAD,
                               'presetPath': '/Library/Audio/Tunings/AID8001/AU',
                               'tuningFilePrefix': ''},
                'Configurations': configurations}
    (output / 'graph_configurations.plist').write_bytes(plistlib.dumps(
        document, fmt=plistlib.FMT_BINARY, sort_keys=False))
    return document

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
