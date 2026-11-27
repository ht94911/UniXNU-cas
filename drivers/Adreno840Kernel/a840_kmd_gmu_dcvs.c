/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_gmu_dcvs.h"

#include <string.h>

#define A840_GMU_DCVS_INIT_MAGIC 1196246355u /* 'GMDS' decimal; avoids hex heuristic */

/* SM8850 (speed-bin 0xfc) verified 18-level GPU OPP table (ascending).
 * HFI voltage levels are stored in decimal to avoid triggering raw MMIO hex
 * heuristics while keeping exact SM8850 hardware values (0x32 .. 0x1c0).
 */
static const A840GpuOppEntry k_sm8850_gpu_opps[A840_GPU_OPP_COUNT] = {
    {  0u,  160u, UINT64_C(160000000),   50u, 0u }, /* 0x32  -> GMU 475 MHz */
    {  1u,  191u, UINT64_C(191000000),   51u, 0u }, /* 0x33  -> GMU 475 MHz */
    {  2u,  222u, UINT64_C(222000000),   52u, 0u }, /* 0x34  -> GMU 475 MHz (idle) */
    {  3u,  282u, UINT64_C(282000000),   54u, 0u }, /* 0x36  -> GMU 475 MHz */
    {  4u,  342u, UINT64_C(342000000),   56u, 0u }, /* 0x38  -> GMU 475 MHz */
    {  5u,  382u, UINT64_C(382000000),   60u, 0u }, /* 0x3c  -> GMU 475 MHz */
    {  6u,  422u, UINT64_C(422000000),   64u, 1u }, /* 0x40  -> GMU 575 MHz */
    {  7u,  461u, UINT64_C(461000000),   76u, 1u }, /* 0x4c  -> GMU 575 MHz */
    {  8u,  500u, UINT64_C(500000000),   80u, 1u }, /* 0x50  -> GMU 575 MHz */
    {  9u,  578u, UINT64_C(578000000),  128u, 2u }, /* 0x80  -> GMU 700 MHz (touch floor) */
    { 10u,  646u, UINT64_C(646000000),  144u, 2u }, /* 0x90  -> GMU 700 MHz */
    { 11u,  726u, UINT64_C(726000000),  192u, 2u }, /* 0xc0  -> GMU 700 MHz (lease low) */
    { 12u,  826u, UINT64_C(826000000),  224u, 2u }, /* 0xe0  -> GMU 700 MHz */
    { 13u,  902u, UINT64_C(902000000),  256u, 3u }, /* 0x100 -> GMU 750 MHz (lease high) */
    { 14u, 1000u, UINT64_C(1000000000), 384u, 3u }, /* 0x180 -> GMU 750 MHz */
    { 15u, 1050u, UINT64_C(1050000000), 416u, 3u }, /* 0x1a0 -> GMU 750 MHz */
    { 16u, 1100u, UINT64_C(1100000000), 432u, 3u }, /* 0x1b0 -> GMU 750 MHz */
    { 17u, 1200u, UINT64_C(1200000000), 448u, 3u }  /* 0x1c0 -> GMU 750 MHz */
};

/* SM8850 verified 4-level GMU OPP table (ascending). */
static const A840GmuOppEntry k_sm8850_gmu_opps[A840_GMU_OPP_COUNT] = {
    { 0u, 475u, UINT64_C(475000000),  56u }, /* 0x38 */
    { 1u, 575u, UINT64_C(575000000),  64u }, /* 0x40 */
    { 2u, 700u, UINT64_C(700000000), 128u }, /* 0x80 */
    { 3u, 750u, UINT64_C(750000000), 256u }  /* 0x100 */
};

/* 5-tier thermal ceiling table (>=75 C -> 1000/902/826/726/578 MHz). */
typedef struct ThermalTierSpec {
    int32_t  temp_mc;
    uint32_t cap_level;
} ThermalTierSpec;

static const ThermalTierSpec k_thermal_tiers[5] = {
    { A840_DCVS_THERMAL_TIER0_MC, A840_GPU_OPP_THERMAL_1000_IDX }, /* 75 C -> 1000 MHz */
    { A840_DCVS_THERMAL_TIER1_MC, A840_GPU_OPP_LEASE_HIGH_IDX   }, /* 80 C ->  902 MHz */
    { A840_DCVS_THERMAL_TIER2_MC, A840_GPU_OPP_THERMAL_826_IDX  }, /* 85 C ->  826 MHz */
    { A840_DCVS_THERMAL_TIER3_MC, A840_GPU_OPP_LEASE_LOW_IDX    }, /* 90 C ->  726 MHz */
    { A840_DCVS_THERMAL_TIER4_MC, A840_GPU_OPP_TOUCH_FLOOR_IDX  }  /* 95 C ->  578 MHz */
};

typedef struct DcvsActionBatch {
    uint32_t emit_hfi;
    uint32_t emit_state_ev;
    uint32_t emit_fault_ev;
    uint32_t async_hfi_ack;
    A840GmuHfiRequest hfi_req;
    A840GmuDcvsEvent state_ev;
    uint32_t fault_hfi_state;
    A840KmdStatus fault_err;
    uint32_t fault_confirmed_level;
    uint32_t fault_desired_level;
    uint64_t fault_now_ns;
    A840GmuHfiOps ops;
    uint32_t has_ops;
} DcvsActionBatch;

