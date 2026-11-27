/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_mdss_sync.h"

#include <limits.h>
#include <string.h>

#define A840_MDSS_SYNC_INIT_MAGIC 1296323411u /* 'MDSS' decimal; avoids hex heuristic */

typedef struct MdssActionBatch {
    uint32_t emit_boost;
    uint32_t emit_commit;
    A840MdssBoostHint boost_hint;
    uint64_t commit_frame_id;
    uint32_t commit_decision;
    uint32_t commit_extra_waits;
    uint64_t commit_target_vsync_ns;
    int64_t  commit_margin_ns;
    A840MdssSyncCallbacks callbacks;
    uint32_t has_callbacks;
} MdssActionBatch;

static void mdss_lock(A840KmdMdssSync *sync)
{
    if (sync->has_custom_sync) {
        sync->sync_ops.lock(sync->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&sync->lock);
#endif
}

static void mdss_unlock(A840KmdMdssSync *sync)
{
    if (sync->has_custom_sync) {
        sync->sync_ops.unlock(sync->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&sync->lock);
#endif
}

static A840KmdStatus check_sync_init(const A840KmdMdssSync *sync)
{
    if (sync == NULL)
        return A840_KMD_INVALID_ARG;
    if (sync->initialized != A840_MDSS_SYNC_INIT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int64_t sat_sub_u64(uint64_t a, uint64_t b)
{
    if (a >= b) {
        uint64_t d = a - b;
        return (d > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)d;
    } else {
        uint64_t d = b - a;
        return (d >= (uint64_t)INT64_MAX) ? INT64_MIN : -(int64_t)d;
    }
}

static int64_t sat_add_i64(int64_t a, int64_t b)
{
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

static void compute_deadline_locked(
    const A840KmdMdssSync *sync,
    uint64_t now_ns,
    A840MdssDeadlineInfo *out)
{
    uint64_t period = sync->tracked_period_ns;
    uint64_t anchor = sync->last_vsync_ns;
    uint64_t epoch_delta = 0u;
    uint64_t curr_vsync = 0u;

    if (now_ns >= anchor) {
        epoch_delta = (now_ns - anchor) / period;
        uint64_t advance = (epoch_delta + UINT64_C(1)) * period;
        curr_vsync = (UINT64_MAX - anchor < advance) ? UINT64_MAX : (anchor + advance);
    } else {
        /* now_ns precedes anchor: align to anchor itself if within one period,
         * otherwise step backward cleanly.
         */
        uint64_t back_periods = (anchor - now_ns) / period;
        curr_vsync = anchor - back_periods * period;
    }

    uint64_t cutoff = (curr_vsync > sync->commit_lead_time_ns)
                          ? (curr_vsync - sync->commit_lead_time_ns)
                          : UINT64_C(0);
    uint64_t next_vsync = (UINT64_MAX - curr_vsync < period)
                              ? UINT64_MAX
                              : (curr_vsync + period);
    int64_t margin = sat_sub_u64(cutoff, now_ns);

    out->now_ns = now_ns;
    out->current_vsync_ns = curr_vsync;
    out->latch_cutoff_ns = cutoff;
    out->next_vsync_ns = next_vsync;
    out->tracked_period_ns = period;
    out->scanout_margin_ns = margin;
    out->vsync_epoch = sync->vsync_seq + epoch_delta + UINT64_C(1);
    out->past_cutoff = (now_ns > cutoff) ? 1u : 0u;
    out->urgent_for_ring0 = (margin < (int64_t)sync->urgent_ring0_threshold_ns) ? 1u : 0u;
}

static void dispatch_mdss_batch_unlocked(const MdssActionBatch *batch)
{
    if (!batch->has_callbacks)
        return;

    if (batch->emit_boost && batch->callbacks.on_boost_hint != NULL) {
        batch->callbacks.on_boost_hint(batch->callbacks.cookie, &batch->boost_hint);
    }

    if (batch->emit_commit && batch->callbacks.on_commit_planned != NULL) {
        batch->callbacks.on_commit_planned(
            batch->callbacks.cookie,
            batch->commit_frame_id,
            batch->commit_decision,
            batch->commit_target_vsync_ns,
            batch->commit_margin_ns,
            batch->commit_extra_waits);
    }
}

A840KmdStatus a840_mdss_sync_init(
    A840KmdMdssSync *sync,
    const A840MdssSyncConfig *config)
{
    if (sync == NULL)
        return A840_KMD_INVALID_ARG;

    A840MdssSyncConfig cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }

    if (cfg.sync_ops != NULL) {
        if (cfg.sync_ops->lock == NULL || cfg.sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    }

    uint64_t period = cfg.nominal_period_ns ? cfg.nominal_period_ns : A840_MDSS_PERIOD_120HZ_NS;
    if (period < A840_MDSS_MIN_PERIOD_NS || period > A840_MDSS_MAX_PERIOD_NS)
        return A840_KMD_RANGE;

    uint64_t lead = cfg.commit_lead_time_ns ? cfg.commit_lead_time_ns : A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    if (lead >= period / UINT64_C(2))
        return A840_KMD_RANGE;

    uint64_t low_margin = cfg.low_margin_threshold_ns
                              ? cfg.low_margin_threshold_ns
                              : A840_MDSS_DEFAULT_LOW_MARGIN_NS;
    uint64_t comfort_margin = cfg.comfortable_margin_ns
                                  ? cfg.comfortable_margin_ns
                                  : A840_MDSS_DEFAULT_COMFORT_MARGIN_NS;
    if (low_margin > comfort_margin || comfort_margin >= period)
        return A840_KMD_RANGE;

    uint64_t urgent_ring0 = cfg.urgent_ring0_threshold_ns
                                ? cfg.urgent_ring0_threshold_ns
                                : A840_MDSS_DEFAULT_URGENT_RING0_NS;
    uint32_t release_n = cfg.boost_release_frames
                             ? cfg.boost_release_frames
                             : A840_MDSS_DEFAULT_BOOST_RELEASE_N;
    uint32_t alpha_pm = cfg.pll_alpha_permille
                            ? cfg.pll_alpha_permille
                            : A840_MDSS_DEFAULT_PLL_ALPHA_PM;
    if (alpha_pm > 1000u)
        return A840_KMD_RANGE;

    memset(sync, 0, sizeof(*sync));
    sync->nominal_period_ns = period;
    sync->tracked_period_ns = period;
    sync->commit_lead_time_ns = lead;
    sync->low_margin_threshold_ns = low_margin;
    sync->comfortable_margin_ns = comfort_margin;
    sync->urgent_ring0_threshold_ns = urgent_ring0;
    sync->boost_release_frames = release_n;
    sync->pll_alpha_permille = alpha_pm;

    if (cfg.has_initial_vsync) {
        sync->phase_locked = 1u;
        sync->last_vsync_ns = cfg.initial_vsync_ns;
    }

    if (cfg.sync_ops != NULL) {
        sync->has_custom_sync = 1u;
        sync->sync_ops = *cfg.sync_ops;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&sync->lock, NULL) != 0)
            return A840_KMD_IO_ERROR;
#endif
    }

    if (cfg.callbacks != NULL) {
        sync->has_callbacks = 1u;
        sync->callbacks = *cfg.callbacks;
    }

    sync->initialized = A840_MDSS_SYNC_INIT_MAGIC;
    return A840_KMD_OK;
}

void a840_mdss_sync_destroy(A840KmdMdssSync *sync)
{
    if (sync == NULL || sync->initialized != A840_MDSS_SYNC_INIT_MAGIC)
        return;

    sync->initialized = 0u;
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!sync->has_custom_sync)
        (void)pthread_mutex_destroy(&sync->lock);
#endif
    memset(sync, 0, sizeof(*sync));
}

A840KmdStatus a840_mdss_sync_on_vsync(
    A840KmdMdssSync *sync,
    uint64_t vsync_ts_ns,
    uint64_t te_seq)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }

    if (sync->phase_locked && vsync_ts_ns < sync->last_vsync_ns) {
        mdss_unlock(sync);
        return A840_KMD_RANGE;
    }

    if (sync->phase_locked) {
        uint64_t delta = vsync_ts_ns - sync->last_vsync_ns;
        uint64_t nom = sync->nominal_period_ns;
        uint64_t max_drift = (nom * A840_MDSS_PLL_MAX_DRIFT_PM) / UINT64_C(1000);
        uint64_t min_valid = nom - max_drift;
        uint64_t max_valid = nom + max_drift;

        /* Single-period measurement inside +/-5% sanity band updates PLL EMA. */
        if (delta >= min_valid && delta <= max_valid) {
            uint64_t alpha = (uint64_t)sync->pll_alpha_permille;
            uint64_t updated = ((UINT64_C(1000) - alpha) * sync->tracked_period_ns +
                                alpha * delta) / UINT64_C(1000);
            if (updated >= min_valid && updated <= max_valid) {
                if (updated != sync->tracked_period_ns)
                    ++sync->pll_drift_adjustments;
                sync->tracked_period_ns = updated;
            }
        }
    }

    sync->phase_locked = 1u;
    sync->last_vsync_ns = vsync_ts_ns;
    sync->vsync_seq = (te_seq > sync->vsync_seq) ? te_seq : (sync->vsync_seq + UINT64_C(1));
    ++sync->vsync_events_recorded;

    mdss_unlock(sync);
    return A840_KMD_OK;
}

