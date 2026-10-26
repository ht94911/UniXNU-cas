# UniXNU for Xiaomi 10 Ultra (cas)

[简体中文](README.zh-CN.md)

Board configuration for running iPadOS 17.7.11 on the Xiaomi 10 Ultra / Mi 10 Ultra (`cas`, SM8250) through [UniXNU](https://github.com/UniXNU/UniXNU). The kernel is XNU 10063.141.1 with 4 KiB pages; the donor is iPad7,1 / 17.7.11 / 21H461 (j120ap).

## Status

| Area | State |
| --- | --- |
| UEFI and UniXNU menu | Verified on hardware: temporary `fastboot boot`, inherited framebuffer, physical buttons, RAM FAT, EFI run and return (tag `cas-complete-001`) |
| XNU | CPU0 reaches VM and early platform initialization on hardware |
| In progress | Standard UniXNU build, UFS root volume on userdata, eight CPUs, Hello screen, ST FTS touch |

## Repositories

| Repository | Role |
| --- | --- |
| UniXNU-cas | Board configuration, device tree, defconfig, board package and patches |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | Mu-Silicium firmware: BootShim, SEC/DXE, GOP and buttons |
| [UniXNU-sm8250-common](https://github.com/ht94911/UniXNU-sm8250-common) | Shared SM8250 CPU, GIC and timer resources |
| [UniXNU](https://github.com/UniXNU/UniXNU) | Build framework, OS profile and shared components |

## Build

From a UniXNU checkout next to this repository:

```sh
./unixnu defconfig cas_defconfig -D ../UniXNU-cas
./unixnu fetch
./unixnu build kernel
```

The stock device tree, IPSW components, Data seed and other private inputs are not distributed.

## Credits

Hardware support follows [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) (Xiaomi Mi 10, same SoC and touch controller), [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) and the HackiOS cas notes. Licensed under [BSD-2-Clause-Patent](LICENSE); reused files keep their original notices.
