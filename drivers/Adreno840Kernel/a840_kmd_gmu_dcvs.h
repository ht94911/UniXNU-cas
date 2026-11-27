/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_GMU_DCVS_H
#define A840_KMD_GMU_DCVS_H

#include <stddef.h>
#include <stdint.h>

#include "Adreno840Regs.h"
#include "NativeAdrenoABI_v3.h"
#include "chip_facts_a840.h"

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

/* SM8850 (speed-bin 0xfc = 252) verified 18-level GPU & 4-level GMU OPP facts. */
#define A840_GPU_OPP_COUNT                  18u
#define A840_GMU_OPP_COUNT                  4u
#define A840_SM8850_SPEED_BIN               252u /* 0xfc */

#define A840_GPU_OPP_MIN_IDX                0u   /*  160 MHz */
#define A840_GPU_OPP_IDLE_IDX               2u   /*  222 MHz (initial / idle) */
#define A840_GPU_OPP_TOUCH_FLOOR_IDX        9u   /*  578 MHz (touch / wake floor) */
#define A840_GPU_OPP_LEASE_LOW_IDX          11u  /*  726 MHz (leased floor tier 1) */
#define A840_GPU_OPP_THERMAL_826_IDX        12u  /*  826 MHz */
#define A840_GPU_OPP_LEASE_HIGH_IDX         13u  /*  902 MHz (leased floor tier 2) */
#define A840_GPU_OPP_THERMAL_1000_IDX       14u  /* 1000 MHz */
#define A840_GPU_OPP_MAX_IDX                17u  /* 1200 MHz */

/* Fixed architectural window sizes & 120Hz DCVS defaults (zero heap allocation). */
#define A840_DCVS_FRAME_WINDOW_LEN          16u
#define A840_DCVS_OSC_RING_LEN              64u
#define A840_DCVS_FRAME_BUDGET_120HZ_NS     UINT64_C(8333333)
#define A840_DCVS_DEFAULT_INTER_FRAME_NS    UINT64_C(16700000)   /* 16.7 ms hold */
#define A840_DCVS_DEFAULT_IDLE_DECAY_NS     UINT64_C(80000000)   /* 80 ms true idle */
#define A840_DCVS_DEFAULT_IDLE_STEP_NS      UINT64_C(16666667)   /* smooth 1-step/2 frames */
#define A840_DCVS_DEFAULT_MIN_DWELL_NS      UINT64_C(33333333)   /* 4 frames @ 120Hz */
#define A840_DCVS_DEFAULT_TOUCH_BOOST_NS    UINT64_C(120000000)  /* 120 ms touch hold */
#define A840_DCVS_DEFAULT_MDSS_HOLD_NS      UINT64_C(66666666)   /* 8 frames @ 120Hz */
#define A840_DCVS_DEFAULT_THERMAL_STALE_NS  UINT64_C(500000000)  /* 500 ms freshness */
#define A840_DCVS_DEFAULT_HFI_TIMEOUT_NS    UINT64_C(5000000)    /* 5 ms HFI timeout */
#define A840_DCVS_OSC_WINDOW_NS             UINT64_C(10000000000)/* 10 s window */
#define A840_DCVS_DEFAULT_MAX_OSC_10S       24u
#define A840_DCVS_DEFAULT_HIGH_UTIL_PM      750u                 /* 75.0% */
#define A840_DCVS_DEFAULT_LOW_UTIL_PM       350u                 /* 35.0% */
#define A840_DCVS_DEFAULT_DOWN_HYST_FRAMES  8u
#define A840_DCVS_DEFAULT_RING0_DENSITY_TH  4u
#define A840_DCVS_DEFAULT_LOW_MARGIN_NS     UINT64_C(1500000)    /* 1.5 ms */

/* Thermal thresholds in milli-Celsius (mC). */
#define A840_DCVS_THERMAL_TIER0_MC          75000  /* >= 75 C -> cap 1000 MHz */
#define A840_DCVS_THERMAL_TIER1_MC          80000  /* >= 80 C -> cap  902 MHz */
#define A840_DCVS_THERMAL_TIER2_MC          85000  /* >= 85 C -> cap  826 MHz */
#define A840_DCVS_THERMAL_TIER3_MC          90000  /* >= 90 C -> cap  726 MHz */
#define A840_DCVS_THERMAL_TIER4_MC          95000  /* >= 95 C -> cap  578 MHz */
#define A840_DCVS_DEFAULT_THERMAL_HYST_MC   2000   /* 2.0 C release hysteresis */
#define A840_DCVS_DEFAULT_LEASE_726_MAX_MC  55000  /* 55.0 C max for 726 MHz lease */
#define A840_DCVS_DEFAULT_LEASE_902_MAX_MC  50000  /* 50.0 C max for 902 MHz lease */

