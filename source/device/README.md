# Bluetooth Native015: existing USB UserLog input

Native015 retains the Native014 device implementation, configuration and typed
KeyStore selection. Its explicit CI overlay supplies the actual XNU12377 BSD
`subr_prf.o.json` to the USB builder's existing `--userlog-compile-record`
option. It also changes `QcomUsbUserLog.c` and the builder's scoped import
audit; the base USB source and these actual overlay bytes are recorded
separately. The 48-byte wire, existing 4 MiB FIFO and USB protocol are retained.

The original root path remains. For this temporary test package, a non-root
producer must match the boot-selected `btlog=<40hex>` code hash returned by
XNU12377's real `cs_get_cdhash(current_proc())`; missing, malformed, NULL or
different hashes are denied. The input must be one STATUS frame of at most
128 bytes, with the exact `[qbt-ui] p=%u pid=%u a=%u b=0\n` scalar schema,
the allowed ten stage/value pairs, and the current PID in text and session.
Without `btlog`, the handler still admits only root. The original daemon UID,
permissions and notification authorization remain unchanged.

XNU12377 registers the OID through the existing SYSCTL / SECOND linker-set
record and `sysctl_register_oid_early`. The target checker verifies object
membership, OID flags, function/argument relocations and final native pointers.
Target compilation, runtime registration and user-to-USB receipt remain
pending. The original private `user_oslog` capability remains unsupported.

# Bluetooth Native013: fixed KeyStore source selection

This independent source candidate retains the Native011/Wi-Fi023 board,
Bluetooth, USB, GPU and RAM Data inputs. Its KeyStore component is the five-file
public6ff snapshot with the reviewed local389c360 contract fixes, selected by
the local feed and the existing explicit module override. The source lock is
`package/keystore013-source-lock.json`; the selected C++ SHA256 is
`dc1c3f8a25b2e993b133e198eaf696084ccb5f911e55c074cdf5be4ffe571ba1`.

Native012's actual build still selected the framework's42ff submodule and was
rejected before prelink or packaging. Native013 reuses the normal component
builder and existing late initializer, checking the selected source, recorded
command, normal/late sections, C ABI getter and exact archive members. Target
compilation, runtime KeyStore behavior and K380 pairing remain pending. The
original Native011 kernel and fixed013 diagnostic package keep their identities.

# Wi-Fi019: reconcile canceled selection against the displayed network

018 hardware reproduced a canceled password sheet leaving an unjoined network
at the top of Settings, while a fresh successful GET103 still described the
real association. Its password prompt remained below five seconds. The phone
returned to Android, passed the short protection checks and was released.

The fixed 24A446 PasswordPromptViewController cancellation reaches the original
coordinator, publishes idle association state and refreshes the actual network.
CurrentNetworkState invokes its onChange callback even for an unchanged name.
CurrentNetworkModel then discards equal old/new names before checking whether
its separate displayed network still contains the canceled selection.

019 supplies one small comparison helper through the existing, signed-page
`native-text-fixup` ADT table. During an active association, or an actual network
change, it keeps the original comparison input. For an idle refresh whose
actual network is unchanged, it compares the displayed network with the fresh
value and lets the original callback perform any required reconciliation.
It uses original Swift string equality, retains all downstream joining guards,
and introduces no connection notification, credential change or radio request.
The input matches the exact cache UUID, original callback bytes and unused
header padding; file contents and the read-only system volume stay unchanged.

`tools/qualify-wifi27-ui-reconcile.py` executes the original guard and new helper
in Unicorn with explicit Swift helper mocks. It covers110 combinations including
nil, stale selection, active/successful joining and separate string storage.
This validates the guard rather than live Swift scheduling. The original idle
transition, refresh delivery, visual result and repeated manual joins still
require hardware testing. Keep the018 evidence and do not claim the UI fixed
before that test. Reuse Native022 and the user-selected complete GPU015 base;
only the ADT input and its payload/reader hashes change. Do not rebuild the
unchanged kernel or take the GPU018 phone window without a new Wi-Fi handoff.

# Wi-Fi018: distinguish current-network replies from canceled UI selection

Wi-Fi017 was tested on the user-selected complete GPU015 base. The user
confirmed two password prompts within 5 seconds and a real manual join/system
IP. Canceling another network's password prompt still leaves a wrong connected
row: the screenshot checks an unjoined network while the actual joined network
is in Other Networks. No new supported join/leave was observed in that window;
unsupported-SET console history had already reached its 16-type ceiling.

