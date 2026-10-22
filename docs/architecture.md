# Architecture and future work

[简体中文](architecture.zh-CN.md) · [Project home](../README.md)

## The verified boot path

```text
USB Fastboot temporary boot
  → stock ABL / device-tree preparation
  → cas BootShim: validate and copy bounded RAM inputs
  → Mu-Silicium SEC / DXE / cas firmware drivers
  → UniXNU from the firmware volume
  → read-only RAM FAT → CasSmoke.efi → return to UniXNU
  → menu reboot → existing Android installation
```

The firmware preserves the bootloader's scanout, provides GOP, reads physical keys through SPMI, and publishes a RAM-backed FAT filesystem. The boot option points to the pinned UniXNU application in the firmware volume. CasSmoke produces an identifiable marker and returns through the EFI application interface.

This target uses emulated variables and read-only RAM storage. It does not include a physical storage controller, a physical variable backend, FTW or a firmware updater. Capsule calls return `EFI_UNSUPPORTED`. These implementation choices are supplemented by the recorded before/after protected-partition checks.

## Source ownership

| Repository | Responsibility now and later |
| --- | --- |
| [UniXNU-cas](https://github.com/ht94911/UniXNU-cas) | Project portal, board configuration, build/packaging checks, evidence summaries; future cas kernel configuration, scripts, patches, drivers and package modules |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | Firmware startup, BootShim, GOP, keys, RAM block I/O, EFI test application and board libraries |
| [UniXNU upstream](https://github.com/UniXNU/UniXNU) | Shared boot manager, DarwinBoot, build driver and shared operating-system components |

One cas device page can link both owned repositories. This follows the [upstream repository scope](https://github.com/UniXNU/UniXNU/blob/main/CONTRIBUTING.md) and the [pipa device page](https://unixnu.com/devices/pipa). A future generic fix should be reviewed for upstream contribution; a cas-specific component belongs with this device.

The current `board.toml` is readable by the pinned upstream context, but building this milestone requires the cas-specific checked entry point. The generic `unixnu build/bootimg` route does not implement the complete cas provenance and BootShim checks. The build guide uses the supported route.

## Future operating-system work

These are possible next stages, **not completed features or release promises**:

| Stage | Work to establish | Where it can grow |
| --- | --- | --- |
| Kernel preparation | Select compatible XNU inputs, configuration and required board description | Device configuration, `patches/`, `scripts/`, `package/` |
| Kernel entry | DarwinBoot handoff, observable early execution and bounded memory assumptions | Device configuration and firmware changes where required |
| Storage and root filesystem | A tested storage path, root-device discovery and filesystem mount | Device drivers and kernel configuration |
| Userspace | Reproducible system inputs and an observed userspace startup | Device package and scripts |
| Hardware support | Display, touch, GPU, power and other drivers, each with its own evidence | Device drivers; shared fixes proposed upstream |

The existing UEFI milestone remains a known baseline during that work. New candidate bytes receive a new identifier and new applicable validation; old hardware results remain attached to their original SHA-256. Public code must not include proprietary system images, stock firmware, device identities or recovery backups.