/* Single GPU OPP entry (logical structured frequency/voltage table). */
typedef struct A840GpuOppEntry {
    uint32_t level_index;       /* 0..17 */
    uint32_t freq_mhz;          /* 160..1200 */
    uint64_t freq_hz;           /* 160000000..1200000000 */
    uint32_t hfi_voltage_level; /* SM8850 HFI voltage corner (0x32..0x1c0) */
    uint32_t min_gmu_level;     /* Paired minimum GMU OPP index (0..3) */
} A840GpuOppEntry;

/* Single GMU OPP entry. */
typedef struct A840GmuOppEntry {
    uint32_t level_index;       /* 0..3 */
    uint32_t freq_mhz;          /* 475..750 */
    uint64_t freq_hz;           /* 475000000..750000000 */
    uint32_t hfi_voltage_level; /* SM8850 GMU HFI corner (0x38..0x100) */
} A840GmuOppEntry;

/* Symbolic hardware register map populated solely from Adreno840Regs.h & chip_facts_a840.h. */
typedef struct A840GmuDcvsHwRegMap {
    uint32_t cp_aperture_cntl_gmu;
    uint32_t cp_cp2gmu_status;
    uint32_t rbbm_clock_cntl_global;
    uint32_t always_on_counter_lo;
    uint32_t always_on_counter_hi;
    uint32_t cp_in_gmu_interrupt;
    uint64_t chip_id;
    uint32_t speed_bin;
    uint32_t gpu_opp_count;
    uint32_t gmu_opp_count;
} A840GmuDcvsHwRegMap;

/* HFI link health & recovery state machine. */
typedef enum A840GmuHfiState {
    A840_GMU_HFI_HEALTHY           = 0u,
    A840_GMU_HFI_PENDING_ACK       = 1u,
    A840_GMU_HFI_DEGRADED_RECOVERY = 2u
} A840GmuHfiState;

/* Reason tags for HFI DCVS frequency change requests and observer events. */
typedef enum A840DcvsReason {
    A840_DCVS_REASON_INIT              = 0u,
    A840_DCVS_REASON_FRAME_UTIL_UP     = 1u,
    A840_DCVS_REASON_FRAME_UTIL_DOWN   = 2u,
    A840_DCVS_REASON_WAKEUP_BURST      = 3u,
    A840_DCVS_REASON_TOUCH_FLOOR       = 4u,
    A840_DCVS_REASON_RING0_DENSITY     = 5u,
    A840_DCVS_REASON_MDSS_LOW_MARGIN   = 6u,
    A840_DCVS_REASON_MDSS_MISSED_VSYNC = 7u,
    A840_DCVS_REASON_LEASE_FLOOR       = 8u,
    A840_DCVS_REASON_THERMAL_CAP       = 9u,
    A840_DCVS_REASON_IDLE_DECAY        = 10u,
    A840_DCVS_REASON_HFI_RECOVERY      = 11u
} A840DcvsReason;

/* Structured HFI frequency transition request passed outside lock to hardware ops. */
typedef struct A840GmuHfiRequest {
    uint64_t seq_id;
    uint64_t now_ns;
    uint32_t gpu_level;
    uint32_t gmu_level;
    uint64_t gpu_freq_hz;
    uint64_t gmu_freq_hz;
    uint32_t gpu_hfi_vote;
    uint32_t gmu_hfi_vote;
    uint32_t reason;            /* A840DcvsReason */
    uint32_t is_recovery_retry;
} A840GmuHfiRequest;

/* Observer notification emitted strictly outside internal lock. */
typedef struct A840GmuDcvsEvent {
    uint64_t seq_id;
    uint64_t now_ns;
    uint32_t prev_gpu_level;
    uint32_t new_gpu_level;
    uint32_t prev_gmu_level;
    uint32_t new_gmu_level;
    uint32_t reason;            /* A840DcvsReason */
    uint32_t hfi_state;         /* A840GmuHfiState */
} A840GmuDcvsEvent;

