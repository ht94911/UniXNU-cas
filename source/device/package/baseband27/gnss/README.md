# nezha iOS27 GNSS source delivery

This increment connects the existing nezha modem/QRTR worker to the original
24A446 Eureka PDS consumer. The user selected GPU051 for integration and
testing on 2026-11-16. The fixed base is device revision
`76c5952683041a6581224135461ab7c21d47f6a5`, boot SHA-256
`4e4b785ca1bf7bea18d68248ade4cde09a9d9efb5d557fe65e4eaa975dac849a`.
The GPU051 shared ALS resource owner supplies the sole transport archive;
the modem backend reuses qualified baseband038 source. Target build, full
package qualification and physical Core Location acceptance are recorded
separately; source and host checks do not establish a satellite fix.

The original task and acceptance requirements are in the local project handoff
`docs/handoffs/IOS27-GNSS-CODEX-20261116.md`. GPS and BeiDou participate in the
first delivery. Core Location application consumption remains a device test.

## Source and shared owners

| Part | Source | Use and evidence |
| --- | --- | --- |
| Remote startup, GPS memory, firmware, QRTR | nezha NAS020/baseband034 source `e816468`, derived from QcomBaseband `dd0199c` and sheng | Existing worker, PAS/MSS, memshare GPS client 0, `gps/gps_service`, original client discovery and queues. These owners are reused. |
| LOC messages, units, satellites | Qualcomm `location_service_v02.h/.c`, `LocApiV02.cpp` at `1ed86b1d4f7f89faa1d4c441702fcad22c9cda0e` | Protocol definitions and behavior reference. LOC v2 service 16/instance0 was observed on this phone's Android. GNSS library/message capture is still incomplete. |
| QMUX/PDS | QcomBaseband `dd0199cf3300764ed52b23f4a47761f91fab21a2`, libqmi `3454c2c619d516fd46a08fe16c6eebdee061857d` | Standard QMUX allocation/response conventions and PDS flags. There is no upstream LOC-to-PDS GNSS implementation in the checked sources. |
| Original OS consumer | iPhone14,4 / d16ap / 24A446, local `locationd` SHA `d00659f0aef2475c0cb1f5358fff411229cbcbd0883b51fc470d40a87a300b18` | Parsed-position reader, lifetime callers, default1s/255s/400m, Eureka factory and report registration inspected. Private Apple bytes stay outside Git. |
| Same hardware and upstream comparison | UniXNU `4a654d1`, myron-next `f1a1bfe`, sheng `d24651f`, pandora `81b71b1`, Pagani `fcab49e`, fairlady `bb05659`, umi `3eb7257`, Trogdor `7e0a044` | No complete public SM8850 GNSS-to-Core-Location source found. sheng provides `gps/gps_service`; Trogdor has a GNSS channel but no inspected GNSS protocol backend. |
| Linux GNSS, QRTR | Linux `fb2c3f44` | GNSS character streams and QRTR ownership are references. Generic GNSS/NMEA support does not provide the modem LOC lifecycle or iOS provider. |
| QQ archive | Human-designated `M:/QQ_History`, existing SQLite/FTS and saved topics |135181 messages in the available archive. GPS/GNSS/BDS/frequencies/QMI/NMEA/CoreLocation keywords and relevant original contexts read locally. “gps也活了” is an author report; retrieved public issue still says “暂未适配GPS”. Neither is nezha hardware proof. |

The local evidence directory is
`/mnt/i/Code/UniXNU-nezha/evidence/gnss27-20261116/`; larger reverse-engineering
scratch files remain in `/home/hua/nezha-work/ios27/gnss27-codex/research/`.
Sources are new board code under BSD-2-Clause. Qualcomm/public protocol values
are referenced; proprietary Apple and vendor files are never embedded here.

## Implemented increment

