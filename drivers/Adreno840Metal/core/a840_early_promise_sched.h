/* SPDX-License-Identifier: MIT */
/* Adreno 840 Native Metal Driver — RenderServer Early Writer Promise Scheduler
 * (Phase 5 / Wave 10 Lane 3: KMD Method 24 Aligned Early-Scheduled Handoff)
 *
 * Pure C11, zero steady-state heap allocation, thread-safe (pthread_mutex_t),
 * strictly invoking all notification callbacks OUTSIDE the internal mutex.
 *
 * Privilege policy (strictly aligned with drivers/Adreno840Kernel/a840_kmd_writer_promise.h
 * and contracts/NativeAdrenoABI_v3.h Method 24):
 *   - RENDER_SERVER (1): when the last writing chunk for a target surface_id is
 *     kicked onto the hardware ring, immediately publishes EARLY_SCHEDULED
 *     (without waiting for GPU retire) carrying exact surface_id, strictly
 *     monotonic generation, chunk_seqno, and ring_id.
 *   - APP (0): strictly forced to DEFER_UNTIL_RETIRED; even explicit requests
 *     for early handoff fail-closed (downgraded to DEFER_UNTIL_RETIRED or
 *     rejected with PERMISSION_DENIED when strict_reject_app_early is enabled).
 */
#ifndef A840_EARLY_PROMISE_SCHED_H
#define A840_EARLY_PROMISE_SCHED_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_EARLY_PROMISE_MAX_SURFACES = 32u,
    A840_EARLY_PROMISE_MAX_HISTORY = 64u,
    A840_EARLY_PROMISE_MAX_ACTIVE = 64u,
    A840_EARLY_PROMISE_MAX_RING_ID = 3u
};

typedef enum A840EarlyPromiseStatus {
    A840_PROMISE_SCHED_OK = 0,
    A840_PROMISE_SCHED_ERR_INVALID_ARG = 1,
    A840_PROMISE_SCHED_ERR_INVALID_STATE = 2,
    A840_PROMISE_SCHED_ERR_PERMISSION_DENIED = 3,
    A840_PROMISE_SCHED_ERR_CAPACITY = 4,
    A840_PROMISE_SCHED_ERR_NOT_FOUND = 5,
    A840_PROMISE_SCHED_ERR_NON_MONOTONIC_GEN = 6,
    A840_PROMISE_SCHED_ERR_CHANNEL_POISONED = 7,
    A840_PROMISE_SCHED_ERR_DUPLICATE = 8
} A840EarlyPromiseStatus;

/* Binary-identical to A840ClientRole in a840_kmd_writer_promise.h */
typedef enum A840EarlyClientRole {
    A840_EARLY_ROLE_APP = 0u,
    A840_EARLY_ROLE_RENDER_SERVER = 1u
} A840EarlyClientRole;

/* Binary-identical to A840PromiseHandoffPolicy in a840_kmd_writer_promise.h */
typedef enum A840EarlyPromisePolicy {
    A840_EARLY_POLICY_NONE = 0u,
    A840_EARLY_POLICY_EARLY_SCHEDULED = 1u,
    A840_EARLY_POLICY_DEFER_UNTIL_RETIRED = 2u
} A840EarlyPromisePolicy;

typedef enum A840EarlyPromiseEventKind {
    A840_PROMISE_EVENT_NONE = 0u,
    A840_PROMISE_EVENT_EARLY_SCHEDULED = 1u,
    A840_PROMISE_EVENT_DEFERRED_ARMED = 2u,
    A840_PROMISE_EVENT_RETIRED_READY = 3u,
    A840_PROMISE_EVENT_PROMISE_CANCELED = 4u
} A840EarlyPromiseEventKind;

typedef enum A840EarlyPromiseCancelReason {
    A840_EARLY_CANCEL_NONE = 0u,
    A840_EARLY_CANCEL_EXPLICIT = 1u,
    A840_EARLY_CANCEL_GPU_FAULT = 2u,
    A840_EARLY_CANCEL_CHANNEL_RESET = 3u,
    A840_EARLY_CANCEL_CHUNK_SUBMIT_FAILED = 4u
} A840EarlyPromiseCancelReason;