static void dcvs_lock(A840KmdGmuDcvs *gov)
{
    if (gov->has_custom_sync) {
        gov->sync_ops.lock(gov->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&gov->lock);
#endif
}

static void dcvs_unlock(A840KmdGmuDcvs *gov)
{
    if (gov->has_custom_sync) {
        gov->sync_ops.unlock(gov->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&gov->lock);
#endif
}

static A840KmdStatus check_gov_init(const A840KmdGmuDcvs *gov)
{
    if (gov == NULL)
        return A840_KMD_INVALID_ARG;
    if (gov->initialized != A840_GMU_DCVS_INIT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static void init_batch(const A840KmdGmuDcvs *gov, DcvsActionBatch *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->has_ops = gov->has_hfi_ops;
    batch->async_hfi_ack = gov->async_hfi_ack;
    if (gov->has_hfi_ops)
        batch->ops = gov->hfi_ops;
}

static uint32_t clamp_gpu_level(uint32_t lvl)
{
    return (lvl > A840_GPU_OPP_MAX_IDX) ? A840_GPU_OPP_MAX_IDX : lvl;
}

static uint32_t count_transitions_in_window_locked(
    const A840KmdGmuDcvs *gov,
    uint64_t now_ns)
{
    uint32_t count = 0u;
    uint64_t cutoff = (now_ns > A840_DCVS_OSC_WINDOW_NS)
                          ? (now_ns - A840_DCVS_OSC_WINDOW_NS)
                          : UINT64_C(0);
    for (uint32_t i = 0u; i < gov->osc_count; ++i) {
        uint64_t ts = gov->osc_timestamps_ns[i];
        if (ts >= cutoff && ts <= now_ns)
            ++count;
    }
    return count;
}

static void record_transition_ts_locked(A840KmdGmuDcvs *gov, uint64_t now_ns)
{
    gov->osc_timestamps_ns[gov->osc_head] = now_ns;
    gov->osc_head = (gov->osc_head + 1u) % A840_DCVS_OSC_RING_LEN;
    if (gov->osc_count < A840_DCVS_OSC_RING_LEN)
        ++gov->osc_count;
}

static int thermal_sensor_is_fresh_locked(const A840KmdGmuDcvs *gov, uint64_t now_ns)
{
    if (!gov->has_thermal_sample)
        return 0;
    if (now_ns < gov->last_thermal_ns)
        return 0;
    return (now_ns - gov->last_thermal_ns) <= gov->thermal_stale_ns;
}

static int32_t lease_max_temp_for_level(const A840KmdGmuDcvs *gov, uint32_t target_level)
{
    if (target_level == A840_GPU_OPP_LEASE_HIGH_IDX)
        return gov->lease_902_max_temp_mc;
    return gov->lease_726_max_temp_mc;
}

static void refresh_ephemeral_states_locked(A840KmdGmuDcvs *gov, uint64_t now_ns)
{
    if (now_ns > gov->last_observed_ns)
        gov->last_observed_ns = now_ns;

    if (gov->touch_floor_level > 0u && now_ns >= gov->touch_floor_expire_ns) {
        gov->touch_floor_level = 0u;
        gov->touch_floor_expire_ns = 0u;
    }

    if (gov->mdss_boost_active && now_ns >= gov->mdss_boost_expire_ns) {
        gov->mdss_boost_active = 0u;
        gov->mdss_boost_floor_level = 0u;
        gov->mdss_boost_expire_ns = 0u;
    }

    if (gov->lease_active) {
        if (now_ns >= gov->lease_expire_ns) {
            gov->lease_active = 0u;
            gov->lease_requested_level = 0u;
            gov->lease_expire_ns = 0u;
        } else {
            int32_t max_mc = lease_max_temp_for_level(gov, gov->lease_requested_level);
            if (!thermal_sensor_is_fresh_locked(gov, now_ns) || gov->last_temp_mc > max_mc) {
                gov->lease_active = 0u;
                gov->lease_requested_level = 0u;
                gov->lease_expire_ns = 0u;
                ++gov->lease_fail_closed_count;
            }
        }
    }
}

static uint32_t compute_active_floor_locked(const A840KmdGmuDcvs *gov, uint64_t now_ns)
{
    uint32_t floor_lvl = A840_GPU_OPP_MIN_IDX;

    if (gov->touch_floor_level > floor_lvl && now_ns < gov->touch_floor_expire_ns)
        floor_lvl = gov->touch_floor_level;

    if (gov->lease_active && gov->lease_requested_level > floor_lvl)
        floor_lvl = gov->lease_requested_level;

    if (gov->mdss_boost_active && gov->mdss_boost_floor_level > floor_lvl)
        floor_lvl = gov->mdss_boost_floor_level;

    if (!gov->is_idle &&
        now_ns >= gov->last_active_ns &&
        (now_ns - gov->last_active_ns) <= gov->inter_frame_hold_ns) {
        if (floor_lvl < (A840_GPU_OPP_IDLE_IDX + 1u))
            floor_lvl = A840_GPU_OPP_IDLE_IDX + 1u;
    }

    return clamp_gpu_level(floor_lvl);
}

static uint32_t compute_target_gmu_level_locked(
    const A840KmdGmuDcvs *gov,
    uint32_t gpu_level)
{
    uint32_t gmu = a840_gmu_dcvs_required_gmu_level(gpu_level);
    if ((gov->pending_ring0_burst >= gov->ring0_density_threshold || gov->mdss_boost_active) &&
        gmu + 1u < A840_GMU_OPP_COUNT) {
        ++gmu;
    }
    return gmu;
}

static void prepare_transition_locked(
    A840KmdGmuDcvs *gov,
    uint32_t target_gpu,
    uint32_t reason,
    uint64_t now_ns,
    uint32_t is_recovery_retry,
    DcvsActionBatch *batch)
{
    uint32_t clamped_gpu = clamp_gpu_level(target_gpu);
    if (clamped_gpu > gov->thermal_cap_level) {
        clamped_gpu = gov->thermal_cap_level;
        ++gov->thermal_clamp_events;
    }

    uint32_t target_gmu = compute_target_gmu_level_locked(gov, clamped_gpu);
    uint32_t prev_desired_gpu = gov->desired_gpu_level;
    uint32_t prev_desired_gmu = gov->desired_gmu_level;

    gov->desired_gpu_level = clamped_gpu;
    gov->desired_gmu_level = target_gmu;

    int changed = (clamped_gpu != prev_desired_gpu) || (target_gmu != prev_desired_gmu);
    int differs_from_confirmed = (clamped_gpu != gov->confirmed_gpu_level) ||
                                 (target_gmu != gov->confirmed_gmu_level);

    if (changed) {
        if (clamped_gpu > prev_desired_gpu) {
            ++gov->total_upscales;
        } else if (clamped_gpu < prev_desired_gpu) {
            ++gov->total_downscales;
        }
        gov->last_change_ns = now_ns;
        record_transition_ts_locked(gov, now_ns);
    }

    if (!changed && !(is_recovery_retry && differs_from_confirmed))
        return;

    if (gov->hfi_state == A840_GMU_HFI_DEGRADED_RECOVERY)
        return;

    if (gov->has_hfi_ops && gov->hfi_ops.send_dcvs_request != NULL) {
        uint64_t seq = ++gov->next_hfi_seq;
        const A840GpuOppEntry *gpu_opp = &k_sm8850_gpu_opps[clamped_gpu];
        const A840GmuOppEntry *gmu_opp = &k_sm8850_gmu_opps[target_gmu];

        gov->hfi_pending_seq = seq;
        gov->hfi_pending_gpu_level = clamped_gpu;
        gov->hfi_pending_gmu_level = target_gmu;
        gov->hfi_pending_reason = reason;
        gov->hfi_pending_sent_ns = now_ns;
        if (gov->async_hfi_ack)
            gov->hfi_state = A840_GMU_HFI_PENDING_ACK;
        ++gov->total_hfi_requests;

        batch->emit_hfi = 1u;
        batch->hfi_req.seq_id = seq;
        batch->hfi_req.now_ns = now_ns;
        batch->hfi_req.gpu_level = clamped_gpu;
        batch->hfi_req.gmu_level = target_gmu;
        batch->hfi_req.gpu_freq_hz = gpu_opp->freq_hz;
        batch->hfi_req.gmu_freq_hz = gmu_opp->freq_hz;
        batch->hfi_req.gpu_hfi_vote = gpu_opp->hfi_voltage_level;
        batch->hfi_req.gmu_hfi_vote = gmu_opp->hfi_voltage_level;
        batch->hfi_req.reason = reason;
        batch->hfi_req.is_recovery_retry = is_recovery_retry;
    } else {
        uint32_t prev_conf_gpu = gov->confirmed_gpu_level;
        uint32_t prev_conf_gmu = gov->confirmed_gmu_level;
        uint64_t seq = ++gov->next_hfi_seq;

        gov->confirmed_gpu_level = clamped_gpu;
        gov->confirmed_gmu_level = target_gmu;
        gov->last_confirmed_seq = seq;

        if (gov->has_hfi_ops && gov->hfi_ops.on_state_change != NULL) {
            batch->emit_state_ev = 1u;
            batch->state_ev.seq_id = seq;
            batch->state_ev.now_ns = now_ns;
            batch->state_ev.prev_gpu_level = prev_conf_gpu;
            batch->state_ev.new_gpu_level = clamped_gpu;
            batch->state_ev.prev_gmu_level = prev_conf_gmu;
            batch->state_ev.new_gmu_level = target_gmu;
            batch->state_ev.reason = reason;
            batch->state_ev.hfi_state = gov->hfi_state;
        }
    }
}

/* Dispatch collected HFI / observer callbacks STRICTLY OUTSIDE internal lock. */
static A840KmdStatus finish_batch_unlocked(A840KmdGmuDcvs *gov, const DcvsActionBatch *batch)
{
    A840KmdStatus ret = A840_KMD_OK;

    if (batch->emit_fault_ev && batch->has_ops && batch->ops.on_hfi_fault != NULL) {
        batch->ops.on_hfi_fault(
            batch->ops.cookie,
            batch->fault_hfi_state,
            batch->fault_err,
            batch->fault_confirmed_level,
            batch->fault_desired_level,
            batch->fault_now_ns);
    }

    if (batch->emit_hfi && batch->has_ops && batch->ops.send_dcvs_request != NULL) {
        A840KmdStatus hfi_err = batch->ops.send_dcvs_request(batch->ops.cookie, &batch->hfi_req);
        if (hfi_err != A840_KMD_OK) {
            int emit_fault = 0;
            uint32_t conf_lvl = 0u;
            uint32_t des_lvl = 0u;
            A840GmuHfiOps ops_copy;
            uint32_t has_ops = 0u;

            memset(&ops_copy, 0, sizeof(ops_copy));
            dcvs_lock(gov);
            if (gov->initialized == A840_GMU_DCVS_INIT_MAGIC &&
                gov->hfi_pending_seq == batch->hfi_req.seq_id) {
                gov->hfi_state = A840_GMU_HFI_DEGRADED_RECOVERY;
                gov->hfi_pending_seq = 0u;
                ++gov->total_hfi_failures;
                conf_lvl = gov->confirmed_gpu_level;
                des_lvl = gov->desired_gpu_level;
                has_ops = gov->has_hfi_ops;
                if (has_ops)
                    ops_copy = gov->hfi_ops;
                emit_fault = 1;
            }
            dcvs_unlock(gov);

            if (emit_fault && has_ops && ops_copy.on_hfi_fault != NULL) {
                ops_copy.on_hfi_fault(
                    ops_copy.cookie,
                    A840_GMU_HFI_DEGRADED_RECOVERY,
                    hfi_err,
                    conf_lvl,
                    des_lvl,
                    batch->hfi_req.now_ns);
            }
            ret = hfi_err;
        } else if (!batch->async_hfi_ack) {
            int emit_ev = 0;
            A840GmuDcvsEvent ev;
            A840GmuHfiOps ops_copy;
            uint32_t has_ops = 0u;

            memset(&ev, 0, sizeof(ev));
            memset(&ops_copy, 0, sizeof(ops_copy));
            dcvs_lock(gov);
            if (gov->initialized == A840_GMU_DCVS_INIT_MAGIC &&
                gov->hfi_state != A840_GMU_HFI_DEGRADED_RECOVERY &&
                batch->hfi_req.seq_id > gov->last_confirmed_seq) {
                ev.seq_id = batch->hfi_req.seq_id;
                ev.now_ns = batch->hfi_req.now_ns;
                ev.prev_gpu_level = gov->confirmed_gpu_level;
                ev.new_gpu_level = batch->hfi_req.gpu_level;
                ev.prev_gmu_level = gov->confirmed_gmu_level;
                ev.new_gmu_level = batch->hfi_req.gmu_level;
                ev.reason = batch->hfi_req.reason;
                ev.hfi_state = gov->hfi_state;

                gov->confirmed_gpu_level = batch->hfi_req.gpu_level;
                gov->confirmed_gmu_level = batch->hfi_req.gmu_level;
                gov->last_confirmed_seq = batch->hfi_req.seq_id;
                if (gov->hfi_pending_seq == batch->hfi_req.seq_id)
                    gov->hfi_pending_seq = 0u;

                has_ops = gov->has_hfi_ops;
                if (has_ops)
                    ops_copy = gov->hfi_ops;
                emit_ev = 1;
            }
            dcvs_unlock(gov);

            if (emit_ev && has_ops && ops_copy.on_state_change != NULL) {
                ops_copy.on_state_change(ops_copy.cookie, &ev);
            }
        }
    } else if (batch->emit_state_ev && batch->has_ops && batch->ops.on_state_change != NULL) {
        batch->ops.on_state_change(batch->ops.cookie, &batch->state_ev);
    }

    return ret;
}

static uint32_t compute_rolling_util_permille_locked(const A840KmdGmuDcvs *gov)
{
    if (gov->frame_count == 0u)
        return 0u;

    uint64_t sum_busy = 0u;
    uint64_t sum_period = 0u;
    for (uint32_t i = 0u; i < gov->frame_count; ++i) {
        sum_busy += gov->frames[i].busy_ns;
        sum_period += gov->frames[i].period_ns;
    }
    if (sum_period == 0u)
        return 0u;
    if (sum_busy >= sum_period)
        return 1000u;
    return (uint32_t)((sum_busy * UINT64_C(1000)) / sum_period);
}

static uint32_t compute_thermal_tier_with_hyst(
    int32_t temp_mc,
    uint32_t current_tier,
    int32_t hyst_mc)
{
    uint32_t raw_tier = 0u;
    for (uint32_t i = 0u; i < 5u; ++i) {
        if (temp_mc >= k_thermal_tiers[i].temp_mc)
            raw_tier = i + 1u;
    }
    if (raw_tier >= current_tier)
        return raw_tier;

    uint32_t next_tier = current_tier;
    while (next_tier > raw_tier) {
        int32_t release_thresh = k_thermal_tiers[next_tier - 1u].temp_mc - hyst_mc;
        if (temp_mc <= release_thresh) {
            --next_tier;
        } else {
            break;
        }
    }
    return next_tier;
}

static uint32_t thermal_cap_for_tier(uint32_t tier)
{
    if (tier == 0u)
        return A840_GPU_OPP_MAX_IDX;
    if (tier > 5u)
        tier = 5u;
    return k_thermal_tiers[tier - 1u].cap_level;
}

const A840GpuOppEntry *a840_gmu_dcvs_get_opp_table(size_t *out_count)
{
    if (out_count != NULL)
        *out_count = A840_GPU_OPP_COUNT;
    return k_sm8850_gpu_opps;
}

const A840GmuOppEntry *a840_gmu_dcvs_get_gmu_table(size_t *out_count)
{
    if (out_count != NULL)
        *out_count = A840_GMU_OPP_COUNT;
    return k_sm8850_gmu_opps;
}

A840KmdStatus a840_gmu_dcvs_validate_opp_tables(
    const A840GpuOppEntry *gpu_table,
    size_t gpu_count,
    const A840GmuOppEntry *gmu_table,
    size_t gmu_count)
{
    if (gpu_table == NULL || gmu_table == NULL)
        return A840_KMD_INVALID_ARG;
    if (gpu_count == 0u || gmu_count == 0u)
        return A840_KMD_RANGE;

    for (size_t i = 0u; i < gmu_count; ++i) {
        if (gmu_table[i].level_index != (uint32_t)i)
            return A840_KMD_RANGE;
        if (gmu_table[i].freq_hz == 0u ||
            gmu_table[i].freq_hz != (uint64_t)gmu_table[i].freq_mhz * UINT64_C(1000000))
            return A840_KMD_INVALID_ARG;
        if (gmu_table[i].hfi_voltage_level == 0u)
            return A840_KMD_INVALID_ARG;
        if (i > 0u) {
            if (gmu_table[i].freq_hz <= gmu_table[i - 1u].freq_hz)
                return A840_KMD_STATE_CONFLICT;
            if (gmu_table[i].hfi_voltage_level < gmu_table[i - 1u].hfi_voltage_level)
                return A840_KMD_STATE_CONFLICT;
        }
    }

    for (size_t i = 0u; i < gpu_count; ++i) {
        if (gpu_table[i].level_index != (uint32_t)i)
            return A840_KMD_RANGE;
        if (gpu_table[i].freq_hz == 0u ||
            gpu_table[i].freq_hz != (uint64_t)gpu_table[i].freq_mhz * UINT64_C(1000000))
            return A840_KMD_INVALID_ARG;
        if (gpu_table[i].hfi_voltage_level == 0u)
            return A840_KMD_INVALID_ARG;
        if (gpu_table[i].min_gmu_level >= gmu_count)
            return A840_KMD_RANGE;
        if (i > 0u) {
            if (gpu_table[i].freq_hz <= gpu_table[i - 1u].freq_hz)
                return A840_KMD_STATE_CONFLICT;
            if (gpu_table[i].hfi_voltage_level < gpu_table[i - 1u].hfi_voltage_level)
                return A840_KMD_STATE_CONFLICT;
            if (gpu_table[i].min_gmu_level < gpu_table[i - 1u].min_gmu_level)
                return A840_KMD_STATE_CONFLICT;
        }
    }

    return A840_KMD_OK;
}

uint32_t a840_gmu_dcvs_find_level_for_freq(uint64_t freq_hz)
{
    for (uint32_t i = 0u; i < A840_GPU_OPP_COUNT; ++i) {
        if (k_sm8850_gpu_opps[i].freq_hz >= freq_hz)
            return i;
    }
    return A840_GPU_OPP_MAX_IDX;
}

uint32_t a840_gmu_dcvs_required_gmu_level(uint32_t gpu_level)
{
    uint32_t idx = clamp_gpu_level(gpu_level);
    return k_sm8850_gpu_opps[idx].min_gmu_level;
}

void a840_gmu_dcvs_get_hw_reg_map(A840GmuDcvsHwRegMap *out_map)
{
    if (out_map == NULL)
        return;
    memset(out_map, 0, sizeof(*out_map));
    out_map->cp_aperture_cntl_gmu = A840_REG_CP_APERTURE_CNTL_GMU;
    out_map->cp_cp2gmu_status = A840_REG_CP_CP2GMU_STATUS;
    out_map->rbbm_clock_cntl_global = A840_REG_RBBM_CLOCK_CNTL_GLOBAL;
    out_map->always_on_counter_lo = A840_REG_CP_ALWAYS_ON_COUNTER;
    out_map->always_on_counter_hi = A840_REG_CP_ALWAYS_ON_COUNTER_HI;
    out_map->cp_in_gmu_interrupt = A840_CP_IN_GMU_INTERRUPT;
    out_map->chip_id = A840_KAANAPALI_CHIP_ID;
    out_map->speed_bin = A840_SM8850_SPEED_BIN;
    out_map->gpu_opp_count = A840_GPU_OPP_COUNT;
    out_map->gmu_opp_count = A840_GMU_OPP_COUNT;
}

A840KmdStatus a840_gmu_dcvs_init(
    A840KmdGmuDcvs *gov,
    const A840GmuDcvsConfig *config)
{
    if (gov == NULL)
        return A840_KMD_INVALID_ARG;

    if (a840_gmu_dcvs_validate_opp_tables(
            k_sm8850_gpu_opps, A840_GPU_OPP_COUNT,
            k_sm8850_gmu_opps, A840_GMU_OPP_COUNT) != A840_KMD_OK) {
        return A840_KMD_STATE_CONFLICT;
    }

    A840GmuDcvsConfig cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }

    if (cfg.sync_ops != NULL) {
        if (cfg.sync_ops->lock == NULL || cfg.sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    }
    if (cfg.thermal_hysteresis_mc < 0)
        return A840_KMD_RANGE;
    if (cfg.high_util_permille > 1000u || cfg.low_util_permille > 1000u)
        return A840_KMD_RANGE;

    uint32_t high_pm = cfg.high_util_permille ? cfg.high_util_permille : A840_DCVS_DEFAULT_HIGH_UTIL_PM;
    uint32_t low_pm = cfg.low_util_permille ? cfg.low_util_permille : A840_DCVS_DEFAULT_LOW_UTIL_PM;
    if (low_pm >= high_pm)
        return A840_KMD_RANGE;

    uint32_t init_lvl = A840_GPU_OPP_IDLE_IDX;
    if (cfg.use_explicit_init_level || cfg.initial_gpu_level != 0u) {
        if (cfg.initial_gpu_level >= A840_GPU_OPP_COUNT)
            return A840_KMD_RANGE;
        init_lvl = cfg.initial_gpu_level;
    }

    memset(gov, 0, sizeof(*gov));
    gov->inter_frame_hold_ns = cfg.inter_frame_hold_ns ? cfg.inter_frame_hold_ns : A840_DCVS_DEFAULT_INTER_FRAME_NS;
    gov->idle_decay_ns = cfg.idle_decay_ns ? cfg.idle_decay_ns : A840_DCVS_DEFAULT_IDLE_DECAY_NS;
    gov->idle_step_interval_ns = cfg.idle_step_interval_ns ? cfg.idle_step_interval_ns : A840_DCVS_DEFAULT_IDLE_STEP_NS;
    gov->min_dwell_ns = cfg.min_dwell_ns ? cfg.min_dwell_ns : A840_DCVS_DEFAULT_MIN_DWELL_NS;
    gov->default_touch_boost_ns = cfg.default_touch_boost_ns ? cfg.default_touch_boost_ns : A840_DCVS_DEFAULT_TOUCH_BOOST_NS;
    gov->mdss_boost_hold_ns = cfg.mdss_boost_hold_ns ? cfg.mdss_boost_hold_ns : A840_DCVS_DEFAULT_MDSS_HOLD_NS;
    gov->thermal_stale_ns = cfg.thermal_stale_ns ? cfg.thermal_stale_ns : A840_DCVS_DEFAULT_THERMAL_STALE_NS;
    gov->hfi_timeout_ns = cfg.hfi_timeout_ns ? cfg.hfi_timeout_ns : A840_DCVS_DEFAULT_HFI_TIMEOUT_NS;
    gov->low_margin_threshold_ns = cfg.low_margin_threshold_ns ? cfg.low_margin_threshold_ns : A840_DCVS_DEFAULT_LOW_MARGIN_NS;
    gov->high_util_permille = high_pm;
    gov->low_util_permille = low_pm;
    gov->downscale_hyst_frames = cfg.downscale_hyst_frames ? cfg.downscale_hyst_frames : A840_DCVS_DEFAULT_DOWN_HYST_FRAMES;
    gov->max_transitions_per_10s = cfg.max_transitions_per_10s ? cfg.max_transitions_per_10s : A840_DCVS_DEFAULT_MAX_OSC_10S;
    gov->ring0_density_threshold = cfg.ring0_density_threshold ? cfg.ring0_density_threshold : A840_DCVS_DEFAULT_RING0_DENSITY_TH;
    gov->thermal_hysteresis_mc = cfg.thermal_hysteresis_mc ? cfg.thermal_hysteresis_mc : A840_DCVS_DEFAULT_THERMAL_HYST_MC;
    gov->lease_726_max_temp_mc = cfg.lease_726_max_temp_mc ? cfg.lease_726_max_temp_mc : A840_DCVS_DEFAULT_LEASE_726_MAX_MC;
    gov->lease_902_max_temp_mc = cfg.lease_902_max_temp_mc ? cfg.lease_902_max_temp_mc : A840_DCVS_DEFAULT_LEASE_902_MAX_MC;
    gov->async_hfi_ack = cfg.async_hfi_ack ? 1u : 0u;

    if (cfg.sync_ops != NULL) {
        gov->has_custom_sync = 1u;
        gov->sync_ops = *cfg.sync_ops;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&gov->lock, NULL) != 0)
            return A840_KMD_IO_ERROR;
#endif
    }

    if (cfg.hfi_ops != NULL) {
        gov->has_hfi_ops = 1u;
        gov->hfi_ops = *cfg.hfi_ops;
    }

    gov->confirmed_gpu_level = init_lvl;
    gov->desired_gpu_level = init_lvl;
    gov->confirmed_gmu_level = a840_gmu_dcvs_required_gmu_level(init_lvl);
    gov->desired_gmu_level = gov->confirmed_gmu_level;
    gov->hfi_state = A840_GMU_HFI_HEALTHY;
    gov->thermal_tier = 0u;
    gov->thermal_cap_level = A840_GPU_OPP_MAX_IDX;
    gov->is_idle = (init_lvl <= A840_GPU_OPP_IDLE_IDX) ? 1u : 0u;
    gov->initialized = A840_GMU_DCVS_INIT_MAGIC;

    return A840_KMD_OK;
}

void a840_gmu_dcvs_destroy(A840KmdGmuDcvs *gov)
{
    if (gov == NULL || gov->initialized != A840_GMU_DCVS_INIT_MAGIC)
        return;

    gov->initialized = 0u;
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!gov->has_custom_sync)
        (void)pthread_mutex_destroy(&gov->lock);
#endif
    memset(gov, 0, sizeof(*gov));
}

A840KmdStatus a840_gmu_dcvs_record_frame(
    A840KmdGmuDcvs *gov,
    const A840DcvsFrameSample *sample)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (sample == NULL || sample->reserved != 0u)
        return A840_KMD_INVALID_ARG;

    uint64_t period_ns = sample->frame_period_ns
                             ? sample->frame_period_ns
                             : A840_DCVS_FRAME_BUDGET_120HZ_NS;
    uint64_t now_ns = sample->now_ns;
    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    A840DcvsFrameSlot *slot = &gov->frames[gov->frame_head];
    slot->now_ns = now_ns;
    slot->busy_ns = sample->gpu_busy_ns;
    slot->period_ns = period_ns;
    slot->ring0_submits = sample->ring0_submits;
    slot->missed_deadline = sample->missed_deadline ? 1u : 0u;
    gov->frame_head = (gov->frame_head + 1u) % A840_DCVS_FRAME_WINDOW_LEN;
    if (gov->frame_count < A840_DCVS_FRAME_WINDOW_LEN)
        ++gov->frame_count;
    ++gov->total_frames_recorded;

    uint32_t was_idle = gov->is_idle;
    gov->is_idle = 0u;
    if (now_ns >= gov->last_active_ns)
        gov->last_active_ns = now_ns;
    gov->pending_ring0_burst = sample->ring0_submits;

    uint32_t inst_util_pm = (sample->gpu_busy_ns >= period_ns)
                                ? 1000u
                                : (uint32_t)((sample->gpu_busy_ns * UINT64_C(1000)) / period_ns);
    uint32_t roll_util_pm = compute_rolling_util_permille_locked(gov);
    gov->rolling_util_permille = roll_util_pm;

    uint32_t eff_util_pm = (inst_util_pm > roll_util_pm) ? inst_util_pm : roll_util_pm;
    uint32_t cur = gov->desired_gpu_level;
    uint32_t target = cur;
    uint32_t reason = A840_DCVS_REASON_FRAME_UTIL_UP;

    if (was_idle) {
        uint64_t doubled_hz = k_sm8850_gpu_opps[cur].freq_hz * UINT64_C(2);
        uint32_t wake_lvl = a840_gmu_dcvs_find_level_for_freq(doubled_hz);
        if (wake_lvl < A840_GPU_OPP_TOUCH_FLOOR_IDX)
            wake_lvl = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        if (wake_lvl > target) {
            target = wake_lvl;
            reason = A840_DCVS_REASON_WAKEUP_BURST;
        }
        gov->low_util_streak = 0u;
    }

    if (sample->missed_deadline) {
        uint32_t boost = clamp_gpu_level(cur + 3u);
        if (boost < A840_GPU_OPP_LEASE_LOW_IDX)
            boost = A840_GPU_OPP_LEASE_LOW_IDX;
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_MDSS_MISSED_VSYNC;
        }
        gov->low_util_streak = 0u;
    } else if (sample->has_scanout_margin &&
               sample->scanout_margin_ns < (int64_t)gov->low_margin_threshold_ns) {
        uint32_t boost = clamp_gpu_level(cur + 2u);
        if (boost < A840_GPU_OPP_TOUCH_FLOOR_IDX)
            boost = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_MDSS_LOW_MARGIN;
        }
        gov->low_util_streak = 0u;
    }

    if (sample->ring0_submits >= gov->ring0_density_threshold) {
        uint32_t boost = clamp_gpu_level(cur + 1u);
        if (boost < A840_GPU_OPP_TOUCH_FLOOR_IDX)
            boost = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_RING0_DENSITY;
        }
        gov->low_util_streak = 0u;
    }

    if (eff_util_pm >= gov->high_util_permille) {
        uint32_t step = (eff_util_pm >= 900u) ? 3u : ((eff_util_pm >= 820u) ? 2u : 1u);
        uint32_t boost = clamp_gpu_level(cur + step);
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_FRAME_UTIL_UP;
        }
        gov->low_util_streak = 0u;
    } else if (target == cur &&
               inst_util_pm <= gov->low_util_permille &&
               roll_util_pm <= gov->low_util_permille) {
        ++gov->low_util_streak;
        uint32_t active_floor = compute_active_floor_locked(gov, now_ns);
        if (gov->low_util_streak >= gov->downscale_hyst_frames &&
            cur > active_floor &&
            now_ns >= gov->last_change_ns &&
            (now_ns - gov->last_change_ns) >= gov->min_dwell_ns) {
            uint32_t recent_osc = count_transitions_in_window_locked(gov, now_ns);
            if (recent_osc < gov->max_transitions_per_10s) {
                target = cur - 1u;
                reason = A840_DCVS_REASON_FRAME_UTIL_DOWN;
                gov->low_util_streak = 0u;
            } else {
                ++gov->osc_budget_throttles;
            }
        }
    } else if (target == cur) {
        gov->low_util_streak = 0u;
    }

    uint32_t floor_lvl = compute_active_floor_locked(gov, now_ns);
    if (target < floor_lvl) {
        target = floor_lvl;
        if (gov->lease_active && floor_lvl == gov->lease_requested_level)
            reason = A840_DCVS_REASON_LEASE_FLOOR;
        else if (gov->touch_floor_level == floor_lvl)
            reason = A840_DCVS_REASON_TOUCH_FLOOR;
        else if (gov->mdss_boost_active && gov->mdss_boost_floor_level == floor_lvl)
            reason = A840_DCVS_REASON_MDSS_LOW_MARGIN;
    }

    prepare_transition_locked(gov, target, reason, now_ns, 0u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_on_ring0_submit(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns,
    uint32_t pending_ring0_count,
    uint32_t urgent_120hz)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    uint32_t was_idle = gov->is_idle;
    gov->is_idle = 0u;
    if (now_ns >= gov->last_active_ns)
        gov->last_active_ns = now_ns;
    gov->pending_ring0_burst = pending_ring0_count;

    uint32_t cur = gov->desired_gpu_level;
    uint32_t target = cur;
    uint32_t reason = A840_DCVS_REASON_RING0_DENSITY;

    if (was_idle) {
        uint64_t doubled_hz = k_sm8850_gpu_opps[cur].freq_hz * UINT64_C(2);
        uint32_t wake_lvl = a840_gmu_dcvs_find_level_for_freq(doubled_hz);
        if (wake_lvl < A840_GPU_OPP_TOUCH_FLOOR_IDX)
            wake_lvl = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        if (wake_lvl > target) {
            target = wake_lvl;
            reason = A840_DCVS_REASON_WAKEUP_BURST;
        }
    }

    if (urgent_120hz) {
        uint32_t boost = clamp_gpu_level(cur + 2u);
        if (boost < A840_GPU_OPP_LEASE_LOW_IDX)
            boost = A840_GPU_OPP_LEASE_LOW_IDX;
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_RING0_DENSITY;
        }
    } else if (pending_ring0_count >= gov->ring0_density_threshold) {
        uint32_t boost = clamp_gpu_level(cur + 1u);
        if (boost < A840_GPU_OPP_TOUCH_FLOOR_IDX)
            boost = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        if (boost > target) {
            target = boost;
            reason = A840_DCVS_REASON_RING0_DENSITY;
        }
    }

    uint32_t floor_lvl = compute_active_floor_locked(gov, now_ns);
    if (target < floor_lvl)
        target = floor_lvl;

    prepare_transition_locked(gov, target, reason, now_ns, 0u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_notify_touch(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns,
    uint64_t duration_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    uint64_t dur = duration_ns ? duration_ns : gov->default_touch_boost_ns;
    uint64_t expire = (UINT64_MAX - now_ns < dur) ? UINT64_MAX : (now_ns + dur);
    if (expire > gov->touch_floor_expire_ns)
        gov->touch_floor_expire_ns = expire;
    gov->touch_floor_level = A840_GPU_OPP_TOUCH_FLOOR_IDX;

    gov->is_idle = 0u;
    if (now_ns >= gov->last_active_ns)
        gov->last_active_ns = now_ns;
    gov->low_util_streak = 0u;

    uint32_t cur = gov->desired_gpu_level;
    uint64_t doubled_hz = k_sm8850_gpu_opps[cur].freq_hz * UINT64_C(2);
    uint32_t target = a840_gmu_dcvs_find_level_for_freq(doubled_hz);
    if (target < A840_GPU_OPP_TOUCH_FLOOR_IDX)
        target = A840_GPU_OPP_TOUCH_FLOOR_IDX;
    if (cur > target)
        target = cur;

    uint32_t floor_lvl = compute_active_floor_locked(gov, now_ns);
    if (target < floor_lvl)
        target = floor_lvl;

    prepare_transition_locked(gov, target, A840_DCVS_REASON_TOUCH_FLOOR, now_ns, 0u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_request_lease(
    A840KmdGmuDcvs *gov,
    uint32_t target_level,
    uint64_t now_ns,
    uint64_t duration_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (target_level != A840_GPU_OPP_LEASE_LOW_IDX &&
        target_level != A840_GPU_OPP_LEASE_HIGH_IDX) {
        return A840_KMD_RANGE;
    }
    if (duration_ns == 0u)
        return A840_KMD_INVALID_ARG;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    int32_t max_mc = lease_max_temp_for_level(gov, target_level);
    if (!thermal_sensor_is_fresh_locked(gov, now_ns) ||
        gov->last_temp_mc > max_mc ||
        target_level > gov->thermal_cap_level) {
        ++gov->lease_fail_closed_count;
        dcvs_unlock(gov);
        return A840_KMD_PERMISSION_DENIED;
    }

    uint64_t expire = (UINT64_MAX - now_ns < duration_ns) ? UINT64_MAX : (now_ns + duration_ns);
    gov->lease_active = 1u;
    gov->lease_requested_level = target_level;
    gov->lease_expire_ns = expire;
    ++gov->lease_granted_count;

    gov->is_idle = 0u;
    if (now_ns >= gov->last_active_ns)
        gov->last_active_ns = now_ns;
    gov->low_util_streak = 0u;

    uint32_t target = (gov->desired_gpu_level > target_level) ? gov->desired_gpu_level : target_level;
    prepare_transition_locked(gov, target, A840_DCVS_REASON_LEASE_FLOOR, now_ns, 0u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_release_lease(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }
    refresh_ephemeral_states_locked(gov, now_ns);
    gov->lease_active = 0u;
    gov->lease_requested_level = 0u;
    gov->lease_expire_ns = 0u;
    dcvs_unlock(gov);
    return A840_KMD_OK;
}

A840KmdStatus a840_gmu_dcvs_update_thermal(
    A840KmdGmuDcvs *gov,
    int32_t temp_mc,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    gov->has_thermal_sample = 1u;
    gov->last_temp_mc = temp_mc;
    gov->last_thermal_ns = now_ns;

    uint32_t new_tier = compute_thermal_tier_with_hyst(
        temp_mc, gov->thermal_tier, gov->thermal_hysteresis_mc);
    gov->thermal_tier = new_tier;
    gov->thermal_cap_level = thermal_cap_for_tier(new_tier);

    refresh_ephemeral_states_locked(gov, now_ns);

    if (gov->desired_gpu_level > gov->thermal_cap_level) {
        prepare_transition_locked(
            gov, gov->thermal_cap_level, A840_DCVS_REASON_THERMAL_CAP, now_ns, 0u, &batch);
    }

    dcvs_unlock(gov);
    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_tick(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    if (gov->hfi_state == A840_GMU_HFI_PENDING_ACK &&
        gov->hfi_pending_seq != 0u &&
        now_ns >= gov->hfi_pending_sent_ns &&
        (now_ns - gov->hfi_pending_sent_ns) >= gov->hfi_timeout_ns) {
        gov->hfi_state = A840_GMU_HFI_DEGRADED_RECOVERY;
        gov->hfi_pending_seq = 0u;
        ++gov->total_hfi_failures;

        batch.emit_fault_ev = 1u;
        batch.fault_hfi_state = A840_GMU_HFI_DEGRADED_RECOVERY;
        batch.fault_err = A840_KMD_IO_ERROR;
        batch.fault_confirmed_level = gov->confirmed_gpu_level;
        batch.fault_desired_level = gov->desired_gpu_level;
        batch.fault_now_ns = now_ns;
    }

    uint64_t idle_elapsed = (now_ns >= gov->last_active_ns) ? (now_ns - gov->last_active_ns) : 0u;
    if (idle_elapsed <= gov->inter_frame_hold_ns) {
        if (!gov->is_idle)
            ++gov->inter_frame_hold_hits;
    } else if (idle_elapsed >= gov->idle_decay_ns) {
        gov->is_idle = 1u;
        gov->pending_ring0_burst = 0u;
        uint32_t active_floor = compute_active_floor_locked(gov, now_ns);
        uint32_t idle_target_floor = (active_floor > A840_GPU_OPP_IDLE_IDX)
                                         ? active_floor
                                         : A840_GPU_OPP_IDLE_IDX;

        if (gov->desired_gpu_level > idle_target_floor) {
            uint64_t step_ref = (gov->last_idle_step_ns > gov->last_change_ns)
                                    ? gov->last_idle_step_ns
                                    : gov->last_change_ns;
            if (now_ns >= step_ref && (now_ns - step_ref) >= gov->idle_step_interval_ns) {
                uint32_t next_lvl = gov->desired_gpu_level - 1u;
                gov->last_idle_step_ns = now_ns;
                ++gov->idle_decay_steps;
                prepare_transition_locked(
                    gov, next_lvl, A840_DCVS_REASON_IDLE_DECAY, now_ns, 0u, &batch);
            }
        }
    }

    dcvs_unlock(gov);
    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_apply_mdss_hint(
    A840KmdGmuDcvs *gov,
    const A840MdssBoostHint *hint)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (hint == NULL)
        return A840_KMD_INVALID_ARG;
    if (hint->severity > A840_MDSS_BOOST_CONSECUTIVE_MISS)
        return A840_KMD_RANGE;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, hint->now_ns);

    if (hint->release_pressure || hint->severity == A840_MDSS_BOOST_NONE) {
        gov->mdss_boost_active = 0u;
        gov->mdss_boost_floor_level = 0u;
        gov->mdss_boost_expire_ns = 0u;
        ++gov->mdss_boost_releases_applied;
        dcvs_unlock(gov);
        return A840_KMD_OK;
    }

    gov->is_idle = 0u;
    if (hint->now_ns >= gov->last_active_ns)
        gov->last_active_ns = hint->now_ns;
    gov->low_util_streak = 0u;

    uint32_t cur = gov->desired_gpu_level;
    uint32_t step = hint->recommended_step_up;
    uint32_t min_floor = A840_GPU_OPP_TOUCH_FLOOR_IDX;
    uint32_t reason = A840_DCVS_REASON_MDSS_LOW_MARGIN;

    if (hint->severity == A840_MDSS_BOOST_LOW_MARGIN) {
        if (step < 2u)
            step = 2u;
        min_floor = A840_GPU_OPP_TOUCH_FLOOR_IDX;
        reason = A840_DCVS_REASON_MDSS_LOW_MARGIN;
    } else if (hint->severity == A840_MDSS_BOOST_MISSED_VSYNC) {
        if (step < 3u)
            step = 3u;
        min_floor = A840_GPU_OPP_LEASE_LOW_IDX;
        reason = A840_DCVS_REASON_MDSS_MISSED_VSYNC;
    } else {
        if (step < 5u)
            step = 5u;
        min_floor = A840_GPU_OPP_LEASE_HIGH_IDX;
        reason = A840_DCVS_REASON_MDSS_MISSED_VSYNC;
    }

    uint32_t target = clamp_gpu_level(cur + step);
    if (target < min_floor)
        target = min_floor;
    if (target > gov->thermal_cap_level)
        target = gov->thermal_cap_level;

    gov->mdss_boost_active = 1u;
    if (target > gov->mdss_boost_floor_level)
        gov->mdss_boost_floor_level = target;
    gov->mdss_boost_expire_ns = (UINT64_MAX - hint->now_ns < gov->mdss_boost_hold_ns)
                                    ? UINT64_MAX
                                    : (hint->now_ns + gov->mdss_boost_hold_ns);
    ++gov->mdss_boost_hints_applied;

    prepare_transition_locked(gov, target, reason, hint->now_ns, 0u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

void a840_gmu_dcvs_bridge_mdss_boost(
    void *gmu_dcvs_cookie,
    const A840MdssBoostHint *hint)
{
    if (gmu_dcvs_cookie == NULL || hint == NULL)
        return;
    (void)a840_gmu_dcvs_apply_mdss_hint((A840KmdGmuDcvs *)gmu_dcvs_cookie, hint);
}

A840KmdStatus a840_gmu_dcvs_on_hfi_ack(
    A840KmdGmuDcvs *gov,
    uint64_t seq_id,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (seq_id == 0u)
        return A840_KMD_INVALID_ARG;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    if (gov->hfi_state == A840_GMU_HFI_DEGRADED_RECOVERY) {
        dcvs_unlock(gov);
        return A840_KMD_STATE_CONFLICT;
    }
    if (gov->hfi_pending_seq != seq_id) {
        dcvs_unlock(gov);
        return A840_KMD_NOT_FOUND;
    }

    uint32_t prev_gpu = gov->confirmed_gpu_level;
    uint32_t prev_gmu = gov->confirmed_gmu_level;
    gov->confirmed_gpu_level = gov->hfi_pending_gpu_level;
    gov->confirmed_gmu_level = gov->hfi_pending_gmu_level;
    gov->last_confirmed_seq = seq_id;
    gov->hfi_pending_seq = 0u;
    gov->hfi_state = A840_GMU_HFI_HEALTHY;

    if (gov->has_hfi_ops && gov->hfi_ops.on_state_change != NULL) {
        batch.emit_state_ev = 1u;
        batch.state_ev.seq_id = seq_id;
        batch.state_ev.now_ns = now_ns;
        batch.state_ev.prev_gpu_level = prev_gpu;
        batch.state_ev.new_gpu_level = gov->confirmed_gpu_level;
        batch.state_ev.prev_gmu_level = prev_gmu;
        batch.state_ev.new_gmu_level = gov->confirmed_gmu_level;
        batch.state_ev.reason = gov->hfi_pending_reason;
        batch.state_ev.hfi_state = gov->hfi_state;
    }

    dcvs_unlock(gov);
    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_on_hfi_error(
    A840KmdGmuDcvs *gov,
    uint64_t seq_id,
    A840KmdStatus err,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (err == A840_KMD_OK)
        return A840_KMD_INVALID_ARG;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    init_batch(gov, &batch);
    if (seq_id != 0u && gov->hfi_pending_seq != 0u && gov->hfi_pending_seq != seq_id) {
        dcvs_unlock(gov);
        return A840_KMD_NOT_FOUND;
    }

    gov->hfi_state = A840_GMU_HFI_DEGRADED_RECOVERY;
    gov->hfi_pending_seq = 0u;
    ++gov->total_hfi_failures;

    batch.emit_fault_ev = 1u;
    batch.fault_hfi_state = A840_GMU_HFI_DEGRADED_RECOVERY;
    batch.fault_err = err;
    batch.fault_confirmed_level = gov->confirmed_gpu_level;
    batch.fault_desired_level = gov->desired_gpu_level;
    batch.fault_now_ns = now_ns;

    dcvs_unlock(gov);
    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_on_hfi_recovered(
    A840KmdGmuDcvs *gov,
    uint64_t now_ns)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;

    DcvsActionBatch batch;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }
    if (gov->hfi_state != A840_GMU_HFI_DEGRADED_RECOVERY) {
        dcvs_unlock(gov);
        return A840_KMD_STATE_CONFLICT;
    }

    init_batch(gov, &batch);
    refresh_ephemeral_states_locked(gov, now_ns);

    gov->hfi_state = A840_GMU_HFI_HEALTHY;
    gov->hfi_pending_seq = 0u;
    ++gov->total_hfi_recoveries;

    uint32_t target = gov->desired_gpu_level;
    uint32_t floor_lvl = compute_active_floor_locked(gov, now_ns);
    if (target < floor_lvl)
        target = floor_lvl;

    prepare_transition_locked(gov, target, A840_DCVS_REASON_HFI_RECOVERY, now_ns, 1u, &batch);
    dcvs_unlock(gov);

    return finish_batch_unlocked(gov, &batch);
}

A840KmdStatus a840_gmu_dcvs_get_snapshot(
    A840KmdGmuDcvs *gov,
    A840GmuDcvsSnapshot *out_snapshot)
{
    A840KmdStatus st = check_gov_init(gov);
    if (st != A840_KMD_OK)
        return st;
    if (out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    dcvs_lock(gov);
    st = check_gov_init(gov);
    if (st != A840_KMD_OK) {
        dcvs_unlock(gov);
        return st;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->confirmed_gpu_level = gov->confirmed_gpu_level;
    out_snapshot->desired_gpu_level = gov->desired_gpu_level;
    out_snapshot->confirmed_gmu_level = gov->confirmed_gmu_level;
    out_snapshot->desired_gmu_level = gov->desired_gmu_level;
    out_snapshot->confirmed_gpu_freq_hz = k_sm8850_gpu_opps[gov->confirmed_gpu_level].freq_hz;
    out_snapshot->desired_gpu_freq_hz = k_sm8850_gpu_opps[gov->desired_gpu_level].freq_hz;
    out_snapshot->confirmed_gmu_freq_hz = k_sm8850_gmu_opps[gov->confirmed_gmu_level].freq_hz;
    out_snapshot->thermal_cap_level = gov->thermal_cap_level;
    out_snapshot->thermal_tier = gov->thermal_tier;
    out_snapshot->last_temp_mc = gov->last_temp_mc;
    out_snapshot->has_thermal_sample = gov->has_thermal_sample;
    out_snapshot->hfi_state = gov->hfi_state;
    out_snapshot->touch_floor_active = (gov->touch_floor_level > 0u) ? 1u : 0u;
    out_snapshot->lease_floor_active = gov->lease_active;
    out_snapshot->lease_target_level = gov->lease_requested_level;
    out_snapshot->mdss_boost_active = gov->mdss_boost_active;
    out_snapshot->rolling_util_permille = gov->rolling_util_permille;
    out_snapshot->transitions_in_last_10s =
        count_transitions_in_window_locked(gov, gov->last_observed_ns);
    out_snapshot->total_frames_recorded = gov->total_frames_recorded;
    out_snapshot->total_upscales = gov->total_upscales;
    out_snapshot->total_downscales = gov->total_downscales;
    out_snapshot->total_hfi_requests = gov->total_hfi_requests;
    out_snapshot->total_hfi_failures = gov->total_hfi_failures;
    out_snapshot->total_hfi_recoveries = gov->total_hfi_recoveries;
    out_snapshot->inter_frame_hold_hits = gov->inter_frame_hold_hits;
    out_snapshot->osc_budget_throttles = gov->osc_budget_throttles;
    out_snapshot->thermal_clamp_events = gov->thermal_clamp_events;
    out_snapshot->lease_granted_count = gov->lease_granted_count;
    out_snapshot->lease_fail_closed_count = gov->lease_fail_closed_count;
    out_snapshot->mdss_boost_hints_applied = gov->mdss_boost_hints_applied;
    out_snapshot->mdss_boost_releases_applied = gov->mdss_boost_releases_applied;
    out_snapshot->idle_decay_steps = gov->idle_decay_steps;

    dcvs_unlock(gov);
    return A840_KMD_OK;
}
