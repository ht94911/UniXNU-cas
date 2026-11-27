/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_v3_freeze.h"

#include <stdatomic.h>
#include <string.h>

#define A840_V3_FREEZE_INIT_MAGIC 1446196826u /* 'V3FZ' decimal; avoids hex heuristic */
#define A840_MHZ_TO_HZ            UINT64_C(1000000)

/* Process-wide lock-free audit counter for rejected legacy Turnip selectors/flags. */
static _Atomic uint64_t g_legacy_rejected_count = 0u;

static void freeze_lock(A840KmdV3FreezePolicy *policy)
{
    if (policy->has_custom_sync) {
        policy->sync_ops.lock(policy->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&policy->lock);
#endif
}

static void freeze_unlock(A840KmdV3FreezePolicy *policy)
{
    if (policy->has_custom_sync) {
        policy->sync_ops.unlock(policy->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&policy->lock);
#endif
}

static int policy_is_ready(const A840KmdV3FreezePolicy *policy)
{
    return (policy != NULL && policy->initialized == A840_V3_FREEZE_INIT_MAGIC) ? 1 : 0;
}

static int str_exact_match(const char *s, const char *expected)
{
    if (s == NULL || expected == NULL)
        return 0;
    return (strcmp(s, expected) == 0) ? 1 : 0;
}

static int range_valid_canonical(uint64_t va, uint64_t bytes)
{
    if (va == 0u || bytes == 0u)
        return 0;
    if (va > A840_PREEMPT_CANONICAL_VA_MAX)
        return 0;
    if ((bytes - UINT64_C(1)) > (A840_PREEMPT_CANONICAL_VA_MAX - va))
        return 0;
    return 1;
}

static int ranges_overlap(uint64_t a_start, uint64_t a_bytes,
                          uint64_t b_start, uint64_t b_bytes)
{
    uint64_t a_end = a_start + (a_bytes - UINT64_C(1));
    uint64_t b_end = b_start + (b_bytes - UINT64_C(1));
    return (a_start <= b_end && b_start <= a_end) ? 1 : 0;
}

void a840_kmd_v3_get_abi_freeze_info(A840KmdV3AbiFreezeInfo *out_info)
{
    if (out_info == NULL)
        return;
    memset(out_info, 0, sizeof(*out_info));
    out_info->abi_version = NativeAdrenoAbiVersionV3;
    out_info->envelope_bytes = NativeAdrenoAbiEnvelopeBytes;
    out_info->page_bytes = A840_KMD_V3_FREEZE_PAGE_BYTES;
    out_info->zero_page_bytes = A840_KMD_V3_FREEZE_ZERO_PAGE_BYTES;
    out_info->prod_selector_min = A840_KMD_V3_PROD_SELECTOR_MIN;
    out_info->prod_selector_max = A840_KMD_V3_PROD_SELECTOR_MAX;
    out_info->gpu_opp_count = A840_GPU_OPP_COUNT;
    out_info->gmu_opp_count = A840_GMU_OPP_COUNT;
    out_info->ring_count = A840_RING_COUNT;
    out_info->ubwc_version = A840_KMD_V3_FREEZE_UBWC_VERSION;
    out_info->highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    out_info->gmem_bytes = A840_KAANAPALI_GMEM_BYTES;
    out_info->chip_id = A840_KAANAPALI_CHIP_ID;
    out_info->prod_cap_mask = A840_KMD_V3_PROD_CAP_MASK;
    out_info->required_prod_caps = A840_KMD_V3_REQUIRED_PROD_CAPS;
}

int a840_kmd_v3_selector_is_production(uint32_t selector)
{
    if (selector >= A840_KMD_V3_PROD_SELECTOR_MIN &&
        selector <= A840_KMD_V3_PROD_SELECTOR_MAX) {
        return 1;
    }
    return 0;
}

A840KmdStatus a840_kmd_v3_validate_production_selector(uint32_t selector)
{
    if (a840_kmd_v3_selector_is_production(selector))
        return A840_KMD_OK;
    (void)atomic_fetch_add_explicit(&g_legacy_rejected_count, UINT64_C(1),
                                    memory_order_relaxed);
    return A840_KMD_UNSUPPORTED;
}

A840KmdStatus a840_kmd_v3_reject_legacy_turnip_flags(uint64_t flags)
{
    if ((flags & ~A840_KMD_V3_PROD_CAP_MASK) != UINT64_C(0)) {
        (void)atomic_fetch_add_explicit(&g_legacy_rejected_count, UINT64_C(1),
                                        memory_order_relaxed);
        return A840_KMD_UNSUPPORTED;
    }
    return A840_KMD_OK;
}

uint64_t a840_kmd_v3_legacy_rejected_count(void)
{
    return atomic_load_explicit(&g_legacy_rejected_count, memory_order_relaxed);
}

void a840_kmd_v3_reset_legacy_rejected_count(void)
{
    atomic_store_explicit(&g_legacy_rejected_count, UINT64_C(0), memory_order_relaxed);
}

A840KmdStatus a840_kmd_v3_validate_preempt_records(
    const A840KmdPreemptRecordDesc *records,
    size_t count)
{
    size_t i;
    size_t j;

    if (records == NULL)
        return A840_KMD_INVALID_ARG;
    if (count != (size_t)A840_RING_COUNT)
        return A840_KMD_RANGE;

    for (i = 0u; i < count; ++i) {
        uint64_t va = records[i].record_va;
        uint64_t bytes = records[i].record_bytes;

        if (bytes < (uint64_t)A840_PREEMPT_RECORD_MIN_BYTES)
            return A840_KMD_RANGE;
        if ((va % (uint64_t)A840_PREEMPT_RECORD_ALIGN_BYTES) != UINT64_C(0) ||
            (bytes % (uint64_t)A840_PREEMPT_RECORD_ALIGN_BYTES) != UINT64_C(0))
            return A840_KMD_ALIGNMENT;
        if (!range_valid_canonical(va, bytes))
            return A840_KMD_RANGE;
    }

    for (i = 0u; i < count; ++i) {
        for (j = i + 1u; j < count; ++j) {
            if (ranges_overlap(records[i].record_va, records[i].record_bytes,
                               records[j].record_va, records[j].record_bytes)) {
                return A840_KMD_STATE_CONFLICT;
            }
        }
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_v3_validate_opp_and_thermal(
    const A840GpuOppEntry *gpu_table,
    size_t gpu_count,
    const A840GmuOppEntry *gmu_table,
    size_t gmu_count,
    uint32_t thermal_ladder_active,
    uint32_t thermal_tier_count,
    int32_t  thermal_min_throttle_mc,
    uint32_t thermal_cap_75c_mhz,
    uint32_t thermal_cap_95c_mhz)
{
    size_t i;

    if (gpu_table == NULL || gmu_table == NULL)
        return A840_KMD_INVALID_ARG;
    if (gpu_count != (size_t)A840_GPU_OPP_COUNT ||
        gmu_count != (size_t)A840_GMU_OPP_COUNT)
        return A840_KMD_RANGE;

    /* Validate 4-level GMU OPP table (475..750 MHz, strictly ascending). */
    for (i = 0u; i < gmu_count; ++i) {
        if (gmu_table[i].level_index != (uint32_t)i)
            return A840_KMD_INVALID_ARG;
        if (gmu_table[i].freq_mhz == 0u || gmu_table[i].hfi_voltage_level == 0u)
            return A840_KMD_INVALID_ARG;
        if (gmu_table[i].freq_hz != (uint64_t)gmu_table[i].freq_mhz * A840_MHZ_TO_HZ)
            return A840_KMD_INVALID_ARG;
        if (i > 0u) {
            if (gmu_table[i].freq_mhz <= gmu_table[i - 1u].freq_mhz ||
                gmu_table[i].freq_hz <= gmu_table[i - 1u].freq_hz ||
                gmu_table[i].hfi_voltage_level < gmu_table[i - 1u].hfi_voltage_level) {
                return A840_KMD_STATE_CONFLICT;
            }
        }
    }
    if (gmu_table[0].freq_mhz != A840_KMD_V3_FREEZE_GMU_MIN_MHZ ||
        gmu_table[gmu_count - 1u].freq_mhz != A840_KMD_V3_FREEZE_GMU_MAX_MHZ) {
        return A840_KMD_RANGE;
    }

    /* Validate 18-level GPU OPP table (160..1200 MHz, strictly ascending). */
    for (i = 0u; i < gpu_count; ++i) {
        if (gpu_table[i].level_index != (uint32_t)i)
            return A840_KMD_INVALID_ARG;
        if (gpu_table[i].freq_mhz == 0u || gpu_table[i].hfi_voltage_level == 0u)
            return A840_KMD_INVALID_ARG;
        if (gpu_table[i].freq_hz != (uint64_t)gpu_table[i].freq_mhz * A840_MHZ_TO_HZ)
            return A840_KMD_INVALID_ARG;
        if (gpu_table[i].min_gmu_level >= (uint32_t)gmu_count)
            return A840_KMD_RANGE;
        if (i > 0u) {
            if (gpu_table[i].freq_mhz <= gpu_table[i - 1u].freq_mhz ||
                gpu_table[i].freq_hz <= gpu_table[i - 1u].freq_hz ||
                gpu_table[i].hfi_voltage_level < gpu_table[i - 1u].hfi_voltage_level ||
                gpu_table[i].min_gmu_level < gpu_table[i - 1u].min_gmu_level) {
                return A840_KMD_STATE_CONFLICT;
            }
        }
    }
    if (gpu_table[0].freq_mhz != A840_KMD_V3_FREEZE_GPU_MIN_MHZ ||
        gpu_table[gpu_count - 1u].freq_mhz != A840_KMD_V3_FREEZE_GPU_MAX_MHZ) {
        return A840_KMD_RANGE;
    }

    /* Validate 75 C+ 5-tier thermal throttle ladder is active. */
    if (thermal_ladder_active != 1u ||
        thermal_tier_count != A840_KMD_V3_FREEZE_THERMAL_TIERS ||
        thermal_min_throttle_mc != A840_DCVS_THERMAL_TIER0_MC ||
        thermal_cap_75c_mhz != A840_KMD_V3_FREEZE_THERMAL_CAP_75C_MHZ ||
        thermal_cap_95c_mhz != A840_KMD_V3_FREEZE_THERMAL_CAP_95C_MHZ) {
        return A840_KMD_STATE_CONFLICT;
    }

    return A840_KMD_OK;
}

static int check_zero_page_ready(const A840KmdProdQualInput *in)
{
    if (in->zero_page_ready != 1u || in->zero_page_readonly != 1u)
        return 0;
    if (in->zero_page_bytes != A840_KMD_ZERO_PAGE_BYTES)
        return 0;
    if (in->zero_page_gpu_va == UINT64_C(0) ||
        (in->zero_page_gpu_va % A840_KMD_ZERO_PAGE_ALIGN) != UINT64_C(0))
        return 0;
    if (in->zero_page_gpu_va >= A840_KMD_GPU_VA_LIMIT ||
        in->zero_page_bytes > (A840_KMD_GPU_VA_LIMIT - in->zero_page_gpu_va))
        return 0;
    if (in->zero_page_phys_addr == UINT64_C(0) ||
        (in->zero_page_phys_addr % A840_KMD_ZERO_PAGE_ALIGN) != UINT64_C(0))
        return 0;
    return 1;
}

static int check_backboardd_ca_policy(const A840KmdProdQualInput *in)
{
    if (in->ca_native_metal_enabled != 1u ||
        in->ca_pack_verified != 1u ||
        in->ca_policy_enforced != 1u)
        return 0;
    if (!str_exact_match(in->ca_target_label, A840_KMD_CA_TARGET_BACKBOARDD))
        return 0;
    if (!str_exact_match(in->ca_env_entry, A840_KMD_CA_ENV_ENTRY))
        return 0;
    return 1;
}

static int check_zero_legacy_turnip(const A840KmdProdQualInput *in)
{
    int clean = 1;
    size_t i;

    if (in->legacy_turnip_shim_flags != UINT64_C(0)) {
        (void)atomic_fetch_add_explicit(&g_legacy_rejected_count, UINT64_C(1),
                                        memory_order_relaxed);
        clean = 0;
    }

    if (a840_kmd_v3_reject_legacy_turnip_flags(in->advertised_cap_flags) != A840_KMD_OK)
        clean = 0;

    if (in->legacy_turnip_paths_present != 0u) {
        (void)atomic_fetch_add_explicit(&g_legacy_rejected_count, UINT64_C(1),
                                        memory_order_relaxed);
        clean = 0;
    }

    if (in->selector_audit_count > 0u) {
        if (in->selector_audit_list == NULL) {
            (void)atomic_fetch_add_explicit(&g_legacy_rejected_count, UINT64_C(1),
                                            memory_order_relaxed);
            clean = 0;
        } else {
            for (i = 0u; i < in->selector_audit_count; ++i) {
                if (a840_kmd_v3_validate_production_selector(in->selector_audit_list[i]) != A840_KMD_OK)
                    clean = 0;
            }
        }
    }

    return clean;
}

A840KmdStatus a840_kmd_v3_qualify_production(
    const A840KmdProdQualInput *in,
    A840KmdProdQualReport *out)
{
    uint32_t passed = 0u;
    uint32_t failed = 0u;
    int abi_ok;
    int chip_ok;
    int zp_ok;
    int ca_ok;
    int preempt_ok;
    int opp_ok;
    int turnip_clean;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->failed_mask = A840_KMD_QUAL_ALL_MASK;
        out->legacy_rejected_count = a840_kmd_v3_legacy_rejected_count();
    }

    if (in == NULL || out == NULL)
        return A840_KMD_INVALID_ARG;

    if (in->reserved != 0u || in->reserved_ca != 0u ||
        in->reserved_preempt != 0u || in->reserved_thermal != 0u) {
        out->abi_version_observed = in->abi_version;
        return A840_KMD_INVALID_ARG;
    }

    /* Check 1: Frozen ABI v3 version, envelope size, and required production caps. */
    abi_ok = (in->abi_version == NativeAdrenoAbiVersionV3 &&
              in->envelope_bytes == NativeAdrenoAbiEnvelopeBytes &&
              (in->advertised_cap_flags & A840_KMD_V3_REQUIRED_PROD_CAPS) ==
                  A840_KMD_V3_REQUIRED_PROD_CAPS &&
              (in->advertised_cap_flags & ~A840_KMD_V3_PROD_CAP_MASK) == UINT64_C(0))
             ? 1 : 0;

    /* Check 2: Kaanapali Adreno 840 chip identity (chip_id, 18 MiB GMEM, HBB 16, UBWC 6, 4 KiB SMMU). */
    chip_ok = (in->chip_id == A840_KAANAPALI_CHIP_ID &&
               in->gmem_bytes == A840_KAANAPALI_GMEM_BYTES &&
               in->highest_bank_bit == A840_KAANAPALI_HIGHEST_BANK_BIT &&
               in->ubwc_version == A840_KMD_V3_FREEZE_UBWC_VERSION &&
               in->smmu_page_shift == A840_KMD_V3_FREEZE_SMMU_PAGE_SHIFT)
              ? 1 : 0;

    /* Check 3: Shared 64 KiB read-only zero page ready. */
    zp_ok = check_zero_page_ready(in);

    /* Check 4: backboardd CA_DISABLE_GENERIC_SHADERS=1 policy enforced. */
    ca_ok = check_backboardd_ca_policy(in);

    /* Check 5: 4-Ring preemption records 4 KiB aligned, >= 64 KiB, non-overlapping. */
    preempt_ok = (in->preempt_ring_count == A840_RING_COUNT &&
                  a840_kmd_v3_validate_preempt_records(
                      in->preempt_records, (size_t)in->preempt_ring_count) == A840_KMD_OK)
                 ? 1 : 0;

    /* Check 6: 18-level GPU (160..1200 MHz) & 4-level GMU (475..750 MHz) + 75 C+ thermal ladder. */
    opp_ok = (a840_kmd_v3_validate_opp_and_thermal(
                  in->gpu_opp_table, in->gpu_opp_count,
                  in->gmu_opp_table, in->gmu_opp_count,
                  in->thermal_ladder_active, in->thermal_tier_count,
                  in->thermal_min_throttle_mc,
                  in->thermal_cap_75c_mhz, in->thermal_cap_95c_mhz) == A840_KMD_OK)
             ? 1 : 0;

    /* Check 7: Zero legacy Turnip compat flags, dead paths, or deprecated selectors. */
    turnip_clean = check_zero_legacy_turnip(in);

    if (abi_ok) passed |= A840_KMD_QUAL_ABI_V3_FROZEN;
    else        failed |= A840_KMD_QUAL_ABI_V3_FROZEN;

    if (chip_ok) passed |= A840_KMD_QUAL_CHIP_IDENTITY;
    else         failed |= A840_KMD_QUAL_CHIP_IDENTITY;

    if (zp_ok) passed |= A840_KMD_QUAL_ZERO_PAGE_READY;
    else       failed |= A840_KMD_QUAL_ZERO_PAGE_READY;

    if (ca_ok) passed |= A840_KMD_QUAL_BACKBOARDD_CA_POLICY;
    else       failed |= A840_KMD_QUAL_BACKBOARDD_CA_POLICY;

    if (preempt_ok) passed |= A840_KMD_QUAL_PREEMPT_4RINGS;
    else            failed |= A840_KMD_QUAL_PREEMPT_4RINGS;

    if (opp_ok) passed |= A840_KMD_QUAL_OPP_TABLES_AND_THERMAL;
    else        failed |= A840_KMD_QUAL_OPP_TABLES_AND_THERMAL;

    if (turnip_clean) passed |= A840_KMD_QUAL_ZERO_LEGACY_TURNIP;
    else              failed |= A840_KMD_QUAL_ZERO_LEGACY_TURNIP;

    out->passed_mask = passed;
    out->failed_mask = failed;
    out->abi_version_frozen = (uint32_t)abi_ok;
    out->turnip_paths_purged = (uint32_t)turnip_clean;
    out->abi_version_observed = in->abi_version;
    out->legacy_rejected_count = a840_kmd_v3_legacy_rejected_count();

    if (failed == 0u && passed == A840_KMD_QUAL_ALL_MASK) {
        out->qualified = 1u;
        return A840_KMD_OK;
    }

    out->qualified = 0u;
    if ((failed & A840_KMD_QUAL_ZERO_LEGACY_TURNIP) != 0u)
        return A840_KMD_UNSUPPORTED;
    return A840_KMD_STATE_CONFLICT;
}

A840KmdStatus a840_kmd_v3_freeze_policy_init(
    A840KmdV3FreezePolicy *policy,
    const A840KmdFreezeSyncOps *sync_ops)
{
    if (policy == NULL)
        return A840_KMD_INVALID_ARG;

    if (sync_ops != NULL) {
        if ((sync_ops->lock == NULL) != (sync_ops->unlock == NULL))
            return A840_KMD_INVALID_ARG;
    }

    memset(policy, 0, sizeof(*policy));
    if (sync_ops != NULL && sync_ops->lock != NULL) {
        policy->sync_ops = *sync_ops;
        policy->has_custom_sync = 1u;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&policy->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#else
        return A840_KMD_INVALID_ARG;
#endif
    }

    policy->initialized = A840_V3_FREEZE_INIT_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_v3_freeze_policy_destroy(A840KmdV3FreezePolicy *policy)
{
    if (!policy_is_ready(policy))
        return;

    policy->initialized = 0u;
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!policy->has_custom_sync)
        (void)pthread_mutex_destroy(&policy->lock);
#endif
    memset(policy, 0, sizeof(*policy));
}

A840KmdStatus a840_kmd_v3_freeze_policy_check_selector(
    A840KmdV3FreezePolicy *policy,
    uint32_t selector)
{
    A840KmdStatus st;

    if (!policy_is_ready(policy))
        return A840_KMD_INVALID_ARG;

    st = a840_kmd_v3_validate_production_selector(selector);

    freeze_lock(policy);
    if (st == A840_KMD_OK) {
        policy->selector_checks_passed++;
    } else {
        policy->legacy_selector_rejects++;
        policy->legacy_rejected_count++;
    }
    freeze_unlock(policy);
    return st;
}

A840KmdStatus a840_kmd_v3_freeze_policy_check_flags(
    A840KmdV3FreezePolicy *policy,
    uint64_t flags)
{
    A840KmdStatus st;

    if (!policy_is_ready(policy))
        return A840_KMD_INVALID_ARG;

    st = a840_kmd_v3_reject_legacy_turnip_flags(flags);

    freeze_lock(policy);
    if (st == A840_KMD_OK) {
        policy->flag_checks_passed++;
    } else {
        policy->legacy_flag_rejects++;
        policy->legacy_rejected_count++;
    }
    freeze_unlock(policy);
    return st;
}

A840KmdStatus a840_kmd_v3_freeze_policy_qualify(
    A840KmdV3FreezePolicy *policy,
    const A840KmdProdQualInput *in,
    A840KmdProdQualReport *out)
{
    A840KmdStatus st;

    if (!policy_is_ready(policy))
        return A840_KMD_INVALID_ARG;

    st = a840_kmd_v3_qualify_production(in, out);

    freeze_lock(policy);
    if (out != NULL) {
        policy->last_passed_mask = out->passed_mask;
        policy->last_failed_mask = out->failed_mask;
    }
    if (st == A840_KMD_OK) {
        policy->qualify_pass_count++;
    } else {
        policy->qualify_fail_count++;
        if (out != NULL && (out->failed_mask & A840_KMD_QUAL_ZERO_LEGACY_TURNIP) != 0u)
            policy->legacy_rejected_count++;
    }
    freeze_unlock(policy);
    return st;
}

A840KmdStatus a840_kmd_v3_freeze_policy_get_stats(
    A840KmdV3FreezePolicy *policy,
    A840KmdV3FreezeStats *out_stats)
{
    if (!policy_is_ready(policy) || out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    freeze_lock(policy);
    out_stats->legacy_rejected_count = policy->legacy_rejected_count;
    out_stats->legacy_selector_rejects = policy->legacy_selector_rejects;
    out_stats->legacy_flag_rejects = policy->legacy_flag_rejects;
    out_stats->selector_checks_passed = policy->selector_checks_passed;
    out_stats->flag_checks_passed = policy->flag_checks_passed;
    out_stats->qualify_pass_count = policy->qualify_pass_count;
    out_stats->qualify_fail_count = policy->qualify_fail_count;
    out_stats->last_passed_mask = policy->last_passed_mask;
    out_stats->last_failed_mask = policy->last_failed_mask;
    freeze_unlock(policy);
    return A840_KMD_OK;
}