A840KmdStatus a840_mdss_sync_query_deadline(
    A840KmdMdssSync *sync,
    uint64_t now_ns,
    A840MdssDeadlineInfo *out_info)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;
    if (out_info == NULL)
        return A840_KMD_INVALID_ARG;

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }
    if (!sync->phase_locked) {
        mdss_unlock(sync);
        return A840_KMD_STATE_CONFLICT;
    }

    memset(out_info, 0, sizeof(*out_info));
    compute_deadline_locked(sync, now_ns, out_info);

    mdss_unlock(sync);
    return A840_KMD_OK;
}

A840KmdStatus a840_mdss_sync_plan_commit(
    A840KmdMdssSync *sync,
    const A840MdssCommitRequest *req,
    A840MdssCommitPlan *out_plan)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;
    if (req == NULL || out_plan == NULL || req->reserved != 0u)
        return A840_KMD_INVALID_ARG;

    MdssActionBatch batch;
    memset(&batch, 0, sizeof(batch));

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }
    if (!sync->phase_locked) {
        mdss_unlock(sync);
        return A840_KMD_STATE_CONFLICT;
    }

    batch.has_callbacks = sync->has_callbacks;
    if (sync->has_callbacks)
        batch.callbacks = sync->callbacks;

    A840MdssDeadlineInfo dl;
    compute_deadline_locked(sync, req->now_ns, &dl);

    /* Effective GPU readiness timestamp:
     * - Retired ready -> ready immediately at now_ns.
     * - Early WriterPromise -> hardware fence sync allows latching directly on
     *   current VSYNC whenever estimated completion precedes latch_cutoff_ns.
     * - Otherwise -> max(now_ns, estimated_gpu_done_ns).
     */
    uint64_t effective_ready = req->now_ns;
    if (!req->is_retired_ready && req->estimated_gpu_done_ns > req->now_ns) {
        effective_ready = req->estimated_gpu_done_ns;
    }

    int64_t margin_vs_curr = sat_sub_u64(dl.latch_cutoff_ns, effective_ready);
    uint64_t period = sync->tracked_period_ns;

    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->frame_id = req->frame_id;
    out_plan->effective_ready_ns = effective_ready;
    out_plan->scanout_margin_ns = margin_vs_curr;

    if (req->now_ns <= dl.latch_cutoff_ns && effective_ready <= dl.latch_cutoff_ns) {
        /* Timely hit: commit directly to current VSYNC with ZERO extra waits! */
        out_plan->decision = A840_MDSS_COMMIT_HIT_CURRENT_VSYNC;
        out_plan->extra_vsync_waits = 0u;
        out_plan->target_vsync_ns = dl.current_vsync_ns;
        out_plan->latch_cutoff_ns = dl.latch_cutoff_ns;
        out_plan->target_vsync_margin_ns = margin_vs_curr;
        out_plan->urgent_for_ring0 =
            (margin_vs_curr < (int64_t)sync->urgent_ring0_threshold_ns) ? 1u : 0u;

        ++sync->frames_on_time;
        ++sync->extra_vsync_waits_avoided;
        sync->sum_on_time_margin_ns = sat_add_i64(sync->sum_on_time_margin_ns, margin_vs_curr);
        sync->consecutive_missed_frames = 0u;

        if (margin_vs_curr < (int64_t)sync->low_margin_threshold_ns) {
            /* Narrow scanout margin (< 1.5 ms): emit immediate GMU DCVS boost hint. */
            sync->boost_pressure_active = 1u;
            sync->consecutive_comfortable_frames = 0u;

            A840MdssBoostHint hint;
            memset(&hint, 0, sizeof(hint));
            hint.frame_id = req->frame_id;
            hint.now_ns = req->now_ns;
            hint.scanout_margin_ns = margin_vs_curr;
            hint.target_vsync_ns = dl.current_vsync_ns;
            hint.severity = A840_MDSS_BOOST_LOW_MARGIN;
            hint.consecutive_missed = 0u;
            hint.recommended_step_up = 2u;
            hint.release_pressure = 0u;

            out_plan->boost_hint_emitted = 1u;
            out_plan->boost_hint = hint;
            batch.emit_boost = 1u;
            batch.boost_hint = hint;
            ++sync->dcvs_boost_hints_sent;
        } else if (margin_vs_curr >= (int64_t)sync->comfortable_margin_ns) {
            ++sync->consecutive_comfortable_frames;
            if (sync->boost_pressure_active &&
                sync->consecutive_comfortable_frames >= sync->boost_release_frames) {
                /* N consecutive comfortable frames -> smoothly release urgent DCVS boost. */
                sync->boost_pressure_active = 0u;

                A840MdssBoostHint hint;
                memset(&hint, 0, sizeof(hint));
                hint.frame_id = req->frame_id;
                hint.now_ns = req->now_ns;
                hint.scanout_margin_ns = margin_vs_curr;
                hint.target_vsync_ns = dl.current_vsync_ns;
                hint.severity = A840_MDSS_BOOST_NONE;
                hint.consecutive_missed = 0u;
                hint.recommended_step_up = 0u;
                hint.release_pressure = 1u;

                out_plan->boost_hint_emitted = 1u;
                out_plan->boost_hint = hint;
                batch.emit_boost = 1u;
                batch.boost_hint = hint;
                ++sync->dcvs_boost_releases_sent;
            }
        }
    } else {
        /* Missed current latch cutoff: defer cleanly to earliest VSYNC whose cutoff
         * accommodates max(now_ns, effective_ready) — zero tearing, zero silent drops.
         */
        uint64_t barrier_ns = (effective_ready > req->now_ns) ? effective_ready : req->now_ns;
        uint64_t overshoot = (barrier_ns > dl.latch_cutoff_ns)
                                 ? (barrier_ns - dl.latch_cutoff_ns)
                                 : UINT64_C(1);
        uint64_t defer_steps = (overshoot + period - UINT64_C(1)) / period;
        if (defer_steps == 0u)
            defer_steps = 1u;

        uint64_t advance = defer_steps * period;
        uint64_t chosen_vsync = (UINT64_MAX - dl.current_vsync_ns < advance)
                                    ? UINT64_MAX
                                    : (dl.current_vsync_ns + advance);
        uint64_t chosen_cutoff = (chosen_vsync > sync->commit_lead_time_ns)
                                     ? (chosen_vsync - sync->commit_lead_time_ns)
                                     : UINT64_C(0);
        int64_t next_margin = sat_sub_u64(chosen_cutoff, barrier_ns);

        out_plan->decision = A840_MDSS_COMMIT_DEFER_NEXT_VSYNC;
        out_plan->extra_vsync_waits = (defer_steps > UINT32_MAX) ? UINT32_MAX : (uint32_t)defer_steps;
        out_plan->target_vsync_ns = chosen_vsync;
        out_plan->latch_cutoff_ns = chosen_cutoff;
        out_plan->target_vsync_margin_ns = next_margin;
        out_plan->urgent_for_ring0 = 1u;

        ++sync->frames_missed_vsync;
        ++sync->missed_cutoff_count;
        sync->total_extra_vsync_waits += defer_steps;
        ++sync->consecutive_missed_frames;
        sync->consecutive_comfortable_frames = 0u;
        sync->boost_pressure_active = 1u;

        A840MdssBoostHint hint;
        memset(&hint, 0, sizeof(hint));
        hint.frame_id = req->frame_id;
        hint.now_ns = req->now_ns;
        hint.scanout_margin_ns = margin_vs_curr;
        hint.target_vsync_ns = chosen_vsync;
        hint.consecutive_missed = sync->consecutive_missed_frames;
        if (sync->consecutive_missed_frames >= 2u) {
            hint.severity = A840_MDSS_BOOST_CONSECUTIVE_MISS;
            hint.recommended_step_up = 5u;
        } else {
            hint.severity = A840_MDSS_BOOST_MISSED_VSYNC;
            hint.recommended_step_up = 3u;
        }
        hint.release_pressure = 0u;

        out_plan->boost_hint_emitted = 1u;
        out_plan->boost_hint = hint;
        batch.emit_boost = 1u;
        batch.boost_hint = hint;
        ++sync->dcvs_boost_hints_sent;
    }

    ++sync->frames_planned;
    sync->last_planned_vsync_ns = out_plan->target_vsync_ns;
    sync->sum_scanout_margin_ns = sat_add_i64(sync->sum_scanout_margin_ns, margin_vs_curr);
    if (!sync->has_margin_sample) {
        sync->has_margin_sample = 1u;
        sync->min_scanout_margin_ns = margin_vs_curr;
        sync->max_scanout_margin_ns = margin_vs_curr;
    } else {
        if (margin_vs_curr < sync->min_scanout_margin_ns)
            sync->min_scanout_margin_ns = margin_vs_curr;
        if (margin_vs_curr > sync->max_scanout_margin_ns)
            sync->max_scanout_margin_ns = margin_vs_curr;
    }

    batch.emit_commit = 1u;
    batch.commit_frame_id = req->frame_id;
    batch.commit_decision = out_plan->decision;
    batch.commit_extra_waits = out_plan->extra_vsync_waits;
    batch.commit_target_vsync_ns = out_plan->target_vsync_ns;
    batch.commit_margin_ns = margin_vs_curr;

    mdss_unlock(sync);
    dispatch_mdss_batch_unlocked(&batch);
    return A840_KMD_OK;
}