018 adds bounded, passive observations to the existing provider 6 snapshot.
GET1/GET9/GET103 record the exact source snapshot used by each successful
reply, including network identity tags, sequence, time, length, state and
result. A failed reply records no successful identity. Four most-recent
distinct unsupported SET types keep counts and explicit eviction accounting
without reading caller buffers. This is diagnostic coverage, not a claim that
the cancellation defect is fixed. It does not change joining, credentials,
link notifications, capabilities, timeouts, GPU or protected-volume behavior.

The additive `apple_io` object uses version 1 and the existing 2048-byte limit.
`queries` rows are `[type,seq,ticks,pid,result,state,bytes,ssid_length,ssid_tag,bssid_tag]`;
`unsupported` rows are `[type,seq,ticks,pid,bytes,calls]`. The tags are 64-bit
FNV-1a over the recorded SSID/BSSID source bytes, with hex strings on the wire.
Raw names, addresses, keys and passphrases are not added to telemetry.

The actual endpoint tests preserve current identity across scan-only selection,
check a concurrent state change during copyout, observe beyond 300 queries and
verify error/caller-buffer isolation. The production provider test validates
worst-width JSON, exact capacity and truncation against the unchanged budget.
Reuse the existing r008 collector and shared-cache decoder. New native and
hardware qualification are still required. Do not stop at this diagnostic:
use the returned values to resolve the driver-versus-currentNetwork cache split
and then repair the proven path.

# Wi-Fi017: original Sharing failure-path diagnostic

Wi-Fi016 reached Settings and completed a real manual target-network join and
system DHCP. The user's later correction confirms the password dialog still
waits over30 seconds; the earlier5-second response is superseded.
Four complete main-thread samples span about30 seconds in WiFiSettingsKit ->
+[SFPasswordSharingService passwordSharingAvailability] -> dispatch semaphore
wait. Each original call waits at most10 seconds for SFClient's asynchronous
request to com.apple.SharingServices. The original launchd plist maps that
Mach service to com.apple.sharingd. Its original proxy error completion returns
value1, the same fallback as timeout, and signals the semaphore.

017 changes only the existing board-policy job list to temporarily isolate
com.apple.sharingd in this independent RAM Wi-Fi diagnostic. Cross-device
sharing, including AirDrop, is unavailable in this candidate. This tests the
original error path without rewriting framework code, keychain behavior,
authentication, capability values or timeout constants. Why the live server did
not reply remains unresolved; this is not a claim that the delay is fixed.
The complete selected GPU015/CPU/USB/Metal dependencies, Wi-Fi/AES driver,
997-byte loader line and dual startup/Settings sampler remain unchanged.

Build a distinct Native021/candidate017 and verify the compiled policy before
hardware testing. GPU016 owns the next phone window under AGENTS128; this source
work does not reclaim the phone. Preserve the tested016 image and logs.

# Wi-Fi27 on the user-selected GPU015 base

The next independent candidate is Wi-Fi016. Its fixed base is GPU015 device
154f8a08e6800734408c99f909c28ceeb94caa3c, shared Adreno
04340ceb5f0ed270b8e3a8a92d606c2eb15678f2 and USB
9bfbf258d8356efcc0fbf03603d1518874829798 (Native033/run42363354270).
The user selected this tested diagnostic base; its glass performance remains
unaccepted. Import the complete CPU, thermal, GPU, USB, root-reference and
SystemOS shutdown dependencies. The private Metal014 resources must be combined
with the existing Wi-Fi addon locally. Keep the Wi-Fi/AES implementation and
independent4GiB RAM Data; no persistent Data map is added.

Wi-Fi015 stopped at the logo and never reached Settings. Its kernel was identical
to014, but selecting Preferences alone removed the SpringBoard startup trace.
The next build keeps Preferences in role S and SpringBoard in role T through the
existing focus options. Settings remains the wait-chain root. The existing
2-second capture worker is unchanged. Its cached full snapshots now export every
10 seconds between boot seconds120 and1200, independently of the30-second summary;
pass/attempt/busy metadata identifies stale captures. Busy, absent or partial
stacks remain inconclusive. No private credentials or network contents are added
to diagnostics, and no keychain or original timeout behavior changes.

