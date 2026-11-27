/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_QUOTA_RECOVERY_H
#define A840_KMD_QUOTA_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "a840_kmd_bo_pool.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_sparse_va.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status alias for quota exhaustion without modifying frozen A840KmdStatus. */
#ifndef A840_KMD_OOM
#define A840_KMD_OOM A840_KMD_NO_MEMORY
#endif

/* Fixed-capacity limits: zero hot-path heap allocations. */
#define A840_KMD_QUOTA_MAX_CLIENTS 64u
#define A840_KMD_QUOTA_MAX_TRACKED_BOS 512u
#define A840_KMD_QUOTA_MAX_CHANNELS 128u
#define A840_KMD_CHANNEL_MAX_INFLIGHT 32u
#define A840_KMD_SUBMIT_MAX_BOS 8u
#define A840_KMD_CHANNEL_MAX_SPARSE_BOS 16u

/* Default global GPU memory budget (12 GiB hard, 9 GiB soft, 512 MiB compositor reserve). */
#define A840_KMD_QUOTA_DEFAULT_GLOBAL_HARD_BYTES UINT64_C(12884901888)
#define A840_KMD_QUOTA_DEFAULT_GLOBAL_SOFT_BYTES UINT64_C(9663676416)
#define A840_KMD_QUOTA_DEFAULT_COMPOSITOR_RESERVE_BYTES UINT64_C(536870912)
#define A840_KMD_RECOVERY_DEFAULT_MAX_RECOVERIES 3u

/* Client priority hierarchy: COMPOSITOR > CAMERA > FOREGROUND_APP > BACKGROUND. */
typedef enum A840KmdClientPriority {
    A840_KMD_PRIO_BACKGROUND = 0u,
    A840_KMD_PRIO_FOREGROUND_APP = 1u,
    A840_KMD_PRIO_CAMERA = 2u,
    A840_KMD_PRIO_COMPOSITOR = 3u,
    A840_KMD_PRIO_COUNT = 4u
} A840KmdClientPriority;

/* Single-channel fault isolation & recovery state machine. */
typedef enum A840KmdChannelState {
    A840_KMD_CHANNEL_HEALTHY = 0u,
    A840_KMD_CHANNEL_FAULTING = 1u,
    A840_KMD_CHANNEL_RESETTING = 2u,
    A840_KMD_CHANNEL_RECOVERED = 3u,
    A840_KMD_CHANNEL_BANNED = 4u
} A840KmdChannelState;

/* Optional custom sleepable lock + monotonic clock ops (binds to XNU IOLock). */
typedef struct A840KmdQuotaSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    uint64_t (*now_ns)(void *cookie);
} A840KmdQuotaSyncOps;

/* External OOM reclamation callbacks.
 * Every callback is invoked STRICTLY AFTER releasing the internal mutex.
 */
typedef struct A840KmdQuotaReclaimOps {
    void *cookie;
    /* Stage 1: Reclaim a specific Parked BO owned by `client_id`. */
    A840KmdStatus (*reclaim_parked_bo)(
        void *cookie,
        uint64_t client_id,
        uint64_t bo_handle,
        uint64_t bytes,
        uint64_t *out_reclaimed_bytes);
    /* Stage 2: Purge a specific VOLATILE BO (guaranteed in_flight_refs == 0) to EMPTY. */
    A840KmdStatus (*purge_volatile_bo)(
        void *cookie,
        uint64_t client_id,
        uint64_t bo_handle,
        uint64_t bytes,
        uint64_t *out_reclaimed_bytes);
    /* Optional bulk Parked BO pool sweep when tracked BOs are insufficient. */
    A840KmdStatus (*sweep_parked_pool)(
        void *cookie,
        uint64_t requester_client_id,
        uint32_t max_victim_priority,
        uint64_t need_bytes,
        uint64_t *out_reclaimed_bytes);
    /* Optional bulk VOLATILE heap sweep when tracked BOs are insufficient. */
    A840KmdStatus (*sweep_volatile_heap)(
        void *cookie,
        uint64_t requester_client_id,
        uint32_t max_victim_priority,
        uint64_t need_bytes,
        uint64_t *out_reclaimed_bytes);
    /* Optional last-resort eviction of a non-critical BACKGROUND client when
     * COMPOSITOR needs global memory; NEVER invoked for COMPOSITOR victims.
     */
    A840KmdStatus (*evict_background_client)(
        void *cookie,
        uint64_t victim_client_id,
        uint64_t need_bytes,
        uint64_t *out_reclaimed_bytes);
} A840KmdQuotaReclaimOps;

