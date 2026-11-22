/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_MEMORY_H
#define NEZHA_CAMERA_NATIVE_MEMORY_H

#include "NezhaCameraDma.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeMemory;

enum NezhaCameraNativeMemoryStep {
    NezhaMemoryIdle, NezhaMemoryArguments, NezhaMemoryLayout,
    NezhaMemoryPool, NezhaMemoryTableAllocate, NezhaMemoryTableSpans,
    NezhaMemoryFrameAllocate, NezhaMemoryFramePublish, NezhaMemoryRanges,
    NezhaMemoryBuildTables, NezhaMemoryTablePublish, NezhaMemoryHold,
    NezhaMemoryRetire, NezhaMemoryRelease, NezhaMemoryReady,
    NezhaMemoryFrameCacheMode
};

/* The existing capture result/console reports the first failed adapter
 * operation, including its actual IOReturn or ArmSmmu4KStatus. No DMA or
 * allocation is performed by this snapshot. */
struct NezhaCameraNativeMemoryFailure {
    uint32_t step, object, status, held;
    uint64_t requested_bytes, spans;
    bool reused, raw_write_combined;
};
void nezha_camera_native_memory_failure(struct NezhaCameraNativeMemoryFailure *out);

/* Normal camera worker only. The existing NativeAdrenoMemory component
 * owns the wired pages and cache operations; the camera owns its IOVAs.
 * Even an unsuccessful allocation can return an object that still owns
 * pages, which must remain alive until cleanup completes. */
bool nezha_camera_native_memory_prepare(struct NezhaCameraNativeMemory **memory,
    unsigned physical_bits, uint32_t width, uint32_t height);
const struct NezhaCameraDmaPlan *nezha_camera_native_memory_plan(const struct NezhaCameraNativeMemory *memory);
const ArmSmmu4KConfig *nezha_camera_native_memory_config(const struct NezhaCameraNativeMemory *memory);

/* Take device references before installing the context. Once BUF_DONE
 * matches a queued buffer, acquire it before CPU access, and republish
 * it before requeue. RAW pages take the existing owner's write-combined
 * attribute before their first device reference. The owner still checks
 * every acquire/publication; explicit device barriers order this sole,
 * uncached mapping without a per-frame cache-line walk. The four stale
 * frame marker pages retain their publication boundaries. Tables keep
 * their normal cache policy. No user mapping is created here. */
bool nezha_camera_native_memory_hold(struct NezhaCameraNativeMemory *memory);
const void *nezha_camera_native_memory_acquire(struct NezhaCameraNativeMemory *memory, unsigned index);
bool nezha_camera_native_memory_publish(struct NezhaCameraNativeMemory *memory, unsigned index);
/* A synchronous CPU reader can drop its claim without requeuing the frame. */
bool nezha_camera_native_memory_drop(struct NezhaCameraNativeMemory *memory, unsigned index);

/* After CPAS drain and SMMU detach/TLBSYNC, retire this session's device
 * references. A completely prepared pool stays wired for the next session,
 * bounded to two 15 MiB RAW frames and 128 KiB of tables. Partial preparation
 * frees only backing whose release succeeds. Uncertain teardown never
 * recycles the pool or drops its device references. */
bool nezha_camera_native_memory_release(struct NezhaCameraNativeMemory **memory, bool dma_detached);

#ifdef __cplusplus
}
#endif
#endif