`NbGnssProtocol` parses whole QMI/TLV chains, preserving valid masks, status,
original WGS84 degrees, receiver UTC, precision, fix ID, system tick, raw GNSS
signal masks and satellites. Coordinates equal to zero are valid. Missing or
non-finite values do not acquire valid bits. The packed PDS field is exactly
105 bytes: valid mask0, UTC ms15, time uncertainty23, latitude27, longitude35,
ellipsoid altitude43, MSL altitude47, speed51, heading59, circular precision63,
ellipse major67/minor71, orientation75, vertical precision79, speed uncertainty83,
horizontal confidence91 and DOP92/96/100. The calendar timestamp is absent.
Kernel code copies IEEE bits and uses integer conversion for time uncertainty;
it is suitable for the existing `-mgeneral-regs-only` recipe.
`raw_valid` records the receiver fields before consumer-specific validity
filtering: an unknown velocity uncertainty suppresses the PDS speed flag;
an ellipse at confidence other than95 is kept in raw values and omitted from
PDS validity. A valid circular precision can still be delivered.

`NbGnss` uses one LOC client owned by the existing baseband worker. It queries
the real LOC service revision and, following the stock minor177 client, registers
position/SV/NMEA/engine/session events, then handles the existing PDS clients.
The optional legacy INFORM_CLIENT_REVISION request no longer gates startup.
Registration carries the original AFW client metadata and a descriptive client
string; the existing operating-system authorization controls application requests.
The initial provider is standalone: mode4, periodic recurrence1, accuracy3,
intermediate reports off2. Every client retains its requested interval and
accuracy threshold. The modem uses the shortest active interval; delivered
updates obey each client's interval/threshold. No EFS/NV, GNSS engine lock,
factory configuration, constellation masks or assistance caches are written.

Client commands queue across async initialization. PDS replies preserve the
native transaction; START/STOP success requires the real reply, and set-mode
requires the response and async status. Query responses reflect acknowledged
state. Unsupported SUPL/MSB/MSA, measurement, raw sensor, assistance and vendor
commands return NOT_SUPPORTED. Configuring a report that is already disabled
does not start hardware. No success is invented for unsupported reports.

Stops, client close, QMUX release/sync, endpoint loss and fatal command errors
retire the module's own session/clients. START reply timeout results in one STOP
for the same session before new requests execute. STOP failure closes the
provider. Reconnect gets a fresh QRTR source port in0x4200..0x42ff; NAS remains
on0x4180/0x4181. Wire transactions and session IDs are not reused on a live
endpoint. This initial implementation permits255 starts per endpoint before
closing the provider; final package integration must decide the needed port
rotation policy if that limit is insufficient.

Only final successful satellite fixes with UTC, coordinates and circular or
95% elliptical precision usable by the original PDS consumer are published.
Multiple signal records for one satellite are retained and unique GPS/BDS
satellites counted once. Duplicate/backward UTC reports and a
receiver-vs-host delay over10s are rejected. A new current-session first fix
still needs device proof of freshness. A forward receiver clock correction
reanchors monotonic comparison; backward clock reset requires endpoint/session
recovery and never rewrites a stale fix to the current time.

## BeiDou scope

The phone's model specification lists B1I/B1C/B2a. The public LOC IDL defines
B1I0x200, B1C0x400, B2a-I0x1000 and B2a-Q0x80000. This source preserves the
signal masks; actual phone firmware availability and tracks are **unverified**.

The position report's0x36 expanded-used list (or0x2c legacy list) is the usage
evidence. BeiDou IDs201..263 identify constellation6;0x37 signal types align
with that same expanded list. A preceding sky report never becomes used-in-fix
evidence. If signal types are missing, constellation usage can be reported but
frequency usage stays unknown. If only satellites are visible, the result is
“已观测到北斗，参与解算待证”.

The existing USB diagnostic catalogue uses provider9 for MSS/GNSS HISTORY
requests; GPU051 retains its audio provider8. Page0 returns the current
valid fix, IEEE coordinate/precision bits, UTC/receive time, generation, sequence
and usage counts; pages1..176 expose individual used satellites. Page256 is the
sky summary, pages257..432 its tracking records. Subsequent pages must carry the
summary cursor; changed generations/reports return EXPIRED. This is a read-only
extension of the existing diagnostic framework. `bds_seen` counts valid BDS
IDs with tracking state3 and a valid positive C/N0, rather than idle catalogue
entries. Sensitive positions remain local.

`tools/gnss27/corelocation_evidence.py` compares saved receiver pages with a
foreground application's ordinary authorized Core Location sample. It requires
the same boot UUID/candidate SHA, full accuracy, a non-simulated source, fresh
application time and coherent used-satellite pages. Missing signal fields remain
unknown. Its local JSON comparison is prepared; synthetic comparison execution
and real application evidence wait for the selected package.

