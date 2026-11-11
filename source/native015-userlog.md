# Native015 UserLog source overlay

Native015 compiles the existing `kernel/QcomUsbUserLog.c` through the USB
builder's `--userlog-compile-record` option with this target's generated
`build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/subr_prf.o.json`.
The normal PE compile record, existing USB archive output and kernel archive
plan remain in use. Target compilation and runtime receipt are pending.

`inputs.json` preserves the original `framework` and `usb` dependency records
as base origins. `userlog_target` separately records the actual overlaid trees,
base and actual source hashes, patch, commands and XNU dependencies. The single
`source/native015-userlog.patch` has two applications:

- Framework: only `scripts/build-qcom-usb-debug.py`.
- USB: only `kernel/QcomUsbUserLog.c` and `scripts/build.py`.

Preparation applies each selected set with `git apply --check --index`, then
compares its exact resulting Git tree. The wrapper accepts staged USB changes
only when `UNIXNU_QCOM_USB_USERLOG_TREE` equals that reviewed tree and no
unstaged or untracked source exists. Its build report distinguishes the base
commit/tree from the actual source tree and directory. Ordinary builds without
`UNIXNU_QCOM_USB_USERLOG=1` retain the original clean-source/PE-only path.

## Temporary non-root producer

The existing Root path continues to accept the original wire. A non-root call
requires the boot-selected `btlog=<40hex>` to match the current process's native
20-byte CDHash. XNU12377 actually declares `cs_get_cdhash` in
`bsd/sys/codesign.h`, with a strong definition in `bsd/kern/kern_cs.c`;
`csproc_getcdhash` is absent. There is no private iOS16 ABI or replacement getter.
Missing, malformed, NULL or different code hashes return EPERM before copyin.
The native `PE_parse_boot_arg_str` forces string parsing and a 42-byte buffer
rejects a longer hash instead of accepting a truncated 40-hex prefix.

The OID adds `CTLFLAG_ANYBODY` so this explicit producer can reach the handler.
The handler then requires one valid STATUS frame, `bytes <= 128`, sequence and
offset zero, total equal to bytes, sessionHi `0x51425455`, and the actual PID in
sessionLo and in this exact ASCII payload:

```text
[qbt-ui] p=%u pid=%u a=%u b=0\n
```

`\n` denotes one terminal newline. Decimal fields use the canonical `%u`
representation. Stages 1 and 4 require `a=1`; 2, 3, 6, 7, 9 and 10 allow 0 or 1;
5 allows 0 through 5 or 7; 8 allows 0 through 3. Other stages, text, values,
PID/session mismatches and non-STATUS frames are denied. No `btlog` keeps the
handler Root-only. The daemon's UID, entitlements, PIN permissions and original
notification authorization are preserved.

The original 48-byte, twelve little-endian u32 wire, CRC32, 1024-byte maximum
Root payload, 4 MiB FIFO and atomic `qcom_usb_debug_try_record` remain. The
producer uses one sysctl call; it never retries or sleeps. The existing
`user_oslog` unsupported capability is unchanged and describes the private OSLog
collector capability.

## Qualification and collected evidence

Host checks exercise the wrapper's actual BSD record routing and the extracted
handler with its original wire/FIFO under ASan/UBSan. Native proc, CDHash,
bootarg and copyin calls are explicitly host fixtures. They do not prove target
compilation, runtime registration or USB receipt.

The target checker validates the exact compiled source and record flags,
arm64e/iOS27 object, archive membership, strong definitions and imports. It
checks the original SYSCTL/SECOND 24-byte linker-set record, its relocations to
`sysctl_register_oid_early` and the OID, OID permission flags, and the actual
native initializer/handler/parent pointers. XNU12377's final pointers use
ARM64E_KERNEL chained format 7. Hidden local strong functions are accepted;
weak definitions and debug STABS entries are not treated as code definitions.
No additional registration mechanism is introduced.

Collected artifact paths are:

- `userlog-source-selection.json`, `userlog-target-qualification.json`.
- `userlog-source/native015-userlog.patch`, the wrapper/checker and exact USB
  source files under their original relative names.
- `archives/qcom-usb-debug/build-report.json` and `libQcomXnuUsbDebug.a`.
- `usb-attempts/archive-<id>/QcomUsbUserLog.o` and its compile log.
- `objects/bsd/DEVELOPMENT/subr_prf.o.json` and the actual native kernel at
  `build/kernel/kernel.macho`.
- `userlog-producer-host/host-producer-report.json`, generated C and host logs.

The original report's evidence directory ends in `archive-<id>/module`.
Artifact mode therefore maps its parent directory name to `usb-attempts`; it
continues comparing the original compile command's `-o` to that original
evidence directory, and never rewrites the report, record or command.

Run preparation and host checks in a new local output directory. Compilation
uses the existing authorized native build command on the actual Xcode host:

```sh
python3 ci/native.py prepare --framework "$FRAMEWORK" --work "$WORK"
python3 ci/test-native-userlog.py --wrapper "$FRAMEWORK/scripts/build-qcom-usb-debug.py"
python3 ci/test-native-userlog-producer.py --component "$WORK/usb" --output "$WORK/userlog-producer-host"
python3 ci/native.py build --framework "$FRAMEWORK" --work "$WORK"
```

For Linux host checks, select a compiler with ASan/UBSan installed, for example
`--cc gcc`. The target checker can inspect collected bytes using the same
frozen CI source/inputs and existing helper:

```sh
python3 ci/check-native-userlog.py --artifacts "$ARTIFACTS" \
  --tree "$FRAMEWORK" --component "$WORK/usb" --llvm "$LLVM_BIN" \
  --kernel "$ARTIFACTS/build/kernel/kernel.macho" --out "$REPORT"
```

In artifact mode, an unassembled framework can use the frozen Device archive
helper from this CI snapshot; its hash must equal the selected Native014 helper.
The original target compile paths may be unavailable locally. The builder's
pure argument reader skips path availability only for this read-only record
inspection; actual compilation retains that check.

The final boot package must use the actual signed Native015 daemon CDHash in
`btlog`, never a placeholder. Root owns that signing/package step and the exact
997-byte to 1017-byte argument replacement. This source preparation performs
no CI dispatch, signing, packaging or device operations.
