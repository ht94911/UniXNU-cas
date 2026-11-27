/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_PREEMPT_RING_H
#define A840_KMD_PREEMPT_RING_H

#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "Adreno840Regs.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef A840_KMD_STATUS_DEFINED
#define A840_KMD_STATUS_DEFINED
typedef enum A840KmdStatus {
    A840_KMD_OK = 0,
    A840_KMD_PASS_TO_V2 = 1,
    A840_KMD_INVALID_ARG = 2,
    A840_KMD_RANGE = 3,
    A840_KMD_ALIGNMENT = 4,
    A840_KMD_PERMISSION_DENIED = 5,
    A840_KMD_CAPACITY = 6,
    A840_KMD_NOT_FOUND = 7,
    A840_KMD_STATE_CONFLICT = 8,
    A840_KMD_STACK_KEY_MISMATCH = 9,
    A840_KMD_UNSUPPORTED = 10,
    A840_KMD_POISONED = 11,
    A840_KMD_DISCONNECTED = 12,
    A840_KMD_IO_ERROR = 13,
    A840_KMD_NO_MEMORY = 14
} A840KmdStatus;
#endif

/* Deterministic queue/submission priorities mapped to the 4 hardware rings. */
#ifndef A840_PRIORITY_DEFINED
#define A840_PRIORITY_DEFINED
typedef enum A840Priority {
    A840_PRIORITY_COMPOSITOR = 0u,
    A840_PRIORITY_HIGH       = 1u,
    A840_PRIORITY_NORMAL     = 2u,
    A840_PRIORITY_BACKGROUND = 3u,
    A840_PRIORITY_COUNT      = 4u
} A840Priority;
#endif

/* Adreno 840 4-Ring hardware priority hierarchy (0 = highest). */
typedef enum A840PreemptRingId {
    A840_RING_0_COMPOSITOR  = 0u,
    A840_RING_1_INTERACTIVE = 1u,
    A840_RING_2_CLIENT_3D   = 2u,
    A840_RING_3_BG_COMPUTE  = 3u,
    A840_RING_COUNT         = 4u
} A840PreemptRingId;

/* Sentinel indicating no ring is currently active / targeted. */
#define A840_RING_NONE ((uint32_t)A840_RING_COUNT)

/* Adreno 840 hardware preemption granularity levels. */
typedef enum A840PreemptLevel {
    A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY  = 1u,
    A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY = 2u
} A840PreemptLevel;

/* Global hardware preemption state machine. */
typedef enum A840PreemptState {
    A840_PREEMPT_IDLE             = 0u,
    A840_PREEMPT_TRIGGERED        = 1u,
    A840_PREEMPT_IN_PROGRESS      = 2u,
    A840_PREEMPT_COMPLETE         = 3u,
    A840_PREEMPT_TIMEOUT_RECOVERY = 4u
} A840PreemptState;

/* Per-ring fault isolation health state. */
typedef enum A840RingHealthState {
    A840_RING_STATE_ACTIVE    = 0u,
    A840_RING_STATE_FAULTED   = 1u,
    A840_RING_STATE_RESETTING = 2u
} A840RingHealthState;

/* Latency sample kinds emitted to external observers (matches wakeup engine). */
typedef enum A840PreemptLatencyKind {
    A840_PREEMPT_LAT_ENQUEUE_TO_DOORBELL = 0u,
    A840_PREEMPT_LAT_TRIGGER_TO_ACTIVE   = 1u
} A840PreemptLatencyKind;