The HT password-dialog delay is not fixed or accepted yet. This integration needs
its own native build, linked-image checks and hardware test. Older completed
qualification and phone commands are not replayed. The sections below describe
historical Wi-Fi candidates and their evidence.

# Wi-Fi27: next pre-password wait observation

GPU014 has completed its113 hardware trial. Other networks now appear within a
few seconds; choosing HT still waits over30 seconds before the password dialog.
The phone returned once through the controlled USB path, passed the short
Android/root protection audit, and was released for GPU009. No further phone
operation follows from preparing this configuration.

The next RAM test selects the real Settings process with the existing compiled
`boot-ui-focus2=Preferences` option. The existing wait-chain follower starts
from this slot (role S); placing Preferences only in focus3 would still follow
SpringBoard's peers. Settings and its observed IPC peers now receive the existing
main/worker stack capture. The runtime command line remains997 bytes, within
the payload builder's1000-byte limit and DarwinBoot's1024-byte buffer. All radio,
GPU, Home, root, RAM Data and reset options stay intact.

The worker samples every2 seconds. Console summaries appear every30 seconds;
complete main/worker stacks appear every60 seconds only between boot seconds120
and600. A30-second stall can fall between complete exports, so missing frames
cannot exclude a path. Start the manual step after a fresh, valid Settings
capture while this existing window is open; preserve the timestamps, partial
walk errors and actual peer metadata. This focus switch needs no new native code.
A final next candidate must first
use the latest actually hardware-tested GPU identity; GPU009 is not assumed
qualified merely because its build is ready.

Original24A446 WiFiKit copies the profile before prompting and that copy reads
`WFNetworkProfile password`, which can call `WiFiNetworkCopyPassword` and the
keychain. The original getter sets its fetched flag even for a nil result;
two password call sites are not evidence of two slow keychain queries. This
static path is not proof of where this phone spent30 seconds. The captured192 ioctl samples
were at most16 microseconds, but their budget was exhausted before the later
selection. Preserve unsupported-operation results and cryptographic checks;
do not manufacture a password, bypass the keychain or lower real timeouts.

The prior qualified014 implementation and evidence follow below.

# Wi-Fi27 GPU014 scan lifecycle and tested GPU integration

The request queue now retains one scan submitted while a real radio-on is
pending. It leaves that power command intact and publishes the scan completion
only when the active radio operation has produced real firmware results.
Normal scans are bound to the request actually taken by the worker, so a stale
Ready/Online publication cannot consume a newly queued scan. Radio-off and
terminal errors still cancel accepted work and expose no old scan cache.

The same production-code harness rejects the old queued-scan completion and
passes the updated queue, data authorization, power, cancellation and terminal
error cases with ASan/UBSan. Original ioctl size, credential and copyout checks
remain. Bounded scalar query timing is retained across scan epochs to distinguish
queries that block from waiting before an association request; no caller buffers
or credential contents are recorded. The password-dialog cause remains pending.

GPU board, input and build integration are copied from the tested persist008
source4e6bfc1623c90d6329731fee14dd1ee96f674711; shared Adreno740 is pinned to
30d45abeeb1d1cf5ca46f252434d9c041a72aa24. Its actual Metal/Turnip addon, trust cache,
root policy and seed identities match the existing private integration inputs.
Wi-Fi keeps its independent RAM profile, qualified SoftwareAES dependency and
one-shot RAM USB return. No active GPU worktree or persistent Data is modified.
Native019/run42277611453/attempt1, strict38 closure, final AES publication/KAT,
1/8 CPU, root policy controls, HID and4GiB RAM qualification all passed.
The fixed GPU014 image is bootff6bffbf7272/payloadc32a44152c76. Its completed113
trial retained initial system DHCP and improved the list to a few seconds, but
the password dialog still took over30 seconds; manual HT association was not
tested in that trial. The original image remains the recorded hardware identity.

Earlier sections below describe the prior candidates and their evidence.

# Wi-Fi27 AES dependency candidate013

012 hardware observations consistently found the platform defaults nub busy
while the Wi-Fi stack, controller and interface were quiet. System DHCP began
after the existing four-minute wait. The actual24A446 AppleImage4 import requests
IOAESAccelerator and passes UINT64_MAX to waitForMatchingService.012 returned to
Android, passed the short protection audit and released the phone to GPU.

