# UniXNU for Xiaomi 10 Ultra

[简体中文](README.zh-CN.md) · [Build](docs/build-format2.md) · [Porting record](porting.toml) · [Validation](docs/validation.md)

Device configuration and platform integration for Xiaomi 10 Ultra / Mi 10 Ultra
(`cas`, SM8250). The current target is XNU 10063.141.1 with native 4 KiB pages and
the iPadOS17.7.11(j120ap) ABI. The boot target uses CPU0 and RAM-backed storage;
SMP follows the minimal userspace path.

## Status

| Capability | Result |
| --- | --- |
| UEFI | Temporary boot, inherited GOP, physical keys, read-only RAM FAT, EFI execution/return and Android recovery verified |
| XNU CPU0 | Kernel entry, VM and early platform/timebase initialization verified on candidate005 |
| Current build | Format2 continuous kernel, matching prelink and own signed static user process built on macos-26/Xcode26.6 |
| Runtime work | Sustained timer IRQs, RAM-root mount, EL0 and the iOS interface require new device evidence |

The [build record](docs/evidence/format2-build-001.json) fixes the source, compiler,
output hashes and CI run. The static process is an independent syscall fixture,
not Apple's launchd. Current scope and remaining hardware work are recorded in
[porting.toml](porting.toml) and [support status](docs/porting-status.md).

## Source layout

| Repository | Responsibility |
| --- | --- |
| UniXNU-cas | Board DTS, RAM reservations, component selection, guarded packaging and device records |
| [UniXNU-sm8250-common](https://github.com/ht94911/UniXNU-sm8250-common) | Shared CPU topology, GIC and timer resources |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | BootShim, SEC/DXE integration, GOP, keys and reset support |
| [UniXNU](https://github.com/UniXNU/UniXNU) | Build framework, OS profiles and shared components |

## Build

Use the framework revision in `ci/xnu/format2-sources.json`. From its checkout:

```sh
./unixnu defconfig cas_continuous_defconfig -D ../UniXNU-cas
./unixnu fetch -D ../UniXNU-cas
./unixnu status --json -D ../UniXNU-cas
./unixnu build kernel -D ../UniXNU-cas -j4
```

The [build guide](docs/build-format2.md) covers source pins, private inputs and
the guarded firmware path. Follow [controls and recovery](docs/operations.md)
for the accepted EFI release. The frozen `cas-complete-001` source and
[v0.1.0 validation](docs/validation.md) remain available independently.

This port uses temporary RAM boot. Proprietary inputs, private ADT/identity,
recovery backups and device-bound boot images are excluded from Git. See
[contributing](CONTRIBUTING.md) and the upstream [device repository guide](https://unixnu.com/porting/device-repository).
