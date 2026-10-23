"""Build one platform/CPU archive using the source kernel's compile-record ABI.

The compile-record method follows the fixed pipa build-arm-driver reference;
this small implementation supports only the three initial CPU/platform components.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=('ArmCpu', 'ArmDTPlatform', 'ArmVirtPlatform'))
    args = parser.parse_args()
    component = args.component
    directories = {'ArmCpu': 'armcpu', 'ArmDTPlatform': 'armdt-platform', 'ArmVirtPlatform': 'armvirt-platform'}
    record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(record_path.read_text().strip().rstrip(','))
    output = ROOT / 'build' / directories[component]
    output.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'drivers' / component / (component + '.cpp')
    destination = output / (component + '.o')
    arguments = iter(record['arguments'])
    command = []
    for argument in arguments:
        if argument == '-o':
            next(arguments)
            command += ['-o', str(destination)]
        elif argument == record['file']:
            command.append(str(source))
        elif argument == '-Xclang':
            value = next(arguments)
            if not value.startswith('-W'):
                command += [argument, value]
        elif not argument.startswith('-W'):
            command.append(argument)
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion',
                '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    (output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (output / 'compile.log').open('w') as log:
        subprocess.run(command, cwd=record['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
    data = destination.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
        raise ValueError('Expected an ordinary ARM64 object')
    deployment, offset = [], 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment.append(struct.unpack_from('<II', data, offset + 8))
        offset += size
    if deployment != [(2, 0x110000)]:
        raise ValueError('Expected exactly one iOS17 deployment record')
    subprocess.run(['xcrun', 'ar', 'rcs', output / ('lib' + component + '.a'), destination], check=True)


if __name__ == '__main__':
    main()
