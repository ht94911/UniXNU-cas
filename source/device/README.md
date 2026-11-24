Camera146 permits a new reservation after a failed stream only when the
existing source stop, IRQ join, fresh CSID reset, CPAS drain, DMA detach and
all owner releases have completed. The failed read and saved error remain;
there is no automatic restart. Uncertain cleanup and unretired IRQ faults
still refuse a new open. The same sole worker uses XNU's normal preempting
kernel priority, following Qualcomm's dedicated camera work queue. Camera145
actually displayed a brief live preview and eight nonzero final frames before
RDI overflow; its crop repair and all previous changes remain enabled.
SMMU faults retain their original quarantine gate. Continuous preview,
reopening and photographs need device verification.

Camera145 adds the measured Camera144 all-NaN normalized FinalCropRect to
its completed 1x sample-copy repair. Only mode1, owned completed output and
four NaN components select the full normalized rectangle. Finite nonempty,
partial, mixed nonfinite, infinite and foreign crops retain the original call.
Camera144 actually entered this node eight times with that undefined crop,
performed no repair and delivered eight black final samples; its normal
Android/root return and protection checks passed. Continuous RAW still ends
with RDI bit19. The new repair, preview and photographs need device validation.

Camera142 normalizes an owned completed-frame empty crop before the original
preview-stitcher calculation. Camera141's native empty-request case proved
that repairing only its returned input leaves a zero-sized destination and
an actual zero-sized dispatch. The original method now computes input, output
and shift together from the completed frame, once. Valid, partial, foreign
and invalid requests retain their original path. The native pixel consumer
checks both rectangles and sentinel replacement; actual Camera preview and
continuous RAW capture remain unverified.

Camera140 joins the stopped stream's IRQ callback before issuing the stock
CSID path-only immediate halt/reset and requiring a fresh reset receipt.
CPAS drain follows that acknowledgement; either timeout retains DMA mappings,
backing and resource ownership and forbids a restart. Existing cleanup codes
and first frame faults remain intact. Local sanitizer and ARM64e checks cover
stale receipts, delayed reset completion, failed MMIO and retained ownership.
Actual stop/drain recovery, continuous preview and photographs remain pending.

Camera137 drains up to32 already queued IRQ notices before the existing
completion wait and treats queued RUP/SOF events separately from an empty
queue. RAW staging copies run only for a pending cdev reader; unread frames
remain valid and active reads exclude copying. The original two-frame control
delay,45 MiB spare pool,cache policy and stop/fault ownership stay intact.
The current Mac target changes only FrameProbe and NativeDevice objects.
Camera136 native rectangle observations remain, while its four completed
once-only consumers are disabled for physical preview trials. Preview and
photographs still need device validation.

Camera135 keeps three owned RAW mappings with only two hardware addresses
queued. After a matched completion the worker submits its already published
spare before acquiring or copying the completed frame. That frame remains
CPU-owned through delivery and publication, then becomes the next spare.
The pool is bounded to45 MiB plus128 KiB of tables and is reused only after
drain and detach. Existing work counters record the address-update submission
separately from CPU publication; neither claims hardware acceptance or FPS.
This follows Qualcomm CAMSS's replacement-before-consumer order and retains
the134 RUP warning,131 cache policy,133 border fix and128 GiB Data. Native
preview, sustained capture and photographs still require device verification.

Camera134 follows the stock common-v1 and ISP-context handling of RUP_MISS:
preserve the observed warning and acknowledge it, then continue processing
real completions. Camera131's first terminal word0x11110 contained only this
bit in the port's extra error mask. Overflow, illegal DT, illegal batch,
queue overflow and failed MMIO still stop and retain the first fault.
This is separate from Camera133's owned border repair and needs hardware
verification of sustained capture, native preview and photos.

Camera133 repairs the completed-frame uncompressed rotation's missing outer
border through the original system border method. Camera132 verified correct
rotated quadrants, unchanged requested border pixels, and correct standalone
border fill. The repair applies only to owned NV12 frames with integral chroma
bounds after a successful rotation; original errors propagate. The original
system downscale consumer checks the resulting16x16 image. Physical preview
and photographs remain pending.

Camera131 uses the existing NativeAdrenoMemory cache-mode transition for the
two exclusive kernel RAW mappings before their first device reference. The
30 MiB pool and immutable SMMU tables remain; tables stay cached. Explicit
device barriers and the original acquire/publication ownership checks replace
each frame's whole-RAW cache-line walk. Cache-mode failures stop before DMA
and preserve their actual return. Ten real adapter cases, including failed
transitions and detached reuse, pass with ASan/UBSan. Camera130 delivered27
RAW frames and verified exposure readback before RDI bit19 backpressure;
physical preview, sustained capture and photographs remain unverified.

Camera130 reads the initial eleven exposure register bytes after the complete mode is applied and before DMA, RDI or sensor streaming. The first completed frame qualifies that actual standby snapshot; the existing two-frame metadata delay and later queued-control readback stay intact. Initial NACK/timeout stops before streaming. This removes the five synchronous initial reads from the live two-buffer path; physical preview and photographs still need device verification.

Camera129 retains the successfully prepared two-frame RAW pool and its immutable SMMU table plan across fully drained, detached close/open cycles. The fixed budget remains 30 MiB of RAW backing plus128 KiB of tables. Real allocation, publication and device-hold errors are copied to the existing capture result and console. Eight actual adapter cases,7680 scatter-backed page translations, four reopens under forced allocator refusal and existing capture/control/Stop tests pass with ASan/UBSan; physical preview and photos remain pending.

