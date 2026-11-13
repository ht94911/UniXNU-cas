# Complete sheng diagnostic port for umi

The implementation is imported from UniXNU's Xiaomi Pad 6S Pro (`sheng`,
SM8550) device repository, not the Mi 6 or the ordinary Pad 6. `upstream.json`
records the pinned commit and the original hashes. Each source retains its
license. The font retains its AOSP license.

This port keeps the official recorder, stage history, console log, independent
pulse, process/thread observers, EL0 fault records, exit records, UI trace,
display handoff, deadline modes and optional driver status views. It does not
replace them with a separate reduced diagnostic.

Umi adaptations:

- Missing storage/root telemetry is an optional import. Its display says
  **not connected**, with root `-1` meaning the root-audit provider is absent.
  No dummy storage driver, success response or exported zero-counter provider
  is added. Linked real providers continue to supply their original counters.
- `umi-build-tag` identifies the image. The 1080 × 2340 phone uses scale 2;
  recording retains all 120 columns, while the visible row clips at the
  framebuffer's width. Other dimensions use the original scale selection.
- Pixel stores use XNU's validated console mapping. An independent persistent
  XNU IO mapping of the actual `PE_current_console` framebuffer supplies the
  pulse before the LegacyDisplay service is available. No fixed physical
  display address or SM8550 controller register is used.
- A bounded 16-word observation records binding and monitor passes 1, 3 and 8.
  Phone builds additionally copy those observations to the existing RAM
  console; QEMU builds contain no phone physical-memory trace.
- The build uses the actual source kernel compile recipes, disables FP/SIMD
  in the observers, and inspects generated ARM64 instructions.

The full upstream `BootWatchdogPolicy.c` is retained. Its modifying paths are
opt-in; the umi menu does not set `sheng-disable-watchdogd` or
`sheng-disable-jobs`. Diagnostic mode `boot-progress=2` continues execution
after a deadline. This work does not enable root-trust overrides.

## Component boundaries

The upstream build guard couples the diagnostic to UFS, Virtio and owner-root
because the original diagnostic directly imports their telemetry. Drawing
text itself uses `boot_progress_bind` and the CPU pixel writer. The assembled
umi build script uses the explicitly selected, provider-aware full port;
the pinned UniXNU build script remains unchanged.

The official `NativeUfs` implementation contains SM8550 controller, clock,
SMMU and DMA assumptions. These need qualification against umi's device tree
and firmware state before enabling the controller. No userdata writes are
part of screen diagnostics.

The official `NativeLegacyDisplay` integrates the original
IOMobileFramebufferLegacy ABI and system-surface scanout. It needs a separate
donor ABI audit/prelink closure and provider validation on umi. A working
diagnostic page alone does not establish that display service, GPU support,
a mounted root volume or running iOS user space.
