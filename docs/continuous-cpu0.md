# Continuous CPU0 startup

The active target uses XNU 10063.141.1, native 4 KiB pages and the iPadOS 17.7.11
ABI. It starts one CPU and continues through scheduler, IOKit, root selection
and init loading. SMP follows the single-core userspace path.

The device XNU series preserves entry and VM observations, restores the normal
XNU exception vectors before machine_startup, and records subsequent phases in
reserved RAM. Successful phases continue. Invalid memory or platform inputs
retain their existing failure paths.

The ArmCpu observer samples the actual PPI27 handler, rtclock_intr return and
ICC_EOIR1_EL1 completion. It samples interrupts 1, 2, 8 and every 64 thereafter.
It does not replace the interrupt controller, alter timer programming, or invoke
the handler to manufacture a result. Its adaptation source is
UniXNU-umi@3eb72578597d2ca16471b172d45eddc096a34a04; retained notices are in
third-party/UniXNU-umi-LICENSE.

| Record | Mailbox byte range | Contents |
| --- | --- | --- |
| Loader |0..256| Boot identity, allocation, entry and CRC; separate raw stage pair |
| Early exception |256..384| Vector, syndrome, fault address, registers and prior phase |
| Platform |384..512| Actual CPU, clock, GIC and volatile NVRAM properties |
| IRQ |512..832| Handler, return, EOI and timer counters |
| Startup |832..960| Scheduler, IOKit, root and init phases |
| First panic |1024..1280| Caller, reason, register snapshot and bounded literal format |

Records use the existing 4 KiB mapping and bind the firmware boot and candidate.
The runtime packets use a sequence, payload XOR and magic/inverse commit. The
1280-byte recovery reader checks stable snapshots; older 512-byte readers do
not cover the new packets.

The minimal root path uses upstream mockfs and an own signed static ARM64/iOS17
process. That process exercises getpid and HW_PAGESIZE; it is not Apple launchd.
The guarded loader accepts the base CPU0 command or its explicitly bound rd=md0
variant, with exact kernel, ADT, root and trust-cache hashes.

See [build](build-format2.md), [status](porting-status.md) and porting.toml for
the source versions, build results and remaining device validation.
