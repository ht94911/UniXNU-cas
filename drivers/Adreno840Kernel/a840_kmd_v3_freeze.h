/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_V3_FREEZE_H
#define A840_KMD_V3_FREEZE_H

#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "chip_facts_a840.h"
#include "a840_abi_v3_host.h"
#include "a840_kmd_zero_page.h"
#include "a840_kmd_userclient_v3.h"
#include "a840_kmd_sparse_va.h"
#include "a840_kmd_ca_env_policy.h"
#include "a840_kmd_preempt_ring.h"
#include "a840_kmd_gmu_dcvs.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Portable compile-time assertion macro (C11 _Static_assert / C++17 static_assert). */
#if defined(__cplusplus)
#define A840_V3_FREEZE_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define A840_V3_FREEZE_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* ============================================================================
 * 1. Official Release Freeze Constants (NativeAdrenoABI v3 Production Surface)
 * ============================================================================ */

#define A840_KMD_V3_FREEZE_ABI_VERSION          3u
#define A840_KMD_V3_FREEZE_ENVELOPE_BYTES       3104u
#define A840_KMD_V3_FREEZE_PAGE_BYTES           4096u
#define A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES      65536u
#define A840_KMD_V3_FREEZE_UBWC_VERSION         6u
#define A840_KMD_V3_FREEZE_UBWC_SWIZZLE         6u
#define A840_KMD_V3_FREEZE_MACROTILE_MODE       1u
#define A840_KMD_V3_FREEZE_SMMU_PAGE_SHIFT      12u
#define A840_KMD_V3_FREEZE_GPU_MIN_MHZ          160u
#define A840_KMD_V3_FREEZE_GPU_MAX_MHZ          1200u
#define A840_KMD_V3_FREEZE_GMU_MIN_MHZ          475u
#define A840_KMD_V3_FREEZE_GMU_MAX_MHZ          750u
#define A840_KMD_V3_FREEZE_THERMAL_TIERS        5u
#define A840_KMD_V3_FREEZE_THERMAL_CAP_75C_MHZ  1000u
#define A840_KMD_V3_FREEZE_THERMAL_CAP_95C_MHZ  578u

/* Frozen v2 core production method numbers (1..19 preserved verbatim by v3). */
enum {
    A840_KMD_V3_METHOD_QUERY           = 1u,
    A840_KMD_V3_METHOD_BO_ALLOC        = 2u,
    A840_KMD_V3_METHOD_BO_MAP          = 3u,
    A840_KMD_V3_METHOD_BO_FREE         = 4u,
    A840_KMD_V3_METHOD_SUBMIT          = 5u,
    A840_KMD_V3_METHOD_WAIT_FENCE      = 6u,
    A840_KMD_V3_METHOD_GET_TIMESTAMP   = 7u,
    A840_KMD_V3_METHOD_GET_STATUS      = 8u,
    A840_KMD_V3_METHOD_SET_METADATA    = 9u,
    A840_KMD_V3_METHOD_GET_METADATA    = 10u,
    A840_KMD_V3_METHOD_CREATE_QUEUE    = 11u,
    A840_KMD_V3_METHOD_CLOSE_QUEUE     = 12u,
    A840_KMD_V3_METHOD_BO_CPU_MAP      = 13u,
    A840_KMD_V3_METHOD_BO_CPU_UNMAP    = 14u,
    A840_KMD_V3_METHOD_GET_DEVICE_INFO = 15u,
    A840_KMD_V3_METHOD_BO_IMPORT       = 16u,
    A840_KMD_V3_METHOD_SET_PRIORITY    = 17u,
    A840_KMD_V3_METHOD_NOTE            = 18u,
    A840_KMD_V3_METHOD_SUBMIT_BATCH    = 19u
};

/* Explicit production selector whitelist bounds [1..26]. */
#define A840_KMD_V3_PROD_SELECTOR_MIN  1u
#define A840_KMD_V3_PROD_SELECTOR_MAX  26u