typedef struct A840EarlyPromiseEvent {
    A840EarlyPromiseEventKind kind;
    A840EarlyClientRole client_role;
    A840EarlyPromisePolicy policy;
    A840EarlyPromiseCancelReason cancel_reason;
    uint64_t client_id;
    uint64_t frame_id;
    uint64_t cb_id;
    uint64_t surface_id;
    uint64_t generation;
    uint64_t chunk_seqno;
    uint64_t promise_token;
    uint32_t ring_id;
    uint32_t display_ready;          /* 1 on EARLY_SCHEDULED or RETIRED_READY */
} A840EarlyPromiseEvent;

/* Invoked STRICTLY OUTSIDE the scheduler internal mutex. */
typedef void (*A840EarlyPromiseCallback)(void *ctx, const A840EarlyPromiseEvent *event);

typedef struct A840EarlyPromiseConfig {
    uint64_t client_id;
    A840EarlyClientRole client_role;
    bool strict_reject_app_early;    /* true: reject APP early request; false: downgrade */
    A840EarlyPromiseCallback callback;
    void *callback_ctx;
} A840EarlyPromiseConfig;

typedef struct A840EarlyPromiseSurfacePlan {
    bool active;
    bool published;
    bool canceled;
    bool early_requested;
    uint64_t surface_id;
    uint64_t explicit_generation;    /* 0 = auto-monotonic next */
    uint32_t expected_write_chunks;  /* 0 = publish on FINAL chunk; >0 = exact count */
    uint32_t kicked_write_chunks;
    uint64_t last_chunk_seqno;       /* Deduplicates multiple writes in same chunk */
} A840EarlyPromiseSurfacePlan;

typedef struct A840EarlyPromiseHistory {
    bool active;
    uint64_t surface_id;
    uint64_t last_generation;
    uint64_t last_chunk_seqno;
    uint64_t last_promise_token;
    uint32_t last_ring_id;
    A840EarlyPromisePolicy last_policy;
    A840EarlyPromiseEventKind last_event;
    A840EarlyPromiseCancelReason last_cancel_reason;
    uint32_t display_ready;
} A840EarlyPromiseHistory;

typedef struct A840EarlyPromiseActiveRecord {
    bool active;
    bool retired;
    A840EarlyPromisePolicy policy;
    A840EarlyPromiseEventKind state;
    uint64_t frame_id;
    uint64_t cb_id;
    uint64_t surface_id;
    uint64_t generation;
    uint64_t chunk_seqno;
    uint64_t promise_token;
    uint32_t ring_id;
} A840EarlyPromiseActiveRecord;

typedef struct A840EarlyPromiseSurfaceSnapshot {
    bool found;
    uint64_t surface_id;
    uint64_t generation;
    uint64_t chunk_seqno;
    uint64_t promise_token;
    uint32_t ring_id;
    A840EarlyPromisePolicy policy;
    A840EarlyPromiseEventKind last_event;
    A840EarlyPromiseCancelReason last_cancel_reason;
    uint32_t display_ready;
} A840EarlyPromiseSurfaceSnapshot;

typedef struct A840EarlyPromiseStats {
    uint64_t early_scheduled_count;
    uint64_t deferred_armed_count;
    uint64_t retired_ready_count;
    uint64_t canceled_broadcast_count;
    uint64_t app_early_downgraded_count;
    uint64_t app_early_rejected_count;
    uint64_t duplicate_writes_deduped;
    uint64_t non_monotonic_gen_rejected;
    uint64_t callbacks_invoked;
    uint32_t active_pending_count;
    uint32_t tracked_frame_surfaces;
} A840EarlyPromiseStats;

typedef struct A840EarlyPromiseSched {
    pthread_mutex_t lock;
    bool lock_initialized;
    bool initialized;
    bool frame_active;
    uint64_t client_id;
    A840EarlyClientRole client_role;
    bool strict_reject_app_early;
    A840EarlyPromiseCallback callback;
    void *callback_ctx;

    uint64_t active_frame_id;
    uint64_t active_cb_id;
    uint32_t active_ring_id;
    uint64_t next_promise_token;
    bool ring_poisoned[A840_EARLY_PROMISE_MAX_RING_ID + 1u];

    A840EarlyPromiseSurfacePlan frame_surfaces[A840_EARLY_PROMISE_MAX_SURFACES];
    A840EarlyPromiseHistory history[A840_EARLY_PROMISE_MAX_HISTORY];
    A840EarlyPromiseActiveRecord active_records[A840_EARLY_PROMISE_MAX_ACTIVE];
    A840EarlyPromiseStats stats;
} A840EarlyPromiseSched;

