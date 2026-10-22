# Hardware acceptance and reproducibility

[简体中文](validation.zh-CN.md) · [Project home](../README.md)

The **`cas-complete-001`** milestone completed on **23 October 2026 (UTC+8)**. The results below concern one Xiaomi 10 Ultra, M2007J1SC / cas, in the recorded approximately 12 GB configuration, running Android 15 / OS3.0.4.0.VMKCNXM with kernel 4.19.157-moon before and after temporary boots.

The accepted boot image is **17,616,896 bytes**, SHA-256:

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

## Three independent runs

All three independent runs used that exact image.

| Trial | Menu and EFI observations | Exit route |
| --- | --- | --- |
| `t02-complete-001` | Four menu items; one-step volume navigation; short-power confirmation; about-one-second power release without accidental confirmation; at least five minutes of observed stable display; CasSmoke marker and return to a navigable menu; missing-file `Not Found` footer | UniXNU **Reboot to Android**, then USB identity and completed startup |
| `t04-complete-002` | A new independent boot; menu/navigation; CasSmoke marker; return with `Success` and usable navigation | UniXNU **Reboot to Android**, then USB confirmation |
| `t04-complete-003` | A new independent boot; menu/navigation; CasSmoke marker; short-power return with `Success`; exit to firmware menu and visible Firmware information | Firmware-menu **Reboot to Android**, then USB confirmation |

The five-minute stability observation belongs to the first run.

## Evidence levels

| Evidence | What was actually recorded |
| --- | --- |
| Operator observations | Screen text, physical-button behavior, menu transitions and Android appearance, recorded with timestamps |
| Host observations | Scoped USB identity, temporary-boot command responses, Android `boot_completed=1`, protected-partition reads and hashes |
| Offline checks | 126 source tests, image/FV/FAT checks, and QEMU execution/return/missing-file recovery using the actual built EFI bytes |
| Independent rebuild | A new complete build and package output, comparing boot image, 28 EFI modules, FD, DTB, gzip, RAM FAT and shim: 34 identical artifacts |

QEMU used a synthetic cas log header and does not validate phone GOP, SPMI, BDS or RAM block I/O. No project UEFI video or serial trace was captured. The [machine-readable acceptance summary](evidence/accepted-results.json) records the three runs, validation methods and original report hashes. Private original evidence remains with the project owner.

## Protected state

Before the first run and after the third, hashes of **18 protected partitions** were unchanged. The **six UFS LUNs' primary/backup GPT checks** passed; 24 corresponding binary blocks retained identical hashes and matched the recovery baseline. The procedure used temporary RAM boot and RAM FAT, with no partition flashing, unlocking, userdata erasure or repartitioning during these trials.

These checks do not establish that every userdata file was unchanged and do not replace personal-data backups.

## Reproducibility and release identity

The [34-artifact comparison](evidence/rebuild-comparison.json) records matching hashes and byte comparisons under the fixed toolchain. [source-pins.json](../source-pins.json) records the three source commits, firmware baseline, dependency revisions, tool versions and private-input hashes.

The original source bundles also passed import into empty bare Git object stores, recovering the expected commits and trees. That is a source-recovery check, separate from the independent full build. Neither result proves reproduction on other operating systems or tool versions.

`cas-complete-001` tags identify the original source commits. The `v0.1.0` publication adds documentation/support files and retains the same firmware implementation. Release downloads have a separate `SHA256SUMS` file.

## Boundaries

Validated: UEFI / UniXNU menu, physical keys, read-only RAM FAT, EFI load/execute/return, missing-file error return, firmware-menu exit, menu reboot to Android and the recorded rebuild.

Not validated: DarwinBoot-to-XNU handoff, XNU / iOS, touch, GPU acceleration, USB keyboard, internal UFS filesystems, charging management, ADB in UEFI, other cas units/memory variants, other system firmware or other devices. This is not a claim that every item in the upstream's numbered porting stages is complete.

The complete boot image and its stock DTB-derived contents remain local. The public release contains reviewed source and source-built components, documentation, licenses and sanitized evidence; no raw device firmware, identity, GPT dump or recovery backup.
