# GPU050 large allocation observation

GPU047, GPU048 and GPU049 each retained a foreground 64 MiB memory creation
inside a 163–167 ms `NBHeap textureWithDescriptor:offset:` call. GPU049 records
165.440 ms wall time and 35.291 ms thread CPU; its contained memory operation
takes 165.212 ms. The corresponding pool is 1 in GPU049 and 0 in the earlier
two examples. These are the same class of delay, not identical allocations.

The selected Native043 driver already measures seven BoAlloc phases through
`native_adreno_bo_hook`. Its final kernel has a zero-initialized hook and no
board installer, so previous captures cannot distinguish create, wire, clean
and lock time. GPU050 installs this existing hook after GPU publication and
before the Metal accelerator is made available. It retains allocations of
at least 256 KiB taking at least 10 ms through the existing event65 metadata
and USB console observer.

The ABI is the unchanged 56-byte `NativeAdrenoBoTrace` in the selected
`NativeAdrenoPublish.cpp`. The installation pattern comes from
UniXNU-sheng `24aab96163799cb1db083d94539a5eefc07e0475`,
`package/BootDiagExport.c:boalloc_hook`; the shared driver's upstream
`54b41b8f59e90a167f6f75160739c5f825945fbc` retains that ABI.
No allocation, mapping, cache maintenance or lock policy changes here.

Each sample has three text records. Join by PID and `i`, the original
`mach_absolute_time` entry counter:

| Record | Values |
| --- | --- |
| `bo0` | `i` entry ticks, `t` caller thread, `k` KiB, `f` original flags, `e` status in hex |
| `bo1` | `l` initial limit-lock phase, `c` descriptor creation, `w` wiring, `z` cleaning |
| `bo2` | `q` second lock wait, `d` dispatch under the lock, `r` discard, `s` physical spans |

All phase durations are the original driver's microseconds. The entry
counter requires the captured boot timebase before comparison with userland.
A partial three-record group remains incomplete; the original trace store
may drop records rather than block the producer. Console print time is not
allocation entry time.

`tests/check-gpu-bo-phases.py --native <selected-adreno-source> --out <workspace-output>`
compares the production structure with the selected driver's ABI and runs
the actual adapter and existing trace store under ASan/UBSan. Disabled,
small, fast, exact-boundary and full-width counter cases pass locally.
The host model does not compile or execute the XNU target.

Native acceptance must confirm a real store of the authenticated callback
to `_native_adreno_bo_hook`, the callback's calls to the existing trace
writer, and the installation log in the final candidate. A new boot must
then supply all three records for the relevant foreground allocation.
GPU049's incomplete Data read remains independent of this source work.