## iOS27 consumer recipe

`tools/gnss27/consumer_fixups.py` reads the exact local original `locationd` and
prepares two entries for the already-used ADT `text-fixups` parser. It selects the
existing Eureka constructor path at the inspected factory and restricts that
consumer's report registration to parsed position. Original PAC vtables,
callbacks, locationd authorization and Core Location delivery are reused. The
script does not edit donor files or build an image. Original signed source pages
and the existing page-substitution mechanism must be qualified in the selected
package. Actual Eureka bootstrap, GNSS lock interaction, unsupported vendor
commands and Core Location app consumption are still required device checks.

`integration/gnss-locationd.patch` keeps locationd enabled only when the typed
`/chosen/nezha-gnss=1` switch is selected. It uses the existing per-job policy
writer and does not change other jobs or Data. The switch also requires
`nezha-baseband=1`. The selected GPU051 candidate enables both switches.
`nezha-gnss-test-app=1` also mounts its signed ordinary test app and current
LaunchServices registration helper through the existing read-only unions.

## GPU051 qualification and physical acceptance

The fixed compiled implementation is device revision
`586340c4f7cd003878f572853989d961e9cb941f`. Native003 run43884693400,
head1159a6c2d40c9b3644a4a985f0c1507e2dc21984, passed23 baseband/GNSS
translation units,355 external imports and the actual KeyStore/USB checks.
Strict prelink passed38 components and348 original imports. The1062 baseband
and754 GPU/Legacy/HID authenticated slots retain their targets and PAC data.
The final kernel passed the production loader parser, HMAC, guarded60-entry
text-fixup parser and single/eight-CPU QEMU controls. QEMU disables physical
Qualcomm drivers; it establishes no satellite result.

The ordinary Core Location scene app and current24A446 modifydb registration
helper passed run43884264768,9 actual consumer registration fixtures, every
4KiB signature page and actual iPhone cache dependencies. The app has empty
entitlements. Its normal WhenInUse/full-accuracy permission is required for
position updates. The signed launch cache preserves GPU051's729 jobs and adds
one bounded launcher;54 non-cache addon entries and all four original trust
modules remain byte-identical.

Stock LOCv2/minor177 descriptors confirm the selected scalar fields, legacy
80-SV arrays, expanded176-SV arrays and28/29-byte satellite wire aggregates.
The larger reports add optional payloads. Stock Android HAL declares B1I,
B1C and B2a; tracking and same-fix use still require physical reports.

Package `gnss001-GPU051` has boot SHA-256
`a5907a796c920216229d5b9d7d14f88eeb07f1e2a13ac7db1f54c26ecb2f9284`,
payload SHA-256
`b96c1dea7b02bf57e6056c047e07891a52ceb2b4b5767ea02f79e54b9561eeb2`.
Its complete embedded payload preserves GPU051's Data generation1 and root
policy; the existing baseband firmware file is only read. Windows r008's75
files and concrete lifecycle passed. Raw/private evidence stays at the local
project `evidence/gnss27-gpu051-20261116` and package001 directory.

The user deferred manual permission and window/outdoor reception. The first
phone trial checks startup, baseband/LOC ready, locationd and ordinary app
registration. Receiver fixes, GPS/BeiDou use and ordinary callbacks remain
unaccepted until matching physical evidence is available.

The integration and acceptance sequence remains:

1. Create its isolated copy. Apply this source delta onto that version; preserve
   newer NAS/VOICE/UIM/WDS/IPA/device identities instead of replacing its bridge
   wholesale. Reconcile shared-owner pins and build lists.
2. Enable typed `nezha-gnss=1`, retain the original baseband startup and root/Data
   policies, ensure locationd runs, and append the two validated consumer entries
   to that package's existing `text-fixups` property. Do not replace older entries.
3. Run `sh tests/gnss27/check.sh`, applicable existing baseband checks, exact
   target SDK/arm64e compile, strict prelink and signed-page/PAC validation.
   GNSS contributes two C objects and the original bridge translation unit.
   The standard recipe remains32jobs. Mac-only work follows the existing
   minimal-source/encrypted-artifact permission and cannot upload private input.