/* Optional custom sleepable lock ops (NULL -> built-in pthread_mutex_t). */
typedef struct A840GmuDcvsSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840GmuDcvsSyncOps;

/* Hardware HFI transport & observer callbacks.
 * CONTRACT: EVERY callback is invoked STRICTLY AFTER releasing the internal lock.
 */
typedef struct A840GmuHfiOps {
    void *cookie;
    A840KmdStatus (*send_dcvs_request)(
        void *cookie,
        const A840GmuHfiRequest *req);
    void (*on_state_change)(
        void *cookie,
        const A840GmuDcvsEvent *ev);
    void (*on_hfi_fault)(
        void *cookie,
        uint32_t hfi_state,
        A840KmdStatus err,
        uint32_t confirmed_gpu_level,
        uint32_t desired_gpu_level,
        uint64_t now_ns);
} A840GmuHfiOps;

/* Governor configuration (0 fields select verified SM8850 120Hz defaults). */
typedef struct A840GmuDcvsConfig {
    uint64_t inter_frame_hold_ns;      /* Default 16.7 ms */
    uint64_t idle_decay_ns;            /* Default 80 ms */
    uint64_t idle_step_interval_ns;    /* Default 16.667 ms */
    uint64_t min_dwell_ns;             /* Default 33.333 ms */
    uint64_t default_touch_boost_ns;   /* Default 120 ms */
    uint64_t mdss_boost_hold_ns;       /* Default 66.667 ms */
    uint64_t thermal_stale_ns;         /* Default 500 ms */
    uint64_t hfi_timeout_ns;           /* Default 5 ms */
    uint64_t low_margin_threshold_ns;  /* Default 1.5 ms */
    uint32_t high_util_permille;       /* Default 750 (75%) */
    uint32_t low_util_permille;        /* Default 350 (35%) */
    uint32_t downscale_hyst_frames;    /* Default 8 frames */
    uint32_t max_transitions_per_10s;  /* Default 24 */
    uint32_t ring0_density_threshold;  /* Default 4 submits/frame */
    int32_t  thermal_hysteresis_mc;    /* Default 2000 (2.0 C); <0 rejected */
    int32_t  lease_726_max_temp_mc;    /* Default 55000 (55.0 C) */
    int32_t  lease_902_max_temp_mc;    /* Default 50000 (50.0 C) */
    uint32_t async_hfi_ack;            /* 0=sync confirm on OK, 1=wait for ack */
    uint32_t initial_gpu_level;        /* 0 -> A840_GPU_OPP_IDLE_IDX (222 MHz); 1..17 explicit */
    uint32_t use_explicit_init_level;  /* Set 1 to allow initial_gpu_level == 0 (160 MHz) */
    const A840GmuDcvsSyncOps *sync_ops;
    const A840GmuHfiOps *hfi_ops;
} A840GmuDcvsConfig;

/* Per-frame workload sample submitted to the 120Hz governor. */
typedef struct A840DcvsFrameSample {
    uint64_t now_ns;
    uint64_t gpu_busy_ns;
    uint64_t frame_period_ns;          /* 0 -> A840_DCVS_FRAME_BUDGET_120HZ_NS */
    int64_t  scanout_margin_ns;        /* Ignored if has_scanout_margin == 0 */
    uint32_t has_scanout_margin;
    uint32_t ring0_submits;
    uint32_t missed_deadline;
    uint32_t reserved;
} A840DcvsFrameSample;

