# CPU0 platform/clock observation005

004 proved VM initialization and early PE return with raw55. This new candidate
records actual SecureDT/PE/CNTFRQ/CNTVCT/MPIDR data and observes cpu_timebase_init
and PE_init_cpu, then stops at66 before machine_startup. It keeps IRQ/FIQ masked
and the terminal early vectors. It does not establish IOKit, real timer IRQs,
SMP, storage or userland. The dedicated512-byte early reader accepts the new
128-byte platform packet at384 only after stable boot/tag/XOR/inverse checks.
Its expected tag is a build argument, avoiding a source-head identity cycle.
No standalone Validate is required; fresh current facts and the full held-memory
probe remain inside every Boot. Use a new image and never borrow004 runtime PASS.