/* Deprecated legacy Turnip / Vulkan compatibility selector IDs (all purged). */
enum {
    A840_KMD_LEGACY_SELECTOR_INVALID            = 0u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_DRM_SUBMIT  = 27u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_SYNCOBJ     = 28u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_TIMELINE    = 29u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_SHADOW_COPY = 30u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_BRIDGE_CALL = 31u,
    A840_KMD_LEGACY_SELECTOR_TURNIP_WSI_SYNC    = 32u
};

/* Frozen production capability flag masks (v2 bits 0..3 + v3 bits 8..14). */
#define A840_KMD_V3_PROD_V2_CAP_MASK \
    ((uint64_t)(NativeAdrenoParamsV2HostImport | \
                NativeAdrenoParamsV2OwnedCoherent | \
                NativeAdrenoParamsV2MemoryPressure | \
                NativeAdrenoParamsV2SubmitBatch))

#define A840_KMD_V3_PROD_V3_CAP_MASK \
    ((uint64_t)(NativeAdrenoParamsV3TimelineWaitAny | \
                NativeAdrenoParamsV3SharedZeroPage | \
                NativeAdrenoParamsV3SparseVmBind | \
                NativeAdrenoParamsV3SharedTier0Pack | \
                NativeAdrenoParamsV3WriterPromise | \
                NativeAdrenoParamsV3UbwcSurfaceMeta | \
                NativeAdrenoParamsV3PreemptRings))

#define A840_KMD_V3_PROD_CAP_MASK \
    (A840_KMD_V3_PROD_V2_CAP_MASK | A840_KMD_V3_PROD_V3_CAP_MASK)

/* Minimum mandatory v3 capability bits required for production qualification. */
#define A840_KMD_V3_REQUIRED_PROD_CAPS \
    ((uint64_t)(NativeAdrenoParamsV2HostImport | \
                NativeAdrenoParamsV2SubmitBatch | \
                NativeAdrenoParamsV3TimelineWaitAny | \
                NativeAdrenoParamsV3SharedZeroPage | \
                NativeAdrenoParamsV3SparseVmBind | \
                NativeAdrenoParamsV3SharedTier0Pack | \
                NativeAdrenoParamsV3WriterPromise | \
                NativeAdrenoParamsV3UbwcSurfaceMeta | \
                NativeAdrenoParamsV3PreemptRings))

/* Deprecated legacy Turnip / compatibility shim flags (strictly fail-closed). */
#define A840_KMD_LEGACY_FLAG_TURNIP_SHIM          (UINT64_C(1) << 4)
#define A840_KMD_LEGACY_FLAG_TURNIP_SHADOW_BLIT   (UINT64_C(1) << 5)
#define A840_KMD_LEGACY_FLAG_TURNIP_WSI_BRIDGE    (UINT64_C(1) << 6)
#define A840_KMD_LEGACY_FLAG_TURNIP_FALLBACK_COMP (UINT64_C(1) << 7)
#define A840_KMD_LEGACY_FLAG_TURNIP_SYNCOBJ_EMU   (UINT64_C(1) << 15)
#define A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE (UINT64_C(1) << 16)
#define A840_KMD_LEGACY_FLAG_ALL_KNOWN_MASK \
    (A840_KMD_LEGACY_FLAG_TURNIP_SHIM | \
     A840_KMD_LEGACY_FLAG_TURNIP_SHADOW_BLIT | \
     A840_KMD_LEGACY_FLAG_TURNIP_WSI_BRIDGE | \
     A840_KMD_LEGACY_FLAG_TURNIP_FALLBACK_COMP | \
     A840_KMD_LEGACY_FLAG_TURNIP_SYNCOBJ_EMU | \
     A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE)

/* ============================================================================
 * 2. Compile-Time Hard Freeze (_Static_assert / static_assert)
 * ============================================================================ */