/* Atomic read-only snapshot of governor state and telemetry. */
typedef struct A840GmuDcvsSnapshot {
    uint32_t confirmed_gpu_level;
    uint32_t desired_gpu_level;
    uint32_t confirmed_gmu_level;
    uint32_t desired_gmu_level;
    uint64_t confirmed_gpu_freq_hz;
    uint64_t desired_gpu_freq_hz;
    uint64_t confirmed_gmu_freq_hz;
    uint32_t thermal_cap_level;
    uint32_t thermal_tier;             /* 0=uncapped, 1..5 = 75/80/85/90/95 C */
    int32_t  last_temp_mc;
    uint32_t has_thermal_sample;
    uint32_t hfi_state;                /* A840GmuHfiState */
    uint32_t touch_floor_active;
    uint32_t lease_floor_active;
    uint32_t lease_target_level;
    uint32_t mdss_boost_active;
    uint32_t rolling_util_permille;
    uint32_t transitions_in_last_10s;
    uint64_t total_frames_recorded;
    uint64_t total_upscales;
    uint64_t total_downscales;
    uint64_t total_hfi_requests;
    uint64_t total_hfi_failures;
    uint64_t total_hfi_recoveries;
    uint64_t inter_frame_hold_hits;
    uint64_t osc_budget_throttles;
    uint64_t thermal_clamp_events;
    uint64_t lease_granted_count;
    uint64_t lease_fail_closed_count;
    uint64_t mdss_boost_hints_applied;
    uint64_t mdss_boost_releases_applied;
    uint64_t idle_decay_steps;
} A840GmuDcvsSnapshot;

/* Internal fixed-size frame window slot. */
typedef struct A840DcvsFrameSlot {
    uint64_t now_ns;
    uint64_t busy_ns;
    uint64_t period_ns;
    uint32_t ring0_submits;
    uint32_t missed_deadline;
} A840DcvsFrameSlot;

