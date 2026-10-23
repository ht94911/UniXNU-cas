# cas RAM memory and log diagnostics

[简体中文](cas-diagnostics.zh-CN.md)

The independent `cas-diag-001` profile builds a RAM-only EFI diagnostic application. Its purpose is to collect current memory evidence, refuse unsafe high-memory access and test recovery of a structured RAM log. It does not load XNU. The default `cas.dsc` / `cas-complete-001` route keeps its original behavior.

## Build and package

Use the registered sibling source directories and the existing pinned environment. Commit changes before building a candidate: the packer requires clean source identities matching the successful firmware build. Every output directory must be new.

```sh
source environment/cas-env.sh
python3 -m unittest discover -s tests -v
python3 scripts/build_firmware.py --uefi ../uefi-cas --diagnostics \
  --out "$HOME/cas-work/builds/cas-diag-001"
python3 scripts/package_boot.py --uefi ../uefi-cas \
  --firmware-build "$HOME/cas-work/builds/cas-diag-001" \
  --environment environment/cas-env.sh --stock-dtb /path/to/private/boot-02.dtb \
  --efi-ram-fat --diagnostics --out "$HOME/cas-work/artifacts/cas-diag-001"
```

The unchanged stock-DTB identity and temporary-boot/recovery procedure are described in [build](build.md) and [operations](operations.md). Packaging binds the diagnostic firmware, shim and FAT profile; a mixed default/diagnostic build is refused. The FAT admits one EFI path, `\EFI\Tools\CasDiag.efi`, plus ordinary Android reset and firmware-return actions. No donor/kernel/root volume is packaged. No script runs ADB/Fastboot.

The diagnostic DSC/FDF uses the existing bounded heap and physical-storage-free driver selection. It adds CasDiag as a source EFI module, sets `CAS_XNU_DIAGNOSTICS=1`, preserves a log tail and bounds the ordinary serial log below that tail. It does not import high RAM into GCD or the UEFI allocator.

## Menu and current evidence

Volume keys select an item; tap Power to run it; hold Power to return. The recovered-log viewer shows bounded pages, with tap Power for the next page and hold Power to leave. Warm reset occurs only through its explicit menu item.

| Item | Action |
| --- | --- |
| Enumerate | Inspect the current bounded FDT copy, RAM/reservation entries, current UEFI memory descriptors, GOP and the RAM-partition protocol when available. Save a structured record in the firmware-owned RAM slot. |
| Probe high 256 MiB | Re-enumerate and check all gates before asking UEFI for an exact allocation. A refusal performs no high-memory access. |
| Read log | Show the previous record's validation status, build, sequence, flags and boot stamp, followed by paged current/history text. |
| Warm reset | Commit a reset-request marker, issue `ResetSystem(EfiResetWarm)` and later check whether the marker survived. Survival is not assumed. |
| Return | Commit an exit marker and return to UniXNU. |

Enumeration does not modify the FDT, SMEM, storage, variables or allocator topology. It uses ordinary bounded pool allocations for its snapshots and writes diagnostic text only to its own RAM log. The current firmware selects **no SMEM/RAM-partition provider**. That absence is reported; raw unmapped SMEM is not read. A future provider's source must be reviewed before relying on its RAM-partition results. Dynamic/unresolved FDT reservations also deny probing.

The copied FDT is accepted only at the validated cas address and size. Its memory and reserved-memory cells must match the supported two-cell widths; every parsed range is checked. The output includes the FDT CRC and full UEFI descriptors with type, physical/virtual address, byte length and attributes. GOP is read for the current framebuffer exclusion. No historical RAM bank alone establishes ownership.

## Probe contract

The fixed candidate is `[0x100000000, 0x110000000)`, 256 MiB inside the larger historical 512 MiB investigation area. It must be aligned to 2 MiB and fit one current FDT bank, one current RAM-partition entry and one UEFI conventional write-back interval. All current FDT reservations, preloaded-image ranges and the current framebuffer are excluded; a broad low-4-GiB exclusion also covers firmware, stack, input corridors and control/log RAM. Arithmetic, counts, overlap and missing-source checks fail closed.

The current firmware publishes only its existing **128 MiB low allocator window** and has no RAM-partition provider, so this profile is expected to **refuse the high-memory test**. That is diagnostic evidence, not a high-memory pass. No `AddMemorySpace`, historical-bank import or direct unowned readback is used to bypass it. X-05 must collect the actual output before deciding how a later allocator/provider candidate should change.

The C transaction runs only after the gate succeeds:

1. `AllocatePages(AllocateAddress, EfiLoaderData)` must succeed at the exact requested address.
2. Write a physical-address-dependent 64-bit pattern across the allocation.
3. Clean and invalidate the data-cache range, fence, then verify every word; repeat with the complement pattern.
4. Clear the allocation, clean/invalidate and fence again, then free its pages.
5. Pass only after readback, cleanup and release all succeed.

An unexpected allocation address is released without being read or written. Ordinary write/cache/read error returns converge on cleanup/release. Host callback tests exercise these paths. They do not prove recovery from a CPU exception, reset or bus fault during a physical access; device observations remain required.

## Retained log format

The diagnostic-only slot is the final 64 KiB of the existing firmware-owned log reservation: `[0x9f970000, 0x9f980000)`. No new physical RAM is claimed. The diagnostic shim clears only the lower `0x30000` bytes and stores a fresh `CNTVCT_EL0` boot stamp in boot-info `Reserved[0]`. The serial logger excludes the retained tail. The default shim still clears its full log area.

The record uses little-endian fields and a **96-byte header**:

| Offset | Field |
| ---: | --- |
| 0 | Eight-byte `CASLOG01` magic, committed last |
| 8, 12 | `u32` version 1 and header length 96 |
| 16, 20 | `u32` total length and ASCII payload length |
| 24, 32 | `u64` sequence and boot stamp |
| 40, 44 | `u32` flags and CRC32 |
| 48 | NUL-terminated, zero-padded 32-byte build ID |
| 80 | 16 reserved zero bytes |

CRC32 covers the header and payload with the CRC field treated as zero. The writer invalidates magic, writes the bounded body and checksum, fences, then commits magic. Readers reject truncation, lengths/versions/flags outside the contract, bad padding, bad CRC and non-ASCII payload. Foreign build IDs are reported separately. Matching boot stamps indicate same-boot reuse; a different stamp is a **prior-boot candidate**, never an automatic persistence pass. ABL, secure firmware or a reset can discard or overwrite the slot.

The EFI viewer is the on-device recovery reader. A saved local slot can also be decoded without device access:

```sh
python3 scripts/read-cas-diag-log.py /path/to/saved-slot.bin \
  --build cas-diag-001 --current-boot 0x1234
```

This command reads a file only; it does not obtain a physical-memory dump. Diagnostic state is not yet an XNU panic/logging adapter; that connection belongs to X-06.

## Verification limits

The portable `DiagCore.c` is the actual guard, log codec and allocation/cleanup transaction used by the EFI program. Twenty-four host tests compile and call that C code, independently compare its record format with the Python reader, corrupt/truncate records and inject callback failures. The first complete source regression run passed 219 tests, including the existing ARM64 host regressions. The diagnostic input preflight and real strict-alignment BootShim compilation also passed.

The complete firmware/image build report identifies actual artifacts separately. Compilation and host tests do not prove current cas allocator ownership, high-address cache behavior, physical keys, warm-reset retention or XNU/iOS startup. Those results must come from a new, hash-bound X-05 device observation; the prior accepted UEFI image's success cannot be reused for this profile.