013 adds the existing SoftwareAES component to this independent RAM candidate.
The original donor audit identifies181 authenticated slots,162 inherited kernel
methods,the0x120 AES/0x88 crypto layouts,and genuine protected constructors and
destructors. The arm64e header retains IOCryptoAccelerator as the declaring
class of getAlgorithmName;the old declaration produces a different PAC value.
The build refuses any compiled slot/import mismatch and never uses the ordinary
ARM64 recorded addresses. An IOBSD bootstrap loads and checks the real family
before registering the source subclass,using the existing late HID contract.

The algorithm remains the maintained software-key-only CBC128/192/256 provider.
Its crypto and buffer/completion self-tests run before publication. The existing
source corecrypto bootstrap,WebKit compatibility,Wi-Fi recovery/manual association,
GPU/Home inputs,and root/persistent-Data protections remain. The source-mode
archive and personality are explicit board integration;the old ordinary AES
prelinked-bootstrap gate is unchanged. Runtime KAT,quiet defaults,and prompt
system DHCP still require execution.013 is not yet a hardware or main-package
promotion.

# nezha iPhone27 GPU and Wi-Fi test integration

010 completed its actual combined trial. Initial WPA2 and system DHCP worked,
with the first DHCP packet 240.095 seconds after the owned network stack was
opened. The original modern auth8 request now passes the decoder and reaches
the worker. The manually selected network still timed out after vdev start;
its failed MLME phase was lost when the state changed to Failed. Pre-key
retirement completed and scanning resumed. This is decoder progress, not a
successful manual connection or a stable-network promotion.

The exact GPU002 Home bridge worked initially, then the user reported severe
password-entry lag and a later failed Calendar-to-Home gesture. The final
photograph shows Sleep/Wake hang detected. Its return addresses resolve in the
actual010 kernel to the power watchdog; the USB log ends during Idle Sleep.
That watchdog stack does not identify the service that stopped progressing.
The phone returned manually to Android, the human-selected short protection
profile passed, and the collector exited. The phone window is released to GPU.
Full root/Data hashes were not repeated or claimed.

011 fixes two source-reproduced timeout boundary errors: the station loop can
queue an AP response in progress() and immediately return Timeout without
consuming it; the WMI completion wait can replace a validated completion with
Timeout from the same poll. Consume queued station responses before testing
for absence and check the real WMI completion before elapsed time. Existing
one-second station/ten-second WMI deadlines, retry counts, packet validation,
transport-error propagation and key authorization remain. Old production
implementations fail the deadline controls; the fixed implementations pass
valid, rejected, malformed, missing and simultaneous-error cases under
ASan/UBSan. Whether this caused010's hardware failure remains unproved.

011 also retains the first failed phase/operation, bounded scalar management
RX rejection observations, and TX completion metadata before retirement. These
records contain no packet payload, SSID, peer address or keys. The registry
sampler now uses IOService::getServiceRoot(), as configd's IOService:/ lookup
does; the global registry root is a different non-service object.010's valid14
snapshot never measured root busy. The old production getter fails the exact
distinct-root control and the corrected getter passes. Actual root busy and
its relationship to the four-minute DHCP delay remain to be measured.

008 remains the verified radio/DMA recovery reference.010's auth8 compatibility
and exact GPU002 edge-origin input bridge are retained. This independent
branch still uses GPU main source675500a and its fixed GPU/Metal/GOP/USB/UFS
inputs, with the existing4GiB RAM Data. It never opens the phone's persistent
Data or modifies another session's source.011 has no hardware result or
main-package promotion; native build and final-image qualification are next.

The sections below retain the original device development history.

# Device template