/* External channel reset / isolation / notification callbacks.
 * Every callback is invoked STRICTLY AFTER releasing the internal mutex.
 */
typedef struct A840KmdRecoveryOps {
    void *cookie;
    /* Deterministic completion notification for normal or aborted fences. */
    void (*fence_notify)(
        void *cookie,
        uint64_t client_id,
        uint64_t queue_id,
        uint64_t fence,
        A840KmdStatus completion_status,
        uint32_t fault_kind,
        uint64_t recovery_generation);
    /* Single-channel hardware ring/context reset hook (healthy channels untouched). */
    A840KmdStatus (*hw_channel_reset)(
        void *cookie,
        uint64_t client_id,
        uint64_t queue_id,
        uint64_t recovery_generation,
        uint32_t fault_kind);
    /* Optional Sparse VA rollback hook per BO attached to the faulting channel. */
    void (*sparse_rollback_bo)(
        void *cookie,
        uint64_t client_id,
        uint64_t queue_id,
        uint64_t bo_handle,
        uint32_t unbound_tiles);
    /* Optional BO in-flight ref release callback during fault drain. */
    void (*bo_end_submit)(
        void *cookie,
        uint64_t client_id,
        uint64_t bo_handle);
} A840KmdRecoveryOps;

typedef struct A840KmdQuotaRecoveryConfig {
    uint64_t global_soft_limit_bytes;    /* 0 -> 75% of global_hard_limit_bytes */
    uint64_t global_hard_limit_bytes;    /* 0 -> 12 GiB default */
    uint64_t compositor_reserve_bytes;   /* 0 -> default 512 MiB (clamped <= hard/2) */
    uint32_t max_recoveries_before_ban;  /* 0 -> explicit 0 if ban_on_first_fault != 0, else 3 */
    uint32_t ban_on_first_fault;         /* 1 = allow max_recoveries_before_ban == 0 */
    uint32_t sync_bo_pool_inflight;      /* 1 = call a840_kmd_bo_pool_{begin,end}_submit outside lock */
    uint32_t reserved;
    A840KmdBoPool *bo_pool;              /* Optional W07-L1 Parked/Volatile BO pool */
    A840KmdSparseManager *sparse_mgr;    /* Optional W07-L1 Sparse VA manager */
    A840KmdFaultCapture *fault_capture;  /* Optional W03-L1 FaultCapture ring */
    const A840KmdQuotaSyncOps *sync_ops;
    const A840KmdQuotaReclaimOps *reclaim_ops;
    const A840KmdRecoveryOps *recovery_ops;
} A840KmdQuotaRecoveryConfig;

typedef struct A840KmdClientQuotaConfig {
    uint64_t client_id;
    uint32_t priority;                   /* A840KmdClientPriority */
    uint32_t reserved;
    uint64_t soft_limit_bytes;           /* must be > 0 and <= hard_limit_bytes */
    uint64_t hard_limit_bytes;           /* must be >= soft_limit_bytes */
} A840KmdClientQuotaConfig;

typedef struct A840KmdClientQuotaInfo {
    uint64_t client_id;
    uint32_t priority;
    uint32_t active;
    uint64_t soft_limit_bytes;
    uint64_t hard_limit_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t parked_bytes;
    uint64_t volatile_bytes;
    uint64_t oom_reject_count;
    uint64_t reclaim_count;
} A840KmdClientQuotaInfo;