/* Zero-heap-allocation Adreno 840 GMU/HFI 18-level DCVS governor instance. */
typedef struct A840KmdGmuDcvs {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_hfi_ops;
    uint32_t async_hfi_ack;

    /* Validated configuration */
    uint64_t inter_frame_hold_ns;
    uint64_t idle_decay_ns;
    uint64_t idle_step_interval_ns;
    uint64_t min_dwell_ns;
    uint64_t default_touch_boost_ns;
    uint64_t mdss_boost_hold_ns;
    uint64_t thermal_stale_ns;
    uint64_t hfi_timeout_ns;
    uint64_t low_margin_threshold_ns;
    uint32_t high_util_permille;
    uint32_t low_util_permille;
    uint32_t downscale_hyst_frames;
    uint32_t max_transitions_per_10s;
    uint32_t ring0_density_threshold;
    int32_t  thermal_hysteresis_mc;
    int32_t  lease_726_max_temp_mc;
    int32_t  lease_902_max_temp_mc;

    /* Live OPP & HFI state */
    uint32_t confirmed_gpu_level;
    uint32_t desired_gpu_level;
    uint32_t confirmed_gmu_level;
    uint32_t desired_gmu_level;
    uint32_t hfi_state;
    uint32_t hfi_pending_gpu_level;
    uint32_t hfi_pending_gmu_level;
    uint32_t hfi_pending_reason;
    uint64_t next_hfi_seq;
    uint64_t hfi_pending_seq;
    uint64_t hfi_pending_sent_ns;
    uint64_t last_confirmed_seq;
    uint64_t last_change_ns;
    uint64_t last_active_ns;
    uint64_t last_idle_step_ns;
    uint64_t last_observed_ns;
    uint32_t is_idle;
    uint32_t low_util_streak;
    uint32_t rolling_util_permille;
    uint32_t pending_ring0_burst;

    /* Touch boost, cold-sensor lease, MDSS boost, and thermal hard cap */
    uint64_t touch_floor_expire_ns;
    uint32_t touch_floor_level;
    uint32_t lease_requested_level;
    uint64_t lease_expire_ns;
    uint32_t lease_active;
    uint32_t mdss_boost_active;
    uint32_t mdss_boost_floor_level;
    uint64_t mdss_boost_expire_ns;

    uint32_t has_thermal_sample;
    int32_t  last_temp_mc;
    uint64_t last_thermal_ns;
    uint32_t thermal_tier;
    uint32_t thermal_cap_level;

    /* Rolling frame history & 10s oscillation timestamp ring */
    uint32_t frame_head;
    uint32_t frame_count;
    A840DcvsFrameSlot frames[A840_DCVS_FRAME_WINDOW_LEN];

    uint32_t osc_head;
    uint32_t osc_count;
    uint64_t osc_timestamps_ns[A840_DCVS_OSC_RING_LEN];

    /* Counters */
    uint64_t total_frames_recorded;
    uint64_t total_upscales;
    uint64_t total_downscales;
    uint64_t total_hfi_requests;
    uint64_t total_hfi_failures;
    uint64_t total_hfi_recoveries;
    uint64_t inter_frame_hold_hits;
    uint64_t osc_budget_throttles;
    uint64_t thermal_clamp_events;
    uint64_t lease_granted_count;
    uint64_t lease_fail_closed_count;
    uint64_t mdss_boost_hints_applied;
    uint64_t mdss_boost_releases_applied;
    uint64_t idle_decay_steps;

    A840GmuDcvsSyncOps sync_ops;
    A840GmuHfiOps hfi_ops;
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdGmuDcvs;

/* Static SM8850 OPP table accessors & validators. */
const A840GpuOppEntry *a840_gmu_dcvs_get_opp_table(size_t *out_count);
const A840GmuOppEntry *a840_gmu_dcvs_get_gmu_table(size_t *out_count);

A840KmdStatus a840_gmu_dcvs_validate_opp_tables(
    const A840GpuOppEntry *gpu_table,
    size_t gpu_count,
    const A840GmuOppEntry *gmu_table,
    size_t gmu_count);

uint32_t a840_gmu_dcvs_find_level_for_freq(uint64_t freq_hz);
uint32_t a840_gmu_dcvs_required_gmu_level(uint32_t gpu_level);
void a840_gmu_dcvs_get_hw_reg_map(A840GmuDcvsHwRegMap *out_map);

/* Lifecycle (zero heap allocation). */
A840KmdStatus a840_gmu_dcvs_init(
    A840KmdGmuDcvs *gov,
    const A840GmuDcvsConfig *config);

void a840_gmu_dcvs_destroy(A840KmdGmuDcvs *gov);

/* Frame workload & Ring 0 compositor pressure inputs. */
A840KmdStatus a840_gmu_dcvs_record_frame(
    A840KmdGmuDcvs *gov,
    const A840DcvsFrameSample *sample);

A840KmdStatus a840_gmu_dcvs_on_ring0_submit(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns,
    uint32_t pending_ring0_count,
    uint32_t urgent_120hz);

/* Touch/animation boost (578 MHz floor, wakeup jump max(2x cur, 578 MHz)). */
A840KmdStatus a840_gmu_dcvs_notify_touch(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns,
    uint64_t duration_ns);

/* Cold-sensor leased floor (726 MHz or 902 MHz only; fail-closed if stale/hot). */
A840KmdStatus a840_gmu_dcvs_request_lease(
    A840KmdGmuDcvs *gov,
    uint32_t target_level,
    uint64_t now_ns,
    uint64_t duration_ns);

A840KmdStatus a840_gmu_dcvs_release_lease(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns);

/* Thermal sensor update (milli-Celsius); enforces 5-tier hard ceiling >=75 C. */
A840KmdStatus a840_gmu_dcvs_update_thermal(
    A840KmdGmuDcvs *gov,
    int32_t temp_mc,
    uint64_t now_ns);

/* Periodic tick for idle decay (after idle_decay_ns), lease freshness, and HFI timeout. */
A840KmdStatus a840_gmu_dcvs_tick(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns);

/* MDSS 120Hz closed-loop boost hint consumer + ready-to-wire callback adapter. */
A840KmdStatus a840_gmu_dcvs_apply_mdss_hint(
    A840KmdGmuDcvs *gov,
    const A840MdssBoostHint *hint);

void a840_gmu_dcvs_bridge_mdss_boost(
    void *gmu_dcvs_cookie,
    const A840MdssBoostHint *hint);

/* HFI async completion, explicit fault/timeout injection, and recovery hooks. */
A840KmdStatus a840_gmu_dcvs_on_hfi_ack(
    A840KmdGmuDcvs *gov,
    uint64_t seq_id,
    uint64_t now_ns);

A840KmdStatus a840_gmu_dcvs_on_hfi_error(
    A840KmdGmuDcvs *gov,
    uint64_t seq_id,
    A840KmdStatus err,
    uint64_t now_ns);

A840KmdStatus a840_gmu_dcvs_on_hfi_recovered(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns);

/* Atomic snapshot query. */
A840KmdStatus a840_gmu_dcvs_get_snapshot(
    A840KmdGmuDcvs *gov,
    A840GmuDcvsSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_GMU_DCVS_H */