Follow [Port a device](https://unixnu.com/porting/) from input collection through publication. Complete the [driver adaptation checklist](https://unixnu.com/porting/drivers) before selecting board-dependent components.

This template selects shared SM8550 and iPadOS 17.7.11 support. Complete the
hardware inputs before building.

1. Copy this directory to `UniXNU-<device>` and read `AGENTS.md`.
2. Set the board name, description, firmware paths, and complete component selection. Add the shared `UniXNURamDiskDxe` and filesystem dependencies to the firmware DSC and FDF using the [firmware integration guide](https://unixnu.com/porting/uefi-firmware#required-integration).
3. Pin a reviewed SoC commit in `feeds.toml`.
4. Rename `configs/example_defconfig` and `dts/board.dts` for the board, then update `kernel.defconfig` and `device_tree.source`. Fill the DTS model, compatible values, memory map, CPU selection, and peripherals.
5. List firmware sources and hashes in `proprietary-files.txt`.
6. Set partition geometry and payload inputs from the device's records.
7. Match the identity in `porting.toml` to `board.toml`. Cover every selected component and dependency, with board-specific ownership paths, findings and completion criteria.
8. Enable the template CI workflow and require its `Porting record` check on the official device branch.
9. Set `example = false`, select the defconfig, fetch, and inspect `status --json`. Commit the generated `unixnu.lock`; keep local overrides in the ignored `local.toml`.
10. Complete the shared [repository checklist](https://unixnu.com/porting/device-repository#completion), build and record results, then follow [publication](https://unixnu.com/porting/device-repository#publish). Add the **`unixnu` GitHub topic** and verify it appears in the repository topics.

The zero memory range and `REQUIRED_` values identify inputs to complete.
`patches/xnu/series` lists board patches in application order. `package/` contains
board packaging. Shared resources come from the selected SoC and component sources.

- [Device repository](https://unixnu.com/porting/device-repository)
- [Device tree](https://unixnu.com/porting/device-tree)
- [Component catalog](https://unixnu.com/reference/components)
- [Migration](https://unixnu.com/porting/migration)

`porting.toml` is the shared maintainer and agent record. Publish it with
`board.toml` and the `unixnu` topic. The central catalog workflow pins the
source and generates both language editions twice daily. Put board driver
submodules in this device repository. See the [record guide](https://unixnu.com/porting/records)
and [UFS guide](https://unixnu.com/porting/ufs).

```sh
python3 scripts/porting-record.py check --device ../UniXNU-example
```

Run this command from UniXNU.

## Local builds and boot diagnosis

Copy `unixnu.example.toml` to `unixnu.local.toml` in the main checkout and fill
in device inputs using the [configuration reference](https://unixnu.com/reference/configuration#local-build-inputs).
Read [boot arguments](https://unixnu.com/xnu/boot-arguments#apply-arguments)
before changing the command line. Rebuild the payload and boot image after
argument changes. Use the [green-bar diagnosis](https://unixnu.com/guide/debugging#green-bar)
for stalls at loader handoff. Record the last confirmed stage and relevant logs.

071 candidate:preserve070 original27/8CPU/DSI0/USB baseline;new4GiB RAM Data uses the pinned official Setup/idle preference plan for24A446 and its own UUID. Typed buttons describe no mechanical Home key;existing auto Home input enabled. Touch coalesces only same-contact motion,retains down/up,uses one reusable source buffer and submits at most one report per16ms callback;an error stops automatic submissions. Host selection/actual class models passed;DriverKit,UI/rendering/USB hardware stability remains pending.070 original QuartzCore push_surface assertion is retained and investigated separately.

072 diagnostic candidate: preserve071 touch decoder,THP threshold,coordinate conversion,HID descriptor,pacing,combined device and RAM Data unchanged. Independent one-second observer records coherent producer/contact edges and HID entry/return/coordinates without using the HID workloop;registry usage properties and synthetic Home are explicitly logged. ASan/UBSan actual-class model checks down/up/return accounting;100000-generation concurrent telemetry model checks snapshot consistency. These computer checks do not prove real finger delivery or desktop operation.

075 correlates raw contacts, HIDs, original service event bytes, client ownership and existing backboard tracepoints in one USB timeline. Its bounded trace queue passed overflow, reentrancy and wrap checks under ASan/UBSan. Native and hardware qualification are pending.

076 is a speculative event-integration correction. Its deployment requires actual075 evidence of missing event DisplayIntegrated despite the physical touchscreen service property. It reuses the latest Sheng authenticated table-copy procedure and changes the root event field before original dispatch. Source-extracted host checks pass. Physical acceptance is pending.

077 adds one opt-in,explicitly marked fixed Settings tap through the HID consumer after automatic Home. It waits for idle input and releases on physical activity. Host checks include one-shot behavior and clock wrap. Synthetic routing and actual physical touch are recorded separately.

078 prepares a touchscreen-first application descriptor while preserving every report ID and byte layout. It addresses the original service's first-pair primary-usage derivation. The actual class host model checks digitizer primary13:4 and all existing reporting behavior. Runtime077 classification evidence gates hardware promotion.
