# cas RAM memory and log diagnostics

[简体中文](cas-diagnostics.zh-CN.md)

The independent `cas-diag-012` profile builds a RAM-only EFI diagnostic application. Its purpose is to collect current memory evidence, refuse unsafe high-memory access and test recovery of a structured RAM log. It does not load XNU. The default `cas.dsc` / `cas-complete-001` route keeps its original behavior.

## Build and package

Use the registered sibling source directories and the existing pinned environment. Commit changes before building a candidate: the packer requires clean source identities matching the successful firmware build. Every output directory must be new.

```sh
source environment/cas-env.sh
python3 -m unittest discover -s tests -v
python3 scripts/build_firmware.py --uefi ../uefi-cas --diagnostics \
  --out "$HOME/cas-work/builds/cas-diag-012"
python3 scripts/package_boot.py --uefi ../uefi-cas \
  --firmware-build "$HOME/cas-work/builds/cas-diag-012" \
  --environment environment/cas-env.sh --stock-dtb /path/to/private/boot-02.dtb \
  --efi-ram-fat --diagnostics --out "$HOME/cas-work/artifacts/cas-diag-012"
```

The unchanged stock-DTB identity and temporary-boot/recovery procedure are described in [build](build.md) and [operations](operations.md). Packaging binds the diagnostic firmware, shim and FAT profile; a mixed default/diagnostic build is refused. The FAT admits one EFI path, `\EFI\Tools\CasDiag.efi`, plus ordinary Android reset and firmware-return actions. No donor/kernel/root volume is packaged. No script runs ADB/Fastboot.

The diagnostic DSC/FDF uses the existing bounded heap and physical-storage-free driver selection. It adds CasDiag as a source EFI module, sets `CAS_XNU_DIAGNOSTICS=1`, preserves a log tail and bounds the ordinary serial log below that tail. It does not import high RAM into GCD or the UEFI allocator.

## Menu and current evidence

Volume keys select an item; tap Power to run it; hold Power to return. The recovered-log viewer shows bounded pages, with tap Power for the next page and hold Power to leave. Warm reset occurs only through its explicit menu item.

| Item | Action |
| --- | --- |
| Enumerate | Inspect the current bounded FDT copy, RAM/reservation entries, current UEFI memory descriptors, GOP and the bounded raw SMEM RAM table. Save a structured record in the firmware-owned RAM slot. |
| Probe high 256 MiB | Re-enumerate and check all gates before asking UEFI for an exact allocation. A refusal performs no high-memory access. |
| Read log | Show the previous record's validation status, build, sequence, flags and boot stamp, followed by paged current/history text. |
| Warm reset | Commit a reset-request marker, issue `ResetSystem(EfiResetWarm)` and later check whether the marker survived. Survival is not assumed. |
| Return | Commit an exit marker and return to UniXNU. |

Enumeration does not modify the FDT, SMEM, storage, variables or allocator topology. It uses ordinary bounded pool allocations for its snapshots and writes diagnostic text only to its own RAM log. The diagnostic-only memory-map library maps the existing SMEM reservation `[0x80900000, 0x80b00000)` as noncached reserved memory. SEC sets RO/XP before DXE. The generated default map stays unchanged; the allocator remains 128 MiB. Before any SMEM read, the app requires the **current** `qcom,smem` FDT node to resolve its `memory-region` phandle to exactly that reserved, `no-map` region, current UEFI reserved/UC coverage, and Memory Attribute Protocol confirmation of RO/XP without RP. No hardware lock or SMEM write is issued. Unresolved FDT reservations deny probing. Bounded dynamic reservations exclude their entire allowed allocation ranges.

`SmemCore.c` reads only allocation metadata and item 402. It supports SMEM heaps 11 and 12 (the v12 **uncached global partition**), and RAM table versions 1/2/3 with 64/72/72-byte entries after a 24-byte header. Auxiliary regions, cached-only items, unknown versions, unsupported duplicates/overlaps (see typed containment below), bad magic/count/padding and overflow are refused. All reads stay inside the 2 MiB reservation: at most 4096 bytes per read, 1200 read calls and a 64 KiB trace per observation, at most 32 RAM rows. Two observations must have identical ordered offsets, lengths and bytes. This detects changes in the observed data; it does not establish an atomic snapshot or replace Qualcomm's remote locking protocol. Other items' payloads are not read or logged.

