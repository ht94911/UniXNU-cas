# cas support status

Target: Xiaomi 10 Ultra(cas), QualcommSM8250, XNU10063.141.1, native 4 KiB pages,
iPadOS 17.7.11(21 H 461), j120ap ABI.

| Area | Established result | Current boundary |
| --- | --- | --- |
| UEFI | Temporary boot, GOP 1080×2340, physical keys, read-only RAM FAT, EFI return and Android recovery | Frozen EFI release; new format 2 profile is compiled separately |
| XNU CPU0 | Kernel entry, VM and early platform/timebase initialization | Candidate 005: RAW 66 and platform validity mask 7 F |
| Format 2 | Layered source resolution,61 ordered patches, DTS→ADT comparison,13-area record | New full macOS build is recorded by its CI run |
| UEFI interfaces | AARCH 64 SEC bridge and shared RAM-disk modules compile | Complete guarded firmware and device execution remain separate checks |
| Interrupts | Real ArmCpu handler/return/EOI observations implemented | Sustained timer IRQs are not yet a device result |
| Root and process | Mockfs and an own signed static process build path | Root mount and EL 0 require runtime evidence |
| iOS interface | Matching OS profile and prelink inputs selected | No interactive interface result |

The active boot target is continuous single-core startup. SMP follows the
minimal userspace/interface path. Each result remains bound to its source and
image hashes. Successful checks for unchanged inputs are reused.

The own static process is a test executable, not Apple's launchd. UFS, persistent
Data, charging policy, GPU acceleration, network, audio and touch are not enabled
by this candidate. Storage is RAM-only. Technical records are in porting.toml;
the [frozen validation](validation.md) retains the accepted EFI evidence.