/* Fixed architectural limits & 120Hz timing constants (zero heap allocation). */
#define A840_PREEMPT_RECORD_ALIGN_BYTES       4096u
#define A840_PREEMPT_RECORD_MIN_BYTES         65536u
#define A840_PREEMPT_RING_MAX_SLOTS           256u
#define A840_PREEMPT_FRAME_BUDGET_120HZ_NS    UINT64_C(8333333)
#define A840_PREEMPT_DEFAULT_MIN_SLICE_NS     UINT64_C(500000)
#define A840_PREEMPT_DEFAULT_TIMEOUT_NS       UINT64_C(2000000)
#define A840_PREEMPT_DEFAULT_URGENT_BUDGET_NS UINT64_C(2500000)
#define A840_PREEMPT_AON_CLOCK_HZ             UINT64_C(19200000)
#define A840_PREEMPT_CANONICAL_VA_MAX         ((UINT64_C(1) << 48) - UINT64_C(1))

/* Symbolic register/opcode table populated exclusively from Adreno840Regs.h
 * and NativeAdrenoABI_v3.h (zero raw magic register offsets).
 */
typedef struct A840PreemptHwRegMap {
    uint32_t rb_base_lo;
    uint32_t rb_base_hi;
    uint32_t rb_cntl;
    uint32_t rb_rptr;
    uint32_t rb_wptr;
    uint32_t context_switch_cntl;
    uint32_t context_switch_level_status;
    uint32_t smmu_info_lo;
    uint32_t smmu_info_hi;
    uint32_t priv_non_secure_restore_lo;
    uint32_t priv_non_secure_restore_hi;
    uint32_t non_priv_restore_lo;
    uint32_t non_priv_restore_hi;
    uint32_t preempt_threshold;
    uint32_t pc_gfx_preempt_mode;
    uint32_t sp_gfx_preempt_safe_mode;
    uint32_t rbbm_int_0_status;
    uint32_t rbbm_int_0_mask;
    uint32_t rbbm_int_clear_cmd;
    uint32_t always_on_counter_lo;
    uint32_t always_on_counter_hi;
    uint32_t cp_in_preempt_opcode;
    uint32_t abi_preempt_cap_flag;
} A840PreemptHwRegMap;

/* Optional custom sleepable lock ops (NULL -> built-in pthread_mutex_t). */
typedef struct A840PreemptSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840PreemptSyncOps;

/* Hardware & observer callbacks.
 * CONTRACT: EVERY callback is invoked STRICTLY AFTER releasing the internal mutex.
 */
typedef struct A840PreemptHwOps {
    void *cookie;
    /* Program CP_RB_WPTR doorbell for ring_id. */
    A840KmdStatus (*write_doorbell)(
        void *cookie,
        uint32_t ring_id,
        uint32_t wptr,
        uint64_t seqno,
        uint32_t reg_wptr_offset);
    /* Issue hardware context switch from from_ring to to_ring. */
    A840KmdStatus (*trigger_preempt)(
        void *cookie,
        uint32_t from_ring,
        uint32_t to_ring,
        uint32_t preempt_level,
        uint64_t preempt_record_va,
        uint64_t preempt_record_bytes,
        uint32_t reg_ctx_switch_cntl);
    /* Optional retirement notification after ring advances completed_seqno. */
    void (*on_retire)(
        void *cookie,
        uint32_t ring_id,
        uint64_t completed_seqno,
        uint64_t now_ns);
    /* Optional PI boost state change notification (wires into wakeup engine). */
    void (*on_pi_boost_change)(
        void *cookie,
        uint32_t ring_id,
        uint32_t boosted,
        uint32_t boosted_by_ring,
        uint64_t boost_until_seqno,
        uint64_t now_ns);
    /* Optional preemption timeout/recovery notification. */
    void (*on_timeout_recovery)(
        void *cookie,
        uint32_t from_ring,
        uint32_t to_ring,
        uint64_t elapsed_ns);
    /* Optional latency measurement sample (enqueue->doorbell, trigger->active). */
    void (*on_latency_sample)(
        void *cookie,
        uint32_t latency_kind,
        uint32_t ring_id,
        uint64_t latency_ns,
        uint64_t now_ns);
} A840PreemptHwOps;