typedef struct A840KmdAllocResult {
    uint64_t requested_bytes;
    uint64_t reclaimed_parked_bytes;
    uint64_t reclaimed_volatile_bytes;
    uint64_t reclaimed_bg_evict_bytes;
    uint64_t client_used_after;
    uint64_t global_used_after;
    uint32_t parked_reclaimed_count;
    uint32_t volatile_purged_count;
    uint32_t volatile_skipped_inflight;
    uint32_t used_compositor_reserve;
    uint32_t admitted;
    uint32_t reserved;
} A840KmdAllocResult;

typedef struct A840KmdChannelInfo {
    uint64_t client_id;
    uint64_t queue_id;
    uint32_t state;                      /* A840KmdChannelState */
    uint32_t active_inflight_count;
    uint32_t fault_count;
    uint32_t recovery_count;
    uint32_t last_fault_kind;            /* A840KmdFaultKind */
    uint32_t last_fault_code;
    uint64_t recovery_generation;
    uint64_t damaged_fence_start;
    uint64_t damaged_fence_end;
    uint64_t submitted_count;
    uint64_t completed_ok_count;
    uint64_t completed_err_count;
    uint64_t last_submitted_fence;
    uint64_t last_completed_fence;
} A840KmdChannelInfo;

typedef struct A840KmdRecoveryReport {
    uint64_t client_id;
    uint64_t queue_id;
    uint32_t old_state;
    uint32_t final_state;
    uint32_t fault_kind;
    uint32_t fault_code;
    uint64_t recovery_generation;
    uint64_t damaged_fence_start;
    uint64_t damaged_fence_end;
    uint32_t aborted_submits;
    uint32_t released_bo_refs;
    uint32_t rolled_back_sparse_bos;
    uint32_t rolled_back_sparse_tiles;
    A840KmdStatus hw_reset_status;
    uint32_t reserved;
    A840KmdFaultSnapshot fault_snapshot;
} A840KmdRecoveryReport;

typedef struct A840KmdQuotaRecoveryStats {
    uint32_t active_clients;
    uint32_t active_tracked_bos;
    uint32_t active_channels;
    uint32_t banned_channels;
    uint64_t global_soft_limit_bytes;
    uint64_t global_hard_limit_bytes;
    uint64_t compositor_reserve_bytes;
    uint64_t global_used_bytes;
    uint64_t global_peak_used_bytes;
    uint64_t total_alloc_requests;
    uint64_t total_alloc_admitted;
    uint64_t total_oom_rejects;
    uint64_t oom_rejects_by_prio[A840_KMD_PRIO_COUNT];
    uint64_t compositor_protected_allocs;
    uint64_t parked_reclaimed_count;
    uint64_t parked_reclaimed_bytes;
    uint64_t volatile_purged_count;
    uint64_t volatile_purged_bytes;
    uint64_t volatile_skipped_inflight;
    uint64_t bg_evict_bytes;
    uint64_t total_faults;
    uint64_t channels_recovered;
    uint64_t channels_banned;
    uint64_t total_aborted_submits;
    uint64_t total_sparse_tiles_rolled_back;
    uint64_t total_healthy_submits_completed;
} A840KmdQuotaRecoveryStats;

/* Internal fixed-capacity slots (exposed so callers can embed without heap allocation). */
typedef struct A840KmdQuotaClientSlot {
    uint32_t active;
    uint32_t priority;
    uint64_t client_id;
    uint64_t soft_limit_bytes;
    uint64_t hard_limit_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t parked_bytes;
    uint64_t volatile_bytes;
    uint64_t oom_reject_count;
    uint64_t reclaim_count;
} A840KmdQuotaClientSlot;

typedef struct A840KmdTrackedBoSlot {
    uint32_t active;
    uint32_t is_parked;
    uint32_t purgeable_state;            /* A840KmdPurgeableState */
    uint32_t in_flight_refs;
    uint32_t reclaiming;
    uint32_t reserved;
    uint64_t slot_gen;
    uint64_t client_id;
    uint64_t bo_handle;
    uint64_t bytes;
    uint64_t lru_seq;
} A840KmdTrackedBoSlot;

