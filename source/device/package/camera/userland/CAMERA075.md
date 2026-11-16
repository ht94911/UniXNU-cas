# Camera075 original LS bootstrap registration

Camera074 reopened the main sensor twice through the corrected SMMU and wrote
all1903 mode registers. Both sessions were cancelled before graph start, with
zero RAW frames. The native Camera was killed by RunningBoard with
`Capture Application Requirements Unmet: Camera not actively used`. The
physical display remained valid, while the app icon was absent.

The original Camera bundle exists. The helper's installed proxy was false and
its current post-install request failed with bootstrap lookup3 on
`com.apple.lsd.modifydb`. Camera075 loads a separate library only in the
original signed mobile `lsd` in explicit camera mode6. It calls the unchanged
original `registerApplication:` for `/Applications/Camera.app` after the
existing seed returns, inside the original server database bootstrap context.
It neither clears the LS store nor adds any Data reset or app-file install.

The original24A446 `createAndSeedLocalDatabase:` encoding is `v24@0:8^B16`;
`registerApplication:` is `B24@0:8@16`. CoreServices UUID is
`51e9d14e5db6374f9908c95386d54c0d`; exact methods are checked before replacement.
The callback preserves the original output pointer. A single attempt keeps
acceptance distinct from a fresh installed/nonplaceholder proxy.

`tests/camera-ls-registration.m` uses the actual Foundation consumer for
installed, absent, placeholder, denied, unreadable and non-server cases.
`python3 scripts/check-nezha27-camera-lsd.py --out <directory>` exercises the
actual exec branch's mode/name and failure handling. The AMFI call90 owner
checks the final original signature after load_machfile; exec argument
extraction precedes signature loading. The existing
SystemVendor observer now reports the original source backing count immediately
after init to distinguish source description from a usable backing.

The launcher also records wide-angle discovery, all video devices and direct
main-device lookup without starting capture or requesting authorization.

Native compile, original server registration, icon, real preview, shutter and
photo review remain unverified until this new candidate runs on the device.
Keep GPU051, the verified072 boot power proxy and074 bank/stream retirement.

Camera075 actual runtime: the library entered the original seed, then the
`_LSDefaults.sharedInstance` accessor raised an original sandbox exception
and an unrecognized-selector exception twenty times. The physical screen
stayed at Apple. Source backing count1 was verified; full/wide/main app
device lists remained0. Normal Android/root/postflight release completed.

Camera076 uses original LSServerMain's C singleton getter at0x186dce8d4,
checked by original UUID and28instruction bytes, with a signed function
pointer. It keeps the original guard and actual server state. The fixture
class accessor now fails if called; the consumer accepts the real singleton
object directly. Actual076 kept original lsd pid110 stable and reached the
physical desktop. Registration still returned0, source backings were1 and
app devices were0. Control Centre Camera remained black and was killed with
the same requirement reason; normal Android/root/postflight release completed.

Camera077 calls the original `__LSRegisterBundleURL` at0x186f368b0 with native
`LSInstallInfo` for the existing Camera System record and a real NSError output.
Original `LSMIResultRegistrantTrueDatabaseContext` uses flags0x02000001 on
the same FindOrRegisterBundleNode path. The BOOL workspace wrapper supplied
nil install info, used flags0x08000000 and discarded the specific error.
The selected LSInstallInfo initializer encoding is `@32@0:8@16@24`,
IMP0x186f5ad48. The original URL entry is checked against its32bytes before
binding; the076 C getter and original seed/guard remain intact.

The source manager observer preserves the original by-value32-byte audit
token (`@56@0:8i16{?=[8I]}20B52`), bincompat value and returned source array.
It records camera clients' PID, source count, actual local/shared backing,
media type, device type, position, format count and missing format count.
Host checks exercise the exact consumer's empty/populated return and both
backing routes. This observation does not start a graph or override filters.
The LS Foundation consumer now covers12 cases including unavailable original
install info, a missing entrypoint and retained native registration errors.
Camera077 target compilation and device acceptance remain pending.

Actual077 registered Camera in original lsd successfully (`registration1`,
`installed1`, error0), but the app-side proxy remained0. Its Camera source
query reached the original manager with pid469 and returned an empty array;
the manager selected shared backings and found0 despite the earlier single
real backing. Camera remained black and was killed with the same requirement.
Normal Android/root and collector48900 release completed.

Camera078 retains the one measured main backing produced by the native
source-info initializer and gives it to the original manager's supported
independent initialization (`@36@0:8@16B24B28B32`, IMP0x1b06e2928).
Only the verified vide/type2/rear/uniqueID list qualifies; native filters,
source creation, activation, graph Start/Stop and controls stay unchanged.
The original shared refresh can remove its live backing; this independent
list follows the existing `captureSourceManagerWithBackings:` contract.
Host tests cover its retained list, local initialization and no-main fallback.

The LS callback also rechecks Camera immediately before the original
`LSDatabaseBuilder setSeedingComplete:` commit/save/notifications. Original
async MI seeding follows the initial bootstrap and can replace its contents.
Only a genuinely absent entry is registered there, once per native phase.
The actual completion BOOL remains unchanged. This addresses publication
ordering without declaring unfinished seeding complete, clearing the store
or replacing native services. Target and device acceptance are pending.

079 continues the failed078 Camera trial. Its original server returned one
main source and the LS seed completed with Camera installed locally, while
the client proxy remained absent and the human observed a black preview
followed by RunningBoard termination. The registration hook now reports
its accepted database change through the original BOOL output. Completion
return and native listener initialization are recorded in bounded stderr.

The new client observer preserves original AVFCapture source ownership,
consuming init semantics, mutable void filtering and camera access verdicts.
Its exact24A446 methods are checked before replacement. It records remote
attributes/formats, device construction and filtering in the original Camera
and existing launcher; it does not start capture or create a device itself.
The Camera insertion uses the native signature and CDHash. Hardware preview,
shutter and readable photos remain pending. Qualified new packages directly
join the shared phone queue; current physical results are requested after boot.
