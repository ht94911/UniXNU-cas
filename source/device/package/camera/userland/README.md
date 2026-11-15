# nezha RAW10 preview processing

`upstream/hakios` contains pinned HakiOS user-space files from
`yfl520/hackios-raphael@f128f1d8e2ef1f588002787c5efb94ec134ddeac`.
`ORIGIN.json` records each original path, original SHA-256 and local hashes; `LICENSE` preserves
the upstream MIT notice. `hakios.camera.h` is needed by the upstream C
processor. Its device path and commands are not used by this adaptation.
The local still renderer returns allocation failure before writing output
when any scratch plane is unavailable. Its Bayer demosaic is preserved.

`NezhaCameraPreview` accepts completed, CPU-owned MIPI RAW10 frames and
renders BGRA or BT.601 NV12 in either full or video range. The stock
Android Camera2 capture for physical camera 2 reports BGGR, black level
64 and white level 1023. The current adapter requires that measured
black/white scale, supports the four CFA layouts, and retains input and
output row strides. Initialization uses identity color correction and
sRGB, with the upstream shadow lift and unmeasured lens shading disabled.
AE commands, private tuning files and kernel resource changes are not
issued by the render adapter.

This is a portable user-space C3 component. It is not linked into XNU,
does not publish an AVFoundation device and has no camera hardware
result. C2 must supply a real completed frame before device integration.
The 24A446 FigCapture provider and its stream lifecycle remain separate
work. Run `scripts/check-nezha27-camera-preview.sh OUTPUT_DIRECTORY` for
packing, color, stride, NV12-range and input-boundary checks.

`NezhaCameraPixelBuffer` renders that same completed RAW into a CoreVideo
pixel buffer obtained from the system capture graph's `BufferPool`. It
uses the buffer's actual BGRA or 420f/420v plane geometry and strides,
checks lock and unlock results, and sets BT.601, BT.709 primaries and sRGB
transfer attachments after unlocking. Its 21 CoreVideo dependencies were
found in the original 24A446 CoreVideo export trie. The host fixture checks
padded output, malformed chroma geometry, partial RAW and failed locks;
the ARM64e object built with fixture declarations only checks code
generation. A target SDK build and graph delivery remain required.
StillImage 420f/420v uses `isp_render_still_nv12` to demosaic individual
sensor sites before scaling; scaler outputs retain the preview renderer.
BGRA uses the upstream preview path. A failed still render reaches the
existing graph failure path before a callback or buffer attachment.

`NezhaFigCapture24A446` binds the selected CoreMedia object constructor
and the device/stream ClassID exports in `CMCaptureDevice.framework`.
It creates objects with the measured immutable callback tables and
exposes their zeroed derived storage. Both framework handles and table
addresses have process lifetime. This factory does not register a system
camera by itself; the stream provider still has to publish measured
formats, connect Start/Stop to completed kernel frames and deliver them
through the graph's output handlers. The host test verifies the exact
framework paths, class versions and unavailable-export behavior.

`NezhaCameraGraphOutput` connects that renderer to one output's actual
`BufferPool` and `PixelBufferHandler`. A provider calls it on its serial
property/frame queue, supplies the completed frame's host-clock timestamp
and per-frame metadata, and enables delivery only after the graph starts.
The helper copies callback blocks, retains three preview buffers and
drops still buffers after delivery. Stop/reset gates future callbacks;
reentrant reset cannot retain an old frame in the next configuration.
The tests use the bundled Blocks runtime plus the real rendering adapter
tests; CoreFoundation, CoreVideo and the capture graph are host fixtures.
The new exports and output key strings were verified in 24A446. The
ARM64e object uses fixture declarations; a target SDK build and live
provider/backend wiring are still needed.

`NezhaCameraFigProvider` creates one rear main device and one stream with
the verified 24A446 callback tables. The caller supplies the measured
format and bounded backend begin/end operations. RequestStreams reserves
the sensor; graph Start separately enables the PrimaryScaler,
SecondaryScaler and requested StillImage callbacks. Only a complete
4096x3072 BGGR RAW10 frame with its actual completion time and metadata
can enter the output path. Stop gates callbacks before backend teardown;
a handler's synchronous Stop/Start cannot send the old frame to the new
session. Missing outputs return unavailable, and a failed still-buffer
allocation leaves the one-frame request pending. The provider test links
the actual graph, pixel-buffer and RAW renderer with native Blocks and
dispatch runtimes. It checks a full-size synthetic RAW, padded graph
buffers, all three handlers and reference release. CF/CM/CV remain host
fixtures; system registration and physical camera frames remain pending.

`NezhaCameraFormats` supplies the one measured main-sensor mode and its
three rendered outputs. It constructs the public CoreMedia 420f format
description, lists BGRA/420f/420v, 2x2 binning and full 4096x3072 crop
geometry, and limits both frame-rate bounds to the mode's 30fps. Those
source capabilities come from the stock OVX module and physical-camera
metadata; the 24A446 format matcher and HakiOS supply the dictionary
contract. The provider returns this description through its advertised
FormatDescription property. It does not claim donor-only capture modes.

`NezhaCameraFactory` supplies the selected vendor's allocator/options/output
creation signature. The platform binds one actual backend and a readiness
check before installing that callback in BWFigCaptureDeviceVendor. A
missing backend returns unavailable; creating a device starts no capture.
The same full pipeline test covers unbound/unready requests and rejects
rebinding. The system vendor still needs to install the callback.

`NezhaCameraDeviceBackend` connects that factory to the native
`/dev/nezha-camera` node. A serial timer reads one full HakiOS header and
RAW10 frame, validates the measured geometry and actual sequence/time,
then queues the frame on the Fig stream. It keeps one CPU buffer alive
until delivery or stale-generation discard completes. Device teardown
stops and unbinds the reader before dropping its owned stream. A malformed
native read closes the reservation. The host test uses real dispatch and
Blocks with the production Fig/RAW path and fixture native reads,
including Stop inside the graph callback and finalization. Target SDK
and on-device integration remain required for this increment.

Each valid frame with an active output now updates one shared white
balance estimate before rendering. The pinned HakiOS neutral/grey-cell
estimator and its smoothing apply the same gains to preview and still
outputs. A new stream start resets those gains. This does not set sensor
exposure or import a donor's color calibration.

The same command builds `nezha-camera-raw-preview`. To decode a real main
camera sample, run `nezha-camera-raw-preview main.raw10 4096 3072 5120 BGGR
1024 768 main-preview.ppm`. It accepts exactly one complete input frame,
creates a new output and reports local CPU processing time. A rendered
test fixture or a host benchmark is not a phone-camera result.
