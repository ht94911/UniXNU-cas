# iOS 27 native adapter

The adapter targets the original iPhone OS 27.0.1 arm64e `bluetoothd` from 24A446:

- SHA-256: `cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1`
- UUID: `8E49197B-E6BC-3F3B-A8D3-DF4BEDD37F01`

`tools/patch-ios27.py` creates the patched executable and a JSON report.
`tools/build-type0.py` builds and signs `/usr/lib/QcaBTType0.dylib`.
`tools/build-userland.py` signs the native daemon with its original identifier
and entitlements, then stages BlueTool, bluetoothd and the capability library.
The board runtime builder installs their files, firmware and signed launchd
cache in the Data addon. Apple binaries remain external inputs.

## Routing

Every product reaches the original constructor with controller type zero, and
a product no factory branch lists falls through to the last constructor. The
daemon's virtual machine check returns false, so chip initialization runs.
Registration accepts zero and preserves the native 1 through 6000 range. Type
zero clears the vendor operation table, four vendor hooks and both vendor
opcodes. Both callback registration paths retain their generic callback.

Optional command 0x0c5b follows byte 18, bit 3 of the native support bitmap;
unsupported controllers advance through the native initialization chain. Host
flow control and erroneous data reporting may be rejected. H4 pre-stop skips its
empty extension and retains normal transport close.

The consuming nezha Wi-Fi023 RAM-only profile permits `-34019` to enter the
original root read result, length and generation checks. Six main root reads
and two non-connectable identity reads follow this policy. Valid stored
material is retained. The ten guarded stores continue only on status zero or
`-34019`; other errors, including CF allocation failure `4`, keep their original
error continuation. Guards preserve the returned status, frame, LR and
callee-saved registers and use only volatile `w8` and NZCV.

Apple defines `-34019` as `errSecUpgradePending`: the Keychain database needs a
schema migration while the device is locked, and a client should wait for an
unlock notification and retry. See the fixed
[Security declaration](https://github.com/apple-oss-distributions/Security/blob/066b1b2826fde61a7da2622197735545a6715c44/base/SecBasePriv.h#L88).
The RAM-only profile's permission to continue is separate from that status
definition; it does not establish the device's actual lock or migration state.

Cloud keypair generation and native IRK/DHK/NC derivation failures keep their
nonzero result. The common failure exit retains `0x82`; NC load preserves its
result, including the original `0x67` regeneration path and `0x82` failure path.
`arc4random_buf` supplies the native random material and has no return status.

The existing RAM candidate still suppresses the separate conflict handler for
new root material while old paired-device records contain nonempty fields.
Persistent bonds, restart restoration and root-generation consistency are
pending. Native type 8 and type 18 stores already lack result checks, so this
policy does not establish complete persistence. It also does not identify the
cause of the observed keyboard or numeric-comparison pairing failures.

nezha Wi-Fi023 的独立 RAM 策略仅允许 `-34019` 进入原读取/长度/生成判断，保存
只有 `0` 或该状态可继续，其余错误保留原失败路径。原 Cloud/IRK/DHK/NC 材料
检查及 NC 的 `0x67` 再生成、`0x82` 失败结果保留。root 更新与旧配对缓存冲突
的原处理仍按既有 RAM 候选抑制；持久 bond、重启恢复和材料代际一致性尚未验。

## Scanning and addresses

Extended scanning uses 0x2041 and 0x2042, because the controller rejects legacy
scan commands once extended advertising commands have been used. Apple's scan
extension is off.

A random address (0x2005) is queued before a scan can be enabled. Starting an
advertising set always sends its address (0x2035). If no resolvable address can
be generated, the refresh uses a non-resolvable address and completes normally.

## Capabilities

The adapter reads Supported Commands and LE Features. Extended scanning,
connection, advertising, periodic advertising, sync transfer, coded PHY and
2M PHY follow the controller's reports. Both LE feature callback versions
preserve their original status and data pointer for capability publication.

An asynchronous LE Read Number of Supported Advertising Sets query supplies the
broadcast capacity. The count is capped at the native array's six records.
Query failures reach the native error path. Model getters, configuration caches
and lower stack flags receive the same capabilities. The broadcaster refreshes
its mode before native GAP initialization, including after reopen.

The library runs during initialization. Normal packet traffic stays in the
native stack and UART.SK. The eleven replacement getters use four instructions
each. Dyld signs their USERLAND24 rebases with the original keys and diversity;
object vtable pointers remain intact. Callback and broadcaster entry patches
retain their original PACIBSP landing pads. Native function calls use signed
IA/0 pointers. The layout follows Apple's
[dyld header](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/fixup-chains.h).

## Runtime interfaces

The board exports /arm-io/bluetooth and a matching IOService named bluetooth.
Transport encoding 1 and speed 3200000 are four-byte little-endian properties;
the stable private address appears as local-mac-address. Public addresses use
network order; the Linux adapter converts to bdaddr_t before calling btqca.
This follows the address representation in
[BlueZ](https://github.com/bluez/bluez/blob/5.79/lib/bluetooth.c).

Read-only unions expose the signed capability library under /usr/lib and both
executables under /usr/sbin. The library mounts first. BlueTool retains its
original Mach service and program path, runs as root and keeps transactions
active. Its launchd job disables pressured exit. It starts the controller by
reading `kern.qbt_power_on`; connecting to UART.SK also starts it. A controller
already powered off acknowledges the sleep request successfully. Other native
jobs are preserved; Wi-Fi and Bluetooth share one signed cache and one appended
trust module.

## Build and verification

From the component checkout, with the original binary at `$BLUETOOTHD`:

```sh
python3 -m venv out/native-venv
out/native-venv/bin/pip install -r tools/requirements-native.txt
out/native-venv/bin/python tools/test-native.py "$BLUETOOTHD"
python3 tools/test-type0.py "$BLUETOOTHD"
python3 tools/build-type0.py
python3 tools/build-userland.py out/runtime --bluetoothd "$BLUETOOTHD"
python3 tools/patch-ios27.py "$BLUETOOTHD" --output out/ios27/bluetoothd.native
```

The instruction cases execute native routing, feature capture, authenticated
call boundaries, getters, lower setters, H4 pre-stop and root-material result
paths at two ASLR slides. Root cases use the original and patched AArch64 with
synthetic Keychain replies and material helpers to test valid reads, bad lengths,
the guarded store status policy, NC regeneration and real failure continuations.
Their case count is recorded in `out/native-test/report.json`. These models do
not verify target Keychain availability, cryptographic output or pairing.
The ten store guards occupy 240 bytes. The current locked adapter uses 764 of
the original 1120-byte executable tail, ending at `0x100b9be9c` with 356 bytes
remaining. Conditional guard branches stay local; ordinary `B` instructions
return to the original continuations. Material/cache helper code, PAC landing
pads and existing fixup authentication metadata remain unchanged.
PAC operations are modeled without device keys. The production C publisher
passes 58 HCI and policy cases under ASan/UBSan, including command and feature
absence, capacity limits, failed replies, submit errors and reopen mode changes.
The arm64e library compiles and passes code-signature verification. Reports
reside in `out/native-test`, `out/type0-test` and `out/userland`.
