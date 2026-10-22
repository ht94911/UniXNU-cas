# Contributing

[简体中文](CONTRIBUTING.zh-CN.md) · [Project home](README.md)

Read the [architecture](docs/architecture.md), [build guide](docs/build.md) and [validation limits](docs/validation.md) before changing the port.

## Choose the source repository

Device configuration, kernel configuration, board scripts, device drivers, patches and public documentation belong in `UniXNU-cas`. Firmware startup, the cas drivers, BootShim and CasSmoke belong in [uefi-cas](https://github.com/ht94911/uefi-cas). Generic changes can be proposed to [UniXNU upstream](https://github.com/UniXNU/UniXNU).

## Submit a change

1. Use a focused branch and preserve existing work. Keep the hardware-tested `cas-complete-001` tags immutable.
2. Explain the trigger, resulting behavior and affected candidate. Keep English and Simplified Chinese documentation aligned.
3. Run the relevant offline checks in the build guide. Report skipped or unavailable checks explicitly.
4. Save new output under a new identifier. Source inspection, a successful build, QEMU and phone observations are distinct evidence levels.
5. If the candidate changes, record new applicable hardware validation. A previous image's success does not transfer to new bytes.

Commit subjects can use `docs:`, `fix(cas):`, `feat(cas):` or `build(cas):`. Describe the final change and the actual validation in the pull request.

## Report a problem

Include the board/model, relevant firmware versions, source commits, image SHA-256, exact menu action, observed result and recovery outcome. State whether the observation is from a person, host command, emulator or serial log. A failed attempt is useful evidence; retain its own identifier.

Remove serial numbers, IMEIs, MAC addresses, account details, IP addresses and tokens from public logs. Do not attach stock firmware, DTBs extracted from a phone, GPT dumps, recovery backups, proprietary OS images or a complete device-bound boot image. Use the [issue form](https://github.com/ht94911/UniXNU-cas/issues/new/choose).

## License

Original contributions follow [BSD-2-Clause-Patent](LICENSE). Retain existing copyright and license notices for reused code. Reference material must be identified accurately.
