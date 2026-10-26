# cas porting status and evidence boundaries

Current development follows **iOS17 / XNU10063.141.1 /4KiB / iPadOS17.7.11 j120ap**.
Single-core continuous startup comes first; SMP follows the minimal userland/UI path.
Successful entry, VM, platform, scheduler, IOKit and root checkpoints log and continue.

The frozen EFI release passed menu, RAM FAT, EFI execution/return and Android recovery.
Later CPU0 candidates entered real XNU;005 reached VM and early platform initialization
(RAW66, platform mask7F). This evidence remains attached to its original source and image.
It is not borrowed as a pass for the new format2 source or binary.

The new source uses the standard architecture/SoC/OS/device layers. Local resolver,
three-bank/31-reservation DTS→ADT comparison,61 ordered patches, and generated ArmCpu
observer staging passed. Full macOS kernel compilation, real timer IRQs, root mount,
EL0 and an iOS interface require their own results. The own static PID1 fixture is
not Apple's launchd and does not establish a working iOS environment.

No UFS, physical variable backend, capsule updater, persistent root or userdata
operation is enabled by this candidate. Device use remains RAM temporary boot.
`porting.toml` records all13 hardware areas; the management ledger remains the
authoritative live task state and retains the private raw observations.
