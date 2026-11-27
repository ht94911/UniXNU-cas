/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_WAKEUP_LATENCY_H
#define A840_KMD_WAKEUP_LATENCY_H

#include <stddef.h>
#include <stdint.h>

#include "a840_kmd_preempt_ring.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed capacities: zero hot-path malloc/free. */
#define A840_WAKEUP_MAX_WAITERS                 128u
#define A840_WAKEUP_EXACT_WINDOW_SAMPLES        2048u
#define A840_WAKEUP_HIST_BUCKETS                64u
#define A840_WAKEUP_DEFAULT_BUDGET_ALERT_NS     UINT64_C(500000)
#define A840_WAKEUP_DEFAULT_R2_COALESCE_NS      UINT64_C(500000)
#define A840_WAKEUP_DEFAULT_R2_MAX_BATCH        4u
#define A840_WAKEUP_DEFAULT_R3_COALESCE_NS      UINT64_C(1000000)
#define A840_WAKEUP_DEFAULT_R3_MAX_BATCH        8u

/* Per-ring wakeup delivery mode. */
typedef enum A840WakeupMode {
    A840_WAKE_MODE_IMMEDIATE = 0u,
    A840_WAKE_MODE_COALESCED = 1u
} A840WakeupMode;

/* Reason passed to a waiter's lock-free wake callback. */
typedef enum A840WakeupReason {
    A840_WAKE_REASON_SEQNO_REACHED = 0u,
    A840_WAKE_REASON_FAST_PATH     = 1u,
    A840_WAKE_REASON_PI_FLUSH      = 2u,
    A840_WAKE_REASON_RING_FAULTED  = 3u
} A840WakeupReason;

/* The 3 tracked nanosecond latency pipelines. */
typedef enum A840WakeupMetricKind {
    A840_LATENCY_ENQUEUE_TO_DOORBELL      = 0u,
    A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE = 1u,
    A840_LATENCY_IRQ_TO_WAITER_WAKE       = 2u,
    A840_LATENCY_METRIC_COUNT             = 3u
} A840WakeupMetricKind;

/* Optional external sleepable mutex + deterministic clock backend. */
typedef struct A840WakeupSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    /* Optional injected monotonic clock (ns). NULL -> use explicit now_ns args. */
    uint64_t (*now_ns)(void *cookie);
} A840WakeupSyncOps;

/* Waiter completion callback.
 * CONTRACT: ALWAYS invoked STRICTLY AFTER releasing the internal mutex.
 */
typedef void (*A840WakeupCallback)(
    void *cookie,
    uint64_t waiter_handle,
    uint32_t ring_id,
    uint64_t target_seqno,
    uint64_t completed_seqno,
    uint32_t reason,
    uint64_t irq_to_wake_ns);

/* Optional over-budget latency alert callback (invoked STRICTLY OUTSIDE lock). */
typedef void (*A840WakeupBudgetAlertCallback)(
    void *cookie,
    uint32_t metric_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t threshold_ns,
    uint64_t now_ns);

/* Per-ring wakeup configuration. */
typedef struct A840WakeupRingConfig {
    uint32_t base_mode;           /* A840_WAKE_MODE_IMMEDIATE or A840_WAKE_MODE_COALESCED */
    uint32_t coalesce_max_batch;  /* Used in COALESCED mode (0 -> ring default) */
    uint64_t coalesce_window_ns;  /* Used in COALESCED mode (0 -> ring default if !allow_zero) */
    uint32_t allow_zero_window;   /* 1 -> treat coalesce_window_ns==0 literally */
    uint32_t reserved;
} A840WakeupRingConfig;

/* Wakeup engine initialization config (pass NULL for defaults). */
typedef struct A840WakeupConfig {
    uint32_t use_custom_ring_cfg;
    uint32_t reserved;
    A840WakeupRingConfig rings[A840_RING_COUNT];
    uint64_t budget_alert_ns[A840_LATENCY_METRIC_COUNT]; /* 0 -> 500,000 ns (500 us) */
    const A840WakeupSyncOps *sync_ops;
    A840WakeupBudgetAlertCallback on_budget_alert;
    void *budget_alert_cookie;
} A840WakeupConfig;

/* Registration result for a waiter. */
typedef struct A840WakeupRegisterResult {
    uint64_t waiter_handle;
    uint32_t already_satisfied;
    uint32_t slot_index;
} A840WakeupRegisterResult;

