# cas source maintenance

Read README.md, CONTRIBUTING.md, porting.toml and docs/build-format2.md before
changing this repository. The device target is Xiaomi10Ultra(cas), SM8250,
XNU10063.141.1, native4KiB pages and iPadOS17.7.11(j120ap).

## Source boundaries

- Board wiring, RAM reservations, policy and packaging belong here.
- Shared CPU/controller resources belong in UniXNU-sm8250-common.
- Generic build logic and OS profiles belong in UniXNU; reusable drivers remain
  in their component repositories. Use canonical component IDs and preserve
  their runtime ABI names.
- The companion uefi-cas repository owns the BootShim and UEFI platform code.

## Changes and validation

Use the standard architecture/SoC/OS/device configuration and patch order.
Keep feeds.toml and unixnu.lock complete. Local paths belong in ignored
local.toml. Stage board-specific component overlays in generated output;
retain clean, pinned component source checkouts.

Update the affected porting.toml areas with concrete findings or evidence.
Record source checks, host builds and device execution separately. Reuse valid
evidence for unchanged inputs. Keep accepted tags, images and reports immutable.
The active boot target is continuous CPU0 startup through minimal userland;
SMP and additional hardware are separate work. Successful checkpoints log
without introducing a stop or a separate reboot cycle.

## Inputs and output

Only RAM temporary boot and read-only RAM storage are supported. Preserve
the bounded BootShim, exact allocation and held-range checks. Exclude original
firmware, IPSW, private ADT/identity, recovery data and private keys from Git.
Public changes require source review, applicable checks and bilingual documents.

References: https://unixnu.com/porting/device-repository,
https://unixnu.com/porting/soc-support and https://unixnu.com/building.
