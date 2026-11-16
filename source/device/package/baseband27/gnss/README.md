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
the real LOC service revision, informs its implemented revision, registers
position/SV/NMEA/engine/session events, then handles the existing PDS clients.
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