4. Validate foreground Core Location authorization with an ordinary application
   and record coordinates, accuracy and timestamp against the receiver fix.
   App selection/signing/installation and fixture permissions are part of the
   specified-package stage. Do not bypass location permissions.
5. Use the file-controlled phone schedule with the fixed image. Capture first
   satellite fix, GPS/BDS use and available frequencies, repeated enable/disable,
   two consumers, cancel/client exit, signal loss/recovery and peer-module
   coexistence. Ask for any necessary outdoor/window conditions in that phase.
6. Return normally to Android, complete the required real protection postflight
   and collector exit, then write the run result and integration/rollback handoff.

Rollback uses the prior selected-package boot/payload. The test app and launcher use the existing
addon directory on Data; they require no phone payload write or Data reset. Full system positioning and device usability
remain unaccepted until the real application/receiver evidence is available.


GNSS001 physical startup on2026-11-17 reached root/Data, locationd and both
readonly app/helper mounts. Its inherited provider8 conflicted with GPU051
audio8, so `nezha_baseband_start` returned before creating the worker.
The fix selects unused provider9, keeps the audio diagnostic identity and
checks startup registration against that catalogue. The ordinary application
registration reached original `com.apple.lsd.modifydb` lookup3/NoSuchProcess;
no app registration or satellite fix was accepted. The run returned normally
to Android/root, verified unchanged17 and Data generation1, stopped its
collector and released without a phone payload write.

The second candidate, `gnss002-GPU051`, retains that GPU051 baseline and
baseband038 backend. Compiled device revision
`9255bde3d5cb573ceec600f035ee3b29a8d07f73` changes MSS/GNSS to provider9;
GPU7 and the original audio8 remain. Native004 run43922194416 at
headc818154a19bda5468abc55a582784098ce2a15b6 passed the23 target units,
355 external imports, strict38-component/348-import closure, authenticated
tables, production loader/HMAC, guarded60 fixups and single/eight-CPU QEMU.
No additional shared transport archive is linked.

Packaging revision `21a7328965fed598b09302372f491b44e7232c7d` selects the
original `LSOperationRequestContext.targetUserID=501`. Original24A446 native
code resolves that context to the existing mobile LaunchServices domain.
Run43925409254 at heade5248ad929e5a5b8aef562c4409c59b12babc445 passed
the9 consumer fixtures, signed pages and actual iPhone dependencies. The
ordinary app retains empty entitlements and normal location authorization;
the launcher uses only its existing application-registration permission.
Successful real mobile-domain registration is still a device check.

The fixed package002 identities are:

- boot: `38861ac45627a7f4cc2e5c6d9e705c97c1afdb437d67d120a7b2225fada0e004`
- embedded payload: `967d44d752f6ba31c32b7fe7509a2169cc876785dd83130fa6762c4b2a79e2bd`
- kernel: `075a15bcb6875f107aa46ee56766b77309f82b7ebf59b1e919ad4d05ebc23dfb`

The176168960-byte boot image embeds its167772160-byte payload. It retains
the original BootShim, GPU051 runtime,54 non-cache addon entries,729 launch
jobs, four original trust modules and Data generation1. The candidate adds
the signed app/helper and one bounded launcher. Existing root and modem
firmware inputs remain read-only. The package and actual113 host-r00875
files passed their checks; remote deployment and the concrete lifecycle
were qualified before queueing. The requested113 trial checks real MSS/LOC
startup, locationd and application registration, then returns normally to
Android/root. Manual permission, outdoor reception, fresh Core Location
callbacks and same-fix GPS/BeiDou use remain deferred by the user.

GNSS002 ran on the local host on2026-11-17, after the user moved the phone
back from113. Fresh provider CAPS identified MSS9 as `nezhamss` because the
retained USB serializer removes punctuation; the original audio8 remained.
The initial collector catalogue predates late MSS registration. A fresh
same-session catalogue and read-only provider9 requests confirmed MSS online,
GLINK,61 QRTR services, zero watchdog events and LOC service revision177.
LOC initialization then ended with INVALID_ARG48, before START or a fix.
Three recorded locationd fatal PAC instructions were BLRAAZ and AUTDA; the
original fatal path exits before the existing later exception recorder.
The recovered launcher stderr contains only repeated4099 modifydb lookup
failures, without a fresh process/boot marker, so new registration is not
accepted from those bytes.