/* Deterministic latency summary (nearest-rank P50/P95/P99 in nanoseconds).
 * For N sorted samples a[0..N-1], percentile P uses index:
 *   idx = ((P * N + 99) / 100) - 1   (clamped to [0, N-1]).
 */
typedef struct A840LatencyStats {
    uint64_t count;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t sum_ns;
    uint64_t budget_threshold_ns;
    uint64_t budget_breach_count;
    uint64_t compositor_budget_breach_count;
} A840LatencyStats;

/* Per-ring wakeup state snapshot. */
typedef struct A840WakeupRingSnapshot {
    uint32_t ring_id;
    uint32_t base_mode;
    uint32_t effective_mode;
    uint32_t pi_boost_active;
    uint32_t coalesce_max_batch;
    uint32_t coalesce_pending_irqs;
    uint32_t active_waiters;
    uint32_t reserved;
    uint64_t coalesce_window_ns;
    uint64_t coalesce_window_start_ns;
    uint64_t completed_seqno;
    uint64_t total_irqs;
    uint64_t total_immediate_wakes;
    uint64_t total_coalesced_flushes;
    uint64_t total_waiters_woken;
} A840WakeupRingSnapshot;

/* Global wakeup engine snapshot. */
typedef struct A840WakeupSnapshot {
    uint32_t active_waiter_count;
    uint32_t peak_waiter_count;
    uint64_t total_registrations;
    uint64_t total_fast_path_hits;
    uint64_t total_cancellations;
    uint64_t total_pool_rejects;
    A840WakeupRingSnapshot rings[A840_RING_COUNT];
    A840LatencyStats metrics[A840_LATENCY_METRIC_COUNT];
} A840WakeupSnapshot;

/* Pre-allocated fixed waiter slot (zero heap allocation). */
typedef struct A840WakeupWaiterSlot {
    uint32_t in_use;
    uint32_t ring_id;
    uint32_t generation;
    uint32_t satisfied_pending;
    uint64_t waiter_handle;
    uint64_t target_seqno;
    uint64_t register_ns;
    uint64_t satisfied_irq_ns;
    A840WakeupCallback wake_cb;
    void *wake_cookie;
} A840WakeupWaiterSlot;

/* Per-metric fixed-storage latency recorder. */
typedef struct A840LatencyMetricState {
    uint64_t count;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t sum_ns;
    uint64_t budget_threshold_ns;
    uint64_t budget_breach_count;
    uint64_t compositor_budget_breach_count;
    uint32_t window_count;
    uint32_t window_cursor;
    uint64_t window_samples[A840_WAKEUP_EXACT_WINDOW_SAMPLES];
    uint64_t hist_buckets[A840_WAKEUP_HIST_BUCKETS];
} A840LatencyMetricState;

/* Per-ring runtime wakeup state. */
typedef struct A840WakeupRingState {
    uint32_t ring_id;
    uint32_t base_mode;
    uint32_t effective_mode;
    uint32_t pi_boost_active;
    uint32_t coalesce_max_batch;
    uint32_t coalesce_pending_irqs;
    uint32_t active_waiters;
    uint32_t reserved;
    uint64_t coalesce_window_ns;
    uint64_t coalesce_window_start_ns;
    uint64_t completed_seqno;
    uint64_t total_irqs;
    uint64_t total_immediate_wakes;
    uint64_t total_coalesced_flushes;
    uint64_t total_waiters_woken;
} A840WakeupRingState;

