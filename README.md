# Xiaomi 10 Ultra (cas) UEFI device repository

Scope: temporary UEFI/UniXNU menu, physical buttons, RAM FAT EFI test and return.
This repository is under development and has no validated cas boot image yet.

Authoritative plan and progress: `/mnt/i/Code/UniXNU-cas/START-HERE.md`.
Windows management path: `I:\Code\UniXNU-cas`.
The management directory and this device source repository have different roles.

## UniXNU integration

board.toml is readable by the pinned upstream Context. Build with the cas
DeviceBuild.py --build --out NEW_DIRECTORY after sourcing tools/cas-env.sh.
Do not use generic unixnu build/bootimg: it does not implement cas checked
BootShim/provenance and can change shared dependencies before preflight.
The menu configuration is config/unixnu.toml; RAM FAT integration follows U-02.