Wire formats were checked against [Linux v4.19 SMEM](https://github.com/torvalds/linux/blob/v4.19/drivers/soc/qcom/smem.c), [fixed LK RAM table definitions](https://github.com/msm8916-mainline/lk2nd/blob/5e6f1adcb1c070e28a70dc1f3d807ce9538f61c5/platform/msm_shared/smem.h), and [Qualcomm's v3 RAM table definitions](https://www.mail-archive.com/u-boot%40lists.denx.de/msg570801.html). The bounded decoder was written independently and does not incorporate those drivers.

Every numeric RAM row is logged. Only valid SDRAM/system-memory rows with explicit read/write attribute and DEFAULT/APPS/shared domain can contribute evidence to the high-memory gate; other rows and unavailable tails are full exclusions. DEFAULT domain0 is unspecified, not allocation permission. MODEM-only/unknown domains and unsupported attributes never qualify. Current FDT, UEFI allocation ownership and all exclusions remain independently required.

The copied FDT is accepted only at the validated cas address and size. Its memory and reserved-memory cells must match the supported two-cell widths; every parsed range is checked. The output includes the FDT CRC and full UEFI descriptors with type, physical/virtual address, byte length and attributes. GOP is read for the current framebuffer exclusion. No historical RAM bank alone establishes ownership.

## Probe contract

The fixed candidate is `[0x100000000, 0x110000000)`, 256 MiB inside the larger historical 512 MiB investigation area. It must be aligned to 2 MiB and fit one current FDT bank, one current RAM-partition entry and one UEFI conventional write-back interval. All current FDT reservations, noneligible SMEM rows/tails and the current framebuffer are excluded; a broad low-4-GiB exclusion also covers firmware, stack, input corridors and control/log RAM. Arithmetic, counts, overlap and missing-source checks fail closed.

The current firmware publishes only its existing **128 MiB low allocator window** so this profile is expected to **refuse the high-memory test**. That is diagnostic evidence, not a high-memory pass. No `AddMemorySpace`, historical-bank import or direct unowned readback is used to bypass it. X-05 must collect the actual output before deciding how a later allocator candidate should change.

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
  --build cas-diag-012 --current-boot 0x1234
```

This command reads a file only; it does not obtain a physical-memory dump. Diagnostic state is not yet an XNU panic/logging adapter; that connection belongs to X-06.

## Verification limits

The portable `DiagCore.c` is the actual guard, log codec and allocation/cleanup transaction used by the EFI program. Twenty-four host tests compile and call that C code, independently compare its record format with the Python reader, corrupt/truncate records and inject callback failures. The SMEM tests compile and call the actual `SmemCore.c` with synthetic memory, inject metadata/data changes and read failures, and check malformed/unknown layouts, overflow, cached/auxiliary refusal and hard budgets. A separate C harness compiles the real diagnostic mapping wrapper and checks that only the SMEM HOB option changes, a second call is stable, a wrong memory type is refused, and the conventional heap remains 128 MiB. Actual test counts and build results are recorded in the candidate report.

The complete firmware/image build report identifies actual artifacts separately. Compilation and host tests do not prove current cas allocator ownership, high-address cache behavior, physical keys, warm-reset retention or XNU/iOS startup. Those results must come from a new, hash-bound X-05 device observation; the prior accepted UEFI image's success cannot be reused for this profile.

## 003 early screen log

One temporary hardware boot of 002 displayed stages 1–7 without a menu; holding
Power recovered Android. Stage 7 follows SEC decompression; stage 8 is actually
in CasBdsInitialize. The cause remains unknown. Only the diagnostic profile
selects SerialDiagLib in 003. It preserves the original case-sensitive RAM log
and mirrors the last 16 lines of 56 characters onto the inherited framebuffer,
with uppercase display glyphs. Text state occupies existing log+0xc00; compile
time checks keep it disjoint from display/input state and the raw log. Bounded
GOP blits and the inherited refresh path preserve the bottom stage/fault bands;
mirroring stops at stage 8. SMEM mapping and the 128MiB allocator remain as in
002. The new screen mirror still requires hardware validation.

## 004 exception summary retention

The 003 photo confirmed screen mirroring and the final CPU exception assertion,
but the stack dump scrolled ESR/ELR/FAR off screen. 004 latches the mirror at the
line-start `Stack dump:` marker, including split serial writes. The exact raw
RAM logger continues receiving the full stack and assertion. Text state is now
version2/936 bytes in the same slot. Exception handling, SMEM permissions and
the allocator are unchanged. The original exception cause awaits actual evidence.

## Diagnostic 005 collection changes

Dynamic reserved-memory nodes are bounded by their current `size`, optional
power-of-two `alignment` and complete `alloc-ranges`. The diagnostic excludes
each entire allowed range; it never guesses where a dynamic region was placed.
Missing or invalid bounds keep the FDT incomplete. SMEM provider, UEFI coverage
and actual RO/XP protocol results are printed separately before any permitted
bounded SMEM read. The allocator and mapping/permission requirements are unchanged.

The log viewer queries the actual console dimensions and explicitly splits
payload rows to fit. Headers show byte offsets. Short Power advances, Volume up
returns to the previous page, and holding Power returns to the diagnostic menu.
This changes only presentation; original RAM log bytes remain unchanged.

## Diagnostic 006 permission preparation

The final 005 hardware page reported mask13/15: provider and UEFI reserved/UC
passed, but live permissions were XP only. The pinned DXE NX policy replaces
access bits, so SEC's RO cannot be assumed to persist; no per-stage hardware
trace has independently identified the last permission writer.
After complete current FDT/UEFI and provider checks, 006 queries the exact 2MiB
SMEM reservation. Only a successful query with no RP or unknown bits permits
adding RO|XP through the Memory Attribute Protocol. It never clears attributes,
adds mappings, imports GCD memory, writes SMEM, or changes allocator topology.
It persists intent before the setter and queries again after success. An already
protected region skips the setter but is still requeried. Readback must equal
RO|XP before the existing bounded reader runs; errors or mismatches refuse it.

Host tests extract and compile the actual EFI permission function and inject
protocol/query/set/readback failures, checking order, exact range, idempotence
and RP refusal. These are not hardware page-table or phone execution tests.
The next hardware attempt is enumeration only: confirm 006, before/verified
permissions, SMEM output and final mask. The 128MiB allocator is unchanged;
complete enumeration would still not prove 256MiB owned memory.

## Diagnostic 007 format rejection location

006 hardware confirmed SMEM permissions changed from 0x4000 to 0x24000 and all
three pre-read gates passed. The reader then returned CdSmemFormat (2), heap12,
without locating the RAM item; the final mask was13/15. Android recovery was confirmed.
007 preserves the 006 predicates and read sequence. It prints stable check IDs
and names, pass, read count, trace bytes, and the last requested offset/length.
It copies at most48 bytes of metadata already read, with no extra physical read.
RAM item payload/names are excluded; a failed callback leaves a zeroed sample.
Host tests compare the actual007 C with frozen006 C on valid and malformed inputs,
including return codes, parsed Info and ordered callback requests.

Key checks:11 primary-header (initialized/reserved),20 directory-header
(magic/version/reserved),22 directory-entry (duplicate global/flags/reserved),
25 partition-header (magic/hosts/size/reserved),28 item-header (canary/reserved).
Metadata is displayed as little-endian32-bit words; +xx is the byte offset from
the last-read offset. Unknown layouts still refuse access pending interpretation.

## Diagnostic 008 v12 directory compatibility

007 hardware check22 captured a48-byte directory entry: offset0x1000,
size0x146000, flags3, hosts0xfffe/0xfffe, cacheline32, seven zero reserved words.
Linux v4.19 defines directory flags as unused and does not gate global lookup on
their value.008 records them without interpreting them as RAM-row permissions.
007's Part>=8400 reused the v11 allocation-table limit.008's v12 profile reserves
the first4KiB and final4KiB directory, requires8-byte partition alignment and
full containment in between. v11 retains its8400-byte allocation-table exclusion.

A unique global partition, zero reserved words, matching partition magic/hosts/
size, uncached item bounds, read budgets, two equal observations and RAM-row
ownership checks remain required. Output adds SMEM global offset/size/flags/
cacheline; rejection telemetry remains available. Synthetic tests reuse only the
observed directory words; the unobserved partition/item bodies are synthetic.
Reference:[Linux v4.19 SMEM](https://github.com/torvalds/linux/blob/v4.19/drivers/soc/qcom/smem.c).

## Diagnostic 009 first PC line retention

008 hardware showed a CPU exception after stage7, ELR0x9bb09500/ESR0x02000000,
then recovered Android by holding Power. The photo lacked a runtime module base,
so static module maps cannot uniquely symbolize that physical address.
009 latches the screen after the first complete source line beginning PC 0x,
preserving preceding load/exception context. The Stack dump latch remains the
fallback when no PC line appears. The raw RAM serial log still receives every
register and stack line. Exception handling, SMEM, permissions and layout are unchanged.
Text state is version3/944 bytes inside the existing log+0xc00 to+0x1000 gap.

Host tests cover split writes, no latch before newline, CRLF/wrapping, embedded
PC text refusal, corrupt-state recovery and complete raw logging after the
screen freezes.009 gathers missing exception evidence; it does not establish a
CPU exception fix. If the menu is reached, enumerate only. If an exception
recurs, capture the first PC and preceding loader context, then recover Android.

## Diagnostic 010 partial numeric RAM rows

009 hardware reached heap12/RAM table3 and refused check44 (overlap), with
item offset0x7ab0/2328 bytes; the conflicting rows were not yet observable.
010 records the decoded prefix length and both zero-based conflict indexes.
On failure the app prints only each decoded row's base/size/available and
attribute/category/domain/type, explicitly marked NO OWNERSHIP. It never prints
names, opaque item payload or unparsed rows. The rejected row is included in the
bounded prefix (at most32). Each observation resets the prefix/index sentinels,
so a second-pass metadata failure cannot expose the first pass as current rows.

No acceptance predicate, physical-read order, permission gate, memory map or
allocator changes. Actual-C tests compare status, Info and callbacks with009,
plus partial/count bounds, conflict fields, work reuse and two-pass failures.
Reaching an overlap is not proof it is safe to remove the gate: interpret actual
row types against format sources, then independently prove owned memory.

## Diagnostic 011 first-PC plus numeric exception summary

010 failed before CasDiag: the observed ArmCpuDxe load0x9bb05000/entry0x9bb061cc
and PC0x9bb07500 bind to actual RVA0x2500 in UpdateRegionMappingRecursive.
The static instruction is ldr x0,[sp,#0x20]; no runtime opcode/ESR/FAR/SP was
captured.009 and010 contain identical ArmCpuDxe bytes, so this does not identify
a new partial-row bug or prove the MMU helper caused the fault.

011 retains the unchanged16-row loading/first-PC screen. After that latch it
parses only the pinned serial handler's exact SP/ELR/SPSR/FPSR and ESR/FAR source
lines, up to16 hex digits per numeric field, once only. Four extra rows above
the frozen context display known fields; absent fields are question marks and
the header says PARTIAL. Printed addresses are never dereferenced. Raw RAM
logging, interrupt/reentrant guards and the stage8 screen stop remain in force.
If another exception banner appears before all fields, it marks NESTED/UNBOUND
and refuses later values instead of pairing a nested ESR with the first PC.
Version4 state is1024 bytes, within the same log+0xc00..+0x1000 gap. This is a
diagnostic improvement, not a CPU/MMU fix; SMEM/permissions/allocator are unchanged.

## Diagnostic 012 typed RAM table containment

011 hardware captured table3/count6, partial5 and overlap indexes4,0. Row4 is
SDRAM/category14/type2 (BOOT_REGION_MEMORY1), entirely inside row0's
category14/type1 system RAM description. The old blanket overlap check rejected
this known carveout before decoding the final row or performing observation2.
The pinned LK and author-submitted Qualcomm v3 definitions also identify domain0
as DEFAULT/unspecified, not a MODEM-only restriction or a UEFI ownership grant.

012 supports only a known Boot1 row with known attribute/domain, properly and
wholly contained in a supported system row. It remains a FULL exclusion, even
if available_length is zero. Duplicate/crossing banks, unknown relationships,
other unqualified overlaps and invalid ranges still refuse. System evidence is
bounded SDRAM14/type1/RW1/domain0,1,3 with a nonzero page-aligned available prefix;
MODEM2/unknown domains, other kinds/attributes and unavailable tails are excluded.
Both equal ordered observations and all current FDT/UEFI/reservation gates remain.
The firmware allocator is still128MiB: this change is parsing/evidence, not a
256MiB ownership pass, RAM publishing or MMU fix.

New actual-C tests compare frozen011, exercise the documented exception in both
row orders, default domain, proper boundaries/unknown refusal, two-pass changes,
and prove an otherwise covered candidate is denied by the full Boot1 exclusion
or missing UEFI ownership. The six-row fixture contains photo-confirmed critical
relationships plus explicitly SYNTHETIC unobserved fields/tail; it is not a raw
capture of the phone's full RAM table.