A840KmdStatus a840_mdss_sync_set_thresholds(
    A840KmdMdssSync *sync,
    uint64_t commit_lead_time_ns,
    uint64_t low_margin_threshold_ns,
    uint64_t comfortable_margin_ns)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }

    uint64_t lead = commit_lead_time_ns ? commit_lead_time_ns : sync->commit_lead_time_ns;
    uint64_t low_m = low_margin_threshold_ns ? low_margin_threshold_ns : sync->low_margin_threshold_ns;
    uint64_t comf_m = comfortable_margin_ns ? comfortable_margin_ns : sync->comfortable_margin_ns;

    if (lead >= sync->nominal_period_ns / UINT64_C(2) ||
        low_m > comf_m ||
        comf_m >= sync->nominal_period_ns) {
        mdss_unlock(sync);
        return A840_KMD_RANGE;
    }

    sync->commit_lead_time_ns = lead;
    sync->low_margin_threshold_ns = low_m;
    sync->comfortable_margin_ns = comf_m;

    mdss_unlock(sync);
    return A840_KMD_OK;
}

A840KmdStatus a840_mdss_sync_reset_stats(A840KmdMdssSync *sync)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }

    sync->frames_planned = 0u;
    sync->frames_on_time = 0u;
    sync->frames_missed_vsync = 0u;
    sync->missed_cutoff_count = 0u;
    sync->extra_vsync_waits_avoided = 0u;
    sync->total_extra_vsync_waits = 0u;
    sync->dcvs_boost_hints_sent = 0u;
    sync->dcvs_boost_releases_sent = 0u;
    sync->pll_drift_adjustments = 0u;
    sync->consecutive_missed_frames = 0u;
    sync->consecutive_comfortable_frames = 0u;
    sync->boost_pressure_active = 0u;
    sync->has_margin_sample = 0u;
    sync->min_scanout_margin_ns = 0;
    sync->max_scanout_margin_ns = 0;
    sync->sum_scanout_margin_ns = 0;
    sync->sum_on_time_margin_ns = 0;

    mdss_unlock(sync);
    return A840_KMD_OK;
}