/* Per-ring initialization configuration. */
typedef struct A840PreemptRingInit {
    uint64_t preempt_record_va;    /* Required: nonzero, 4 KiB aligned, canonical 48-bit */
    uint64_t preempt_record_bytes; /* Required: >= 64 KiB, 4 KiB aligned */
    uint64_t rb_base_va;           /* Optional: if nonzero, must be 4 KiB aligned */
    uint32_t slot_capacity;        /* 0 -> A840_PREEMPT_RING_MAX_SLOTS; else power-of-2 <= 256 */
    uint32_t preempt_level;        /* L1A_BIN_BOUNDARY or L1B_DRAW_BOUNDARY */
} A840PreemptRingInit;

/* Scheduler initialization configuration. */
typedef struct A840PreemptConfig {
    A840PreemptRingInit rings[A840_RING_COUNT];
    uint64_t min_slice_ns;            /* 0 -> A840_PREEMPT_DEFAULT_MIN_SLICE_NS */
    uint64_t preempt_timeout_ns;      /* 0 -> A840_PREEMPT_DEFAULT_TIMEOUT_NS */
    uint64_t frame_budget_ns;         /* 0 -> A840_PREEMPT_FRAME_BUDGET_120HZ_NS */
    uint64_t deadline_urgent_ns;      /* 0 -> A840_PREEMPT_DEFAULT_URGENT_BUDGET_NS */
    uint32_t allow_zero_min_slice;    /* 1 -> treat min_slice_ns==0 literally as 0 */
    uint32_t reserved;
    const A840PreemptSyncOps *sync_ops;
    const A840PreemptHwOps *hw_ops;
} A840PreemptConfig;

/* Per-submission request descriptor. */
typedef struct A840PreemptSubmitReq {
    uint32_t ring_id;             /* 0..3 */
    uint32_t ib_dwords;           /* optional IB size metadata */
    uint64_t ib_va;               /* optional IB GPU VA (if nonzero, 4-byte aligned) */
    uint64_t enqueue_ns;          /* injected monotonic timestamp */
    uint64_t frame_deadline_ns;   /* optional 120Hz frame deadline (0 = none) */
    uint32_t has_dependency;      /* 1 = depends on dep_ring + dep_seqno */
    uint32_t dep_ring;            /* 0..3 when has_dependency != 0 */
    uint64_t dep_seqno;           /* >0 when has_dependency != 0 */
} A840PreemptSubmitReq;

/* Result of a ring submission. */
typedef struct A840PreemptSubmitResult {
    uint64_t seqno;
    uint32_t wptr;
    uint32_t doorbell_emitted;
    uint32_t preempt_triggered;
    uint32_t preempt_deferred_min_slice;
    uint32_t deadline_exempted;
    uint32_t pi_boost_applied;
} A840PreemptSubmitResult;

/* Per-ring slot stored inside the fixed circular queue. */
typedef struct A840PreemptSlot {
    uint64_t seqno;
    uint64_t ib_va;
    uint64_t enqueue_ns;
    uint64_t frame_deadline_ns;
    uint64_t dep_seqno;
    uint32_t ib_dwords;
    uint32_t has_dep;
    uint32_t dep_ring;
    uint32_t doorbell_sent;
    uint32_t valid;
    uint32_t reserved;
} A840PreemptSlot;

/* Per-ring internal state. */
typedef struct A840PreemptRingState {
    uint32_t ring_id;
    uint32_t health_state;
    uint32_t capacity;
    uint32_t rptr;
    uint32_t wptr;
    uint32_t inflight_count;
    uint32_t preempt_level;
    uint32_t effective_priority;
    uint32_t pi_boost_active;
    uint32_t boosted_by_ring;
    uint64_t preempt_record_va;
    uint64_t preempt_record_bytes;
    uint64_t rb_base_va;
    uint64_t submitted_seqno;
    uint64_t completed_seqno;
    uint64_t boost_until_seqno;
    uint64_t active_since_ns;
    uint64_t last_doorbell_wptr;
    uint64_t last_doorbell_seqno;
    uint64_t fault_count;
    uint64_t reset_count;
    uint64_t dep_max_seqno[A840_RING_COUNT];
    A840PreemptSlot slots[A840_PREEMPT_RING_MAX_SLOTS];
} A840PreemptRingState;

