/* SPDX-License-Identifier: MIT */
#ifndef A840_PACK_CACHE_RT_H
#define A840_PACK_CACHE_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "a840_emit_3d.h"
#include "../../../adreno_compile/pack/a840_pack_v1.h"
#include "a840_runtime_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum A840PackHitTier {
    A840_PACK_HIT_NONE = 0,
    A840_PACK_HIT_TIER0 = 1,
    A840_PACK_HIT_TIER1 = 2
} A840PackHitTier;

typedef struct A840PackTierMount {
    A840PackV1View view;
    uint64_t gpu_va_base; /* 0 if CPU-only; 128B-aligned RO GPU VA when GPU-mapped */
    uint64_t bo_handle;   /* 0 for KMD Method 23 shared mapping, or BO handle */
    bool mounted;
} A840PackTierMount;

typedef struct A840PackCacheStats {
    uint64_t tier0_hits;
    uint64_t tier1_hits;
    uint64_t misses;
} A840PackCacheStats;

/* Zero-copy two-tier runtime lookup engine over immutable pack.bin v1 views:
 *   Tier-0: KMD Method 23 read-only system shared pack (cross-process)
 *   Tier-1: Optional per-process / persistent read-only pack
 * Lookup order: Tier-0 O(1) -> Tier-1 O(1) -> MISS (zero heap allocation,
 * zero payload memcpy, strict stack_key + abi_fingerprint validation). */
typedef struct A840PackCacheRt {
    uint64_t expected_stack_key;
    uint64_t expected_abi_fingerprint;
    A840PackTierMount tier0;
    A840PackTierMount tier1;
    A840PackCacheStats stats;
} A840PackCacheRt;

typedef struct A840PackStageGpuAddrs {
    uint64_t bo_handle;
    uint64_t isa_offset;
    uint64_t isa_gpu_va;   /* 128B-aligned SP_*_OBJ_START GPU VA (zero ISA upload) */
    uint32_t isa_dwords;
    uint64_t const_offset;
    uint64_t const_gpu_va;
    uint32_t const_dwords;
    uint64_t sds_offset;
    uint64_t sds_gpu_va;   /* Direct CP_SET_DRAW_STATE GPU VA */
    uint32_t sds_dwords;
} A840PackStageGpuAddrs;

typedef struct A840PackLookupResult {
    A840PackHitTier hit_tier;
    A840PackV1Entry entry;
    A840PackStageGpuAddrs stage_gpu[2];
} A840PackLookupResult;

A840RtStatus a840_pack_cache_rt_init(A840PackCacheRt *cache,
                                     uint64_t expected_stack_key,
                                     uint64_t expected_abi_fp);

void a840_pack_cache_rt_reset(A840PackCacheRt *cache);

/* Mount read-only Tier-0 shared pack (tier must equal 0). Failure leaves
 * existing Tier-0 mount unchanged. */
A840RtStatus a840_pack_cache_rt_mount_tier0(A840PackCacheRt *cache,
                                            const void *cpu_base,
                                            size_t bytes,
                                            uint64_t gpu_va_base,
                                            uint64_t bo_handle);

/* Convenience mount directly from a KMD Method 23 SharedPackReplyV3. */
A840RtStatus a840_pack_cache_rt_mount_tier0_reply(
    A840PackCacheRt *cache,
    const struct NativeAdrenoAbiSharedPackReplyV3 *reply,
    uint64_t bo_handle);

/* Mount optional read-only Tier-1 process/persistent pack (tier must equal 1). */
A840RtStatus a840_pack_cache_rt_mount_tier1(A840PackCacheRt *cache,
                                            const void *cpu_base,
                                            size_t bytes,
                                            uint64_t gpu_va_base,
                                            uint64_t bo_handle);

void a840_pack_cache_rt_unmount_tier0(A840PackCacheRt *cache);
void a840_pack_cache_rt_unmount_tier1(A840PackCacheRt *cache);

/* Hot-path O(1) zero-copy lookup: Tier-0 -> Tier-1 -> MISS.
 * Returns A840_RT_OK on hit, A840_RT_FALLBACK_REQUIRED on miss. */
A840RtStatus a840_pack_cache_rt_lookup(A840PackCacheRt *cache,
                                       const A840PipelineKey *key,
                                       A840PackLookupResult *out_result);

A840RtStatus a840_pack_cache_rt_reconstruct_record(const A840PackCacheRt *cache,
                                                   const A840PackLookupResult *lookup,
                                                   A840PipelineRecord *out_record);

/* Direct GPU VA conversion helpers for SP_*_OBJ_START and CP_SET_DRAW_STATE. */
A840RtStatus a840_pack_cache_rt_make_shader_code(const A840PackLookupResult *lookup,
                                                 uint32_t stage_index,
                                                 const A840CompiledStage *stage_meta,
                                                 A840ShaderCode *out_code);

A840RtStatus a840_pack_cache_rt_make_sds_group(const A840PackLookupResult *lookup,
                                               uint32_t stage_index,
                                               uint32_t group_id,
                                               uint32_t enable_mask,
                                               A840SdsGroupDesc *out_sds);

A840PackCacheStats a840_pack_cache_rt_stats(const A840PackCacheRt *cache);
void a840_pack_cache_rt_reset_stats(A840PackCacheRt *cache);

#ifdef __cplusplus
}
#endif

#endif /* A840_PACK_CACHE_RT_H */
