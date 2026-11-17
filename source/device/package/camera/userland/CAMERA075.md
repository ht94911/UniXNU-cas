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


Actual079 registered Camera and returned from seeding completion. All seven
original service objects were created; the client still received bootstrap
lookup3 for mapdb and modifydb. The original Camera observation file was
denied by its sandbox before observation installation. The native reservation
produced two RAW frames and then IRQ fault2/errno5; no graph started, and the
human again saw no icon and a black preview followed by termination.

080 moves only the Camera observation file to the original profile's allowed
/tmp directory. It observes the original owned property query and LS
connection initializer, preserving both return ownership and options. The
original server reports listener names, resume, actual libxpc descriptions
and incoming connection verdicts. Source-manager observations read and release
four native owned properties without changing the returned sources. Native
teardown reports the IRQ owner's already captured failure words after cleanup.
These observations distinguish publication and serialization failures;
physical preview, shutter and photo review remain required.

Actual080 reached the physical system desktop. Camera's icon remained absent,
and Control Centre opened a black preview followed by termination. Its original
AVFCapture client returned one real rear main source, four formats, Connected
true, a nonnil native video device and camera access true. No capture graph
started and no RAW frames completed. RunningBoard again terminated Camera with
the same requirements message. Original lsd published its native services and
accepted 24 observed clients; its native seed completed with Camera installed
locally. The background helper's later lookup3 is a separate client result and
does not prove the native service was unpublished. Android/root postflight and
the collector's actual exit/release completed.

081 observes the next original Camera boundaries: capabilities, preferred
device, engine/device input, AVCaptureDeviceInput's owned initializer and
NSError, canAddInput, addInput, commitConfiguration and startRunning. It retains
all native selections, output ownership, completion blocks and void semantics.
The capabilities getter also records the real hw.target and capture model.
Only the original Camera installs these observations, with exact24A446 UUID,
method address and encoding checks. Logs use its existing permitted /tmp path.

The original LS read replies and database-rebuilt request are observed in lsd.
Their callbacks preserve the actual query dictionary, store, catalogue,
blocking endpoint, manual-rebuild BOOL and NSError. A queued request is not a
completion. No extra installation notification or database reset is added.
081 retains the qualified080 kernel, GPU051,072 boot power proxy and074 SMMU
retirement. Physical icon, preview and photo acceptance remain unfinished.

081 was qualified and queued but never claimed or booted. Further original
model-query analysis found that its retained GPU051 ADT had an empty /product
node. In24A446, HWModelDescriptionForCamera resolves to that node's unique-model
property. FigCaptureGetModelSpecificName strips the AP suffix before AVGestalt
selects its feature table. The original target tree supplies D16AP; the retained
root already identifies iPhone14,4/D16. Without this input the feature builder
returns before adding rear-camera capability, independently of real device
discovery. The unstarted081 waiter was stopped and its queue entry withdrawn.

082 adds only /product/unique-model=D16AP through the normal typed board ADT
configuration. The package finalizer inserts this one field into the retained
tree, keeping all existing property bytes, hardware nodes, platform identity
and seed. It retains081's original input/session and LS observations for actual
confirmation, the same qualified080 kernel/provider and exact079 helper. This
fix addresses a missing native model input; it does not prove the icon, preview
or photo flow until the corrected image runs.

Actual082 confirmed D16AP→D16 in the original Camera, back/front capability
true and multiCam false. The rear wide device input was created without error,
canAddInput returned true, add/commit completed and native Start ran. Four
provider graph-start records returned0. Physical onboarding and lens buttons
appeared, but the icon stayed absent and the preview remained black before
exit. No RAW or delivered output frame was verified. The camera service
terminated17times at `-[NBFunctionConstant required]`: GPU051's actual class
only exposes `isRequired`. Original Core Image's shader setup calls `required`.
SpringBoard30 received the actual LS store and catalogue after original seeding
and database-rebuilt notification. Android/root postflight and57392 collector
exit/release completed.

083 keeps GPU051's plugin and runtime byte-identical. Inside original Camera
and cameracaptured, the existing image-arrival callback checks that plugin's
UUID, exact `isRequired` IMP0x22890, original8bytes and `B16@0:8` type, then
adds only its missing `required` selector with that same original getter.
The native flag and all shader/rendering/memory behavior stay original. The
production Metal source separately adds the same public getter, reading the
existing flag, on a GPU051-based worktree. Native consumer cases verify true
and false values, repeated installation and unrelated classes. The original
device/provider/input/graph contract remains unchanged. New build and physical
icon, preview, shutter and photo review are still required.

The still-camera icon tag has its own native hardware-description query.
Original SpringBoard calls MGIsQuestionValid for each supported application
tag, then MobileGestalt's still-camera answer0x1afa295b0 reads camera-front or
`/arm-io/isp/camera-rear`. The retained tree lacks that rear property even
though082 verified the real rear source, input and graph. 084 declares only
that actual rear presence in the package ADT; the logical isp node has no
Apple compatible, registers or service. The normal board record selects this
through camera.adt.rear_present. No front camera or aggregate device is added.
The package keeps083's exact GPU051 getter correction and082's D16AP model.
Physical icon and frames still require the corrected new image's result.

Actual084 confirmed the original Camera icon on the physical desktop. All
displayed magnifications remained black. Intermittent exit was independent
of magnification changes. The service still reported fifteen missing
`NBFunctionConstant required` exceptions; the runtime alias was never
observed installed. The last session produced seven RAW frames and one
PrimaryScaler callback, then FramesIo and an incomplete CPAS drain. Its
printed IRQ zero preceded IRQ sampling and did not clear the fault.

085 builds the production `required` getter from the GPU051 Metal source,
reading the existing `isRequired` flag. It retains the original GPU051
runtime recipe and all earlier board corrections. On a failed drain, the
native worker keeps DMA mappings and resource leases, stops only its IRQ
callback, and reads that owner's preserved fault. The existing console
reports CPAS handshake, sampled versus requested controls and the already
captured receiver registers after cleanup. No additional IRQ logging or
polling is introduced. Continuous preview, shutter and photo review still
require device verification.

Actual085 removed the `required` exception. The first two Camera launches
timed out while creating the scene. After reopening, the physical preview
was solid green, then black, and no longer exited; the displayed default
was 2x. Magnification changes did not cause exit. Nineteen RAW frames and
one PrimaryScaler callback preceded the preserved RDI bit19 overflow
recovery event. CPAS RT status5 failed drain, with no control write attempted
and a successful eleven-byte readback. One normal Android/root return,
protection postflight, collector exit and targeted logs completed.

086 reuses the qualified085 production Metal. RAW acquisition still
invalidates the entire frame; publication cleans only the four pages that
contain the CPU's stale-frame markers. The sole staging copy moves outside
the camera mutex, with readiness cleared before copy and committed after
copy. Readers cannot enter a partial frame, and Close joins this worker.
The existing userland console samples the first four rendered buffers while
CoreVideo holds its write lock, before Unlock and graph delivery. These
samples record real Y/UV or BGRA values and strides; their colour is not an
application-preview acceptance result.