/* Read-only snapshot of one ring (safe to copy out under lock). */
typedef struct A840PreemptRingSnapshot {
    uint32_t ring_id;
    uint32_t health_state;
    uint32_t capacity;
    uint32_t rptr;
    uint32_t wptr;
    uint32_t inflight_count;
    uint32_t preempt_level;
    uint32_t effective_priority;
    uint32_t pi_boost_active;
    uint32_t boosted_by_ring;
    uint64_t preempt_record_va;
    uint64_t preempt_record_bytes;
    uint64_t submitted_seqno;
    uint64_t completed_seqno;
    uint64_t boost_until_seqno;
    uint64_t active_since_ns;
    uint64_t fault_count;
    uint64_t reset_count;
} A840PreemptRingSnapshot;

/* Global scheduler statistics & state snapshot. */
typedef struct A840PreemptSnapshot {
    uint32_t state;
    uint32_t active_ring;
    uint32_t target_ring;
    uint32_t from_ring;
    uint32_t active_preempt_level;
    uint32_t deferred_preempt_pending;
    uint64_t preempt_generation;
    uint64_t trigger_ns;
    uint64_t total_submits;
    uint64_t total_retires;
    uint64_t total_doorbells;
    uint64_t total_preempt_triggers;
    uint64_t total_preempt_completes;
    uint64_t total_preempt_timeouts;
    uint64_t total_min_slice_deferrals;
    uint64_t total_deadline_exemptions;
    uint64_t total_pi_boosts;
    uint64_t total_cycle_rejects;
    A840PreemptRingSnapshot rings[A840_RING_COUNT];
} A840PreemptSnapshot;

