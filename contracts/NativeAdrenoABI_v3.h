/* SPDX-License-Identifier: MIT */
/* Frozen Contract Draft: NativeAdrenoABI_v3.h (v3.0-draft-phase0)
 * Owner: Lead Architect (Coordinator). Workers MUST NOT edit this file directly.
 *
 * Strictly backward-compatible extension on top of NativeAdrenoABI v2
 * (methods 1..19 and all v2 fixed-size record layouts stay byte-identical).
 * New v3 methods (20..26) are feature-gated by NativeAdrenoAbiParams.flags
 * and fit inside the existing 3104-byte IOKit external-method envelope.
 */
#ifndef NATIVE_ADRENO_ABI_V3_H
#define NATIVE_ADRENO_ABI_V3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NativeAdrenoAbiVersionV2 = 2u,
    NativeAdrenoAbiVersionV3 = 3u,
    NativeAdrenoAbiEnvelopeBytes = 3104u,

    /* Existing v2 params flags (bits 0..3 preserved verbatim) */
    NativeAdrenoParamsV2HostImport = 1u << 0,
    NativeAdrenoParamsV2OwnedCoherent = 1u << 1,
    NativeAdrenoParamsV2MemoryPressure = 1u << 2,
    NativeAdrenoParamsV2SubmitBatch = 1u << 3,

    /* New v3 capability flags (bits 8..14) */
    NativeAdrenoParamsV3TimelineWaitAny = 1u << 8,   /* Interrupt-backed multi-fence WAIT_ANY */
    NativeAdrenoParamsV3SharedZeroPage = 1u << 9,    /* Read-only 64KiB GPU zero page for unbound slots */
    NativeAdrenoParamsV3SparseVmBind = 1u << 10,     /* Phase 4: sparse zero-mapped VA ranges */
    NativeAdrenoParamsV3SharedTier0Pack = 1u << 11,  /* Phase 3: kernel shared RO Tier-0 shader pack */
    NativeAdrenoParamsV3WriterPromise = 1u << 12,    /* Phase 3: compositor early-schedule -> MDSS fence */
    NativeAdrenoParamsV3UbwcSurfaceMeta = 1u << 13,  /* Phase 3: UBWC 6.0 IOSurface layout negotiation */
    NativeAdrenoParamsV3PreemptRings = 1u << 14,     /* Phase 5: high-prio RenderServer ring preemption */

    /* New v3 method numbers (v2 methods 1..19 unchanged) */
    NativeAdrenoMethodWaitAnyV3 = 20u,
    NativeAdrenoMethodGetZeroPageV3 = 21u,
    NativeAdrenoMethodMapSparseV3 = 22u,
    NativeAdrenoMethodMapSharedPackV3 = 23u,
    NativeAdrenoMethodWriterPromiseV3 = 24u,
    NativeAdrenoMethodSurfaceMetaV3 = 25u,
    NativeAdrenoMethodQueueConfigV3 = 26u
};

/* Method 20: Interrupt-backed wait for ANY/ALL of up to 32 fences. */
struct NativeAdrenoAbiWaitAnyV3 {
    uint32_t count;          /* 1..32 */
    uint32_t flags;          /* bit0: wait_all (0=wait_any, 1=wait_all) */
    uint64_t timeout_ns;
    uint64_t queues[32];
    uint64_t fences[32];
};

struct NativeAdrenoAbiWaitAnyReplyV3 {
    uint32_t satisfied_mask;
    uint32_t timed_out;
    uint64_t observed_ticks;
};

/* Method 21: Query client-mapped 64KiB read-only GPU zero page (Phase 2/5). */
struct NativeAdrenoAbiZeroPageReplyV3 {
    uint64_t gpu_va;         /* 64KiB aligned, read-only, always mapped */
    uint64_t bytes;          /* >= 65536 */
};

/* Method 23: Map kernel-owned read-only Tier-0 AOT shader pack into client GPU+CPU VA. */
struct NativeAdrenoAbiSharedPackReqV3 {
    uint64_t expected_stack_key;
    uint32_t map_cpu;        /* 1: also return RO CPU VA */
    uint32_t reserved;
};

struct NativeAdrenoAbiSharedPackReplyV3 {
    uint64_t gpu_va;
    uint64_t cpu_va;
    uint64_t bytes;
    uint64_t actual_stack_key;
};

/* Method 24: Compositor writer-promise registration / cancellation for MDSS display handoff. */
enum {
    NativeAdrenoPromiseMake = 1u,
    NativeAdrenoPromiseFulfill = 2u,
    NativeAdrenoPromiseCancel = 3u
};

struct NativeAdrenoAbiWriterPromiseV3 {
    uint32_t op;             /* Make / Fulfill / Cancel */
    uint32_t surface_count;  /* 1..16 */
    uint64_t promise_token;  /* 0 on Make; nonzero on Fulfill/Cancel */
    uint64_t queue;
    uint64_t fence;          /* Nonzero on Fulfill */
    uint64_t surface_ids[16];
};

struct NativeAdrenoAbiWriterPromiseReplyV3 {
    uint64_t promise_token;
    uint32_t accepted;
    uint32_t reserved;
};

/* Method 25: UBWC 6.0 / Kaanapali compressed IOSurface metadata negotiation. */
struct NativeAdrenoAbiSurfaceMetaV3 {
    uint64_t bo_handle;
    uint32_t op;             /* 0=get, 1=set */
    uint32_t ubwc_enabled;
    uint32_t ubwc_version;   /* 6 */
    uint32_t highest_bank_bit; /* 16 on Kaanapali */
    uint32_t swizzle_levels; /* 6 (levels 2+3) */
    uint32_t macrotile_mode; /* 1 */
    uint64_t meta_offset;
    uint32_t meta_pitch;
    uint32_t flags;          /* bit0:scanout_eligible */
};

#if defined(__cplusplus)
static_assert(sizeof(struct NativeAdrenoAbiWaitAnyV3) <= NativeAdrenoAbiEnvelopeBytes, "WaitAny fits envelope");
static_assert(sizeof(struct NativeAdrenoAbiWaitAnyReplyV3) == 16, "WaitAnyReply layout locked");
static_assert(sizeof(struct NativeAdrenoAbiZeroPageReplyV3) == 16, "ZeroPageReply layout locked");
static_assert(sizeof(struct NativeAdrenoAbiSharedPackReqV3) == 16, "SharedPackReq layout locked");
static_assert(sizeof(struct NativeAdrenoAbiSharedPackReplyV3) == 32, "SharedPackReply layout locked");
static_assert(sizeof(struct NativeAdrenoAbiWriterPromiseV3) == 160, "WriterPromise layout locked");
static_assert(sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3) == 16, "WriterPromiseReply layout locked");
static_assert(sizeof(struct NativeAdrenoAbiSurfaceMetaV3) == 48, "SurfaceMeta layout locked");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct NativeAdrenoAbiWaitAnyV3) <= NativeAdrenoAbiEnvelopeBytes, "WaitAny fits envelope");
_Static_assert(sizeof(struct NativeAdrenoAbiWaitAnyReplyV3) == 16, "WaitAnyReply layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiZeroPageReplyV3) == 16, "ZeroPageReply layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiSharedPackReqV3) == 16, "SharedPackReq layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiSharedPackReplyV3) == 32, "SharedPackReply layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiWriterPromiseV3) == 160, "WriterPromise layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3) == 16, "WriterPromiseReply layout locked");
_Static_assert(sizeof(struct NativeAdrenoAbiSurfaceMetaV3) == 48, "SurfaceMeta layout locked");
#endif

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_ADRENO_ABI_V3_H */