The run returned normally through sessionc719aaa40e2a442b/request9097df2f.
Actual Android/root, unchanged135168-byte17 prefix, Data generation1,
protected mappings/pins and collector17988 exit passed before release at
2026-11-17T05:14:11Z. Data's ordinary new allocations added36 pages; no
payload or partition write occurred. Exact USB raw3238853 bytes and the
two launcher files were recovered with5775360 bytes of read-only APFS input.
Private raw evidence is in local project
`runs/2026-11-17-ios27-gnss002-gpu051-local002/`.

The next source increment follows stock direct event registration with AFW
metadata, retains failed-command and ACK/indication state in HISTORY, and
uses the existing bounded QMI header tracer for LOC. The first three locationd
PAC faults are captured before the unchanged fatal handler. The launcher job
runs as the existing mobile user and logs its actual PID/UID/boot/uptime.
Native005 run44028683069 at heade9d4c282d7fb11fdc160cdc2ce7e24a12c2b638c
compiled the23 target units and resolved355 external imports. The216 other
runtime objects match native004; only the GNSS state machine and included
bridge translation units change. Native fatal-capture code is present. Strict
prelink passes38 components/348 imports,1062 baseband and168/379/207
GPU/Legacy/HID PAC slots, and seven full original IOSurface routine hashes.
The final entry/HMAC kernel passes its current production parser, single
no-root-map refusal and eight-CPU display/HID/backlight6 controls with seven
secondary CPUs and zero skipped targets.

The mobile launcher and retained ordinary app passed run44028701894,
head5f3bc06abe1a87cbe8a6d69b6eb6451e03f41783,9 interface fixtures and every
4KiB signature page. The normal app remains byte-identical. Only the helper,
its launch-cache job and detached signature change; all729 original jobs,
54 GPU051 non-cache addon entries and four original trust modules remain.
Package003 uses boot SHA-256
`a50ed09ac5e7d0a2a8975603b636492575245a127933730c4aa9c862d94a0be8`,
payload SHA-256
`98ca0a5ef4227e4996494cc9d192cd054266fea538dca292cd29f49ababa8dd9`,
kernel SHA-256
`cdda3c7a9b6a434238985c196d6edfc65a6a01a09d2caba8e85b1f9851ba96bc`.
The original boot-loader/FD prefix is identical to package002. Current
host-r00875 files and the concrete local lifecycle checks pass before
queueing. Fresh CAPS avoids the stale early catalogue and validates the
actual sanitized provider name, session and reply provider. The third local
trial remains pending its selected file window; manual positioning stays
deferred.

GNSS003 local startup on2026-11-17 confirmed the service-revision query's
reply and indication, then the original REG_EVENTS0x21 reply returned
ARGUMENT_TOO_LONG19. The header trace and failed-command33 fields identify
the exact request. This increment used a16-byte client string although
both inspected LOC IDLs limit that field to4 bytes. The next request uses
`NZCL` and remains the ordinary AFW client. Its complete packet is36 bytes.
No START or fix was produced. This run showed no locationd PAC report in
the recorded window. The extracted launcher file was unchanged from the
previous run and had no new scheduled/startup marker.

The next launcher job uses the already-tested RunAtLoad/LaunchOnlyOnce
condition. The process logs its actual PID/UID immediately, waits120 seconds
for startup, then uses the original45-second registration deadline. It runs
as mobile and retains normal application authorization. The job's initial
KeepAlive condition is removed by LaunchOnlyOnce after its first exit.
The completed GNSS003 run returned normally to Android/root, verified the
unchanged135168-byte17 and27 prefixes and Data generation1, confirmed
collector90388 exit and released at2026-11-17T08:29:52Z. Raw USB2103041 bytes,
SHA42c714f1ac7e8cb81364dfbb3308a0cddc250cb5a30f6534546dacef3e3458ff,
and launcher results remain in local project
`runs/2026-11-17-ios27-gnss003-gpu051-local001/`. New native and application
qualification are required for the4-byte registration and launch condition.


GNSS004 continues from source `770e14435cbcf4e2e3e9bcf933eca7cdc9cb8794`
in local branch `codex/gnss27-gpu051-continue-20261117`. The selected package
remains GPU051. Upstream `630b85a307f19c8d8a4699d5bfc12f6796b874aa` and
myron-next `f1a1bfe` were fetched on 2026-11-17; neither supplies a new GNSS
backend. The upstream registration agent change targets macOS Aqua and
does not change this iOS mobile registration path.