typedef struct A840KmdPreemptScheduler {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_hw_ops;
    uint32_t state;
    uint32_t active_ring;
    uint32_t target_ring;
    uint32_t from_ring;
    uint32_t active_preempt_level;
    uint32_t deferred_preempt_pending;
    uint32_t reserved;
    uint64_t min_slice_ns;
    uint64_t preempt_timeout_ns;
    uint64_t frame_budget_ns;
    uint64_t deadline_urgent_ns;
    uint64_t preempt_generation;
    uint64_t trigger_ns;
    uint64_t total_submits;
    uint64_t total_retires;
    uint64_t total_doorbells;
    uint64_t total_preempt_triggers;
    uint64_t total_preempt_completes;
    uint64_t total_preempt_timeouts;
    uint64_t total_min_slice_deferrals;
    uint64_t total_deadline_exemptions;
    uint64_t total_pi_boosts;
    uint64_t total_cycle_rejects;
    A840PreemptSyncOps sync_ops;
    A840PreemptHwOps hw_ops;
    A840PreemptRingState rings[A840_RING_COUNT];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdPreemptScheduler;

/* Populate symbolic hardware register/opcode table from Adreno840Regs.h. */
A840KmdStatus a840_kmd_preempt_get_hw_reg_map(A840PreemptHwRegMap *out_map);

/* Deterministic priority -> hardware ring_id mapping with strict bounds check. */
A840KmdStatus a840_kmd_preempt_priority_to_ring(
    uint32_t priority,
    uint32_t *out_ring_id);

/* Deterministic hardware ring_id -> priority mapping with strict bounds check. */
A840KmdStatus a840_kmd_preempt_ring_to_priority(
    uint32_t ring_id,
    uint32_t *out_priority);

/* Validate a preemption context record VA & byte size (4 KiB alignment, >= 64 KiB). */
A840KmdStatus a840_kmd_preempt_validate_record(
    uint64_t record_va,
    uint64_t record_bytes);

/* Initialize / destroy the 4-ring preemption scheduler (zero heap allocation). */
A840KmdStatus a840_kmd_preempt_init(
    A840KmdPreemptScheduler *sched,
    const A840PreemptConfig *config);

void a840_kmd_preempt_destroy(A840KmdPreemptScheduler *sched);

/* Configure a ring's preemption level (L1A_BIN_BOUNDARY / L1B_DRAW_BOUNDARY). */
A840KmdStatus a840_kmd_preempt_set_ring_level(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint32_t preempt_level);

/* Enqueue a submission into ring_id; evaluates PI boost, cycle detection,
 * anti-thrash min_slice_ns, 120Hz deadline exemption, doorbell emission, and
 * preemption triggering. All callbacks run strictly outside internal lock.
 */
A840KmdStatus a840_kmd_preempt_submit(
    A840KmdPreemptScheduler *sched,
    const A840PreemptSubmitReq *req,
    A840PreemptSubmitResult *out_res);

/* Convenience wrapper taking A840_PRIORITY_* instead of raw ring_id. */
A840KmdStatus a840_kmd_preempt_submit_priority(
    A840KmdPreemptScheduler *sched,
    uint32_t priority,
    const A840PreemptSubmitReq *req_template,
    A840PreemptSubmitResult *out_res);

/* Hardware signals CP entered A840_CP_IN_PREEMPT (TRIGGERED -> IN_PROGRESS). */
A840KmdStatus a840_kmd_preempt_on_hw_in_progress(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns);

/* Hardware signals preemption switch completed (IN_PROGRESS -> COMPLETE).
 * Immediately activates target_ring and writes target_ring doorbell (wptr)
 * strictly outside the internal lock.
 */
A840KmdStatus a840_kmd_preempt_on_switch_done(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns);

/* Explicitly settle COMPLETE -> IDLE. */
A840KmdStatus a840_kmd_preempt_ack_complete(
    A840KmdPreemptScheduler *sched);

/* Check whether an in-flight preemption has timed out (TRIGGERED/IN_PROGRESS ->
 * TIMEOUT_RECOVERY when elapsed >= preempt_timeout_ns).
 */
A840KmdStatus a840_kmd_preempt_check_timeout(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns,
    uint32_t *out_timed_out);

/* Complete timeout recovery (TIMEOUT_RECOVERY -> IDLE or next healthy ring).
 * Optionally isolates stalled from_ring into FAULTED state.
 */
A840KmdStatus a840_kmd_preempt_recover_from_timeout(
    A840KmdPreemptScheduler *sched,
    uint32_t fault_stalled_from_ring,
    uint64_t now_ns);

/* Advance completed_seqno for ring_id (RB_DONE_TS interrupt).
 * Automatically retires slots, clears expired PI boosts, and dispatches next
 * runnable ring (strictly outside lock).
 */
A840KmdStatus a840_kmd_preempt_on_ring_retire(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t completed_seqno,
    uint64_t now_ns);

/* Re-evaluate deferred min-slice preemptions and timeout checks at now_ns. */
A840KmdStatus a840_kmd_preempt_tick(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns);

/* Fault-ring isolation lifecycle (never blocks RING_0_COMPOSITOR):
 * ACTIVE -> FAULTED -> RESETTING -> ACTIVE.
 */
A840KmdStatus a840_kmd_preempt_mark_ring_faulted(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t now_ns);

A840KmdStatus a840_kmd_preempt_begin_ring_reset(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id);

A840KmdStatus a840_kmd_preempt_complete_ring_reset(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t now_ns);

/* Capture atomic read-only snapshot of scheduler & all 4 rings. */
A840KmdStatus a840_kmd_preempt_get_snapshot(
    A840KmdPreemptScheduler *sched,
    A840PreemptSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_PREEMPT_RING_H */
