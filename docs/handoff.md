# XNU host integration (X-06)

The independent `cas-x06-host-fixture` profile loads the actual X-02 unstripped
XNU and X-03 CPU0 ADT, constructs Darwin boot_args, then frees its allocation.
ExitBootServices and DarwinEnter are compiled out. The 32MiB FAT retains the
CasDiag, Android reset and firmware-return menu entries and injects the pinned
DarwinBoot v2 through the existing UniXNU driver mechanism. The accepted 16MiB
profile and diagnostic firmware remain unchanged.

This profile is **host-only**: its hash-bound ADT fixture has no entropy, and its
selected physical range starts at 0x100004000. The exact X-05 range started at
0x100000000, so its success does not cover the shifted tail. It cannot be promoted
to a phone candidate. See config/handoff-contract.json for the full ABI and gates.

`scripts/cas_handoff.py` stages an immutable build overlay from clean DarwinBoot
d099354d0eab34df5088ca1ba89c9c88bc3260bb, checks the actual source hash and saves
the diff and license. It does not modify the upstream checkout. Source and build
validation are separate from physical hardware observations.

`scripts/check_handoff_host.py --driver-build BUILD --evidence-root EVIDENCE
--out NEW_OUTPUT` runs the entire transformed loader C code with host UEFI service
doubles. It verifies copied segments, BSS, ADT, boot_args, release, failure paths
and mailbox CRC. Only register reads and host service bindings are substituted.
The target ARM64 EFI is built separately; a host pass does not execute that EFI.

The 256-byte mailbox is inside the existing reserved NC log region. C records
cover bytes0..231 with CRC32 at232; the assembly stage/inverse pair at240/248 is
outside that CRC and must be checked independently with the current boot stamp.
Stages1..5 describe loader work. A loader green bar does not prove XNU entry.
The separate optional source-kernel `--entry-trace` adds stages0x40/0x41 at
`__start` and `start_first_cpu`, using only reviewed scratch registers before MMU
enable. It retains the fixed 10063/4KiB baseline and hashes its exact input/patch.
No post-MMU XNU trace, CPU0 runtime, or iOS success is claimed here.

The existing macos-15/Xcode16.4 encrypted source-build workflow accepts the
explicit entry_trace option. A local Mac is unnecessary. Preserve remote run,
commit, artifact digest, decrypted kernel and matching symbols for every build.

Reference: reAAAq/UniXNU-umi@3eb72578597d2ca16471b172d45eddc096a34a04;
its scratch-register/phase separation informed this integration. Its ramoops
address and page-table mapping are not copied. Original additions follow the
repository BSD-2-Clause-Patent license; Apple source patches retain APSL context.