typedef struct A840KmdChannelSubmitSlot {
    uint32_t active;
    uint32_t bo_count;
    uint64_t fence;
    uint64_t bo_handles[A840_KMD_SUBMIT_MAX_BOS];
} A840KmdChannelSubmitSlot;

typedef struct A840KmdChannelSlot {
    uint32_t active;
    uint32_t state;                      /* A840KmdChannelState */
    uint32_t fault_count;
    uint32_t recovery_count;
    uint32_t last_fault_kind;
    uint32_t last_fault_code;
    uint32_t sparse_bo_count;
    uint32_t active_inflight_count;
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t recovery_generation;
    uint64_t damaged_fence_start;
    uint64_t damaged_fence_end;
    uint64_t submitted_count;
    uint64_t completed_ok_count;
    uint64_t completed_err_count;
    uint64_t last_submitted_fence;
    uint64_t last_completed_fence;
    A840KmdFaultSnapshot last_fault_snapshot;
    uint64_t sparse_bo_handles[A840_KMD_CHANNEL_MAX_SPARSE_BOS];
    A840KmdChannelSubmitSlot inflight[A840_KMD_CHANNEL_MAX_INFLIGHT];
} A840KmdChannelSlot;

typedef struct A840KmdQuotaRecovery {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_reclaim_ops;
    uint32_t has_recovery_ops;
    uint32_t max_recoveries_before_ban;
    uint32_t sync_bo_pool_inflight;
    uint64_t global_soft_limit_bytes;
    uint64_t global_hard_limit_bytes;
    uint64_t compositor_reserve_bytes;
    uint64_t global_used_bytes;
    uint64_t global_peak_used_bytes;
    uint64_t next_lru_seq;
    uint64_t next_slot_gen;
    A840KmdBoPool *bo_pool;
    A840KmdSparseManager *sparse_mgr;
    A840KmdFaultCapture *fault_capture;
    A840KmdQuotaSyncOps sync_ops;
    A840KmdQuotaReclaimOps reclaim_ops;
    A840KmdRecoveryOps recovery_ops;
    A840KmdQuotaRecoveryStats stats;
    A840KmdQuotaClientSlot clients[A840_KMD_QUOTA_MAX_CLIENTS];
    A840KmdTrackedBoSlot bos[A840_KMD_QUOTA_MAX_TRACKED_BOS];
    A840KmdChannelSlot channels[A840_KMD_QUOTA_MAX_CHANNELS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdQuotaRecovery;

/* ---------------- Lifecycle ---------------- */

A840KmdStatus a840_kmd_qr_init(
    A840KmdQuotaRecovery *qr,
    const A840KmdQuotaRecoveryConfig *config);

void a840_kmd_qr_destroy(A840KmdQuotaRecovery *qr);

/* ---------------- Multi-Client Quota & OOM Reclamation ---------------- */

A840KmdStatus a840_kmd_qr_register_client(
    A840KmdQuotaRecovery *qr,
    const A840KmdClientQuotaConfig *client_cfg);

A840KmdStatus a840_kmd_qr_update_client_quota(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t soft_limit_bytes,
    uint64_t hard_limit_bytes);

/* Atomically unregister client, releasing its charged bytes, tracked BOs,
 * and channels (invoking error notifications outside the lock if any
 * channel still had pending in-flight submissions).
 */
A840KmdStatus a840_kmd_qr_unregister_client(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id);

A840KmdStatus a840_kmd_qr_query_client(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    A840KmdClientQuotaInfo *out_info);

/* Track a client-owned BO for deterministic OOM reclamation and in-flight
 * protection. If `charge_usage != 0`, adds `bytes` to client & global usage
 * (subject to hard quota; returns A840_KMD_OOM without partial mutation).
 * If `charge_usage == 0`, attaches BO metadata to already-admitted quota.
 */
A840KmdStatus a840_kmd_qr_track_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t bytes,
    uint32_t is_parked,
    A840KmdPurgeableState purgeable_state,
    uint32_t charge_usage);

A840KmdStatus a840_kmd_qr_untrack_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t release_usage);

