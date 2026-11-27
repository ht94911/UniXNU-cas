/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_MDSS_SYNC_H
#define A840_KMD_MDSS_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "Adreno840Regs.h"
#include "NativeAdrenoABI_v3.h"

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

/* Shared MDSS -> GMU DCVS closed-loop boost hint contract. */
#ifndef A840_MDSS_BOOST_HINT_DEFINED
#define A840_MDSS_BOOST_HINT_DEFINED
typedef enum A840MdssBoostSeverity {
    A840_MDSS_BOOST_NONE             = 0u,
    A840_MDSS_BOOST_LOW_MARGIN       = 1u,
    A840_MDSS_BOOST_MISSED_VSYNC     = 2u,
    A840_MDSS_BOOST_CONSECUTIVE_MISS = 3u
} A840MdssBoostSeverity;

typedef struct A840MdssBoostHint {
    uint64_t frame_id;
    uint64_t now_ns;
    int64_t  scanout_margin_ns;
    uint64_t target_vsync_ns;
    uint32_t severity;             /* A840MdssBoostSeverity */
    uint32_t consecutive_missed;
    uint32_t recommended_step_up;
    uint32_t release_pressure;     /* 1 = release urgent boost after N good frames */
} A840MdssBoostHint;
#endif

/* 120Hz MDSS scanout timing constants (zero heap allocation). */
#define A840_MDSS_PERIOD_120HZ_NS          UINT64_C(8333333)
#define A840_MDSS_MIN_PERIOD_NS            UINT64_C(1000000)   /* 1000 Hz upper bound */
#define A840_MDSS_MAX_PERIOD_NS            UINT64_C(100000000) /* 10 Hz lower bound */
#define A840_MDSS_DEFAULT_COMMIT_LEAD_NS   UINT64_C(800000)    /* 800 us latch lead */
#define A840_MDSS_DEFAULT_LOW_MARGIN_NS    UINT64_C(1500000)   /* 1.5 ms boost trigger */
#define A840_MDSS_DEFAULT_COMFORT_MARGIN_NS UINT64_C(2500000)  /* 2.5 ms comfortable */
#define A840_MDSS_DEFAULT_URGENT_RING0_NS  UINT64_C(2500000)   /* matches preempt urgent */
#define A840_MDSS_DEFAULT_BOOST_RELEASE_N  6u
#define A840_MDSS_DEFAULT_PLL_ALPHA_PM     125u                /* 12.5% EMA drift lock */
#define A840_MDSS_PLL_MAX_DRIFT_PM         50u                 /* +/-5% sanity band */

/* Commit scheduling decision returned by MDSS 120Hz sync coordinator. */
typedef enum A840MdssCommitDecision {
    A840_MDSS_COMMIT_HIT_CURRENT_VSYNC = 0u, /* Zero extra VSYNC waits (EXTRA_VSYNC_WAITS = 0) */
    A840_MDSS_COMMIT_DEFER_NEXT_VSYNC  = 1u  /* Past latch cutoff -> clean next VSYNC, no tear */
} A840MdssCommitDecision;

/* Optional custom sleepable lock ops (NULL -> built-in pthread_mutex_t). */
typedef struct A840MdssSyncLockOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840MdssSyncLockOps;

/* Closed-loop callbacks invoked STRICTLY AFTER releasing internal lock. */
typedef struct A840MdssSyncCallbacks {
    void *cookie;
    void (*on_boost_hint)(
        void *cookie,
        const A840MdssBoostHint *hint);
    void (*on_commit_planned)(
        void *cookie,
        uint64_t frame_id,
        uint32_t decision,
        uint64_t target_vsync_ns,
        int64_t  scanout_margin_ns,
        uint32_t extra_vsync_waits);
} A840MdssSyncCallbacks;

/* Configuration for MDSS 120Hz coordinator (0 fields pick verified defaults). */
typedef struct A840MdssSyncConfig {
    uint64_t nominal_period_ns;          /* Default 8,333,333 ns (120Hz) */
    uint64_t commit_lead_time_ns;        /* Default 800 us */
    uint64_t low_margin_threshold_ns;    /* Default 1,500 us */
    uint64_t comfortable_margin_ns;      /* Default 2,500 us */
    uint64_t urgent_ring0_threshold_ns;  /* Default 2,500 us */
    uint32_t boost_release_frames;       /* Default 6 consecutive comfortable frames */
    uint32_t pll_alpha_permille;         /* Default 125 (1..1000) */
    uint64_t initial_vsync_ns;           /* Initial VSYNC anchor timestamp */
    uint32_t has_initial_vsync;          /* 1 = phase locked at initial_vsync_ns */
    const A840MdssSyncLockOps *sync_ops;
    const A840MdssSyncCallbacks *callbacks;
} A840MdssSyncConfig;

/* Deadline query snapshot for a given injected now_ns. */
typedef struct A840MdssDeadlineInfo {
    uint64_t now_ns;
    uint64_t current_vsync_ns;
    uint64_t latch_cutoff_ns;
    uint64_t next_vsync_ns;
    uint64_t tracked_period_ns;
    int64_t  scanout_margin_ns;          /* latch_cutoff_ns - now_ns */
    uint64_t vsync_epoch;
    uint32_t past_cutoff;
    uint32_t urgent_for_ring0;           /* 1 if Ring 0 should treat frame as urgent */
} A840MdssDeadlineInfo;

/* Input frame commit request for MDSS scanout synchronization. */
typedef struct A840MdssCommitRequest {
    uint64_t frame_id;
    uint64_t now_ns;
    uint64_t estimated_gpu_done_ns;      /* 0 -> ready at now_ns */
    uint32_t has_early_writer_promise;   /* RenderServer early WriterPromise scheduled */
    uint32_t is_retired_ready;           /* GPU fence already retired */
    uint32_t ring0_compositor;           /* Originated from Ring 0 compositor */
    uint32_t reserved;                   /* Must be 0 (fail-closed) */
} A840MdssCommitRequest;

