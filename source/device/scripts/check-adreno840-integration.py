#!/usr/bin/env python3
"""Check the actual Format2 archive plan and bounded GPU/CPU boot policy.

This runs the maintained resolver/planner; it neither emulates GPU hardware nor
qualifies target instructions. Those remain separate Apple-build/QEMU/device gates.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
import tomllib
from types import SimpleNamespace


def require(ok, message):
    if not ok:
        raise ValueError(message)


def arguments(text):
    result = {}
    for item in text.split():
        key, _, value = item.partition('=')
        require(key not in result, 'duplicate argument: ' + key)
        result[key] = value or '1'
    return result


def validate(framework, device, board, builtin, wifi_ram_test=False):
    from unixnu import components, kernel
    ctx = SimpleNamespace(root=framework, board=board, schema_version=2)
    ctx.component_specs = components.selected(ctx)
    plan = kernel.archive_plan(ctx)
    ordered = [('component:' + entry['id']) if kind == 'component' else entry
               for kind, entry in plan]
    gpu = ordered.index('component:Adreno840')
    for prerequisite in ('build-nezha27-input.py', 'build-gop-legacy-display-27.py',
                         'build-nezha27-usb-control.py', 'component:RamBlock'):
        require(ordered.index(prerequisite) < gpu, 'provider must precede A840: ' + prerequisite)
    require(ordered.index('nezha-armcpu-asm-fix.py') < ordered.index('component:ArmCpu') <
            ordered.index('qualify-armcpu-instructions.py') < gpu,
            'verified ArmCpu staging/build/instruction qualification order')
    for kind, entry in plan:
        if kind == 'script':
            require((device / 'scripts' / entry.split()[0]).is_file() or
                    (framework / 'scripts' / entry.split()[0]).is_file(),
                    'selected archive recipe not available: ' + entry)
    line = ' '.join(board['payload']['kernel_args'])
    require(len(line.encode('ascii')) <= 1000, 'payload arguments exceed Format2 1000-byte limit')
    explicit = arguments(line)
    effective = arguments(builtin) | explicit
    wanted = {'cpus': '8', 'sheng-smp': '1', 'armcpu-self-ipi': '1',
              'armcpu-spi-ipi': '0', 'armcpu-edge': '1', 'nezha-gpu': '1',
              'nezha-gpu-metal': '1', 'sheng-gpu-trace': '1', 'sheng-gpu-addon': '1',
              'boot-progress': '2', 'offline-legacy-cpu-present': '1',
              'sheng-fb-gpu': '1', 'sheng-dpu-scanout': '0',
              'nezha-reboot-after': '0', 'nezha-panic-reset': '0',
              'sheng-data-persist': '2', 'ram-data-volatile-4g': '0',
              'sheng-data-reset': '0', 'nezha-usb-reboot': '1',
              'boot-diag-overlay': '0', 'input-test': '0',
              'nezha-cpufreq': '3', 'nezha-thermal': '1',
              'sheng-vsync': '120', 'sheng-vsync-fixed': '1'}
    if wifi_ram_test:
        wanted.update({'sheng-data-persist': '0', 'ram-data-volatile-4g': '1',
                       'nezha-usb-reboot': '2', 'nezha_wifi': '1',
                       'boot-ui-focus2': 'Preferences',
                       'boot-ui-focus3': 'SpringBoard', 'boot-ui-follow': '1'})
        require('build-nezha27-wifi.py' in ordered, 'Wi-Fi archive absent')
        require(ordered.index('build-nezha27-wifi.py') < gpu,
                'Wi-Fi final-link provider must precede A840 qualification')
        require(board['adt']['chosen']['u32'].get('nezha-wifi-autojoin-rearm') == 1,
                'explicit test-network rearm property absent')
        require(effective.get('sheng-disable-jobs') ==
                'com.apple.cameracaptured,com.apple.CommCenter',
                'qualified camera/CommCenter RAM policy changed')
    require(all(effective.get(key) == value for key, value in wanted.items()),
            'effective CPU/GPU/diagnostic/protection policy changed')
    require(explicit.get('cpus') == '8', 'loader must explicitly select eight CPUs')
    return {'archive_plan': ordered, 'command_line_bytes': len(line.encode('ascii')),
            'command_line_limit': 1000, 'effective_required_arguments': wanted,
            'profile': 'wifi-ram-test' if wifi_ram_test else 'persistent-main'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--device', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--wifi-ram-test', action='store_true',
                        help='Explicit independent Wi-Fi test; reject persistent Data access')
    args = parser.parse_args()
    framework, device = args.framework.resolve(), args.device.resolve()
    sys.path.insert(0, str(framework / 'scripts'))
    board = tomllib.loads((device / 'board.toml').read_text())
    policy = (device / 'package/BootWatchdogPolicy.c').read_text()
    matches = re.findall(r'char PE_builtin_boot_args\[\]\s*=\s*("[^"\n]*");', policy)
    require(len(matches) == 1, 'one existing board builtin argument definition')
    builtin = json.loads(matches[0])
    result = validate(framework, device, board, builtin, args.wifi_ram_test)
    rejected = []
    for name in ('late-hid-provider', 'unqualified-armcpu', 'oversized-line',
                 'single-cpu-regression', 'fixed-reset-enabled', 'missing-smp-default',
                 'diagnostic-not-rendering', 'missing-addon', 'missing-gpu-default',
                 'volatile-data', 'destructive-reset', 'direct-persistent-reset', 'overlay-enabled', 'automatic-settings',
                 'cpu-governor-disabled', 'thermal-disabled'):
        candidate, defaults = copy.deepcopy(board), builtin
        archive = candidate['kernel']['archives']
        mutations={'volatile-data':('sheng-data-persist=2','sheng-data-persist=0'),
                   'destructive-reset':('sheng-data-reset=0','sheng-data-reset=1'),
                   'direct-persistent-reset':('nezha-usb-reboot=1','nezha-usb-reboot=2'),
                   'overlay-enabled':('boot-diag-overlay=0','boot-diag-overlay=1'),
                   'automatic-settings':('input-test=0','input-test=1')}
        if args.wifi_ram_test:
            mutations.update({'volatile-data': ('sheng-data-persist=0', 'sheng-data-persist=2'),
                              'direct-persistent-reset': ('nezha-usb-reboot=2', 'nezha-usb-reboot=1')})
        if name in ('cpu-governor-disabled', 'thermal-disabled'):
            defaults = defaults.replace('nezha-cpufreq=3', 'nezha-cpufreq=0') if name == 'cpu-governor-disabled' else defaults.replace('nezha-thermal=1', 'nezha-thermal=0')
        elif name in mutations:
            old,new=mutations[name]
            candidate['payload']['kernel_args']=[x.replace(old,new) for x in candidate['payload']['kernel_args']]
        elif name == 'late-hid-provider':
            archive.remove('build-nezha27-input.py')
            archive.append('build-nezha27-input.py')
        elif name == 'unqualified-armcpu':
            archive.remove('nezha-armcpu-asm-fix.py')
            archive.append('nezha-armcpu-asm-fix.py')
        elif name == 'oversized-line':
            candidate['payload']['kernel_args'].append('diagnostic=' + 'x' * 1000)
        elif name == 'single-cpu-regression':
            candidate['payload']['kernel_args'] = [x.replace('cpus=8', 'cpus=1')
                                                 for x in candidate['payload']['kernel_args']]
        elif name == 'fixed-reset-enabled':
            candidate['payload']['kernel_args'] = [x.replace('nezha-reboot-after=0', 'nezha-reboot-after=5')
                                                 for x in candidate['payload']['kernel_args']]
        elif name == 'missing-smp-default':
            defaults = defaults.replace('sheng-smp=1', 'sheng-smp=0')
        elif name == 'diagnostic-not-rendering':
            candidate['payload']['kernel_args'] = [x.replace('nezha-gpu-metal=1', 'nezha-gpu-metal=2')
                                                 for x in candidate['payload']['kernel_args']]
        elif name == 'missing-addon':
            candidate['payload']['kernel_args'] = [x.replace('sheng-gpu-addon=1', 'sheng-gpu-addon=0')
                                                 for x in candidate['payload']['kernel_args']]
        elif name == 'missing-gpu-default':
            defaults = defaults.replace('nezha-gpu=1', '')
        try:
            validate(framework, device, candidate, defaults, args.wifi_ram_test)
        except ValueError as error:
            rejected.append({'case': name, 'reason': str(error)})
        else:
            raise ValueError('negative case was accepted: ' + name)
    if args.wifi_ram_test:
        for name in ('missing-wifi', 'late-wifi-provider', 'missing-rearm', 'missing-CommCenter-policy',
                     'missing-startup-observer', 'missing-settings-observer', 'not-persistent-main'):
            candidate, defaults = copy.deepcopy(board), builtin
            if name == 'missing-wifi':
                candidate['payload']['kernel_args'] = [x.replace('nezha_wifi=1', 'nezha_wifi=0')
                                                       for x in candidate['payload']['kernel_args']]
            elif name == 'late-wifi-provider':
                candidate['kernel']['archives'].remove('build-nezha27-wifi.py')
                candidate['kernel']['archives'].append('build-nezha27-wifi.py')
            elif name == 'missing-rearm':
                candidate['adt']['chosen']['u32']['nezha-wifi-autojoin-rearm'] = 0
            elif name == 'missing-CommCenter-policy':
                defaults = defaults.replace(',com.apple.CommCenter', '')
            elif name == 'missing-startup-observer':
                defaults = defaults.replace(' boot-ui-focus3=SpringBoard', '')
            elif name == 'missing-settings-observer':
                candidate['payload']['kernel_args'] = [x for x in candidate['payload']['kernel_args']
                                                      if not x.startswith('boot-ui-focus2=')]
            try:
                validate(framework, device, candidate, defaults, name != 'not-persistent-main')
            except ValueError as error:
                rejected.append({'case': name, 'reason': str(error)})
            else:
                raise ValueError('negative case was accepted: ' + name)
    result.update(passed=True, rejected_cases=rejected,
                  board_sha256=hashlib.sha256((device / 'board.toml').read_bytes()).hexdigest(),
                  builtin_source_sha256=hashlib.sha256(policy.encode()).hexdigest(),
                  scope='actual source resolver and policy checks; no target execution',
                  hardware_qualified=False, phone_operations=False)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open('x') as output:
            output.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