/* ABI versions and 3104-byte IOKit external-method envelope. */
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoAbiVersionV2 == 2u,
    "NativeAdrenoAbiVersionV2 frozen at 2");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoAbiVersionV3 == A840_KMD_V3_FREEZE_ABI_VERSION,
    "NativeAdrenoAbiVersionV3 frozen at 3");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoAbiEnvelopeBytes == A840_KMD_V3_FREEZE_ENVELOPE_BYTES,
    "NativeAdrenoAbiEnvelopeBytes frozen at 3104 bytes");

/* Capability flag bit positions and zero overlap with legacy Turnip flags. */
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV2HostImport == (1u << 0), "V2HostImport bit 0");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV2OwnedCoherent == (1u << 1), "V2OwnedCoherent bit 1");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV2MemoryPressure == (1u << 2), "V2MemoryPressure bit 2");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV2SubmitBatch == (1u << 3), "V2SubmitBatch bit 3");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3TimelineWaitAny == (1u << 8), "V3TimelineWaitAny bit 8");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3SharedZeroPage == (1u << 9), "V3SharedZeroPage bit 9");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3SparseVmBind == (1u << 10), "V3SparseVmBind bit 10");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3SharedTier0Pack == (1u << 11), "V3SharedTier0Pack bit 11");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3WriterPromise == (1u << 12), "V3WriterPromise bit 12");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3UbwcSurfaceMeta == (1u << 13), "V3UbwcSurfaceMeta bit 13");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoParamsV3PreemptRings == (1u << 14), "V3PreemptRings bit 14");
A840_V3_FREEZE_STATIC_ASSERT((A840_KMD_V3_PROD_CAP_MASK & A840_KMD_LEGACY_FLAG_ALL_KNOWN_MASK) == 0u,
    "Production capability mask and legacy Turnip flags must be strictly disjoint");

/* Method numbers: v2 Methods 1..19 (incl. 10..19) and v3 Methods 20..26 (incl. 20..24). */
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V2_METHOD_MIN == 1u, "v2 min method == 1");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V2_METHOD_MAX == 19u, "v2 max method == 19");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_GET_METADATA == 10u, "Method 10 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_CREATE_QUEUE == 11u, "Method 11 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_CLOSE_QUEUE == 12u, "Method 12 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_BO_CPU_MAP == 13u, "Method 13 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_BO_CPU_UNMAP == 14u, "Method 14 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_GET_DEVICE_INFO == 15u, "Method 15 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_BO_IMPORT == 16u, "Method 16 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_SET_PRIORITY == 17u, "Method 17 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_NOTE == 18u, "Method 18 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_V3_METHOD_SUBMIT_BATCH == 19u, "Method 19 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodWaitAnyV3 == 20u, "Method 20 WaitAnyV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodGetZeroPageV3 == 21u, "Method 21 GetZeroPageV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodMapSparseV3 == 22u, "Method 22 MapSparseV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodMapSharedPackV3 == 23u, "Method 23 MapSharedPackV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodWriterPromiseV3 == 24u, "Method 24 WriterPromiseV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodSurfaceMetaV3 == 25u, "Method 25 SurfaceMetaV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoMethodQueueConfigV3 == 26u, "Method 26 QueueConfigV3 locked");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoPromiseMake == 1u, "PromiseMake == 1");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoPromiseFulfill == 2u, "PromiseFulfill == 2");
A840_V3_FREEZE_STATIC_ASSERT(NativeAdrenoPromiseCancel == 3u, "PromiseCancel == 3");