A840EarlyPromiseStatus a840_early_promise_init(A840EarlyPromiseSched *sched,
                                               const A840EarlyPromiseConfig *cfg);

void a840_early_promise_destroy(A840EarlyPromiseSched *sched);

/* Begin tracking a frame/CB on `ring_id` (0..3). */
A840EarlyPromiseStatus a840_early_promise_begin_frame(A840EarlyPromiseSched *sched,
                                                      uint64_t frame_id,
                                                      uint64_t cb_id,
                                                      uint32_t ring_id);

/* Declare/plan a surface written in the current frame (up to 32 surfaces/frame).
 * - expected_write_chunks > 0: publish immediately when kicked_write_chunks reaches
 *   expected_write_chunks (even before CB FINAL if other surfaces continue).
 * - expected_write_chunks == 0: hold until is_final_chunk == true.
 * - explicit_generation == 0: auto-increment monotonic generation; nonzero must be
 *   strictly greater than previous generation for this surface_id.
 * - request_early: honored ONLY for A840_EARLY_ROLE_RENDER_SERVER. For APP,
 *   either rejected (strict_reject_app_early) or downgraded to DEFER_UNTIL_RETIRED. */
A840EarlyPromiseStatus a840_early_promise_plan_surface(A840EarlyPromiseSched *sched,
                                                       uint64_t surface_id,
                                                       uint32_t expected_write_chunks,
                                                       uint64_t explicit_generation,
                                                       bool request_early);

/* Notify scheduler that a chunk has been kicked onto the GPU ring.
 * Deduplicates repeated surface_ids within the same chunk_seqno, never publishes
 * early while later writing chunks remain, and invokes callbacks OUTSIDE mutex. */
A840EarlyPromiseStatus a840_early_promise_on_chunk_kicked(A840EarlyPromiseSched *sched,
                                                          uint64_t cb_id,
                                                          uint64_t chunk_seqno,
                                                          uint32_t ring_id,
                                                          const uint64_t *written_surface_ids,
                                                          size_t written_surface_count,
                                                          bool is_final_chunk);

/* Notify scheduler that GPU hardware retired all chunks <= completed_chunk_seqno
 * on `ring_id`. Transitions DEFER_UNTIL_RETIRED (APP) promises to RETIRED_READY
 * and invokes callbacks OUTSIDE mutex. */
size_t a840_early_promise_on_chunk_retired(A840EarlyPromiseSched *sched,
                                           uint32_t ring_id,
                                           uint64_t completed_chunk_seqno);

/* Cancel all non-retired promises and planned surfaces for `cb_id`, broadcasting
 * PROMISE_CANCELED notifications OUTSIDE mutex. */
size_t a840_early_promise_cancel_cb(A840EarlyPromiseSched *sched,
                                    uint64_t cb_id,
                                    A840EarlyPromiseCancelReason reason);

/* Cancel all non-retired promises on `ring_id` (e.g. channel reset / GPU fault),
 * optionally poisoning the ring until reset_ring is called. Callbacks OUTSIDE mutex. */
size_t a840_early_promise_cancel_ring(A840EarlyPromiseSched *sched,
                                      uint32_t ring_id,
                                      A840EarlyPromiseCancelReason reason,
                                      bool poison_ring);

A840EarlyPromiseStatus a840_early_promise_reset_ring(A840EarlyPromiseSched *sched,
                                                     uint32_t ring_id);

A840EarlyPromiseStatus a840_early_promise_query_surface(A840EarlyPromiseSched *sched,
                                                        uint64_t surface_id,
                                                        A840EarlyPromiseSurfaceSnapshot *out_snapshot);

A840EarlyPromiseStats a840_early_promise_get_stats(A840EarlyPromiseSched *sched);

/* Populate frozen 160-byte KMD Method 24 ABI struct from an event. */
A840EarlyPromiseStatus a840_early_promise_fill_abi_v3(
    const A840EarlyPromiseEvent *ev,
    struct NativeAdrenoAbiWriterPromiseV3 *out_abi);

#ifdef __cplusplus
}
#endif

#endif /* A840_EARLY_PROMISE_SCHED_H */
