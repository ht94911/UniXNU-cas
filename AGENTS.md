# cas source contributions

Read README.md, docs/build.md, docs/validation.md and CONTRIBUTING.md before
changing the port. This is the public device source repository. The companion
uefi-cas repository owns firmware sources; both form one cas device project.

Keep hardware-tested tags and accepted artifacts immutable. Distinguish source,
host, build, emulator and actual device evidence. Never claim a new image works
on hardware based on another image's result. Keep bilingual docs in sync.

If a local management/evidence workspace is supplied by the human, follow its
AGENTS.md and validated ledger as well. Do not publish private device data or
original firmware. Do not spawn subagents without explicit human authorization.

2026-10-25 latest human instruction: do not push to GitHub without a new explicit request. Do not dispatch remote Actions or publish artifacts without that request. Continue local work and save local commits/patches; read-only reference queries remain permitted.

## Current format2 integration

Use board.toml schema2, feeds.toml/unixnu.lock, the SM8250 common layer and the
matched iPadOS17.7.11 profile. The active build is docs/build-format2.md; old EFI
and source-only profiles remain historical. Keep runtime identifiers distinct
from canonical repository names. Standard XNU series order is arch/SoC/OS/device.
Keep the source module checkouts unchanged when staging cas-only observations.
Record all13 areas in porting.toml; source/host/device evidence are distinct.
The active direction is single-core continuous startup toward userland/UI, SMP
afterward. Successful checkpoints never require a separate stop or old test rerun.
Private images, identities and keys stay outside Git; device use is RAM-only.
