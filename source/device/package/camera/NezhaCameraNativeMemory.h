/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_MEMORY_H
#define NEZHA_CAMERA_NATIVE_MEMORY_H

#include "NezhaCameraDma.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeMemory;

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
 * it before requeue. No user mapping is created here. */
bool nezha_camera_native_memory_hold(struct NezhaCameraNativeMemory *memory);
const void *nezha_camera_native_memory_acquire(struct NezhaCameraNativeMemory *memory, unsigned index);
bool nezha_camera_native_memory_publish(struct NezhaCameraNativeMemory *memory, unsigned index);
/* A synchronous CPU reader can drop its claim without requeuing the frame. */
bool nezha_camera_native_memory_drop(struct NezhaCameraNativeMemory *memory, unsigned index);

/* No backing may be freed before CPAS drain and SMMU detach/TLBSYNC.
 * An uncertain teardown keeps the object and its device references. */
bool nezha_camera_native_memory_release(struct NezhaCameraNativeMemory **memory, bool dma_detached);

#ifdef __cplusplus
}
#endif
#endif
