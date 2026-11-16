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