Camera127 selects the existing 128 GiB AppSync Data seed and pinned file. Its fixed typed volume UUID input is changed in the original kernel string; all executable bytes, bounded provider wait, GPU115 and Camera126 provider stay byte-identical. The actual 4,363 Data and 9,006 protected extents fit the original 16,384-entry mapper. ASan/UBSan covers those real maps and the sparse seed; the current kernel and packed input reached the eight-CPU QEMU display/HID checkpoint. That VM does not start persistent Data. Physical mount, runtime logs, native preview and photos require device evidence.

Camera122 retains the verified Camera121 original renderer fallback and disables its completed authored-pixel helper. This candidate holds the native Camera scene for physical preview, shutter and photo review. Its renderer provider, GPU115 and kernel092 are byte-identical to Camera121.

Camera121 chooses the original uncompressed renderer when FigM2MController reports no positive scale capabilities. The original renderer methods keep the same pool, fractional rectangle setting and call count; existing native scaler limits are not changed. Camera120 verified zero limits and a nil controller on nezha. Native Camera preview and photos remain unverified.

Camera119 sends the unchanged Camera118 original-controller probe output to /dev/console so a full iOS Data volume does not prevent launchd from opening its new log file. GPU115 and kernel092 remain byte-identical. The current Data generation is preserved.

Camera118 reads the original FigM2MController scale capabilities and uses its native black fill, centred transfer, downsample and expansion methods on authored NV12 images. The system generates conversion parameters. Runtime method encodings are checked against retained24A446 metadata; no camera is opened. Camera115 GPU fixes and kernel092 remain the hardware baseline. Native preview and photos are unverified.

Camera117 calls the original VTPixelTransferSession API for uniform and
letterboxed NV12 inputs. The system selects its transfer implementation and
generates all conversion parameters. Four normal and letterbox transfers keep
the centre luminance and report output rows, with no camera or permission
request. This follows Camera116's verified known-pixel downscale and keeps
physical Camera preview and photo acceptance separate.

Camera116 replays the original VideoToolbox downscale from a 1200×2608 NV12
surface into a 16×16 surface, using the constants from Camera115 command27.
Independent checks read the source, sample it on the GPU, and write the same
packed R32Uint plane views. Three known colour inputs include one written by
the GPU before sampling. Original system shaders, filter tables and approximate
uniform bytes are supplied only from the local fixture directory. The helper
opens no camera and requests no user permissions; preview and photos remain
separate device acceptance.

Camera108 negotiates the existing BGRA renderer for PrimaryScaler preview.
SecondaryScaler and StillImage retain BGRA and both NV12 ranges. The measured
RAW geometry, frame timing and capture lifecycle stay the same. Host checks
cover the advertised output formats and real pool callbacks; native Camera
preview and photos require device validation.

Camera107 uses the existing texture-log switch and stderr to observe the render
server's IOSurface imports, with its original executable and Mach services.
The current host entry reads its package directly from I:/Code/UniXNU-nezha;
the Ubuntu source and build directories are hosted by the I-drive VHDX.
Camera106's filter coefficients are retained, so the raw coefficient dump ends.
Physical preview and photos remain unverified.

Camera091 groups the original mode1 fourteen exposure/gain writes into one locked CCI queue submission with one completion REPORT. Separate register messages, original byte order, sensor group brackets, two-frame setting delay and readback remain. A failed group has an unknown applied prefix and retains its group state for the original teardown. Continuous host/arm64e checks pass; reduced RDI backpressure remains a hardware check.

Camera089: the original cameracaptured exposure ioctl now passes both file and vnode checks for the existing exact signed-reader/native-node predicate. GPU051, MMU prefetch and the mobile launcher are retained. The full source consumer checks both stages and refusal cases; main preview and photos require hardware verification.

GPU047 joint candidate: see `docs/GPU047-INTEGRATION.md` for selected successful modules, resource integration and pending native/package validation. GPU performance work continues after integration.

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

GPU046 tests a conditional 24A446 AssistantServices fixup over GPU045. When the
offline dictation status is nil, the original availability query leaves its
cache nil and queues another availability notification. UIKit's keyboard dock
handles that notification by querying availability and looking up glyph images
again. GPU045 sampled this chain while its main thread used 36.391 seconds of
CPU in a 40.900-second bracket; the exact runtime nil object was not captured.
The fixup omits only that nil-status notification. Non-nil results, language
selection, ARC cleanup and the original block payload remain unchanged.
Twenty-four executions of the original ARM64 instructions with modelled
framework calls pass, including three slides and the nil callback cycle.
The native signed-page parser/apply checks pass twenty cases. Native042,
Metal045, the PNG043 fixup and Data generation 1 are retained. Device response
and icon-service stability remain pending.

Camera105 prepares a finite 718MiB self-only increase for cameracaptured through
XNU12377 development command22, the original dyld root-image interface. Existing
active/inactive fatal attributes stay in the kernel. The injected provider logs
the command result and task footprint/remaining-limit observations once at load.
The exact process/pid/command/error consumer passes ten ASan/UBSan cases; native
compilation and actual service lifetime still require validation.

Camera111 uses the existing once-only Metal helper to check imported R8Uint and
R16Uint writes over 64 by 16 and 1200 by 2608 NV12 surfaces. Integer patterns
check every Y and UV byte. A separate combined half sampler passes float4
values through another lane's threadgroup memory at 16 KiB and 32 KiB before
writing both planes. The helper opens no camera, requests no permission and
records completion separately from pixel comparison. Physical Camera preview
and photo acceptance remain pending.
