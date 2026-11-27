/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_quota_recovery.h"

#include <string.h>
#include <time.h>

#define A840_KMD_QR_MAGIC 1364415058u
#define A840_KMD_QR_DEFER_CHUNK 16u

typedef struct QrReclaimCandidate {
    size_t slot_index;
    uint64_t slot_gen;
    uint64_t client_id;
    uint64_t bo_handle;
    uint64_t bytes;
    uint32_t is_parked;
    uint32_t victim_priority;
} QrReclaimCandidate;

typedef struct QrFenceEvent {
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t fence;
    A840KmdStatus status;
    uint32_t fault_kind;
    uint64_t recovery_generation;
} QrFenceEvent;

typedef struct QrBoRefEvent {
    uint64_t client_id;
    uint64_t bo_handle;
} QrBoRefEvent;

static void qr_lock(A840KmdQuotaRecovery *qr)
{
    if (qr->has_custom_sync) {
        qr->sync_ops.lock(qr->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&qr->lock);
#endif
}

static void qr_unlock(A840KmdQuotaRecovery *qr)
{
    if (qr->has_custom_sync) {
        qr->sync_ops.unlock(qr->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&qr->lock);
#endif
}

static A840KmdStatus check_init(const A840KmdQuotaRecovery *qr)
{
    if (qr == NULL)
        return A840_KMD_INVALID_ARG;
    if (qr->initialized != A840_KMD_QR_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int valid_priority(uint32_t prio)
{
    return prio < (uint32_t)A840_KMD_PRIO_COUNT;
}

static int valid_fault_kind(uint32_t kind)
{
    return kind >= (uint32_t)A840_KMD_FAULT_GPU_HANG &&
           kind <= (uint32_t)A840_KMD_FAULT_IB_ERROR;
}

static int add_u64_overflow(uint64_t a, uint64_t b, uint64_t *out_sum)
{
    if (b > UINT64_MAX - a)
        return 1;
    *out_sum = a + b;
    return 0;
}

static uint64_t sub_sat_u64(uint64_t a, uint64_t b)
{
    return (a > b) ? (a - b) : 0u;
}

static A840KmdQuotaClientSlot *find_client_locked(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id)
{
    size_t i;
    for (i = 0u; i < A840_KMD_QUOTA_MAX_CLIENTS; ++i) {
        if (qr->clients[i].active && qr->clients[i].client_id == client_id)
            return &qr->clients[i];
    }
    return NULL;
}

static A840KmdTrackedBoSlot *find_bo_locked(
    A840KmdQuotaRecovery *qr,
    uint64_t bo_handle)
{
    size_t i;
    for (i = 0u; i < A840_KMD_QUOTA_MAX_TRACKED_BOS; ++i) {
        if (qr->bos[i].active && qr->bos[i].bo_handle == bo_handle)
            return &qr->bos[i];
    }
    return NULL;
}

static A840KmdChannelSlot *find_channel_locked(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id)
{
    size_t i;
    for (i = 0u; i < A840_KMD_QUOTA_MAX_CHANNELS; ++i) {
        if (qr->channels[i].active &&
            qr->channels[i].client_id == client_id &&
            qr->channels[i].queue_id == queue_id) {
            return &qr->channels[i];
        }
    }
    return NULL;
}

static uint64_t effective_global_hard_locked(
    const A840KmdQuotaRecovery *qr,
    uint32_t priority)
{
    if (priority == (uint32_t)A840_KMD_PRIO_COMPOSITOR)
        return qr->global_hard_limit_bytes;
    return sub_sat_u64(qr->global_hard_limit_bytes, qr->compositor_reserve_bytes);
}

static uint64_t effective_global_soft_locked(
    const A840KmdQuotaRecovery *qr,
    uint32_t priority)
{
    uint64_t eff_hard = effective_global_hard_locked(qr, priority);
    return (qr->global_soft_limit_bytes < eff_hard) ? qr->global_soft_limit_bytes : eff_hard;
}

static void compute_needs_locked(
    const A840KmdQuotaRecovery *qr,
    const A840KmdQuotaClientSlot *client,
    uint64_t requested_bytes,
    uint64_t *out_client_soft_need,
    uint64_t *out_client_hard_need,
    uint64_t *out_global_soft_need,
    uint64_t *out_global_hard_need)
{
    uint64_t client_proj = 0u;
    uint64_t global_proj = 0u;
    uint64_t eff_hard = effective_global_hard_locked(qr, client->priority);
    uint64_t eff_soft = effective_global_soft_locked(qr, client->priority);

    if (add_u64_overflow(client->used_bytes, requested_bytes, &client_proj))
        client_proj = UINT64_MAX;
    if (add_u64_overflow(qr->global_used_bytes, requested_bytes, &global_proj))
        global_proj = UINT64_MAX;

    *out_client_soft_need = sub_sat_u64(client_proj, client->soft_limit_bytes);
    *out_client_hard_need = sub_sat_u64(client_proj, client->hard_limit_bytes);
    *out_global_soft_need = sub_sat_u64(global_proj, eff_soft);
    *out_global_hard_need = sub_sat_u64(global_proj, eff_hard);
}

static int bo_eligible_for_reclaim_locked(
    A840KmdQuotaRecovery *qr,
    const A840KmdTrackedBoSlot *bo,
    uint64_t requester_client_id,
    uint32_t requester_priority,
    int allow_lower_prio,
    int stage_parked,
    uint32_t *out_victim_priority)
{
    A840KmdQuotaClientSlot *owner;

    if (!bo->active || bo->reclaiming)
        return 0;

    if (stage_parked) {
        if (!bo->is_parked)
            return 0;
    } else {
        if (bo->is_parked || bo->purgeable_state != A840_PURGE_VOLATILE)
            return 0;
        if (bo->in_flight_refs > 0u)
            return 0;
    }

    owner = find_client_locked(qr, bo->client_id);
    if (owner == NULL)
        return 0;

    if (owner->client_id == requester_client_id) {
        *out_victim_priority = owner->priority;
        return 1;
    }

    if (!allow_lower_prio)
        return 0;

    /* Strict protection: NEVER reclaim from COMPOSITOR on behalf of another client,
     * and only reclaim from clients strictly lower in priority than requester.
     */
    if (owner->priority == (uint32_t)A840_KMD_PRIO_COMPOSITOR)
        return 0;
    if (owner->priority >= requester_priority)
        return 0;

    *out_victim_priority = owner->priority;
    return 1;
}

static size_t pick_reclaim_batch_locked(
    A840KmdQuotaRecovery *qr,
    uint64_t requester_client_id,
    uint32_t requester_priority,
    uint64_t client_need,
    uint64_t global_need,
    int stage_parked,
    QrReclaimCandidate *out_batch,
    size_t max_batch)
{
    size_t count = 0u;
    uint64_t planned_client = 0u;
    uint64_t planned_global = 0u;

    while (count < max_batch &&
           (planned_client < client_need || planned_global < global_need)) {
        size_t best_idx = A840_KMD_QUOTA_MAX_TRACKED_BOS;
        int best_tier = 999;
        uint64_t best_lru = UINT64_MAX;
        uint32_t best_prio = 0u;
        int need_same_only = (planned_client < client_need) && (planned_global >= global_need);
        int allow_lower = (!need_same_only) && (planned_global < global_need);
        size_t i;

        for (i = 0u; i < A840_KMD_QUOTA_MAX_TRACKED_BOS; ++i) {
            A840KmdTrackedBoSlot *bo = &qr->bos[i];
            uint32_t vprio = 0u;
            int tier;

            if (!bo_eligible_for_reclaim_locked(
                    qr, bo, requester_client_id, requester_priority,
                    allow_lower, stage_parked, &vprio)) {
                continue;
            }

            /* Priority tiering:
             * - If requester is COMPOSITOR with global pressure, sweep lower-priority
             *   clients (BACKGROUND=0, FOREGROUND=1, CAMERA=2) BEFORE COMPOSITOR's own BOs (tier 10).
             * - Otherwise sweep same client first (tier 0) so per-client quota is relieved
             *   before touching lower-priority peers (tier 1 + vprio).
             */
            if (requester_priority == (uint32_t)A840_KMD_PRIO_COMPOSITOR &&
                planned_global < global_need &&
                planned_client == 0u) {
                tier = (bo->client_id == requester_client_id) ? 10 : (int)vprio;
            } else {
                tier = (bo->client_id == requester_client_id) ? 0 : (1 + (int)vprio);
            }

            if (tier < best_tier || (tier == best_tier && bo->lru_seq < best_lru)) {
                best_tier = tier;
                best_lru = bo->lru_seq;
                best_idx = i;
                best_prio = vprio;
            }
        }

        if (best_idx == A840_KMD_QUOTA_MAX_TRACKED_BOS)
            break;

        qr->bos[best_idx].reclaiming = 1u;
        out_batch[count].slot_index = best_idx;
        out_batch[count].slot_gen = qr->bos[best_idx].slot_gen;
        out_batch[count].client_id = qr->bos[best_idx].client_id;
        out_batch[count].bo_handle = qr->bos[best_idx].bo_handle;
        out_batch[count].bytes = qr->bos[best_idx].bytes;
        out_batch[count].is_parked = stage_parked ? 1u : 0u;
        out_batch[count].victim_priority = best_prio;
        count++;

        planned_global += qr->bos[best_idx].bytes;
        if (qr->bos[best_idx].client_id == requester_client_id)
            planned_client += qr->bos[best_idx].bytes;
    }

    return count;
}

static void record_unique_bo(
    uint64_t *list,
    size_t *count,
    size_t cap,
    uint64_t bo_handle)
{
    size_t i;
    if (bo_handle == 0u)
        return;
    for (i = 0u; i < *count; ++i) {
        if (list[i] == bo_handle)
            return;
    }
    if (*count < cap) {
        list[*count] = bo_handle;
        (*count)++;
    }
}

static void decrement_tracked_bo_ref_locked(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle)
{
    A840KmdTrackedBoSlot *bo = find_bo_locked(qr, bo_handle);
    if (bo != NULL && bo->client_id == client_id && bo->in_flight_refs > 0u)
        bo->in_flight_refs--;
}

static void fill_channel_info_locked(
    const A840KmdChannelSlot *ch,
    A840KmdChannelInfo *out_info)
{
    memset(out_info, 0, sizeof(*out_info));
    out_info->client_id = ch->client_id;
    out_info->queue_id = ch->queue_id;
    out_info->state = ch->state;
    out_info->active_inflight_count = ch->active_inflight_count;
    out_info->fault_count = ch->fault_count;
    out_info->recovery_count = ch->recovery_count;
    out_info->last_fault_kind = ch->last_fault_kind;
    out_info->last_fault_code = ch->last_fault_code;
    out_info->recovery_generation = ch->recovery_generation;
    out_info->damaged_fence_start = ch->damaged_fence_start;
    out_info->damaged_fence_end = ch->damaged_fence_end;
    out_info->submitted_count = ch->submitted_count;
    out_info->completed_ok_count = ch->completed_ok_count;
    out_info->completed_err_count = ch->completed_err_count;
    out_info->last_submitted_fence = ch->last_submitted_fence;
    out_info->last_completed_fence = ch->last_completed_fence;
}

/* ---------------- Lifecycle ---------------- */

A840KmdStatus a840_kmd_qr_init(
    A840KmdQuotaRecovery *qr,
    const A840KmdQuotaRecoveryConfig *config)
{
    uint64_t g_hard;
    uint64_t g_soft;
    uint64_t c_reserve;
    uint32_t max_rec;

    if (qr == NULL)
        return A840_KMD_INVALID_ARG;

    if (config != NULL) {
        if (config->reserved != 0u)
            return A840_KMD_INVALID_ARG;
        if (config->sync_ops != NULL) {
            if (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL)
                return A840_KMD_INVALID_ARG;
        }
        g_hard = (config->global_hard_limit_bytes != 0u)
            ? config->global_hard_limit_bytes
            : A840_KMD_QUOTA_DEFAULT_GLOBAL_HARD_BYTES;
        g_soft = (config->global_soft_limit_bytes != 0u)
            ? config->global_soft_limit_bytes
            : (g_hard - (g_hard / 4u));
        if (g_soft > g_hard)
            return A840_KMD_INVALID_ARG;
        if (config->compositor_reserve_bytes != 0u) {
            if (config->compositor_reserve_bytes >= g_hard)
                return A840_KMD_INVALID_ARG;
            c_reserve = config->compositor_reserve_bytes;
        } else {
            uint64_t def_res = A840_KMD_QUOTA_DEFAULT_COMPOSITOR_RESERVE_BYTES;
            c_reserve = (def_res < (g_hard / 2u)) ? def_res : (g_hard / 4u);
        }
        if (config->ban_on_first_fault != 0u) {
            max_rec = config->max_recoveries_before_ban;
        } else {
            max_rec = (config->max_recoveries_before_ban != 0u)
                ? config->max_recoveries_before_ban
                : A840_KMD_RECOVERY_DEFAULT_MAX_RECOVERIES;
        }
    } else {
        g_hard = A840_KMD_QUOTA_DEFAULT_GLOBAL_HARD_BYTES;
        g_soft = A840_KMD_QUOTA_DEFAULT_GLOBAL_SOFT_BYTES;
        c_reserve = A840_KMD_QUOTA_DEFAULT_COMPOSITOR_RESERVE_BYTES;
        max_rec = A840_KMD_RECOVERY_DEFAULT_MAX_RECOVERIES;
    }

#if defined(A840_KMD_CUSTOM_SYNC)
    if (config == NULL || config->sync_ops == NULL)
        return A840_KMD_INVALID_ARG;
#endif

    memset(qr, 0, sizeof(*qr));
    qr->global_hard_limit_bytes = g_hard;
    qr->global_soft_limit_bytes = g_soft;
    qr->compositor_reserve_bytes = c_reserve;
    qr->max_recoveries_before_ban = max_rec;
    qr->next_lru_seq = 1u;
    qr->next_slot_gen = 1u;

    if (config != NULL) {
        qr->sync_bo_pool_inflight = config->sync_bo_pool_inflight ? 1u : 0u;
        qr->bo_pool = config->bo_pool;
        qr->sparse_mgr = config->sparse_mgr;
        qr->fault_capture = config->fault_capture;
        if (config->sync_ops != NULL) {
            qr->sync_ops = *config->sync_ops;
            qr->has_custom_sync = 1u;
        }
        if (config->reclaim_ops != NULL) {
            qr->reclaim_ops = *config->reclaim_ops;
            qr->has_reclaim_ops = 1u;
        }
        if (config->recovery_ops != NULL) {
            qr->recovery_ops = *config->recovery_ops;
            qr->has_recovery_ops = 1u;
        }
    }

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!qr->has_custom_sync) {
        if (pthread_mutex_init(&qr->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
    }
#endif

    qr->stats.global_soft_limit_bytes = g_soft;
    qr->stats.global_hard_limit_bytes = g_hard;
    qr->stats.compositor_reserve_bytes = c_reserve;
    qr->initialized = A840_KMD_QR_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_qr_destroy(A840KmdQuotaRecovery *qr)
{
    uint32_t custom_sync;
    if (check_init(qr) != A840_KMD_OK)
        return;

    qr_lock(qr);
    custom_sync = qr->has_custom_sync;
    qr->initialized = 0u;
    qr_unlock(qr);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!custom_sync)
        (void)pthread_mutex_destroy(&qr->lock);
#else
    (void)custom_sync;
#endif
}

/* ---------------- Client Quota Management ---------------- */

A840KmdStatus a840_kmd_qr_register_client(
    A840KmdQuotaRecovery *qr,
    const A840KmdClientQuotaConfig *client_cfg)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_cfg == NULL || client_cfg->client_id == 0u || client_cfg->reserved != 0u)
        return A840_KMD_INVALID_ARG;
    if (!valid_priority(client_cfg->priority))
        return A840_KMD_INVALID_ARG;
    if (client_cfg->soft_limit_bytes == 0u ||
        client_cfg->hard_limit_bytes < client_cfg->soft_limit_bytes) {
        return A840_KMD_INVALID_ARG;
    }

    qr_lock(qr);
    for (i = 0u; i < A840_KMD_QUOTA_MAX_CLIENTS; ++i) {
        if (qr->clients[i].active) {
            if (qr->clients[i].client_id == client_cfg->client_id) {
                qr_unlock(qr);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &qr->clients[i];
        }
    }
    if (free_slot == NULL) {
        qr_unlock(qr);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->priority = client_cfg->priority;
    free_slot->client_id = client_cfg->client_id;
    free_slot->soft_limit_bytes = client_cfg->soft_limit_bytes;
    free_slot->hard_limit_bytes = client_cfg->hard_limit_bytes;
    qr->stats.active_clients++;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_update_client_quota(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t soft_limit_bytes,
    uint64_t hard_limit_bytes)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || soft_limit_bytes == 0u || hard_limit_bytes < soft_limit_bytes)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    client = find_client_locked(qr, client_id);
    if (client == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    client->soft_limit_bytes = soft_limit_bytes;
    client->hard_limit_bytes = hard_limit_bytes;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_unregister_client(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;
    A840KmdRecoveryOps rec_ops;
    uint32_t has_rec_ops = 0u;
    QrFenceEvent aborted[A840_KMD_CHANNEL_MAX_INFLIGHT];
    size_t aborted_count = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&rec_ops, 0, sizeof(rec_ops));

    qr_lock(qr);
    client = find_client_locked(qr, client_id);
    if (client == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }

    has_rec_ops = qr->has_recovery_ops;
    if (has_rec_ops)
        rec_ops = qr->recovery_ops;

    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, client->used_bytes);
    qr->stats.global_used_bytes = qr->global_used_bytes;

    for (i = 0u; i < A840_KMD_QUOTA_MAX_TRACKED_BOS; ++i) {
        if (qr->bos[i].active && qr->bos[i].client_id == client_id) {
            memset(&qr->bos[i], 0, sizeof(qr->bos[i]));
            if (qr->stats.active_tracked_bos > 0u)
                qr->stats.active_tracked_bos--;
        }
    }

    for (i = 0u; i < A840_KMD_QUOTA_MAX_CHANNELS; ++i) {
        A840KmdChannelSlot *ch = &qr->channels[i];
        size_t s;
        if (!ch->active || ch->client_id != client_id)
            continue;
        for (s = 0u; s < A840_KMD_CHANNEL_MAX_INFLIGHT; ++s) {
            if (ch->inflight[s].active && aborted_count < A840_KMD_CHANNEL_MAX_INFLIGHT) {
                aborted[aborted_count].client_id = client_id;
                aborted[aborted_count].queue_id = ch->queue_id;
                aborted[aborted_count].fence = ch->inflight[s].fence;
                aborted[aborted_count].status = A840_KMD_DISCONNECTED;
                aborted[aborted_count].fault_kind = A840_KMD_FAULT_NONE;
                aborted[aborted_count].recovery_generation = ch->recovery_generation;
                aborted_count++;
            }
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_BANNED && qr->stats.banned_channels > 0u)
            qr->stats.banned_channels--;
        memset(ch, 0, sizeof(*ch));
        if (qr->stats.active_channels > 0u)
            qr->stats.active_channels--;
    }

    memset(client, 0, sizeof(*client));
    if (qr->stats.active_clients > 0u)
        qr->stats.active_clients--;
    qr_unlock(qr);

    if (has_rec_ops && rec_ops.fence_notify != NULL) {
        for (i = 0u; i < aborted_count; ++i) {
            rec_ops.fence_notify(
                rec_ops.cookie,
                aborted[i].client_id,
                aborted[i].queue_id,
                aborted[i].fence,
                aborted[i].status,
                aborted[i].fault_kind,
                aborted[i].recovery_generation);
        }
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_query_client(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    A840KmdClientQuotaInfo *out_info)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_info == NULL)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    client = find_client_locked(qr, client_id);
    if (client == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->client_id = client->client_id;
    out_info->priority = client->priority;
    out_info->active = client->active;
    out_info->soft_limit_bytes = client->soft_limit_bytes;
    out_info->hard_limit_bytes = client->hard_limit_bytes;
    out_info->used_bytes = client->used_bytes;
    out_info->peak_used_bytes = client->peak_used_bytes;
    out_info->parked_bytes = client->parked_bytes;
    out_info->volatile_bytes = client->volatile_bytes;
    out_info->oom_reject_count = client->oom_reject_count;
    out_info->reclaim_count = client->reclaim_count;
    qr_unlock(qr);
    return A840_KMD_OK;
}

/* ---------------- Tracked BO Registry ---------------- */

A840KmdStatus a840_kmd_qr_track_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t bytes,
    uint32_t is_parked,
    A840KmdPurgeableState purgeable_state,
    uint32_t charge_usage)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;
    A840KmdTrackedBoSlot *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u || bytes == 0u)
        return A840_KMD_INVALID_ARG;
    if (purgeable_state != A840_PURGE_NONVOLATILE &&
        purgeable_state != A840_PURGE_VOLATILE &&
        purgeable_state != A840_PURGE_EMPTY) {
        return A840_KMD_INVALID_ARG;
    }
    if (is_parked != 0u && purgeable_state == A840_PURGE_EMPTY)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    client = find_client_locked(qr, client_id);
    if (client == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }

    for (i = 0u; i < A840_KMD_QUOTA_MAX_TRACKED_BOS; ++i) {
        if (qr->bos[i].active) {
            if (qr->bos[i].bo_handle == bo_handle) {
                qr_unlock(qr);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &qr->bos[i];
        }
    }
    if (free_slot == NULL) {
        qr_unlock(qr);
        return A840_KMD_CAPACITY;
    }

    if (charge_usage != 0u && purgeable_state != A840_PURGE_EMPTY) {
        uint64_t new_client_used = 0u;
        uint64_t new_global_used = 0u;
        uint64_t eff_global_hard = effective_global_hard_locked(qr, client->priority);

        if (add_u64_overflow(client->used_bytes, bytes, &new_client_used) ||
            add_u64_overflow(qr->global_used_bytes, bytes, &new_global_used) ||
            new_client_used > client->hard_limit_bytes ||
            new_global_used > eff_global_hard) {
            client->oom_reject_count++;
            qr->stats.total_oom_rejects++;
            qr->stats.oom_rejects_by_prio[client->priority]++;
            qr_unlock(qr);
            return A840_KMD_OOM;
        }
        client->used_bytes = new_client_used;
        if (client->used_bytes > client->peak_used_bytes)
            client->peak_used_bytes = client->used_bytes;
        qr->global_used_bytes = new_global_used;
        qr->stats.global_used_bytes = qr->global_used_bytes;
        if (qr->global_used_bytes > qr->global_peak_used_bytes) {
            qr->global_peak_used_bytes = qr->global_used_bytes;
            qr->stats.global_peak_used_bytes = qr->global_peak_used_bytes;
        }
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->is_parked = is_parked ? 1u : 0u;
    free_slot->purgeable_state = (uint32_t)purgeable_state;
    free_slot->slot_gen = qr->next_slot_gen++;
    free_slot->client_id = client_id;
    free_slot->bo_handle = bo_handle;
    free_slot->bytes = bytes;
    free_slot->lru_seq = qr->next_lru_seq++;

    if (free_slot->is_parked)
        client->parked_bytes += bytes;
    else if (purgeable_state == A840_PURGE_VOLATILE)
        client->volatile_bytes += bytes;

    qr->stats.active_tracked_bos++;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_untrack_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t release_usage)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;
    A840KmdTrackedBoSlot *bo;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    bo = find_bo_locked(qr, bo_handle);
    if (bo == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bo->client_id != client_id) {
        qr_unlock(qr);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (bo->reclaiming || bo->in_flight_refs > 0u) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }

    client = find_client_locked(qr, client_id);
    if (client != NULL) {
        if (bo->is_parked)
            client->parked_bytes = sub_sat_u64(client->parked_bytes, bo->bytes);
        else if (bo->purgeable_state == (uint32_t)A840_PURGE_VOLATILE)
            client->volatile_bytes = sub_sat_u64(client->volatile_bytes, bo->bytes);

        if (release_usage != 0u && bo->purgeable_state != (uint32_t)A840_PURGE_EMPTY) {
            uint64_t deduct = (client->used_bytes < bo->bytes) ? client->used_bytes : bo->bytes;
            client->used_bytes -= deduct;
            qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, deduct);
            qr->stats.global_used_bytes = qr->global_used_bytes;
        }
    }

    memset(bo, 0, sizeof(*bo));
    if (qr->stats.active_tracked_bos > 0u)
        qr->stats.active_tracked_bos--;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_set_bo_parked(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t is_parked)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;
    A840KmdTrackedBoSlot *bo;
    uint32_t want_park = is_parked ? 1u : 0u;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    bo = find_bo_locked(qr, bo_handle);
    if (bo == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bo->client_id != client_id) {
        qr_unlock(qr);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (bo->reclaiming) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }
    if (want_park && (bo->in_flight_refs > 0u || bo->purgeable_state == (uint32_t)A840_PURGE_EMPTY)) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }

    client = find_client_locked(qr, client_id);
    if (bo->is_parked != want_park) {
        if (client != NULL) {
            if (bo->is_parked) {
                client->parked_bytes = sub_sat_u64(client->parked_bytes, bo->bytes);
                if (bo->purgeable_state == (uint32_t)A840_PURGE_VOLATILE)
                    client->volatile_bytes += bo->bytes;
            } else {
                if (bo->purgeable_state == (uint32_t)A840_PURGE_VOLATILE)
                    client->volatile_bytes = sub_sat_u64(client->volatile_bytes, bo->bytes);
                client->parked_bytes += bo->bytes;
            }
        }
        bo->is_parked = want_park;
    }
    bo->lru_seq = qr->next_lru_seq++;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_set_bo_purgeable(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdPurgeableState new_state)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;
    A840KmdTrackedBoSlot *bo;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;
    if (new_state != A840_PURGE_NONVOLATILE &&
        new_state != A840_PURGE_VOLATILE &&
        new_state != A840_PURGE_EMPTY) {
        return A840_KMD_INVALID_ARG;
    }

    qr_lock(qr);
    bo = find_bo_locked(qr, bo_handle);
    if (bo == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bo->client_id != client_id) {
        qr_unlock(qr);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (bo->reclaiming || bo->is_parked) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }
    if (new_state == A840_PURGE_EMPTY && bo->in_flight_refs > 0u) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }

    client = find_client_locked(qr, client_id);
    if (bo->purgeable_state == (uint32_t)A840_PURGE_VOLATILE &&
        new_state != A840_PURGE_VOLATILE && client != NULL) {
        client->volatile_bytes = sub_sat_u64(client->volatile_bytes, bo->bytes);
    }
    if (bo->purgeable_state != (uint32_t)A840_PURGE_VOLATILE &&
        new_state == A840_PURGE_VOLATILE && client != NULL) {
        client->volatile_bytes += bo->bytes;
    }
    if (bo->purgeable_state != (uint32_t)A840_PURGE_EMPTY &&
        new_state == A840_PURGE_EMPTY && client != NULL) {
        uint64_t deduct = (client->used_bytes < bo->bytes) ? client->used_bytes : bo->bytes;
        client->used_bytes -= deduct;
        qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, deduct);
        qr->stats.global_used_bytes = qr->global_used_bytes;
    }

    bo->purgeable_state = (uint32_t)new_state;
    bo->lru_seq = qr->next_lru_seq++;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_bo_begin_submit(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs)
{
    A840KmdStatus status = check_init(qr);
    A840KmdTrackedBoSlot *bo;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    bo = find_bo_locked(qr, bo_handle);
    if (bo == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bo->client_id != client_id) {
        qr_unlock(qr);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (bo->reclaiming || bo->is_parked || bo->purgeable_state == (uint32_t)A840_PURGE_EMPTY) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }
    if (bo->in_flight_refs == UINT32_MAX) {
        qr_unlock(qr);
        return A840_KMD_RANGE;
    }
    bo->in_flight_refs++;
    if (out_in_flight_refs != NULL)
        *out_in_flight_refs = bo->in_flight_refs;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_bo_end_submit(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs)
{
    A840KmdStatus status = check_init(qr);
    A840KmdTrackedBoSlot *bo;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    bo = find_bo_locked(qr, bo_handle);
    if (bo == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bo->client_id != client_id) {
        qr_unlock(qr);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (bo->in_flight_refs == 0u) {
        qr_unlock(qr);
        return A840_KMD_STATE_CONFLICT;
    }
    bo->in_flight_refs--;
    if (out_in_flight_refs != NULL)
        *out_in_flight_refs = bo->in_flight_refs;
    qr_unlock(qr);
    return A840_KMD_OK;
}

/* ---------------- Escalating Quota & OOM Reclamation ---------------- */

A840KmdStatus a840_kmd_qr_request_alloc(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t requested_bytes,
    A840KmdAllocResult *out_result)
{
    A840KmdStatus status = check_init(qr);
    A840KmdAllocResult res;
    A840KmdQuotaReclaimOps reclaim_ops;
    uint32_t has_reclaim_ops = 0u;
    A840KmdBoPool *bo_pool = NULL;
    uint32_t req_priority = 0u;
    int pass;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || requested_bytes == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&res, 0, sizeof(res));
    memset(&reclaim_ops, 0, sizeof(reclaim_ops));
    res.requested_bytes = requested_bytes;

    /* Verify client existence and snapshot callbacks/pool pointer. */
    qr_lock(qr);
    {
        A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
        if (client == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        req_priority = client->priority;
        qr->stats.total_alloc_requests++;
        has_reclaim_ops = qr->has_reclaim_ops;
        if (has_reclaim_ops)
            reclaim_ops = qr->reclaim_ops;
        bo_pool = qr->bo_pool;
    }
    qr_unlock(qr);

    /* Stage 1: Reclaim Parked BOs first (same client, then strictly lower-priority clients). */
    for (pass = 0; pass < 32; ++pass) {
        QrReclaimCandidate batch[A840_KMD_QR_DEFER_CHUNK];
        uint64_t batch_reclaimed[A840_KMD_QR_DEFER_CHUNK];
        A840KmdStatus batch_status[A840_KMD_QR_DEFER_CHUNK];
        size_t batch_count = 0u;
        uint64_t c_soft = 0u, c_hard = 0u, g_soft = 0u, g_hard = 0u;
        size_t i;

        qr_lock(qr);
        {
            A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
            if (client == NULL) {
                qr_unlock(qr);
                return A840_KMD_NOT_FOUND;
            }
            req_priority = client->priority;
            compute_needs_locked(qr, client, requested_bytes, &c_soft, &c_hard, &g_soft, &g_hard);
            if (c_soft == 0u && g_soft == 0u && c_hard == 0u && g_hard == 0u) {
                qr_unlock(qr);
                break;
            }
            batch_count = pick_reclaim_batch_locked(
                qr, client_id, req_priority, c_soft, g_soft, 1, batch, A840_KMD_QR_DEFER_CHUNK);
        }
        qr_unlock(qr);

        if (batch_count == 0u)
            break;

        /* Execute Parked BO reclamation strictly OUTSIDE the lock. */
        for (i = 0u; i < batch_count; ++i) {
            uint64_t reclaimed = batch[i].bytes;
            A840KmdStatus cb_st = A840_KMD_OK;

            if (has_reclaim_ops && reclaim_ops.reclaim_parked_bo != NULL) {
                reclaimed = 0u;
                cb_st = reclaim_ops.reclaim_parked_bo(
                    reclaim_ops.cookie,
                    batch[i].client_id,
                    batch[i].bo_handle,
                    batch[i].bytes,
                    &reclaimed);
            } else if (bo_pool != NULL) {
                A840KmdStatus bp_st = a840_kmd_bo_pool_release(
                    bo_pool, batch[i].client_id, batch[i].bo_handle, 0u);
                if (bp_st == A840_KMD_NOT_FOUND)
                    reclaimed = batch[i].bytes;
                else if (bp_st != A840_KMD_OK) {
                    cb_st = bp_st;
                    reclaimed = 0u;
                }
            }
            batch_status[i] = cb_st;
            batch_reclaimed[i] = (cb_st == A840_KMD_OK) ? reclaimed : 0u;
        }

        /* Commit reclaimed Parked BOs under lock. */
        qr_lock(qr);
        for (i = 0u; i < batch_count; ++i) {
            A840KmdTrackedBoSlot *slot = &qr->bos[batch[i].slot_index];
            if (!slot->active || slot->slot_gen != batch[i].slot_gen)
                continue;
            if (batch_status[i] == A840_KMD_OK && batch_reclaimed[i] > 0u) {
                A840KmdQuotaClientSlot *victim = find_client_locked(qr, slot->client_id);
                uint64_t eff_reclaimed = batch_reclaimed[i];
                if (victim != NULL) {
                    uint64_t deduct = (victim->used_bytes < eff_reclaimed) ? victim->used_bytes : eff_reclaimed;
                    victim->used_bytes -= deduct;
                    victim->parked_bytes = sub_sat_u64(victim->parked_bytes, slot->bytes);
                    victim->reclaim_count++;
                    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, deduct);
                    qr->stats.global_used_bytes = qr->global_used_bytes;
                }
                res.reclaimed_parked_bytes += eff_reclaimed;
                res.parked_reclaimed_count++;
                qr->stats.parked_reclaimed_bytes += eff_reclaimed;
                qr->stats.parked_reclaimed_count++;
                memset(slot, 0, sizeof(*slot));
                if (qr->stats.active_tracked_bos > 0u)
                    qr->stats.active_tracked_bos--;
            } else {
                slot->reclaiming = 0u;
            }
        }
        qr_unlock(qr);
    }

    /* Optional Stage 1b: bulk parked pool sweep callback if still above soft/hard limit. */
    if (has_reclaim_ops && reclaim_ops.sweep_parked_pool != NULL) {
        uint64_t c_soft = 0u, c_hard = 0u, g_soft = 0u, g_hard = 0u;
        uint64_t need = 0u;
        uint32_t max_vprio = (req_priority > 0u) ? (req_priority - 1u) : 0u;

        qr_lock(qr);
        {
            A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
            if (client != NULL && qr->global_used_bytes > 0u) {
                compute_needs_locked(qr, client, requested_bytes, &c_soft, &c_hard, &g_soft, &g_hard);
                need = (c_soft > g_soft) ? c_soft : g_soft;
            }
        }
        qr_unlock(qr);

        if (need > 0u) {
            uint64_t swept = 0u;
            A840KmdStatus sw_st = reclaim_ops.sweep_parked_pool(
                reclaim_ops.cookie, client_id, max_vprio, need, &swept);
            if (sw_st == A840_KMD_OK && swept > 0u) {
                qr_lock(qr);
                {
                    A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
                    if (client != NULL) {
                        uint64_t deduct = (client->used_bytes < swept) ? client->used_bytes : swept;
                        client->used_bytes -= deduct;
                        client->reclaim_count++;
                    }
                    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, swept);
                    qr->stats.global_used_bytes = qr->global_used_bytes;
                    res.reclaimed_parked_bytes += swept;
                    res.parked_reclaimed_count++;
                    qr->stats.parked_reclaimed_bytes += swept;
                    qr->stats.parked_reclaimed_count++;
                }
                qr_unlock(qr);
            }
        }
    }

    /* Stage 2: Reclaim VOLATILE Heap BOs with in_flight_refs == 0 in LRU order.
     * NEVER touches BOs with in_flight_refs > 0.
     */
    for (pass = 0; pass < 32; ++pass) {
        QrReclaimCandidate batch[A840_KMD_QR_DEFER_CHUNK];
        uint64_t batch_reclaimed[A840_KMD_QR_DEFER_CHUNK];
        A840KmdStatus batch_status[A840_KMD_QR_DEFER_CHUNK];
        size_t batch_count = 0u;
        uint64_t c_soft = 0u, c_hard = 0u, g_soft = 0u, g_hard = 0u;
        size_t i;

        qr_lock(qr);
        {
            A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
            if (client == NULL) {
                qr_unlock(qr);
                return A840_KMD_NOT_FOUND;
            }
            req_priority = client->priority;
            compute_needs_locked(qr, client, requested_bytes, &c_soft, &c_hard, &g_soft, &g_hard);
            if (c_soft == 0u && g_soft == 0u && c_hard == 0u && g_hard == 0u) {
                qr_unlock(qr);
                break;
            }
            batch_count = pick_reclaim_batch_locked(
                qr, client_id, req_priority, c_soft, g_soft, 0, batch, A840_KMD_QR_DEFER_CHUNK);
            if (batch_count == 0u) {
                /* Count VOLATILE BOs safely protected by in_flight_refs > 0. */
                for (i = 0u; i < A840_KMD_QUOTA_MAX_TRACKED_BOS; ++i) {
                    const A840KmdTrackedBoSlot *bo = &qr->bos[i];
                    if (bo->active && !bo->is_parked &&
                        bo->purgeable_state == (uint32_t)A840_PURGE_VOLATILE &&
                        bo->in_flight_refs > 0u) {
                        A840KmdQuotaClientSlot *owner = find_client_locked(qr, bo->client_id);
                        if (owner != NULL &&
                            (owner->client_id == client_id ||
                             (owner->priority < req_priority &&
                              owner->priority != (uint32_t)A840_KMD_PRIO_COMPOSITOR))) {
                            res.volatile_skipped_inflight++;
                            qr->stats.volatile_skipped_inflight++;
                        }
                    }
                }
            }
        }
        qr_unlock(qr);

        if (batch_count == 0u)
            break;

        /* Execute VOLATILE purge strictly OUTSIDE the lock. */
        for (i = 0u; i < batch_count; ++i) {
            uint64_t reclaimed = batch[i].bytes;
            A840KmdStatus cb_st = A840_KMD_OK;

            if (has_reclaim_ops && reclaim_ops.purge_volatile_bo != NULL) {
                reclaimed = 0u;
                cb_st = reclaim_ops.purge_volatile_bo(
                    reclaim_ops.cookie,
                    batch[i].client_id,
                    batch[i].bo_handle,
                    batch[i].bytes,
                    &reclaimed);
            } else if (bo_pool != NULL) {
                A840KmdPurgeResult pr;
                A840KmdStatus bp_st = a840_kmd_bo_pool_set_purgeable(
                    bo_pool, batch[i].client_id, batch[i].bo_handle,
                    A840_PURGE_EMPTY, &pr);
                if (bp_st != A840_KMD_OK) {
                    cb_st = bp_st;
                    reclaimed = 0u;
                }
            }
            batch_status[i] = cb_st;
            batch_reclaimed[i] = (cb_st == A840_KMD_OK) ? reclaimed : 0u;
        }

        /* Commit purged VOLATILE BOs under lock. */
        qr_lock(qr);
        for (i = 0u; i < batch_count; ++i) {
            A840KmdTrackedBoSlot *slot = &qr->bos[batch[i].slot_index];
            if (!slot->active || slot->slot_gen != batch[i].slot_gen)
                continue;
            slot->reclaiming = 0u;
            if (batch_status[i] == A840_KMD_OK && batch_reclaimed[i] > 0u) {
                A840KmdQuotaClientSlot *victim = find_client_locked(qr, slot->client_id);
                uint64_t eff_reclaimed = batch_reclaimed[i];
                if (victim != NULL) {
                    uint64_t deduct = (victim->used_bytes < eff_reclaimed) ? victim->used_bytes : eff_reclaimed;
                    victim->used_bytes -= deduct;
                    victim->volatile_bytes = sub_sat_u64(victim->volatile_bytes, slot->bytes);
                    victim->reclaim_count++;
                    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, deduct);
                    qr->stats.global_used_bytes = qr->global_used_bytes;
                }
                slot->purgeable_state = (uint32_t)A840_PURGE_EMPTY;
                res.reclaimed_volatile_bytes += eff_reclaimed;
                res.volatile_purged_count++;
                qr->stats.volatile_purged_bytes += eff_reclaimed;
                qr->stats.volatile_purged_count++;
            }
        }
        qr_unlock(qr);
    }

    /* Optional Stage 2b: bulk volatile sweep callback if still above soft/hard limit. */
    if (has_reclaim_ops && reclaim_ops.sweep_volatile_heap != NULL) {
        uint64_t c_soft = 0u, c_hard = 0u, g_soft = 0u, g_hard = 0u;
        uint64_t need = 0u;
        uint32_t max_vprio = (req_priority > 0u) ? (req_priority - 1u) : 0u;

        qr_lock(qr);
        {
            A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
            if (client != NULL && qr->global_used_bytes > 0u) {
                compute_needs_locked(qr, client, requested_bytes, &c_soft, &c_hard, &g_soft, &g_hard);
                need = (c_soft > g_soft) ? c_soft : g_soft;
            }
        }
        qr_unlock(qr);

        if (need > 0u) {
            uint64_t swept = 0u;
            A840KmdStatus sw_st = reclaim_ops.sweep_volatile_heap(
                reclaim_ops.cookie, client_id, max_vprio, need, &swept);
            if (sw_st == A840_KMD_OK && swept > 0u) {
                qr_lock(qr);
                {
                    A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
                    if (client != NULL) {
                        uint64_t deduct = (client->used_bytes < swept) ? client->used_bytes : swept;
                        client->used_bytes -= deduct;
                        client->reclaim_count++;
                    }
                    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, swept);
                    qr->stats.global_used_bytes = qr->global_used_bytes;
                    res.reclaimed_volatile_bytes += swept;
                    res.volatile_purged_count++;
                    qr->stats.volatile_purged_bytes += swept;
                    qr->stats.volatile_purged_count++;
                }
                qr_unlock(qr);
            }
        }
    }

    /* Stage 2c (COMPOSITOR anti-starvation): if COMPOSITOR still needs global room
     * and an evict_background_client callback is installed, reclaim from BACKGROUND
     * clients strictly outside lock. NEVER touches COMPOSITOR or CAMERA.
     */
    if (req_priority == (uint32_t)A840_KMD_PRIO_COMPOSITOR &&
        has_reclaim_ops && reclaim_ops.evict_background_client != NULL) {
        for (pass = 0; pass < 8; ++pass) {
            uint64_t victim_id = 0u;
            uint64_t g_hard = 0u;
            uint64_t c_soft = 0u, c_hard = 0u, g_soft = 0u;
            uint64_t max_used = 0u;
            size_t i;

            qr_lock(qr);
            {
                A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
                if (client != NULL) {
                    compute_needs_locked(qr, client, requested_bytes, &c_soft, &c_hard, &g_soft, &g_hard);
                    if (g_hard > 0u) {
                        for (i = 0u; i < A840_KMD_QUOTA_MAX_CLIENTS; ++i) {
                            if (qr->clients[i].active &&
                                qr->clients[i].priority == (uint32_t)A840_KMD_PRIO_BACKGROUND &&
                                qr->clients[i].used_bytes > max_used) {
                                max_used = qr->clients[i].used_bytes;
                                victim_id = qr->clients[i].client_id;
                            }
                        }
                    }
                }
            }
            qr_unlock(qr);

            if (victim_id == 0u || g_hard == 0u)
                break;

            {
                uint64_t evicted = 0u;
                A840KmdStatus ev_st = reclaim_ops.evict_background_client(
                    reclaim_ops.cookie, victim_id, g_hard, &evicted);
                if (ev_st != A840_KMD_OK || evicted == 0u)
                    break;
                qr_lock(qr);
                {
                    A840KmdQuotaClientSlot *victim = find_client_locked(qr, victim_id);
                    if (victim != NULL) {
                        uint64_t deduct = (victim->used_bytes < evicted) ? victim->used_bytes : evicted;
                        victim->used_bytes -= deduct;
                        victim->reclaim_count++;
                        qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, deduct);
                        qr->stats.global_used_bytes = qr->global_used_bytes;
                        res.reclaimed_bg_evict_bytes += deduct;
                        qr->stats.bg_evict_bytes += deduct;
                    }
                }
                qr_unlock(qr);
            }
        }
    }

    /* Stage 3: Final hard-limit admission check under lock. */
    qr_lock(qr);
    {
        A840KmdQuotaClientSlot *client = find_client_locked(qr, client_id);
        uint64_t new_client_used = 0u;
        uint64_t new_global_used = 0u;
        uint64_t eff_global_hard;
        uint64_t non_comp_ceiling;

        if (client == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        eff_global_hard = effective_global_hard_locked(qr, client->priority);
        non_comp_ceiling = sub_sat_u64(qr->global_hard_limit_bytes, qr->compositor_reserve_bytes);

        if (add_u64_overflow(client->used_bytes, requested_bytes, &new_client_used) ||
            add_u64_overflow(qr->global_used_bytes, requested_bytes, &new_global_used) ||
            new_client_used > client->hard_limit_bytes ||
            new_global_used > eff_global_hard) {
            client->oom_reject_count++;
            qr->stats.total_oom_rejects++;
            qr->stats.oom_rejects_by_prio[client->priority]++;
            res.client_used_after = client->used_bytes;
            res.global_used_after = qr->global_used_bytes;
            res.admitted = 0u;
            qr_unlock(qr);
            if (out_result != NULL)
                *out_result = res;
            return A840_KMD_OOM;
        }

        client->used_bytes = new_client_used;
        if (client->used_bytes > client->peak_used_bytes)
            client->peak_used_bytes = client->used_bytes;

        qr->global_used_bytes = new_global_used;
        qr->stats.global_used_bytes = qr->global_used_bytes;
        if (qr->global_used_bytes > qr->global_peak_used_bytes) {
            qr->global_peak_used_bytes = qr->global_used_bytes;
            qr->stats.global_peak_used_bytes = qr->global_peak_used_bytes;
        }
        qr->stats.total_alloc_admitted++;

        if (client->priority == (uint32_t)A840_KMD_PRIO_COMPOSITOR &&
            new_global_used > non_comp_ceiling) {
            res.used_compositor_reserve = 1u;
            qr->stats.compositor_protected_allocs++;
        }

        res.client_used_after = client->used_bytes;
        res.global_used_after = qr->global_used_bytes;
        res.admitted = 1u;
    }
    qr_unlock(qr);

    if (out_result != NULL)
        *out_result = res;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_release_bytes(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t bytes)
{
    A840KmdStatus status = check_init(qr);
    A840KmdQuotaClientSlot *client;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bytes == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    client = find_client_locked(qr, client_id);
    if (client == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    if (bytes > client->used_bytes) {
        qr_unlock(qr);
        return A840_KMD_RANGE;
    }
    client->used_bytes -= bytes;
    qr->global_used_bytes = sub_sat_u64(qr->global_used_bytes, bytes);
    qr->stats.global_used_bytes = qr->global_used_bytes;
    qr_unlock(qr);
    return A840_KMD_OK;
}

/* ---------------- Single-Channel Fault Isolation & Recovery ---------------- */

A840KmdStatus a840_kmd_qr_open_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id)
{
    A840KmdStatus status = check_init(qr);
    A840KmdChannelSlot *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    if (find_client_locked(qr, client_id) == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    for (i = 0u; i < A840_KMD_QUOTA_MAX_CHANNELS; ++i) {
        if (qr->channels[i].active) {
            if (qr->channels[i].client_id == client_id &&
                qr->channels[i].queue_id == queue_id) {
                qr_unlock(qr);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &qr->channels[i];
        }
    }
    if (free_slot == NULL) {
        qr_unlock(qr);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->state = (uint32_t)A840_KMD_CHANNEL_HEALTHY;
    free_slot->client_id = client_id;
    free_slot->queue_id = queue_id;
    qr->stats.active_channels++;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_close_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id)
{
    A840KmdStatus status = check_init(qr);
    A840KmdChannelSlot *ch;
    A840KmdRecoveryOps rec_ops;
    uint32_t has_rec_ops = 0u;
    QrFenceEvent aborted[A840_KMD_CHANNEL_MAX_INFLIGHT];
    size_t aborted_count = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&rec_ops, 0, sizeof(rec_ops));

    qr_lock(qr);
    ch = find_channel_locked(qr, client_id, queue_id);
    if (ch == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    has_rec_ops = qr->has_recovery_ops;
    if (has_rec_ops)
        rec_ops = qr->recovery_ops;

    for (i = 0u; i < A840_KMD_CHANNEL_MAX_INFLIGHT; ++i) {
        if (ch->inflight[i].active) {
            uint32_t b;
            for (b = 0u; b < ch->inflight[i].bo_count; ++b)
                decrement_tracked_bo_ref_locked(qr, client_id, ch->inflight[i].bo_handles[b]);
            if (aborted_count < A840_KMD_CHANNEL_MAX_INFLIGHT) {
                aborted[aborted_count].client_id = client_id;
                aborted[aborted_count].queue_id = queue_id;
                aborted[aborted_count].fence = ch->inflight[i].fence;
                aborted[aborted_count].status = A840_KMD_DISCONNECTED;
                aborted[aborted_count].fault_kind = A840_KMD_FAULT_NONE;
                aborted[aborted_count].recovery_generation = ch->recovery_generation;
                aborted_count++;
            }
        }
    }
    if (ch->state == (uint32_t)A840_KMD_CHANNEL_BANNED && qr->stats.banned_channels > 0u)
        qr->stats.banned_channels--;
    memset(ch, 0, sizeof(*ch));
    if (qr->stats.active_channels > 0u)
        qr->stats.active_channels--;
    qr_unlock(qr);

    if (has_rec_ops && rec_ops.fence_notify != NULL) {
        for (i = 0u; i < aborted_count; ++i) {
            rec_ops.fence_notify(
                rec_ops.cookie,
                aborted[i].client_id,
                aborted[i].queue_id,
                aborted[i].fence,
                aborted[i].status,
                aborted[i].fault_kind,
                aborted[i].recovery_generation);
        }
    }
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_channel_attach_sparse_bo(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t bo_handle)
{
    A840KmdStatus status = check_init(qr);
    A840KmdChannelSlot *ch;
    uint32_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    ch = find_channel_locked(qr, client_id, queue_id);
    if (ch == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    for (i = 0u; i < ch->sparse_bo_count; ++i) {
        if (ch->sparse_bo_handles[i] == bo_handle) {
            qr_unlock(qr);
            return A840_KMD_OK;
        }
    }
    if (ch->sparse_bo_count >= A840_KMD_CHANNEL_MAX_SPARSE_BOS) {
        qr_unlock(qr);
        return A840_KMD_CAPACITY;
    }
    ch->sparse_bo_handles[ch->sparse_bo_count++] = bo_handle;
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_submit_begin(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t fence,
    const uint64_t *bo_handles,
    uint32_t bo_count)
{
    A840KmdStatus status = check_init(qr);
    A840KmdBoPool *bo_pool = NULL;
    uint32_t sync_pool = 0u;
    uint32_t bp_pinned = 0u;
    uint32_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || fence == 0u || bo_count > A840_KMD_SUBMIT_MAX_BOS)
        return A840_KMD_INVALID_ARG;
    if (bo_count > 0u && bo_handles == NULL)
        return A840_KMD_INVALID_ARG;
    for (i = 0u; i < bo_count; ++i) {
        if (bo_handles[i] == 0u)
            return A840_KMD_INVALID_ARG;
    }

    qr_lock(qr);
    bo_pool = qr->bo_pool;
    sync_pool = qr->sync_bo_pool_inflight;
    qr_unlock(qr);

    /* If linked with A840KmdBoPool in-flight sync, pin BOs OUTSIDE our mutex. */
    if (bo_pool != NULL && sync_pool != 0u && bo_count > 0u) {
        for (i = 0u; i < bo_count; ++i) {
            A840KmdStatus bp_st = a840_kmd_bo_pool_begin_submit(
                bo_pool, client_id, bo_handles[i], NULL);
            if (bp_st != A840_KMD_OK) {
                while (bp_pinned > 0u) {
                    bp_pinned--;
                    (void)a840_kmd_bo_pool_end_submit(
                        bo_pool, client_id, bo_handles[bp_pinned], NULL);
                }
                return bp_st;
            }
            bp_pinned++;
        }
    }

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, client_id, queue_id);
        A840KmdChannelSubmitSlot *free_sub = NULL;
        size_t s;

        if (ch == NULL) {
            status = A840_KMD_NOT_FOUND;
            goto fail_unlock;
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_FAULTING ||
            ch->state == (uint32_t)A840_KMD_CHANNEL_BANNED) {
            status = A840_KMD_POISONED;
            goto fail_unlock;
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_RESETTING) {
            status = A840_KMD_STATE_CONFLICT;
            goto fail_unlock;
        }
        if (fence <= ch->last_submitted_fence) {
            status = A840_KMD_RANGE;
            goto fail_unlock;
        }

        for (s = 0u; s < A840_KMD_CHANNEL_MAX_INFLIGHT; ++s) {
            if (!ch->inflight[s].active) {
                free_sub = &ch->inflight[s];
                break;
            }
        }
        if (free_sub == NULL) {
            status = A840_KMD_CAPACITY;
            goto fail_unlock;
        }

        /* Validate all tracked BOs before mutating any in_flight_refs. */
        for (i = 0u; i < bo_count; ++i) {
            A840KmdTrackedBoSlot *bo = find_bo_locked(qr, bo_handles[i]);
            if (bo != NULL) {
                if (bo->client_id != client_id) {
                    status = A840_KMD_PERMISSION_DENIED;
                    goto fail_unlock;
                }
                if (bo->reclaiming || bo->is_parked ||
                    bo->purgeable_state == (uint32_t)A840_PURGE_EMPTY) {
                    status = A840_KMD_STATE_CONFLICT;
                    goto fail_unlock;
                }
            }
        }

        memset(free_sub, 0, sizeof(*free_sub));
        free_sub->active = 1u;
        free_sub->fence = fence;
        free_sub->bo_count = bo_count;
        for (i = 0u; i < bo_count; ++i) {
            A840KmdTrackedBoSlot *bo = find_bo_locked(qr, bo_handles[i]);
            free_sub->bo_handles[i] = bo_handles[i];
            if (bo != NULL && bo->in_flight_refs < UINT32_MAX)
                bo->in_flight_refs++;
        }

        ch->active_inflight_count++;
        ch->submitted_count++;
        ch->last_submitted_fence = fence;
    }
    qr_unlock(qr);
    return A840_KMD_OK;

fail_unlock:
    qr_unlock(qr);
    if (bo_pool != NULL && sync_pool != 0u) {
        while (bp_pinned > 0u) {
            bp_pinned--;
            (void)a840_kmd_bo_pool_end_submit(bo_pool, client_id, bo_handles[bp_pinned], NULL);
        }
    }
    return status;
}

A840KmdStatus a840_kmd_qr_submit_complete(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    uint64_t fence,
    uint32_t *out_completed_count)
{
    A840KmdStatus status = check_init(qr);
    A840KmdRecoveryOps rec_ops;
    uint32_t has_rec_ops = 0u;
    A840KmdBoPool *bo_pool = NULL;
    uint32_t sync_pool = 0u;
    QrFenceEvent completed[A840_KMD_CHANNEL_MAX_INFLIGHT];
    size_t completed_count = 0u;
    QrBoRefEvent bo_releases[A840_KMD_CHANNEL_MAX_INFLIGHT * A840_KMD_SUBMIT_MAX_BOS];
    size_t bo_release_count = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || fence == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&rec_ops, 0, sizeof(rec_ops));

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, client_id, queue_id);
        if (ch == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_FAULTING ||
            ch->state == (uint32_t)A840_KMD_CHANNEL_RESETTING ||
            ch->state == (uint32_t)A840_KMD_CHANNEL_BANNED) {
            qr_unlock(qr);
            return A840_KMD_POISONED;
        }

        has_rec_ops = qr->has_recovery_ops;
        if (has_rec_ops)
            rec_ops = qr->recovery_ops;
        bo_pool = qr->bo_pool;
        sync_pool = qr->sync_bo_pool_inflight;

        for (i = 0u; i < A840_KMD_CHANNEL_MAX_INFLIGHT; ++i) {
            A840KmdChannelSubmitSlot *sub = &ch->inflight[i];
            if (!sub->active || sub->fence > fence)
                continue;
            {
                uint32_t b;
                for (b = 0u; b < sub->bo_count; ++b) {
                    decrement_tracked_bo_ref_locked(qr, client_id, sub->bo_handles[b]);
                    if (bo_release_count < (A840_KMD_CHANNEL_MAX_INFLIGHT * A840_KMD_SUBMIT_MAX_BOS)) {
                        bo_releases[bo_release_count].client_id = client_id;
                        bo_releases[bo_release_count].bo_handle = sub->bo_handles[b];
                        bo_release_count++;
                    }
                }
            }
            if (completed_count < A840_KMD_CHANNEL_MAX_INFLIGHT) {
                completed[completed_count].client_id = client_id;
                completed[completed_count].queue_id = queue_id;
                completed[completed_count].fence = sub->fence;
                completed[completed_count].status = A840_KMD_OK;
                completed[completed_count].fault_kind = A840_KMD_FAULT_NONE;
                completed[completed_count].recovery_generation = ch->recovery_generation;
                completed_count++;
            }
            if (sub->fence > ch->last_completed_fence)
                ch->last_completed_fence = sub->fence;
            memset(sub, 0, sizeof(*sub));
            if (ch->active_inflight_count > 0u)
                ch->active_inflight_count--;
            ch->completed_ok_count++;
            qr->stats.total_healthy_submits_completed++;
        }
    }
    qr_unlock(qr);

    /* Release bo_pool refs and notify fences strictly OUTSIDE the lock. */
    for (i = 0u; i < bo_release_count; ++i) {
        if (bo_pool != NULL && sync_pool != 0u)
            (void)a840_kmd_bo_pool_end_submit(bo_pool, bo_releases[i].client_id, bo_releases[i].bo_handle, NULL);
        if (has_rec_ops && rec_ops.bo_end_submit != NULL)
            rec_ops.bo_end_submit(rec_ops.cookie, bo_releases[i].client_id, bo_releases[i].bo_handle);
    }

    if (has_rec_ops && rec_ops.fence_notify != NULL) {
        for (i = 0u; i < completed_count; ++i) {
            rec_ops.fence_notify(
                rec_ops.cookie,
                completed[i].client_id,
                completed[i].queue_id,
                completed[i].fence,
                completed[i].status,
                completed[i].fault_kind,
                completed[i].recovery_generation);
        }
    }

    if (out_completed_count != NULL)
        *out_completed_count = (uint32_t)completed_count;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_report_channel_fault(
    A840KmdQuotaRecovery *qr,
    const A840KmdFaultInput *fault_input,
    uint32_t fault_code,
    A840KmdRecoveryReport *out_report)
{
    A840KmdStatus status = check_init(qr);
    A840KmdFaultCapture *fc = NULL;
    A840KmdFaultSnapshot snap;
    A840KmdRecoveryReport report;

    if (status != A840_KMD_OK)
        return status;
    if (fault_input == NULL || fault_input->client_id == 0u ||
        !valid_fault_kind(fault_input->fault_kind)) {
        return A840_KMD_INVALID_ARG;
    }

    memset(&snap, 0, sizeof(snap));
    memset(&report, 0, sizeof(report));

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, fault_input->client_id, fault_input->queue_id);
        uint64_t min_f = UINT64_MAX;
        uint64_t max_f = 0u;
        size_t i;

        if (ch == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_BANNED) {
            qr_unlock(qr);
            return A840_KMD_POISONED;
        }
        if (ch->state == (uint32_t)A840_KMD_CHANNEL_FAULTING ||
            ch->state == (uint32_t)A840_KMD_CHANNEL_RESETTING) {
            qr_unlock(qr);
            return A840_KMD_STATE_CONFLICT;
        }

        report.client_id = ch->client_id;
        report.queue_id = ch->queue_id;
        report.old_state = ch->state;

        for (i = 0u; i < A840_KMD_CHANNEL_MAX_INFLIGHT; ++i) {
            if (ch->inflight[i].active) {
                if (ch->inflight[i].fence < min_f)
                    min_f = ch->inflight[i].fence;
                if (ch->inflight[i].fence > max_f)
                    max_f = ch->inflight[i].fence;
            }
        }
        if (min_f == UINT64_MAX) {
            min_f = fault_input->fence;
            max_f = fault_input->fence;
        } else if (fault_input->fence != 0u) {
            if (fault_input->fence < min_f)
                min_f = fault_input->fence;
            if (fault_input->fence > max_f)
                max_f = fault_input->fence;
        }

        /* Isolate ONLY this channel; healthy channels remain untouched. */
        ch->state = (uint32_t)A840_KMD_CHANNEL_FAULTING;
        ch->fault_count++;
        ch->last_fault_kind = fault_input->fault_kind;
        ch->last_fault_code = fault_code;
        ch->damaged_fence_start = min_f;
        ch->damaged_fence_end = max_f;
        qr->stats.total_faults++;

        report.final_state = ch->state;
        report.fault_kind = ch->last_fault_kind;
        report.fault_code = ch->last_fault_code;
        report.recovery_generation = ch->recovery_generation;
        report.damaged_fence_start = ch->damaged_fence_start;
        report.damaged_fence_end = ch->damaged_fence_end;
        fc = qr->fault_capture;
    }
    qr_unlock(qr);

    /* Capture hardware snapshot into FaultCapture strictly OUTSIDE our mutex. */
    if (fc != NULL) {
        (void)a840_kmd_fault_record(fc, fault_input, &snap);
    } else {
        snap.client_id = fault_input->client_id;
        snap.queue_id = fault_input->queue_id;
        snap.fence = fault_input->fence;
        snap.ib1_pc = fault_input->ib1_pc;
        snap.ib2_pc = fault_input->ib2_pc;
        snap.rbbm_status = fault_input->rbbm_status;
        snap.cp_status = fault_input->cp_status;
        snap.smmu_fault_va = fault_input->smmu_fault_va;
        snap.smmu_fsr = fault_input->smmu_fsr;
        snap.fault_kind = fault_input->fault_kind;
        snap.timestamp_ticks = fault_input->timestamp_ticks;
    }

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, fault_input->client_id, fault_input->queue_id);
        if (ch != NULL)
            ch->last_fault_snapshot = snap;
    }
    qr_unlock(qr);

    report.fault_snapshot = snap;
    if (out_report != NULL)
        *out_report = report;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_begin_channel_reset(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdRecoveryReport *out_report)
{
    A840KmdStatus status = check_init(qr);
    A840KmdRecoveryOps rec_ops;
    uint32_t has_rec_ops = 0u;
    A840KmdBoPool *bo_pool = NULL;
    A840KmdSparseManager *sparse_mgr = NULL;
    uint32_t sync_pool = 0u;
    QrFenceEvent aborted[A840_KMD_CHANNEL_MAX_INFLIGHT];
    size_t aborted_count = 0u;
    QrBoRefEvent bo_releases[A840_KMD_CHANNEL_MAX_INFLIGHT * A840_KMD_SUBMIT_MAX_BOS];
    size_t bo_release_count = 0u;
    uint64_t unique_bos[A840_KMD_CHANNEL_MAX_SPARSE_BOS +
                        (A840_KMD_CHANNEL_MAX_INFLIGHT * A840_KMD_SUBMIT_MAX_BOS)];
    size_t unique_bo_count = 0u;
    A840KmdRecoveryReport report;
    uint64_t next_gen = 0u;
    uint32_t f_kind = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&rec_ops, 0, sizeof(rec_ops));
    memset(&report, 0, sizeof(report));

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, client_id, queue_id);
        if (ch == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        if (ch->state != (uint32_t)A840_KMD_CHANNEL_FAULTING) {
            qr_unlock(qr);
            return A840_KMD_STATE_CONFLICT;
        }

        has_rec_ops = qr->has_recovery_ops;
        if (has_rec_ops)
            rec_ops = qr->recovery_ops;
        bo_pool = qr->bo_pool;
        sparse_mgr = qr->sparse_mgr;
        sync_pool = qr->sync_bo_pool_inflight;

        report.client_id = client_id;
        report.queue_id = queue_id;
        report.old_state = ch->state;
        ch->state = (uint32_t)A840_KMD_CHANNEL_RESETTING;
        report.final_state = ch->state;
        report.fault_kind = ch->last_fault_kind;
        report.fault_code = ch->last_fault_code;
        report.damaged_fence_start = ch->damaged_fence_start;
        report.damaged_fence_end = ch->damaged_fence_end;
        report.fault_snapshot = ch->last_fault_snapshot;
        next_gen = ch->recovery_generation + 1u;
        report.recovery_generation = next_gen;
        f_kind = ch->last_fault_kind;

        /* Collect explicitly attached sparse BOs for zero-page rollback. */
        for (i = 0u; i < ch->sparse_bo_count; ++i) {
            record_unique_bo(
                unique_bos, &unique_bo_count,
                sizeof(unique_bos) / sizeof(unique_bos[0]),
                ch->sparse_bo_handles[i]);
        }

        /* Drain all in-flight submits on ONLY this faulting channel. */
        for (i = 0u; i < A840_KMD_CHANNEL_MAX_INFLIGHT; ++i) {
            A840KmdChannelSubmitSlot *sub = &ch->inflight[i];
            uint32_t b;
            if (!sub->active)
                continue;

            for (b = 0u; b < sub->bo_count; ++b) {
                decrement_tracked_bo_ref_locked(qr, client_id, sub->bo_handles[b]);
                if (bo_release_count < (A840_KMD_CHANNEL_MAX_INFLIGHT * A840_KMD_SUBMIT_MAX_BOS)) {
                    bo_releases[bo_release_count].client_id = client_id;
                    bo_releases[bo_release_count].bo_handle = sub->bo_handles[b];
                    bo_release_count++;
                }
                record_unique_bo(
                    unique_bos, &unique_bo_count,
                    sizeof(unique_bos) / sizeof(unique_bos[0]),
                    sub->bo_handles[b]);
            }

            if (aborted_count < A840_KMD_CHANNEL_MAX_INFLIGHT) {
                aborted[aborted_count].client_id = client_id;
                aborted[aborted_count].queue_id = queue_id;
                aborted[aborted_count].fence = sub->fence;
                aborted[aborted_count].status = A840_KMD_POISONED;
                aborted[aborted_count].fault_kind = ch->last_fault_kind;
                aborted[aborted_count].recovery_generation = next_gen;
                aborted_count++;
            }

            memset(sub, 0, sizeof(*sub));
            if (ch->active_inflight_count > 0u)
                ch->active_inflight_count--;
            ch->completed_err_count++;
            qr->stats.total_aborted_submits++;
        }
    }
    qr_unlock(qr);

    /* All external notifications, BO ref releases, Sparse VA zero-page rollbacks,
     * and hardware channel reset run STRICTLY OUTSIDE the internal mutex.
     */
    if (has_rec_ops && rec_ops.fence_notify != NULL) {
        for (i = 0u; i < aborted_count; ++i) {
            rec_ops.fence_notify(
                rec_ops.cookie,
                aborted[i].client_id,
                aborted[i].queue_id,
                aborted[i].fence,
                aborted[i].status,
                aborted[i].fault_kind,
                aborted[i].recovery_generation);
        }
    }

    for (i = 0u; i < bo_release_count; ++i) {
        if (bo_pool != NULL && sync_pool != 0u)
            (void)a840_kmd_bo_pool_end_submit(bo_pool, bo_releases[i].client_id, bo_releases[i].bo_handle, NULL);
        if (has_rec_ops && rec_ops.bo_end_submit != NULL)
            rec_ops.bo_end_submit(rec_ops.cookie, bo_releases[i].client_id, bo_releases[i].bo_handle);
    }

    for (i = 0u; i < unique_bo_count; ++i) {
        uint32_t unbound_tiles = 0u;
        uint64_t tlb_gen = 0u;
        int did_rollback = 0;

        if (sparse_mgr != NULL) {
            A840KmdStatus sp_st = a840_kmd_sparse_unbind_bo(
                sparse_mgr, client_id, unique_bos[i], 0u, &unbound_tiles, &tlb_gen);
            if (sp_st == A840_KMD_OK) {
                report.rolled_back_sparse_bos++;
                report.rolled_back_sparse_tiles += unbound_tiles;
                did_rollback = 1;
            }
        }
        if (has_rec_ops && rec_ops.sparse_rollback_bo != NULL) {
            rec_ops.sparse_rollback_bo(
                rec_ops.cookie, client_id, queue_id, unique_bos[i], unbound_tiles);
            if (!did_rollback)
                report.rolled_back_sparse_bos++;
        }
    }

    if (report.rolled_back_sparse_tiles > 0u) {
        qr_lock(qr);
        qr->stats.total_sparse_tiles_rolled_back += report.rolled_back_sparse_tiles;
        qr_unlock(qr);
    }

    report.hw_reset_status = A840_KMD_OK;
    if (has_rec_ops && rec_ops.hw_channel_reset != NULL) {
        report.hw_reset_status = rec_ops.hw_channel_reset(
            rec_ops.cookie, client_id, queue_id, next_gen, f_kind);
    }

    report.aborted_submits = (uint32_t)aborted_count;
    report.released_bo_refs = (uint32_t)bo_release_count;
    if (out_report != NULL)
        *out_report = report;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_finish_channel_reset(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdStatus hw_reset_status,
    A840KmdRecoveryReport *out_report)
{
    A840KmdStatus status = check_init(qr);
    A840KmdRecoveryReport report;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&report, 0, sizeof(report));

    qr_lock(qr);
    {
        A840KmdChannelSlot *ch = find_channel_locked(qr, client_id, queue_id);
        if (ch == NULL) {
            qr_unlock(qr);
            return A840_KMD_NOT_FOUND;
        }
        if (ch->state != (uint32_t)A840_KMD_CHANNEL_RESETTING) {
            qr_unlock(qr);
            return A840_KMD_STATE_CONFLICT;
        }

        report.client_id = client_id;
        report.queue_id = queue_id;
        report.old_state = ch->state;
        report.fault_kind = ch->last_fault_kind;
        report.fault_code = ch->last_fault_code;
        report.damaged_fence_start = ch->damaged_fence_start;
        report.damaged_fence_end = ch->damaged_fence_end;
        report.hw_reset_status = hw_reset_status;
        report.fault_snapshot = ch->last_fault_snapshot;

        ch->recovery_generation++;
        if (hw_reset_status != A840_KMD_OK ||
            ch->recovery_count >= qr->max_recoveries_before_ban) {
            ch->state = (uint32_t)A840_KMD_CHANNEL_BANNED;
            qr->stats.channels_banned++;
            qr->stats.banned_channels++;
        } else {
            ch->state = (uint32_t)A840_KMD_CHANNEL_RECOVERED;
            ch->recovery_count++;
            qr->stats.channels_recovered++;
        }

        report.final_state = ch->state;
        report.recovery_generation = ch->recovery_generation;
    }
    qr_unlock(qr);

    if (out_report != NULL)
        *out_report = report;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_handle_channel_fault(
    A840KmdQuotaRecovery *qr,
    const A840KmdFaultInput *fault_input,
    uint32_t fault_code,
    A840KmdRecoveryReport *out_report)
{
    A840KmdRecoveryReport r_fault;
    A840KmdRecoveryReport r_reset;
    A840KmdRecoveryReport r_finish;
    A840KmdStatus st;

    memset(&r_fault, 0, sizeof(r_fault));
    memset(&r_reset, 0, sizeof(r_reset));
    memset(&r_finish, 0, sizeof(r_finish));

    st = a840_kmd_qr_report_channel_fault(qr, fault_input, fault_code, &r_fault);
    if (st != A840_KMD_OK)
        return st;

    st = a840_kmd_qr_begin_channel_reset(
        qr, fault_input->client_id, fault_input->queue_id, &r_reset);
    if (st != A840_KMD_OK)
        return st;

    st = a840_kmd_qr_finish_channel_reset(
        qr, fault_input->client_id, fault_input->queue_id,
        r_reset.hw_reset_status, &r_finish);
    if (st != A840_KMD_OK)
        return st;

    if (out_report != NULL) {
        r_finish.old_state = r_fault.old_state;
        r_finish.aborted_submits = r_reset.aborted_submits;
        r_finish.released_bo_refs = r_reset.released_bo_refs;
        r_finish.rolled_back_sparse_bos = r_reset.rolled_back_sparse_bos;
        r_finish.rolled_back_sparse_tiles = r_reset.rolled_back_sparse_tiles;
        *out_report = r_finish;
    }
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_query_channel(
    A840KmdQuotaRecovery *qr,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdChannelInfo *out_info)
{
    A840KmdStatus status = check_init(qr);
    A840KmdChannelSlot *ch;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_info == NULL)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    ch = find_channel_locked(qr, client_id, queue_id);
    if (ch == NULL) {
        qr_unlock(qr);
        return A840_KMD_NOT_FOUND;
    }
    fill_channel_info_locked(ch, out_info);
    qr_unlock(qr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_qr_get_stats(
    A840KmdQuotaRecovery *qr,
    A840KmdQuotaRecoveryStats *out_stats)
{
    A840KmdStatus status = check_init(qr);
    if (status != A840_KMD_OK)
        return status;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    qr_lock(qr);
    *out_stats = qr->stats;
    qr_unlock(qr);
    return A840_KMD_OK;
}
