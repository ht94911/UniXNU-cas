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