/* Exact wire struct sizes and key field offsets for all NativeAdrenoABI_v3.h structs. */
A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiWaitAnyV3) == 528u,
    "WaitAnyV3 exact wire size == 528");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiWaitAnyV3) <= NativeAdrenoAbiEnvelopeBytes,
    "WaitAnyV3 fits 3104-byte envelope");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyV3, count) == 0u, "WaitAnyV3.count@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyV3, flags) == 4u, "WaitAnyV3.flags@4");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyV3, timeout_ns) == 8u, "WaitAnyV3.timeout_ns@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyV3, queues) == 16u, "WaitAnyV3.queues@16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyV3, fences) == 272u, "WaitAnyV3.fences@272");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiWaitAnyReplyV3) == 16u,
    "WaitAnyReplyV3 exact wire size == 16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyReplyV3, satisfied_mask) == 0u,
    "WaitAnyReplyV3.satisfied_mask@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyReplyV3, timed_out) == 4u,
    "WaitAnyReplyV3.timed_out@4");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWaitAnyReplyV3, observed_ticks) == 8u,
    "WaitAnyReplyV3.observed_ticks@8");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiZeroPageReplyV3) == 16u,
    "ZeroPageReplyV3 exact wire size == 16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiZeroPageReplyV3, gpu_va) == 0u,
    "ZeroPageReplyV3.gpu_va@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiZeroPageReplyV3, bytes) == 8u,
    "ZeroPageReplyV3.bytes@8");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiSharedPackReqV3) == 16u,
    "SharedPackReqV3 exact wire size == 16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReqV3, expected_stack_key) == 0u,
    "SharedPackReqV3.expected_stack_key@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReqV3, map_cpu) == 8u,
    "SharedPackReqV3.map_cpu@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReqV3, reserved) == 12u,
    "SharedPackReqV3.reserved@12");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiSharedPackReplyV3) == 32u,
    "SharedPackReplyV3 exact wire size == 32");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReplyV3, gpu_va) == 0u,
    "SharedPackReplyV3.gpu_va@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReplyV3, cpu_va) == 8u,
    "SharedPackReplyV3.cpu_va@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReplyV3, bytes) == 16u,
    "SharedPackReplyV3.bytes@16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSharedPackReplyV3, actual_stack_key) == 24u,
    "SharedPackReplyV3.actual_stack_key@24");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiWriterPromiseV3) == 160u,
    "WriterPromiseV3 exact wire size == 160");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, op) == 0u,
    "WriterPromiseV3.op@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, surface_count) == 4u,
    "WriterPromiseV3.surface_count@4");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, promise_token) == 8u,
    "WriterPromiseV3.promise_token@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, queue) == 16u,
    "WriterPromiseV3.queue@16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, fence) == 24u,
    "WriterPromiseV3.fence@24");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseV3, surface_ids) == 32u,
    "WriterPromiseV3.surface_ids@32");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3) == 16u,
    "WriterPromiseReplyV3 exact wire size == 16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseReplyV3, promise_token) == 0u,
    "WriterPromiseReplyV3.promise_token@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseReplyV3, accepted) == 8u,
    "WriterPromiseReplyV3.accepted@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiWriterPromiseReplyV3, reserved) == 12u,
    "WriterPromiseReplyV3.reserved@12");

A840_V3_FREEZE_STATIC_ASSERT(sizeof(struct NativeAdrenoAbiSurfaceMetaV3) == 48u,
    "SurfaceMetaV3 exact wire size == 48");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, bo_handle) == 0u,
    "SurfaceMetaV3.bo_handle@0");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, op) == 8u,
    "SurfaceMetaV3.op@8");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, ubwc_enabled) == 12u,
    "SurfaceMetaV3.ubwc_enabled@12");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, ubwc_version) == 16u,
    "SurfaceMetaV3.ubwc_version@16");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, highest_bank_bit) == 20u,
    "SurfaceMetaV3.highest_bank_bit@20");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, swizzle_levels) == 24u,
    "SurfaceMetaV3.swizzle_levels@24");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, macrotile_mode) == 28u,
    "SurfaceMetaV3.macrotile_mode@28");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, meta_offset) == 32u,
    "SurfaceMetaV3.meta_offset@32");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, meta_pitch) == 40u,
    "SurfaceMetaV3.meta_pitch@40");
A840_V3_FREEZE_STATIC_ASSERT(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, flags) == 44u,
    "SurfaceMetaV3.flags@44");

/* Method 22 Sparse VM bind wire layouts and chip facts. */
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseBindOp) == 48u, "SparseBindOp == 48");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseTileInfo) == 48u, "SparseTileInfo == 48");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseBindReqV3) == 1568u, "SparseBindReqV3 == 1568");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseBindReplyV3) == 1568u, "SparseBindReplyV3 == 1568");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseBindReqV3) <= NativeAdrenoAbiEnvelopeBytes,
    "SparseBindReqV3 fits 3104-byte envelope");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840KmdSparseBindReplyV3) <= NativeAdrenoAbiEnvelopeBytes,
    "SparseBindReplyV3 fits 3104-byte envelope");
