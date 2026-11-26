/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_fault_capture.h"
#include <string.h>

#define A840_KMD_FAULT_MAGIC 1179407444u

#if !defined(A840_KMD_CUSTOM_SYNC)
static void fc_lock(A840KmdFaultCapture *fc)
{
    (void)pthread_mutex_lock(&fc->lock);
}

static void fc_unlock(A840KmdFaultCapture *fc)
{
    (void)pthread_mutex_unlock(&fc->lock);
}
#else
static void fc_lock(A840KmdFaultCapture *fc) { (void)fc; }
static void fc_unlock(A840KmdFaultCapture *fc) { (void)fc; }
#endif

static A840KmdStatus check_init(const A840KmdFaultCapture *fc)
{
    if (fc == NULL)
        return A840_KMD_INVALID_ARG;
    if (fc->initialized != A840_KMD_FAULT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int valid_fault_kind(uint32_t kind)
{
    return kind >= (uint32_t)A840_KMD_FAULT_GPU_HANG &&
           kind <= (uint32_t)A840_KMD_FAULT_IB_ERROR;
}

A840KmdStatus a840_kmd_fault_init(A840KmdFaultCapture *fc)
{
    if (fc == NULL)
        return A840_KMD_INVALID_ARG;
    memset(fc, 0, sizeof(*fc));
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (pthread_mutex_init(&fc->lock, NULL) != 0)
        return A840_KMD_NO_MEMORY;
#endif
    fc->initialized = A840_KMD_FAULT_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_fault_destroy(A840KmdFaultCapture *fc)
{
    if (check_init(fc) != A840_KMD_OK)
        return;
    fc_lock(fc);
    fc->poison_cb = NULL;
    fc->poison_cookie = NULL;
    fc->initialized = 0u;
    fc_unlock(fc);
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_destroy(&fc->lock);
#endif
}

A840KmdStatus a840_kmd_fault_set_poison_callback(
    A840KmdFaultCapture *fc,
    A840KmdFaultPoisonCallback cb,
    void *cookie)
{
    A840KmdStatus status = check_init(fc);
    if (status != A840_KMD_OK)
        return status;
    fc_lock(fc);
    fc->poison_cb = cb;
    fc->poison_cookie = cookie;
    fc_unlock(fc);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_fault_record(
    A840KmdFaultCapture *fc,
    const A840KmdFaultInput *input,
    A840KmdFaultSnapshot *out_snapshot)
{
    A840KmdStatus status;
    A840KmdFaultSnapshot snap;
    A840KmdFaultPoisonCallback cb = NULL;
    void *cookie = NULL;
    A840KmdPoisonedChannel *channel = NULL;
    A840KmdPoisonedChannel *free_slot = NULL;
    size_t i;

    status = check_init(fc);
    if (status != A840_KMD_OK)
        return status;
    if (input == NULL || input->client_id == 0u || !valid_fault_kind(input->fault_kind))
        return A840_KMD_INVALID_ARG;

    fc_lock(fc);

    for (i = 0u; i < A840_KMD_FAULT_MAX_POISONED_CHANNELS; ++i) {
        if (fc->poisoned[i].active) {
            if (fc->poisoned[i].client_id == input->client_id &&
                fc->poisoned[i].queue_id == input->queue_id) {
                channel = &fc->poisoned[i];
                break;
            }
        } else if (free_slot == NULL) {
            free_slot = &fc->poisoned[i];
        }
    }

    if (channel == NULL && free_slot == NULL) {
        fc_unlock(fc);
        return A840_KMD_CAPACITY;
    }

    memset(&snap, 0, sizeof(snap));
    fc->total_faults++;
    snap.sequence = fc->total_faults;
    snap.client_id = input->client_id;
    snap.queue_id = input->queue_id;
    snap.fence = input->fence;
    snap.ib1_pc = input->ib1_pc;
    snap.ib2_pc = input->ib2_pc;
    snap.rbbm_status = input->rbbm_status;
    snap.cp_status = input->cp_status;
    snap.smmu_fault_va = input->smmu_fault_va;
    snap.smmu_fsr = input->smmu_fsr;
    snap.fault_kind = input->fault_kind;
    snap.timestamp_ticks = input->timestamp_ticks;

    fc->ring[fc->write_index] = snap;
    fc->write_index = (fc->write_index + 1u) % A840_KMD_FAULT_RING_CAPACITY;
    if (fc->ring_count < A840_KMD_FAULT_RING_CAPACITY)
        fc->ring_count++;

    if (channel != NULL) {
        channel->fault_count++;
        channel->last_sequence = snap.sequence;
    } else {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->active = 1u;
        free_slot->fault_count = 1u;
        free_slot->client_id = input->client_id;
        free_slot->queue_id = input->queue_id;
        free_slot->last_sequence = snap.sequence;
        fc->poisoned_count++;
    }

    cb = fc->poison_cb;
    cookie = fc->poison_cookie;
    fc_unlock(fc);

    if (out_snapshot != NULL)
        *out_snapshot = snap;

    /* Fire callback outside fc->lock to preserve strict lock hierarchy. */
    if (cb != NULL)
        cb(cookie, snap.client_id, snap.queue_id, &snap);

    return A840_KMD_OK;
}

int a840_kmd_fault_is_channel_poisoned(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id)
{
    size_t i;
    int found = 0;

    if (check_init(fc) != A840_KMD_OK || client_id == 0u)
        return 0;

    fc_lock(fc);
    for (i = 0u; i < A840_KMD_FAULT_MAX_POISONED_CHANNELS; ++i) {
        if (fc->poisoned[i].active &&
            fc->poisoned[i].client_id == client_id &&
            fc->poisoned[i].queue_id == queue_id) {
            found = 1;
            break;
        }
    }
    fc_unlock(fc);
    return found;
}

A840KmdStatus a840_kmd_fault_check_channel(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id)
{
    A840KmdStatus status = check_init(fc);
    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;
    return a840_kmd_fault_is_channel_poisoned(fc, client_id, queue_id) ?
        A840_KMD_POISONED : A840_KMD_OK;
}

size_t a840_kmd_fault_clear_client(
    A840KmdFaultCapture *fc,
    uint64_t client_id)
{
    size_t i;
    size_t cleared = 0u;

    if (check_init(fc) != A840_KMD_OK || client_id == 0u)
        return 0u;

    fc_lock(fc);
    for (i = 0u; i < A840_KMD_FAULT_MAX_POISONED_CHANNELS; ++i) {
        if (fc->poisoned[i].active && fc->poisoned[i].client_id == client_id) {
            memset(&fc->poisoned[i], 0, sizeof(fc->poisoned[i]));
            if (fc->poisoned_count > 0u)
                fc->poisoned_count--;
            cleared++;
        }
    }
    fc_unlock(fc);
    return cleared;
}

size_t a840_kmd_fault_ring_count(A840KmdFaultCapture *fc)
{
    size_t count;
    if (check_init(fc) != A840_KMD_OK)
        return 0u;
    fc_lock(fc);
    count = fc->ring_count;
    fc_unlock(fc);
    return count;
}

uint64_t a840_kmd_fault_total_count(A840KmdFaultCapture *fc)
{
    uint64_t total;
    if (check_init(fc) != A840_KMD_OK)
        return 0u;
    fc_lock(fc);
    total = fc->total_faults;
    fc_unlock(fc);
    return total;
}

size_t a840_kmd_fault_poisoned_channel_count(A840KmdFaultCapture *fc)
{
    size_t count;
    if (check_init(fc) != A840_KMD_OK)
        return 0u;
    fc_lock(fc);
    count = fc->poisoned_count;
    fc_unlock(fc);
    return count;
}

A840KmdStatus a840_kmd_fault_get_snapshot(
    A840KmdFaultCapture *fc,
    size_t index_from_oldest,
    A840KmdFaultSnapshot *out_snapshot)
{
    A840KmdStatus status = check_init(fc);
    size_t oldest_index;
    size_t physical_index;

    if (status != A840_KMD_OK)
        return status;
    if (out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    fc_lock(fc);
    if (index_from_oldest >= fc->ring_count) {
        fc_unlock(fc);
        return A840_KMD_RANGE;
    }
    if (fc->ring_count < A840_KMD_FAULT_RING_CAPACITY)
        oldest_index = 0u;
    else
        oldest_index = fc->write_index;
    physical_index = (oldest_index + index_from_oldest) % A840_KMD_FAULT_RING_CAPACITY;
    *out_snapshot = fc->ring[physical_index];
    fc_unlock(fc);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_fault_get_latest(
    A840KmdFaultCapture *fc,
    A840KmdFaultSnapshot *out_snapshot)
{
    A840KmdStatus status = check_init(fc);
    size_t latest_index;

    if (status != A840_KMD_OK)
        return status;
    if (out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    fc_lock(fc);
    if (fc->ring_count == 0u) {
        fc_unlock(fc);
        return A840_KMD_NOT_FOUND;
    }
    latest_index = (fc->write_index + A840_KMD_FAULT_RING_CAPACITY - 1u) %
        A840_KMD_FAULT_RING_CAPACITY;
    *out_snapshot = fc->ring[latest_index];
    fc_unlock(fc);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_fault_get_channel_latest(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdFaultSnapshot *out_snapshot)
{
    A840KmdStatus status = check_init(fc);
    size_t step;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    fc_lock(fc);
    for (step = 0u; step < fc->ring_count; ++step) {
        size_t idx = (fc->write_index + A840_KMD_FAULT_RING_CAPACITY - 1u - step) %
            A840_KMD_FAULT_RING_CAPACITY;
        if (fc->ring[idx].client_id == client_id &&
            fc->ring[idx].queue_id == queue_id) {
            *out_snapshot = fc->ring[idx];
            fc_unlock(fc);
            return A840_KMD_OK;
        }
    }
    fc_unlock(fc);
    return A840_KMD_NOT_FOUND;
}