/* Computed scanout commit plan returned to NativeMobileDisplay / compositor. */
typedef struct A840MdssCommitPlan {
    uint64_t frame_id;
    uint32_t decision;                   /* A840MdssCommitDecision */
    uint32_t extra_vsync_waits;          /* 0 on timely hit; >=1 on deferred cutoff miss */
    uint64_t target_vsync_ns;
    uint64_t latch_cutoff_ns;
    uint64_t effective_ready_ns;
    int64_t  scanout_margin_ns;          /* Margin vs current VSYNC latch cutoff (neg if missed) */
    int64_t  target_vsync_margin_ns;     /* Margin vs chosen target VSYNC latch cutoff (>=0) */
    uint32_t urgent_for_ring0;
    uint32_t boost_hint_emitted;
    A840MdssBoostHint boost_hint;
} A840MdssCommitPlan;

/* Atomic telemetry snapshot of MDSS 120Hz coordinator. */
typedef struct A840MdssSyncStats {
    uint64_t frames_planned;
    uint64_t frames_on_time;
    uint64_t frames_missed_vsync;
    uint64_t missed_cutoff_count;
    uint64_t extra_vsync_waits_avoided;
    uint64_t total_extra_vsync_waits;
    uint64_t dcvs_boost_hints_sent;
    uint64_t dcvs_boost_releases_sent;
    uint64_t vsync_events_recorded;
    uint64_t pll_drift_adjustments;
    uint64_t tracked_period_ns;
    uint64_t nominal_period_ns;
    uint64_t last_vsync_ns;
    uint64_t vsync_seq;
    int64_t  min_scanout_margin_ns;
    int64_t  max_scanout_margin_ns;
    int64_t  avg_scanout_margin_ns;
    int64_t  avg_on_time_margin_ns;
    uint32_t consecutive_missed_frames;
    uint32_t consecutive_comfortable_frames;
    uint32_t boost_pressure_active;
    uint32_t phase_locked;
} A840MdssSyncStats;

/* Zero-heap-allocation MDSS 120Hz scanout sync coordinator instance. */
typedef struct A840KmdMdssSync {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_callbacks;
    uint32_t phase_locked;

    uint64_t nominal_period_ns;
    uint64_t tracked_period_ns;
    uint64_t commit_lead_time_ns;
    uint64_t low_margin_threshold_ns;
    uint64_t comfortable_margin_ns;
    uint64_t urgent_ring0_threshold_ns;
    uint32_t boost_release_frames;
    uint32_t pll_alpha_permille;

    uint64_t last_vsync_ns;
    uint64_t vsync_seq;
    uint64_t last_planned_vsync_ns;

    uint32_t consecutive_missed_frames;
    uint32_t consecutive_comfortable_frames;
    uint32_t boost_pressure_active;
    uint32_t has_margin_sample;

    uint64_t frames_planned;
    uint64_t frames_on_time;
    uint64_t frames_missed_vsync;
    uint64_t missed_cutoff_count;
    uint64_t extra_vsync_waits_avoided;
    uint64_t total_extra_vsync_waits;
    uint64_t dcvs_boost_hints_sent;
    uint64_t dcvs_boost_releases_sent;
    uint64_t vsync_events_recorded;
    uint64_t pll_drift_adjustments;
    int64_t  min_scanout_margin_ns;
    int64_t  max_scanout_margin_ns;
    int64_t  sum_scanout_margin_ns;
    int64_t  sum_on_time_margin_ns;

    A840MdssSyncLockOps sync_ops;
    A840MdssSyncCallbacks callbacks;
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdMdssSync;

/* Lifecycle (zero heap allocation). */
A840KmdStatus a840_mdss_sync_init(
    A840KmdMdssSync *sync,
    const A840MdssSyncConfig *config);

void a840_mdss_sync_destroy(A840KmdMdssSync *sync);

/* Record hardware VSYNC / TE interrupt timestamp and update 120Hz PLL phase. */
A840KmdStatus a840_mdss_sync_on_vsync(
    A840KmdMdssSync *sync,
    uint64_t vsync_ts_ns,
    uint64_t te_seq);

/* Query next VSYNC deadline, latch cutoff, and scanout margin at injected now_ns. */
A840KmdStatus a840_mdss_sync_query_deadline(
    A840KmdMdssSync *sync,
    uint64_t now_ns,
    A840MdssDeadlineInfo *out_info);

/* Plan frame commit window (eliminates extra VSYNC waits when ready before cutoff,
 * defers cleanly without tear if past cutoff, and feeds GMU DCVS boost hints
 * strictly outside internal lock).
 */
A840KmdStatus a840_mdss_sync_plan_commit(
    A840KmdMdssSync *sync,
    const A840MdssCommitRequest *req,
    A840MdssCommitPlan *out_plan);

/* Update runtime thresholds (commit lead time, low/comfortable margins). */
A840KmdStatus a840_mdss_sync_set_thresholds(
    A840KmdMdssSync *sync,
    uint64_t commit_lead_time_ns,
    uint64_t low_margin_threshold_ns,
    uint64_t comfortable_margin_ns);

/* Reset telemetry counters (retains phase lock & configuration). */
A840KmdStatus a840_mdss_sync_reset_stats(A840KmdMdssSync *sync);

/* Capture atomic telemetry snapshot. */
A840KmdStatus a840_mdss_sync_get_stats(
    A840KmdMdssSync *sync,
    A840MdssSyncStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_MDSS_SYNC_H */
