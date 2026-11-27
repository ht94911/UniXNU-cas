/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_DISPLAY_HANDOFF_H
#define A840_KMD_DISPLAY_HANDOFF_H

#include <stddef.h>
#include <stdint.h>
#include "a840_kmd_ubwc_surface.h"
#include "a840_kmd_writer_promise.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Scanout mode decided by MDSS negotiation for a frame handoff. */
typedef enum A840DisplayScanoutMode {
    A840_DISPLAY_SCANOUT_NONE = 0u,
    A840_DISPLAY_SCANOUT_UBWC6_DIRECT = 1u,
    A840_DISPLAY_SCANOUT_LINEAR_DIRECT = 2u,
    A840_DISPLAY_SCANOUT_LINEAR_FALLBACK = 3u
} A840DisplayScanoutMode;

/* Synchronization mode attached to the MDSS commit descriptor. */
typedef enum A840DisplaySyncMode {
    A840_DISPLAY_SYNC_NONE = 0u,
    A840_DISPLAY_SYNC_EARLY_GPU_FENCE = 1u,
    A840_DISPLAY_SYNC_RETIRED_READY = 2u
} A840DisplaySyncMode;

/* Decision status for a NativeMobileDisplay / MDSS scanout submission. */
typedef enum A840DisplayHandoffOutcome {
    A840_DISPLAY_HANDOFF_REJECTED = 0u,
    A840_DISPLAY_HANDOFF_COMMITTED = 1u,
    A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE = 2u,
    A840_DISPLAY_HANDOFF_CANCELED_ON_FAULT = 3u,
    A840_DISPLAY_HANDOFF_TIMED_OUT = 4u
} A840DisplayHandoffOutcome;

/* Flags controlling a single display handoff request. */
enum {
    A840_DISPLAY_REQ_NONE = 0u,
    A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK = 1u << 0,
    A840_DISPLAY_REQ_WAIT_FOR_PROMISE = 1u << 1,
    A840_DISPLAY_REQ_KNOWN_MASK =
        (A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK |
         A840_DISPLAY_REQ_WAIT_FOR_PROMISE)
};

/* Optional external sleepable mutex for handoff diagnostics/commit serialization. */
typedef struct A840KmdDisplaySyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840KmdDisplaySyncOps;

/* MDSS commit descriptor passed to the display backend callback. */
typedef struct A840KmdMdssCommitDesc {
    uint64_t surface_identity;
    uint64_t bo_handle;
    uint64_t writer_client;
    uint64_t generation;
    uint64_t promise_token;
    uint64_t gpu_queue;
    uint64_t gpu_fence;
    uint32_t scanout_mode;       /* A840DisplayScanoutMode */
    uint32_t sync_mode;          /* A840DisplaySyncMode */
    uint32_t ubwc_enabled;
    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t swizzle_levels;
    uint32_t macrotile_mode;
    uint32_t meta_pitch;
    uint64_t meta_offset;
} A840KmdMdssCommitDesc;

/* Optional MDSS hardware commit hook, invoked strictly outside all KMD locks. */
typedef A840KmdStatus (*A840KmdMdssCommitFn)(
    void *cookie,
    const A840KmdMdssCommitDesc *commit_desc);

/* Deterministic Phase 5 performance & reliability counters. */
typedef struct A840KmdDisplayHandoffStats {
    uint64_t early_scheduled_handoffs;
    uint64_t deferred_app_handoffs;
    uint64_t fence_satisfied_handoffs;
    uint64_t canceled_on_fault;
    uint64_t linear_fallback_handoffs;
    uint64_t ubwc_direct_handoffs;
    uint64_t linear_direct_handoffs;
    uint64_t rejected_handoffs;
    uint64_t timed_out_handoffs;
} A840KmdDisplayHandoffStats;

typedef struct A840KmdDisplayHandoffConfig {
    A840KmdSurfaceManager *surface_mgr;
    A840KmdWriterPromiseRegistry *promise_reg;
    const A840KmdDisplaySyncOps *sync_ops; /* NULL = built-in pthread_mutex */
    A840KmdMdssCommitFn commit_fn;         /* Optional MDSS sink callback */
    void *commit_cookie;
} A840KmdDisplayHandoffConfig;

/* Per-frame handoff submission request from NativeMobileDisplay / compositor. */
typedef struct A840KmdDisplayFrameRequest {
    uint64_t consumer_client;    /* Display / RenderServer handle namespace for surface_mgr */
    uint64_t surface_handle;     /* Primary (potentially UBWC) BO handle in consumer_client */
    uint64_t surface_identity;   /* Kernel surface identity matching WriterPromise */
    uint64_t min_generation;     /* Minimum writer generation required (0 = any latest) */
    uint64_t timeout_ns;         /* Wait timeout when A840_DISPLAY_REQ_WAIT_FOR_PROMISE is set */
    uint64_t linear_resolve_handle;   /* Optional separate linear BO handle for fallback */
    uint64_t linear_resolve_identity; /* Optional separate linear surface identity */
    uint32_t flags;              /* A840_DISPLAY_REQ_* */
    uint32_t reserved;           /* MUST be 0 (fail-closed) */
    const A840KmdMdssCaps *mdss_caps; /* Current active MDSS output capabilities */
} A840KmdDisplayFrameRequest;

/* Result of a single MDSS scanout negotiation + WriterPromise handoff. */
typedef struct A840KmdDisplayHandoffResult {
    uint32_t outcome;            /* A840DisplayHandoffOutcome */
    uint32_t scanout_mode;       /* A840DisplayScanoutMode */
    uint32_t sync_mode;          /* A840DisplaySyncMode */
    uint32_t promise_state;      /* A840PromiseState */
    uint32_t promise_policy;     /* A840PromiseHandoffPolicy */
    uint32_t cancel_reason;      /* A840PromiseCancelReason */
    uint32_t fallback_linear_required;
    uint32_t timed_out;
    A840KmdSurfaceResult surface_result;
    A840KmdSurfacePromiseSnapshot promise_snapshot;
    A840KmdMdssCommitDesc commit;
} A840KmdDisplayHandoffResult;

typedef struct A840KmdDisplayHandoff {
    uint32_t initialized;
    uint32_t use_custom_sync;
    A840KmdSurfaceManager *surface_mgr;
    A840KmdWriterPromiseRegistry *promise_reg;
    A840KmdDisplaySyncOps sync_ops;
    A840KmdMdssCommitFn commit_fn;
    void *commit_cookie;
    A840KmdDisplayHandoffStats stats;
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdDisplayHandoff;

/* Lifecycle */
A840KmdStatus a840_kmd_display_handoff_init(
    A840KmdDisplayHandoff *dh,
    const A840KmdDisplayHandoffConfig *config);

void a840_kmd_display_handoff_destroy(A840KmdDisplayHandoff *dh);

/* Negotiate UBWC 6.0 vs linear scanout via a840_kmd_ubwc_surface, synchronize
 * with the latest WriterPromise (early GPU fence for RenderServer, retired-only
 * for non-compositor Apps, immediate fault cancellation zero-deadlock), and
 * optionally commit the resulting descriptor to MDSS.
 */
A840KmdStatus a840_kmd_display_handoff_submit_frame(
    A840KmdDisplayHandoff *dh,
    const A840KmdDisplayFrameRequest *req,
    A840KmdDisplayHandoffResult *out_result);

A840KmdStatus a840_kmd_display_handoff_get_stats(
    A840KmdDisplayHandoff *dh,
    A840KmdDisplayHandoffStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_DISPLAY_HANDOFF_H */
