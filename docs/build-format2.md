# Current format2 build

The normative upstream baseline is UniXNU@ae687009348296e97e49ce6e925795044f363ce6.
The device owns wiring and memory layout; the independent SM8250 common repository
owns SoC facts; framework/components own generic logic. Preserve iOS17, continuous
CPU0 startup and deferred SMP. The frozen EFI build remains documented in build.md.

```sh
./unixnu defconfig cas_continuous_defconfig -D ../UniXNU-cas
./unixnu fetch -D ../UniXNU-cas
./unixnu status --json -D ../UniXNU-cas
./unixnu tree -D ../UniXNU-cas
./unixnu build kernel -D ../UniXNU-cas -j4
```

The CI source manifest pins the reviewed framework fixes and canonical component
revisions; `feeds.toml`/`unixnu.lock` pin SoC and source references. Development
overrides belong in ignored `local.toml` or explicit `--local-module soc:sm8250=PATH`.
Output is `out/cas/ipados-17.7.11-21H461/cas_continuous/`. Kernel build includes prelink
against the hash-checked matching kernelcache. The standard series order is arch,
SoC, OS, then device. Do not replay the historical late-patch wrapper mutations.

`build-cas-armcpu-overlay.py` stages declared diagnostics only in generated output,
then the canonical ArmCpu recipe builds the real driver; the source module remains
untouched. Multi-bank ADT is explicitly enabled, preserves holes/reservations, and
does not grant ownership. Actual UEFI allocation/held-probe checks remain required.

The GitHub job uses macos-26/Xcode26.6, records the actual SDK/compiler and builds
the continuous kernel plus own signed iOS17 user fixture. Only encrypted owner
results are uploaded; private keys, IPSW/DTB/firmware and raw device data stay out
of Git. A successful host build is not a device, root or interface result.

Firmware and boot image assembly must retain the cas BootShim, correct EDK2 ABI,
loader ownership guard and bounded input audit. The generic packer cannot replace
the checked cas packer by simply changing a command. Never run `userdata` for this
RAM-only target. See [status](porting-status.md) and `porting.toml` for actual limits.

The guarded firmware step also requires `--edk2 UEFI/Mu_Basecore`, `CAS_ADT_AUDIT`
and `CAS_CPU0_CONTRACT` referring to a new private candidate. The former is generated
by `scripts/audit_cas_format2.py` from the actual canonical ADT; the latter must
bind the actual new kernel hash. Missing or mismatched inputs refuse the build.

## Minimal RAM root

The upstream mockfs fixture accepts an own signed static Mach-O as the memory
device backing `/sbin/launchd`. Build it with `build-native-user-fixture.py`, then
pass the resulting process as `--data-ramdisk` and its bundle as `--trust-cache`.
The candidate command adds `rd=md0`. The device loader accepts this variant only
through an explicit contract binding the kernel, ADT, root and trust-cache hashes.
The process is a minimal syscall test, not Apple's launchd or an iOS interface.
