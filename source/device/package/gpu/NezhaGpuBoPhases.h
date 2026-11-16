/* SPDX-License-Identifier: MIT */
/* NativeAdrenoPublish.cpp's existing 56-byte BoAlloc phase-hook ABI.
 * Source: UniXNU/Adreno740, 54b41b8f59e90a167f6f75160739c5f825945fbc.
 * Hook installation follows UniXNU-sheng BootDiagExport.c; records use the
 * already retained GPU problem-text channel instead of another export ring.
 */
#ifndef NEZHA_GPU_BO_PHASES_H
#define NEZHA_GPU_BO_PHASES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "NativeGpuTrace.h"

struct NativeAdrenoBoTrace {
    uint64_t entered;
    uint32_t kib, flags, limit_us, create_us, wire_us, clean_us, lock_us,
        dispatch_us, discard_us, spans;
    int32_t status;
    uint32_t reserved;
};
static_assert(sizeof(NativeAdrenoBoTrace) == 56, "existing BoAlloc hook ABI");
static_assert(offsetof(NativeAdrenoBoTrace, limit_us) == 16 &&
              offsetof(NativeAdrenoBoTrace, status) == 48, "BoAlloc phase offsets");
extern "C" {
extern void (*native_adreno_bo_hook)(const NativeAdrenoBoTrace *);
}

/* i is the hook's original mach_absolute_time entry counter, not microseconds.
 * Join all three records by (PID,i). The thread is the calling allocation
 * thread. l/c/w/z/q/d/r are the existing limit/create/wire/clean/lock/dispatch/
 * discard durations in microseconds; k is KiB, f flags, s spans, e status.
 * Each line fits the existing 88-byte text payload even at integer maxima.
 * The trace writer is nonblocking and retains its existing drop accounting.
 */
static void nezhaGpuRecordBoPhases(const NativeAdrenoBoTrace *trace,
                                 uint32_t pid, uint64_t thread)
{
    if (!native_gpu_trace_enabled()) return;
    const uint64_t total = uint64_t(trace->limit_us) + trace->create_us +
        trace->wire_us + trace->clean_us + trace->lock_us +
        trace->dispatch_us + trace->discard_us;
    if (trace->kib < 256 || total < 10000) return;
    char text[3][89]{};
    const auto entered = static_cast<unsigned long long>(trace->entered);
    snprintf(text[0], sizeof text[0], "bo0 i=%llu t=%llu k=%u f=%x e=%x",
             entered, static_cast<unsigned long long>(thread), trace->kib,
             trace->flags, uint32_t(trace->status));
    snprintf(text[1], sizeof text[1], "bo1 i=%llu l=%u c=%u w=%u z=%u",
             entered, trace->limit_us, trace->create_us, trace->wire_us,
             trace->clean_us);
    snprintf(text[2], sizeof text[2], "bo2 i=%llu q=%u d=%u r=%u s=%u",
             entered, trace->lock_us, trace->dispatch_us, trace->discard_us,
             trace->spans);
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t values[12]{};
        memcpy(&values[1], text[i], 88);
        native_gpu_trace_record(GpuTraceProblem, pid, values, 12);
    }
}
#endif