A840_V3_FREEZE_STATIC_ASSERT(sizeof(A840ChipFacts) == A840_CHIP_FACTS_BYTES, "ChipFacts == 168");

/* 4 KiB page alignment, 64 KiB zero page, 18 MiB GMEM, HBB 16, OPP & 4-Ring counts. */
A840_V3_FREEZE_STATIC_ASSERT(A840_ABI_V3_PAGE_BYTES == A840_KMD_V3_FREEZE_PAGE_BYTES,
    "4 KiB ABI page alignment locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_GMEM_BYTES_ALIGNMENT == A840_KMD_V3_FREEZE_PAGE_BYTES,
    "4 KiB GMEM alignment locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_PREEMPT_RECORD_ALIGN_BYTES == A840_KMD_V3_FREEZE_PAGE_BYTES,
    "4 KiB preempt record alignment locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_ABI_V3_ZERO_PAGE_BYTES == A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES,
    "64 KiB ABI zero page size locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_ZERO_PAGE_BYTES == A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES,
    "64 KiB KMD zero page size locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KMD_ZERO_PAGE_ALIGN == A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES,
    "64 KiB KMD zero page alignment locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_PREEMPT_RECORD_MIN_BYTES == A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES,
    "64 KiB minimum preempt record size locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KAANAPALI_GMEM_BYTES == (18u * 1024u * 1024u),
    "Kaanapali 18 MiB GMEM locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_KAANAPALI_HIGHEST_BANK_BIT == 16u,
    "Kaanapali HBB 16 locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_GPU_OPP_COUNT == 18u, "18-level GPU OPP count locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_GMU_OPP_COUNT == 4u, "4-level GMU OPP count locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_RING_COUNT == 4u, "4 hardware preemption rings locked");
A840_V3_FREEZE_STATIC_ASSERT(A840_PRIORITY_COUNT == 4u, "4 priority levels locked");

/* ============================================================================
 * 3. Production Qualification Bitmasks & Data Structures
 * ============================================================================ */

enum {
    A840_KMD_QUAL_ABI_V3_FROZEN          = (UINT32_C(1) << 0),
    A840_KMD_QUAL_CHIP_IDENTITY          = (UINT32_C(1) << 1),
    A840_KMD_QUAL_ZERO_PAGE_READY        = (UINT32_C(1) << 2),
    A840_KMD_QUAL_BACKBOARDD_CA_POLICY   = (UINT32_C(1) << 3),
    A840_KMD_QUAL_PREEMPT_4RINGS         = (UINT32_C(1) << 4),
    A840_KMD_QUAL_OPP_TABLES_AND_THERMAL = (UINT32_C(1) << 5),
    A840_KMD_QUAL_ZERO_LEGACY_TURNIP     = (UINT32_C(1) << 6),
    A840_KMD_QUAL_ALL_MASK               = ((UINT32_C(1) << 7) - UINT32_C(1))
};

/* Descriptor for one preemption context record (checked for 4 KiB align, >= 64 KiB, non-overlap). */
typedef struct A840KmdPreemptRecordDesc {
    uint64_t record_va;
    uint64_t record_bytes;
} A840KmdPreemptRecordDesc;

/* Read-only summary of compile-time frozen ABI v3 & hardware constants. */
typedef struct A840KmdV3AbiFreezeInfo {
    uint32_t abi_version;
    uint32_t envelope_bytes;
    uint32_t page_bytes;
    uint32_t zero_page_bytes;
    uint32_t prod_selector_min;
    uint32_t prod_selector_max;
    uint32_t gpu_opp_count;
    uint32_t gmu_opp_count;
    uint32_t ring_count;
    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t gmem_bytes;
    uint64_t chip_id;
    uint64_t prod_cap_mask;
    uint64_t required_prod_caps;
} A840KmdV3AbiFreezeInfo;

/* Input bundle for unified production kernel qualification (strictly fail-closed). */
typedef struct A840KmdProdQualInput {
    /* 1) Frozen ABI v3 version & capability flags */
    uint32_t abi_version;
    uint32_t envelope_bytes;
    uint64_t advertised_cap_flags;

    /* 2) Kaanapali Adreno 840 chip identity & memory facts */
    uint64_t chip_id;
    uint32_t gmem_bytes;
    uint32_t highest_bank_bit;
    uint32_t ubwc_version;
    uint32_t smmu_page_shift;

    /* 3) Shared 64 KiB read-only zero page readiness */
    uint32_t zero_page_ready;
    uint32_t zero_page_readonly;
    uint64_t zero_page_gpu_va;
    uint64_t zero_page_phys_addr;
    uint64_t zero_page_bytes;

    /* 4) backboardd CA_DISABLE_GENERIC_SHADERS=1 enforcement */
    const char *ca_target_label;
    const char *ca_env_entry;
    uint32_t ca_native_metal_enabled;
    uint32_t ca_pack_verified;
    uint32_t ca_policy_enforced;
    uint32_t reserved_ca;

    /* 5) 4-Ring preemption records (4 KiB aligned, >= 64 KiB, non-overlapping) */
    uint32_t preempt_ring_count;
    uint32_t reserved_preempt;
    A840KmdPreemptRecordDesc preempt_records[A840_RING_COUNT];

    /* 6) 18-level GPU (160..1200 MHz) & 4-level GMU (475..750 MHz) OPP tables + >=75 C thermal ladder */
    const A840GpuOppEntry *gpu_opp_table;
    size_t gpu_opp_count;
    const A840GmuOppEntry *gmu_opp_table;
    size_t gmu_opp_count;
    uint32_t thermal_ladder_active;
    uint32_t thermal_tier_count;
    int32_t  thermal_min_throttle_mc;
    uint32_t thermal_cap_75c_mhz;
    uint32_t thermal_cap_95c_mhz;
    uint32_t reserved_thermal;

    /* 7) Zero Legacy Turnip Policy audit inputs */
    uint64_t legacy_turnip_shim_flags;
    uint32_t legacy_turnip_paths_present;
    uint32_t reserved;
    const uint32_t *selector_audit_list;
    size_t selector_audit_count;
} A840KmdProdQualInput;

/* Bitmask qualification report returned by a840_kmd_v3_qualify_production(). */
typedef struct A840KmdProdQualReport {
    uint32_t passed_mask;
    uint32_t failed_mask;
    uint32_t abi_version_frozen;
    uint32_t turnip_paths_purged;
    uint32_t qualified;
    uint32_t abi_version_observed;
    uint64_t legacy_rejected_count;
} A840KmdProdQualReport;

/* Optional custom sleepable lock ops for stateful freeze policy audit instance. */
typedef struct A840KmdFreezeSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840KmdFreezeSyncOps;

/* Atomic read-only snapshot of stateful freeze policy counters. */
typedef struct A840KmdV3FreezeStats {
    uint64_t legacy_rejected_count;
    uint64_t legacy_selector_rejects;
    uint64_t legacy_flag_rejects;
    uint64_t selector_checks_passed;
    uint64_t flag_checks_passed;
    uint64_t qualify_pass_count;
    uint64_t qualify_fail_count;
    uint32_t last_passed_mask;
    uint32_t last_failed_mask;
} A840KmdV3FreezeStats;

/* Zero-heap-allocation stateful production freeze & audit tracker. */
typedef struct A840KmdV3FreezePolicy {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t last_passed_mask;
    uint32_t last_failed_mask;
    uint64_t legacy_rejected_count;
    uint64_t legacy_selector_rejects;
    uint64_t legacy_flag_rejects;
    uint64_t selector_checks_passed;
    uint64_t flag_checks_passed;
    uint64_t qualify_pass_count;
    uint64_t qualify_fail_count;
    A840KmdFreezeSyncOps sync_ops;
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdV3FreezePolicy;

/* ============================================================================
 * 4. Public C11 API (Zero Heap Allocation, Thread-Safe, Fail-Closed)
 * ============================================================================ */

/* Populate compile-time frozen ABI v3 metadata snapshot. */
void a840_kmd_v3_get_abi_freeze_info(A840KmdV3AbiFreezeInfo *out_info);

/* Explicit production selector whitelist predicate (1 = production [1..26], 0 = rejected). */
int a840_kmd_v3_selector_is_production(uint32_t selector);

/* Fail-closed selector gate: returns A840_KMD_OK for production [1..26];
 * otherwise returns A840_KMD_UNSUPPORTED and atomically increments legacy_rejected_count.
 */
A840KmdStatus a840_kmd_v3_validate_production_selector(uint32_t selector);

/* Fail-closed flag gate: returns A840_KMD_OK iff every set bit is inside
 * A840_KMD_V3_PROD_CAP_MASK; any legacy Turnip/compat or unknown bit returns
 * A840_KMD_UNSUPPORTED and atomically increments legacy_rejected_count.
 */
A840KmdStatus a840_kmd_v3_reject_legacy_turnip_flags(uint64_t flags);

/* Read or reset the process-wide atomic legacy rejection audit counter. */
uint64_t a840_kmd_v3_legacy_rejected_count(void);
void a840_kmd_v3_reset_legacy_rejected_count(void);

/* Standalone validators for 4-Ring preemption records and OPP/thermal readiness. */
A840KmdStatus a840_kmd_v3_validate_preempt_records(
    const A840KmdPreemptRecordDesc *records,
    size_t count);

A840KmdStatus a840_kmd_v3_validate_opp_and_thermal(
    const A840GpuOppEntry *gpu_table,
    size_t gpu_count,
    const A840GmuOppEntry *gmu_table,
    size_t gmu_count,
    uint32_t thermal_ladder_active,
    uint32_t thermal_tier_count,
    int32_t  thermal_min_throttle_mc,
    uint32_t thermal_cap_75c_mhz,
    uint32_t thermal_cap_95c_mhz);

/* Unified fail-closed production kernel qualification entrypoint.
 * Returns A840_KMD_OK iff ALL 7 production checks pass (passed_mask == ALL_MASK,
 * failed_mask == 0, abi_version_frozen == 1, turnip_paths_purged == 1, qualified == 1).
 * Returns A840_KMD_UNSUPPORTED if any legacy Turnip flag/selector/path is active,
 * A840_KMD_INVALID_ARG on NULL/nonzero reserved fields, or A840_KMD_STATE_CONFLICT
 * if any other readiness prerequisite is missing.
 */
A840KmdStatus a840_kmd_v3_qualify_production(
    const A840KmdProdQualInput *in,
    A840KmdProdQualReport *out);

/* Stateful per-instance policy & audit tracker (zero heap, TSan-safe). */
A840KmdStatus a840_kmd_v3_freeze_policy_init(
    A840KmdV3FreezePolicy *policy,
    const A840KmdFreezeSyncOps *sync_ops);

void a840_kmd_v3_freeze_policy_destroy(A840KmdV3FreezePolicy *policy);

A840KmdStatus a840_kmd_v3_freeze_policy_check_selector(
    A840KmdV3FreezePolicy *policy,
    uint32_t selector);

A840KmdStatus a840_kmd_v3_freeze_policy_check_flags(
    A840KmdV3FreezePolicy *policy,
    uint64_t flags);

A840KmdStatus a840_kmd_v3_freeze_policy_qualify(
    A840KmdV3FreezePolicy *policy,
    const A840KmdProdQualInput *in,
    A840KmdProdQualReport *out);

A840KmdStatus a840_kmd_v3_freeze_policy_get_stats(
    A840KmdV3FreezePolicy *policy,
    A840KmdV3FreezeStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_V3_FREEZE_H */
