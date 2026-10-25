# Continuous CPU0 boot toward iOS 17

This local source profile follows the 2026-10-26 direction to keep XNU10063 / native4KiB / iPadOS17.7.11, run one CPU toward userspace first, and defer SMP until after an interactive interface. Frozen005 has already reached its deliberate VM/platform stop; it is not repeated.

`ci/xnu/build_source.py --continuous-boot` retains the proven entry and memory path, restores XNU's normal exception vector base before `machine_startup`, and continues through scheduler, IOKit, root selection and init loading. Successful checkpoints only write the reserved RAM trace. They do not stop or reboot the machine. Existing failure gates still stop invalid memory/platform inputs.

The ArmCpu observer is adapted from UniXNU-umi3eb72578597d2ca16471b172d45eddc096a34a04. It observes the actual PPI27 handler, `rtclock_intr` return and `ICC_EOIR1_EL1`; it leaves GIC and timer behavior unchanged. It samples the first, second and eighth IRQ, then every64IRQs. The typed packets bind boot identity, candidate tag, sequence, payload XOR and magic inverse. Writes fit inside the existing4KiB mailbox mapping. A separate first-panic record retains the caller, reason, exception register snapshot and up to111bytes of literal format text.

`--user-fixture` additionally compiles XNU mockfs support, runtime trust-cache support and the framework's own signed static ARM64/iOS17 process. Its two intended syscalls are getpid and HW_PAGESIZE. This is a minimal userspace instrument, not Apple launchd or an iOS interface. The exported process and trust cache still need a new bounded RAM payload/ADT/loader contract before any phone execution; the frozen CPU0 payload deliberately rejects those additions.

The main packets occupy offsets512..832 (IRQ),832..960 (startup), and1024..1280 (panic). Original256B header and platform/early-exception records retain their offsets. A matching recovery reader is required for the enlarged records. The old512B reader cannot qualify new runtime packets.

Validation so far: exact seven-file XNU patch application and reversal, exact adapted ArmCpu patch application, real packet writer under ASan/UBSan, and ARM64/iOS17 compilation of the writer and observer. Full XNU linking, own-process signing, enlarged payload and phone behavior remain untested. No Github push or Actions dispatch is implied by this local profile.