Native006 run `44048805411`, head
`a0040b8c65e40217adcd360b002fa70f3c923f7b`, compiled the 23 target units
and resolved all 355 external imports. All 841 returned file identities
match. The four integration patches regenerate each compiled source hash;
of 219 runtime objects, only `NbGnss.c.o` differs from Native005. The LOC
client identifier is four bytes (`NZCL`), and the subscription packet is
36 bytes. CoreLocation006 run `44048997325`, head
`ee55a42c69a52d26bfddbf77dcda2419493e0336`, passed the nine original
registration-consumer cases and the iPhone target build. The ordinary app
is byte-identical; the mobile helper records its scheduled PID/UID, delays
120 seconds and attempts registration once.

Package004 passed the strict 38-component / 348-import closure, 1,062
baseband and 168/379/207 GPU/display/HID table slots, seven original
IOSurface routine hashes, final kernel entry and POST field, and the current
single/eight-CPU startup checks. Seven secondary CPUs reached display/HID
and backlight stage 6 with no skipped targets. The original ADT, BootShim
and FD remain byte-identical to Package003. All 729 original launch jobs,
54 non-cache GPU051 addon members and four original trust modules remain.
The actual Windows entry fixture and 75 host-r008 files passed.

The fixed Package004 identities are:

- boot: `c416ac00d2a0065228ec82a8b42cdc6c205ccf4fe04bf38a2f1447c635b352c2`
- payload: `4ce004714776deb17dcf9e41eb3e51fcf15ccc12628c455539c31855e46e02d2`
- kernel: `c1f80a006593a52b4564d9120004b26db1c300cf127acbc6de065a597812a743`

The candidate is queued for one local bottom-layer/startup trial. It checks
the current LOC subscription reply, provider 9 and the helper's actual
process/boot markers, then returns normally to Android/root with the
existing postflight and collector-exit checks. Manual location permission
and sky reception remain deferred by the user. A satellite fix, same-fix
GPS/BeiDou use and ordinary Core Location delivery remain unverified.
Rollback uses retained Package003 or the original selected GPU051 image.
Local package and raw evidence: `gnss27-codex/package004` and project
`evidence/gnss27-gpu051-20261116`; physical results belong in the actual run.


GNSS004 physical startup ran at 2026-11-17T09:52:31Z. The LOC177 query
reply and indication succeeded. REG_EVENTS transaction2 was the intended
36 bytes but returned INVALID_ARG48, followed by a second INTERNAL3 reply.
Eight fresh provider9 reports consistently recorded failed_command33,
phase5, no START and no position delivery. The helper output was the same
1,296 bytes with an old timestamp and no new process/boot marker.
Normal XNU request6c769f17/session5c436df2604e0aee returned to Android/root;
protected mappings, unchanged17 135168-byte prefix, Data27 generation1 and
collector7976 exit passed. The window released at09:59:32Z. Raw USB
2,999,803 bytes (SHA6d0f9d1b64f8) and both helper files were recovered with
4,726,784 bytes of bounded read-only APFS input. This is protocol diagnosis
progress; registration, application startup and satellite positioning remain
unverified.

The next source matches the actual stock `locClientRegisterEventMask`
request: client string `HAL` (3 bytes), AFW type1, notification valid1/value0.
The inspected24-byte structure and LOC177 descriptor were replayed through
the existing QMI codec under ASan/UBSan: its body is28 bytes, with no inner
length or terminating NUL in the string TLV, and the complete packet is35
bytes. No different client type or receiver configuration is requested.

The previous runtime pack updated the cached job but retained the old
`daemons/org.unixnu.nezha.location-launch.plist`: root, RunAtLoad=false and
StartInterval120. That daemon/cache mismatch is a concrete packaging error.
`tools/gnss27/launcher_job.py` now applies one source job to both members and
compares their decoded dictionaries before packaging. The job uses mobile
in the System session, RunAtLoad/LaunchOnlyOnce, KeepAlive=false; the
existing helper owns its120-second startup delay and45-second deadline.
The mismatch regression and33 protocol /65 lifecycle /10 QMUX checks pass.
GNSS005 target, complete package and actual startup are still required.