typedef struct A840KmdWakeupEngine {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t active_waiter_count;
    uint32_t peak_waiter_count;
    uint32_t next_generation;
    uint32_t reserved;
    uint64_t total_registrations;
    uint64_t total_fast_path_hits;
    uint64_t total_cancellations;
    uint64_t total_pool_rejects;
    A840WakeupSyncOps sync_ops;
    A840WakeupBudgetAlertCallback on_budget_alert;
    void *budget_alert_cookie;
    A840WakeupRingState rings[A840_RING_COUNT];
    A840WakeupWaiterSlot waiters[A840_WAKEUP_MAX_WAITERS];
    A840LatencyMetricState metrics[A840_LATENCY_METRIC_COUNT];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdWakeupEngine;

/* Initialize / destroy wakeup & latency engine (zero heap allocation). */
A840KmdStatus a840_kmd_wakeup_init(
    A840KmdWakeupEngine *engine,
    const A840WakeupConfig *config);

void a840_kmd_wakeup_destroy(A840KmdWakeupEngine *engine);

/* Dynamically update a ring's base wakeup mode & coalescing parameters.
 * RING_0_COMPOSITOR and RING_1_INTERACTIVE reject COALESCED mode (fail-closed).
 */
A840KmdStatus a840_kmd_wakeup_configure_ring(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    const A840WakeupRingConfig *ring_cfg);

/* Update over-budget alert threshold (ns) for a latency metric. */
A840KmdStatus a840_kmd_wakeup_set_budget_threshold(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    uint64_t threshold_ns);

/* Register a zero-allocation waiter for (ring_id, target_seqno).
 * Fast-path: if completed_seqno >= target_seqno, invokes wake_cb immediately
 * outside lock without consuming a pool slot.
 */
A840KmdStatus a840_kmd_wakeup_register_waiter(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t target_seqno,
    uint64_t now_ns,
    A840WakeupCallback wake_cb,
    void *wake_cookie,
    A840WakeupRegisterResult *out_res);

/* Cancel an active waiter and release its pool slot. */
A840KmdStatus a840_kmd_wakeup_cancel_waiter(
    A840KmdWakeupEngine *engine,
    uint64_t waiter_handle);

/* Process hardware RB_DONE_TS interrupt for ring_id at injected irq_ns.
 * - IMMEDIATE_WAKE (RING_0/RING_1 or PI-boosted RING_2/RING_3): wakes all
 *   satisfied waiters immediately outside the lock.
 * - COALESCED_WAKE (non-boosted RING_2/RING_3): batches until window or
 *   max_batch is reached.
 */
A840KmdStatus a840_kmd_wakeup_on_irq(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t completed_seqno,
    uint64_t irq_ns,
    uint32_t *out_woken_count);

/* Flush any coalesced waiters on ring_id when now_ns reaches the coalescing
 * window (or unconditionally when force_flush != 0).
 */
A840KmdStatus a840_kmd_wakeup_flush_coalesced(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t now_ns,
    uint32_t force_flush,
    uint32_t *out_woken_count);

/* Poll all rings at now_ns and flush any ring whose coalescing window expired. */
A840KmdStatus a840_kmd_wakeup_poll_timers(
    A840KmdWakeupEngine *engine,
    uint64_t now_ns,
    uint32_t *out_total_woken);

/* Toggle Priority Inheritance boost for ring_id.
 * Entering boosted state immediately switches RING_2/RING_3 to IMMEDIATE_WAKE
 * and flushes any pending coalesced waiters at now_ns outside the lock.
 */
A840KmdStatus a840_kmd_wakeup_set_pi_boost(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint32_t boosted,
    uint64_t now_ns,
    uint32_t *out_flushed_woken);

/* Wake and drain all active waiters on a faulted ring with RING_FAULTED reason. */
A840KmdStatus a840_kmd_wakeup_fault_ring(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t now_ns,
    uint32_t *out_woken_count);

/* Record a deterministic nanosecond latency sample for any of the 3 metrics. */
A840KmdStatus a840_kmd_wakeup_record_latency(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t now_ns);

/* Query deterministic stats (count/min/max/p50/p95/p99/breaches) for one metric. */
A840KmdStatus a840_kmd_wakeup_get_latency_stats(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    A840LatencyStats *out_stats);

/* Reset all latency histograms/samples (retains thresholds & active waiters). */
A840KmdStatus a840_kmd_wakeup_reset_latency_stats(
    A840KmdWakeupEngine *engine);

/* Capture atomic snapshot of wakeup engine, rings, and all 3 latency metrics. */
A840KmdStatus a840_kmd_wakeup_get_snapshot(
    A840KmdWakeupEngine *engine,
    A840WakeupSnapshot *out_snapshot);

/* Ready-to-wire bridge callbacks compatible with A840PreemptHwOps:
 * Pass (A840KmdWakeupEngine *) as cookie, or invoke from your own wrapper.
 */
void a840_kmd_wakeup_bridge_pi_boost(
    void *wakeup_engine_cookie,
    uint32_t ring_id,
    uint32_t boosted,
    uint32_t boosted_by_ring,
    uint64_t boost_until_seqno,
    uint64_t now_ns);

void a840_kmd_wakeup_bridge_latency(
    void *wakeup_engine_cookie,
    uint32_t preempt_latency_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_WAKEUP_LATENCY_H */