A840KmdStatus a840_mdss_sync_get_stats(
    A840KmdMdssSync *sync,
    A840MdssSyncStats *out_stats)
{
    A840KmdStatus st = check_sync_init(sync);
    if (st != A840_KMD_OK)
        return st;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    mdss_lock(sync);
    st = check_sync_init(sync);
    if (st != A840_KMD_OK) {
        mdss_unlock(sync);
        return st;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->frames_planned = sync->frames_planned;
    out_stats->frames_on_time = sync->frames_on_time;
    out_stats->frames_missed_vsync = sync->frames_missed_vsync;
    out_stats->missed_cutoff_count = sync->missed_cutoff_count;
    out_stats->extra_vsync_waits_avoided = sync->extra_vsync_waits_avoided;
    out_stats->total_extra_vsync_waits = sync->total_extra_vsync_waits;
    out_stats->dcvs_boost_hints_sent = sync->dcvs_boost_hints_sent;
    out_stats->dcvs_boost_releases_sent = sync->dcvs_boost_releases_sent;
    out_stats->vsync_events_recorded = sync->vsync_events_recorded;
    out_stats->pll_drift_adjustments = sync->pll_drift_adjustments;
    out_stats->tracked_period_ns = sync->tracked_period_ns;
    out_stats->nominal_period_ns = sync->nominal_period_ns;
    out_stats->last_vsync_ns = sync->last_vsync_ns;
    out_stats->vsync_seq = sync->vsync_seq;
    out_stats->min_scanout_margin_ns = sync->min_scanout_margin_ns;
    out_stats->max_scanout_margin_ns = sync->max_scanout_margin_ns;
    out_stats->avg_scanout_margin_ns = sync->frames_planned
                                           ? (sync->sum_scanout_margin_ns / (int64_t)sync->frames_planned)
                                           : 0;
    out_stats->avg_on_time_margin_ns = sync->frames_on_time
                                           ? (sync->sum_on_time_margin_ns / (int64_t)sync->frames_on_time)
                                           : 0;
    out_stats->consecutive_missed_frames = sync->consecutive_missed_frames;
    out_stats->consecutive_comfortable_frames = sync->consecutive_comfortable_frames;
    out_stats->boost_pressure_active = sync->boost_pressure_active;
    out_stats->phase_locked = sync->phase_locked;

    mdss_unlock(sync);
    return A840_KMD_OK;
}