A840KmdStatus a840_kmd_qr_set_bo_parked(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t is_parked);

A840KmdStatus a840_kmd_qr_set_bo_purgeable(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdPurgeableState new_state);

A840KmdStatus a840_kmd_qr_bo_begin_submit(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs);

A840KmdStatus a840_kmd_qr_bo_end_submit(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs);

/* Request GPU memory allocation under per-client and global quotas.
 * Escalating lock-free reclamation ladder before rejecting:
 *   1) Reclaim Parked BOs (same client first, then strictly lower-priority clients).
 *   2) Purge VOLATILE Heap BOs with in_flight_refs == 0 in LRU order
 *      (NEVER touches BOs with in_flight_refs > 0).
 *   3) If still short: reject non-critical clients with A840_KMD_OOM;
 *      COMPOSITOR retains protected reserve and may invoke evict_background_client.
 */
A840KmdStatus a840_kmd_qr_request_alloc(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t requested_bytes,
    A840KmdAllocResult *out_result);

A840KmdStatus a840_kmd_qr_release_bytes(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bytes);

/* ---------------- Single-Channel Fault Isolation & Recovery ---------------- */

A840KmdStatus a840_kmd_qr_open_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id);

A840KmdStatus a840_kmd_qr_close_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id);

/* Associate a Sparse BO with a channel so channel fault rollback unbinds
 * its Sparse VA tiles to the 64 KiB read-only zero page automatically.
 */
A840KmdStatus a840_kmd_qr_channel_attach_sparse_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t bo_handle);

A840KmdStatus a840_kmd_qr_submit_begin(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t fence,
    const uint64_t *bo_handles,
    uint32_t bo_count);

/* Mark all in-flight submissions on (client_id, queue_id) with fence <= `fence`
 * as completed successfully, decrementing BO in_flight_refs and invoking
 * recovery_ops.fence_notify outside the lock.
 */
A840KmdStatus a840_kmd_qr_submit_complete(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t fence,
    uint32_t *out_completed_count);

/* Phase 1 of recovery: isolate ONLY the faulting channel (HEALTHY/RECOVERED -> FAULTING),
 * record fault code + damaged fence range, and capture FaultSnapshot outside lock.
 */
A840KmdStatus a840_kmd_qr_report_channel_fault(
    A840KmdQuotaRecovery *qr,
    const A840KmdFaultInput *fault_input,
    uint32_t fault_code,
    A840KmdRecoveryReport *out_report);

/* Phase 2 of recovery: FAULTING -> RESETTING, drain in-flight submits with
 * deterministic error completion outside lock, release BO in_flight_refs,
 * roll back Sparse VA tiles to read-only zero page, and invoke hw_channel_reset
 * outside lock.
 */
A840KmdStatus a840_kmd_qr_begin_channel_reset(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdRecoveryReport *out_report);

/* Phase 3 of recovery: RESETTING -> RECOVERED (bumping recovery_generation)
 * or RESETTING -> BANNED if recovery threshold exceeded or hw_reset_status != OK.
 */
A840KmdStatus a840_kmd_qr_finish_channel_reset(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdStatus hw_reset_status,
    A840KmdRecoveryReport *out_report);

/* One-shot fault isolation + reset + recovery/ban helper combining Phases 1..3. */
A840KmdStatus a840_kmd_qr_handle_channel_fault(
    A840KmdQuotaRecovery *qr,
    const A840KmdFaultInput *fault_input,
    uint32_t fault_code,
    A840KmdRecoveryReport *out_report);

A840KmdStatus a840_kmd_qr_query_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdChannelInfo *out_info);

A840KmdStatus a840_kmd_qr_get_stats(
    A840KmdQuotaRecovery *qr,
    A840KmdQuotaRecoveryStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_QUOTA_RECOVERY_H */
