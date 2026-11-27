/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_bo_pool.h"
#include <string.h>
#include <time.h>

#define A840_KMD_BO_POOL_MAGIC 1112494156u
#define A840_KMD_BO_POOL_MIN_ALIGN UINT64_C(4096)
#define A840_KMD_BO_POOL_SYNTH_VA_BASE UINT64_C(16777216)
#define A840_KMD_BO_POOL_SYNTH_PHYS_BASE UINT64_C(268435456)
#define A840_KMD_BO_POOL_DEFER_CHUNK 32u

static const uint64_t g_bucket_sizes[A840_KMD_BO_POOL_BUCKET_COUNT] = {
    UINT64_C(4096),      /* 4 KiB */
    UINT64_C(16384),     /* 16 KiB */
    UINT64_C(65536),     /* 64 KiB */
    UINT64_C(262144),    /* 256 KiB */
    UINT64_C(1048576),   /* 1 MiB */
    UINT64_C(4194304),   /* 4 MiB */
    UINT64_C(16777216)   /* 16 MiB */
};

typedef struct BoDeferredOp {
    uint32_t is_purge;
    uint32_t reserved;
    uint64_t client_id;
    void *smmu_ctx;
    uint64_t gpu_va;
    uint64_t phys_addr;
    void *backing_handle;
    uint64_t bytes;
} BoDeferredOp;

static void pool_lock(A840KmdBoPool *pool)
{
    if (pool->has_custom_sync) {
        pool->sync_ops.lock(pool->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&pool->lock);
#endif
}

static void pool_unlock(A840KmdBoPool *pool)
{
    if (pool->has_custom_sync) {
        pool->sync_ops.unlock(pool->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&pool->lock);
#endif
}

static uint64_t pool_now_ns(const A840KmdBoPool *pool)
{
    if (pool->has_custom_sync && pool->sync_ops.now_ns != NULL)
        return pool->sync_ops.now_ns(pool->sync_ops.cookie);
    if (pool->sync_ops.now_ns != NULL)
        return pool->sync_ops.now_ns(pool->sync_ops.cookie);
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return 0u;
        return ((uint64_t)ts.tv_sec * UINT64_C(1000000000)) + (uint64_t)ts.tv_nsec;
    }
}

static A840KmdStatus check_init(const A840KmdBoPool *pool)
{
    if (pool == NULL)
        return A840_KMD_INVALID_ARG;
    if (pool->initialized != A840_KMD_BO_POOL_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int is_power_of_two(uint64_t v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

static int align_up_u64(uint64_t value, uint64_t align, uint64_t *out_aligned)
{
    uint64_t rem;
    if (align == 0u || !is_power_of_two(align))
        return 0;
    rem = value % align;
    if (rem == 0u) {
        *out_aligned = value;
        return 1;
    }
    if (align - rem > UINT64_MAX - value)
        return 0;
    *out_aligned = value + (align - rem);
    return 1;
}

static void fill_desc_from_slot(
    const A840KmdBoPoolSlot *slot,
    uint32_t from_parked,
    uint32_t is_scrubbed,
    A840KmdBoDesc *out_desc)
{
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->bo_handle = slot->bo_handle;
    out_desc->client_id = slot->client_id;
    out_desc->gpu_va = slot->gpu_va;
    out_desc->phys_addr = slot->phys_addr;
    out_desc->cpu_ptr = slot->cpu_ptr;
    out_desc->backing_handle = slot->backing_handle;
    out_desc->requested_bytes = slot->requested_bytes;
    out_desc->bucket_bytes = slot->bucket_bytes;
    out_desc->alignment = slot->alignment;
    out_desc->permissions = slot->permissions;
    out_desc->cache_mode = slot->cache_mode;
    out_desc->purgeable_state = slot->purgeable_state;
    out_desc->in_flight_refs = slot->in_flight_refs;
    out_desc->from_parked_pool = from_parked ? 1u : 0u;
    out_desc->is_scrubbed = is_scrubbed ? 1u : 0u;
}

static A840KmdBoPoolSlot *find_active_by_handle_locked(
    A840KmdBoPool *pool,
    uint64_t bo_handle)
{
    size_t i;
    if (bo_handle == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
        if (pool->slots[i].entry_state == A840_KMD_BO_ENTRY_ACTIVE &&
            pool->slots[i].bo_handle == bo_handle)
            return &pool->slots[i];
    }
    return NULL;
}

static A840KmdBoPoolSlot *find_free_slot_locked(A840KmdBoPool *pool)
{
    size_t i;
    for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
        if (pool->slots[i].entry_state == A840_KMD_BO_ENTRY_FREE)
            return &pool->slots[i];
    }
    return NULL;
}

static uint64_t next_lru_seq_locked(A840KmdBoPool *pool)
{
    pool->next_lru_seq++;
    if (pool->next_lru_seq == 0u)
        pool->next_lru_seq = 1u;
    return pool->next_lru_seq;
}

static uint64_t next_bo_handle_locked(A840KmdBoPool *pool)
{
    uint64_t h = pool->next_bo_handle++;
    if (h == 0u)
        h = pool->next_bo_handle++;
    return h;
}

static void record_deferred_unmap(
    const A840KmdBoPoolSlot *slot,
    BoDeferredOp *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->is_purge = 0u;
    dst->client_id = slot->client_id;
    dst->smmu_ctx = slot->smmu_ctx;
    dst->gpu_va = slot->gpu_va;
    dst->phys_addr = slot->phys_addr;
    dst->backing_handle = slot->backing_handle;
    dst->bytes = slot->bucket_bytes;
}

static void remove_parked_slot_locked(
    A840KmdBoPool *pool,
    A840KmdBoPoolSlot *slot,
    BoDeferredOp *out_op)
{
    if (out_op != NULL)
        record_deferred_unmap(slot, out_op);
    if (slot->bucket_index < A840_KMD_BO_POOL_BUCKET_COUNT &&
        pool->bucket_parked_count[slot->bucket_index] > 0u)
        pool->bucket_parked_count[slot->bucket_index]--;
    if (pool->stats.parked_bos > 0u)
        pool->stats.parked_bos--;
    if (pool->stats.parked_bytes >= slot->bucket_bytes)
        pool->stats.parked_bytes -= slot->bucket_bytes;
    else
        pool->stats.parked_bytes = 0u;
    memset(slot, 0, sizeof(*slot));
}

static void remove_active_slot_locked(
    A840KmdBoPool *pool,
    A840KmdBoPoolSlot *slot,
    BoDeferredOp *out_op)
{
    if (out_op != NULL)
        record_deferred_unmap(slot, out_op);
    if (pool->stats.active_bos > 0u)
        pool->stats.active_bos--;
    if (slot->purgeable_state == A840_PURGE_VOLATILE &&
        pool->stats.volatile_bos > 0u)
        pool->stats.volatile_bos--;
    else if (slot->purgeable_state == A840_PURGE_EMPTY &&
             pool->stats.empty_bos > 0u)
        pool->stats.empty_bos--;
    memset(slot, 0, sizeof(*slot));
}

static A840KmdBoPoolSlot *find_oldest_parked_locked(
    A840KmdBoPool *pool,
    int filter_bucket)
{
    A840KmdBoPoolSlot *oldest = NULL;
    size_t i;
    for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
        A840KmdBoPoolSlot *s = &pool->slots[i];
        if (s->entry_state != A840_KMD_BO_ENTRY_PARKED)
            continue;
        if (filter_bucket >= 0 && s->bucket_index != (uint32_t)filter_bucket)
            continue;
        if (oldest == NULL || s->lru_seq < oldest->lru_seq)
            oldest = s;
    }
    return oldest;
}

static A840KmdBoPoolSlot *find_oldest_purgeable_volatile_locked(A840KmdBoPool *pool)
{
    A840KmdBoPoolSlot *oldest = NULL;
    size_t i;
    for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
        A840KmdBoPoolSlot *s = &pool->slots[i];
        if (s->entry_state != A840_KMD_BO_ENTRY_ACTIVE)
            continue;
        if (s->purgeable_state != A840_PURGE_VOLATILE)
            continue;
        if (s->in_flight_refs != 0u || s->pending_destroy != 0u)
            continue;
        if (oldest == NULL || s->lru_seq < oldest->lru_seq)
            oldest = s;
    }
    return oldest;
}

static void run_deferred_ops_unlocked(
    const A840KmdBoPoolBackingOps *backing_ops,
    uint32_t has_backing_ops,
    const BoDeferredOp *ops,
    size_t count,
    uint64_t zero_page_phys)
{
    size_t i;
    if (!has_backing_ops || count == 0u)
        return;
    for (i = 0u; i < count; ++i) {
        if (ops[i].is_purge) {
            if (backing_ops->purge_to_empty != NULL) {
                backing_ops->purge_to_empty(backing_ops->cookie,
                    ops[i].client_id, ops[i].smmu_ctx, ops[i].gpu_va,
                    ops[i].phys_addr, ops[i].backing_handle, ops[i].bytes,
                    zero_page_phys);
            }
        } else {
            if (backing_ops->unmap_and_free != NULL) {
                backing_ops->unmap_and_free(backing_ops->cookie,
                    ops[i].client_id, ops[i].smmu_ctx, ops[i].gpu_va,
                    ops[i].phys_addr, ops[i].backing_handle, ops[i].bytes);
            }
        }
    }
}

static A840KmdStatus run_zero_scrub_unlocked(
    const A840KmdBoPoolBackingOps *backing_ops,
    uint32_t has_backing_ops,
    uint64_t client_id,
    uint64_t phys_addr,
    void *cpu_ptr,
    void *backing_handle,
    uint64_t bytes)
{
    if (has_backing_ops && backing_ops->zero_scrub != NULL) {
        return backing_ops->zero_scrub(backing_ops->cookie, client_id,
            phys_addr, cpu_ptr, backing_handle, bytes);
    }
    if (cpu_ptr != NULL)
        memset(cpu_ptr, 0, (size_t)bytes);
    return A840_KMD_OK;
}

static uint64_t query_zero_page_phys_unlocked(A840KmdZeroPageManager *zp)
{
    A840KmdZeroPageStats zp_stats;
    if (zp == NULL)
        return 0u;
    memset(&zp_stats, 0, sizeof(zp_stats));
    if (a840_kmd_zero_page_get_stats(zp, &zp_stats) != A840_KMD_OK)
        return 0u;
    if (!zp_stats.backing_allocated)
        return 0u;
    return zp_stats.backing_phys_addr;
}

uint64_t a840_kmd_bo_pool_bucket_size(uint32_t bucket_index)
{
    if (bucket_index >= A840_KMD_BO_POOL_BUCKET_COUNT)
        return 0u;
    return g_bucket_sizes[bucket_index];
}

int a840_kmd_bo_pool_select_bucket(uint64_t bytes, uint64_t *out_bucket_bytes)
{
    uint32_t i;
    if (bytes == 0u) {
        if (out_bucket_bytes != NULL)
            *out_bucket_bytes = 0u;
        return -1;
    }
    for (i = 0u; i < A840_KMD_BO_POOL_BUCKET_COUNT; ++i) {
        if (bytes <= g_bucket_sizes[i]) {
            if (out_bucket_bytes != NULL)
                *out_bucket_bytes = g_bucket_sizes[i];
            return (int)i;
        }
    }
    if (out_bucket_bytes != NULL)
        *out_bucket_bytes = 0u;
    return -1;
}

A840KmdStatus a840_kmd_bo_pool_init(
    A840KmdBoPool *pool,
    const A840KmdBoPoolConfig *config)
{
    if (pool == NULL)
        return A840_KMD_INVALID_ARG;

    if (config != NULL) {
        if (config->sync_ops != NULL &&
            (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL))
            return A840_KMD_INVALID_ARG;
        if (config->backing_ops != NULL &&
            (config->backing_ops->alloc_and_map == NULL ||
             config->backing_ops->unmap_and_free == NULL))
            return A840_KMD_INVALID_ARG;
        if (config->max_parked_per_bucket > A840_KMD_BO_POOL_MAX_PER_BUCKET)
            return A840_KMD_RANGE;
    }

#if defined(A840_KMD_CUSTOM_SYNC)
    if (config == NULL || config->sync_ops == NULL)
        return A840_KMD_INVALID_ARG;
#endif

    memset(pool, 0, sizeof(*pool));
    pool->max_parked_bytes = A840_KMD_BO_POOL_DEFAULT_MAX_BYTES;
    pool->max_parked_per_bucket = A840_KMD_BO_POOL_DEFAULT_BUCKET_LIMIT;
    pool->scrub_on_park = 0u;
    pool->parked_ttl_ns = 0u;

    if (config != NULL) {
        if (config->max_parked_bytes != 0u)
            pool->max_parked_bytes = config->max_parked_bytes;
        if (config->max_parked_per_bucket != 0u)
            pool->max_parked_per_bucket = config->max_parked_per_bucket;
        pool->scrub_on_park = config->scrub_on_park ? 1u : 0u;
        pool->parked_ttl_ns = config->parked_ttl_ns;
        pool->zero_page_mgr = config->zero_page_mgr;
        if (config->sync_ops != NULL) {
            pool->has_custom_sync = 1u;
            pool->sync_ops = *config->sync_ops;
        }
        if (config->backing_ops != NULL) {
            pool->has_backing_ops = 1u;
            pool->backing_ops = *config->backing_ops;
        }
    }

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!pool->has_custom_sync) {
        if (pthread_mutex_init(&pool->lock, NULL) != 0)
            return A840_KMD_IO_ERROR;
    }
#endif

    pool->next_bo_handle = 1u;
    pool->next_synthetic_va = A840_KMD_BO_POOL_SYNTH_VA_BASE;
    pool->next_synthetic_phys = A840_KMD_BO_POOL_SYNTH_PHYS_BASE;
    pool->next_lru_seq = 1u;
    pool->initialized = A840_KMD_BO_POOL_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_bo_pool_destroy(A840KmdBoPool *pool)
{
    BoDeferredOp chunk[A840_KMD_BO_POOL_DEFER_CHUNK];
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint32_t has_custom_sync = 0u;
    size_t i;

    if (check_init(pool) != A840_KMD_OK)
        return;

    memset(&backing_ops, 0, sizeof(backing_ops));

    for (;;) {
        size_t chunk_count = 0u;
        int more = 0;

        pool_lock(pool);
        if (pool->initialized != A840_KMD_BO_POOL_MAGIC) {
            pool_unlock(pool);
            return;
        }
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        has_custom_sync = pool->has_custom_sync;

        for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
            if (pool->slots[i].entry_state == A840_KMD_BO_ENTRY_FREE)
                continue;
            if (chunk_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                more = 1;
                break;
            }
            if (pool->slots[i].entry_state == A840_KMD_BO_ENTRY_PARKED)
                remove_parked_slot_locked(pool, &pool->slots[i], &chunk[chunk_count++]);
            else
                remove_active_slot_locked(pool, &pool->slots[i], &chunk[chunk_count++]);
        }

        if (!more)
            pool->initialized = 0u;
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, chunk, chunk_count, 0u);
        if (!more)
            break;
    }

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!has_custom_sync)
        (void)pthread_mutex_destroy(&pool->lock);
#else
    (void)has_custom_sync;
#endif
    memset(pool, 0, sizeof(*pool));
}

A840KmdStatus a840_kmd_bo_pool_acquire(
    A840KmdBoPool *pool,
    const A840KmdBoAcquireReq *req,
    A840KmdBoDesc *out_desc)
{
    A840KmdStatus status = check_init(pool);
    uint64_t effective_align;
    uint64_t alloc_bytes = 0u;
    int bucket_idx;
    uint32_t slot_bucket;
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    BoDeferredOp expired_ops[A840_KMD_BO_POOL_DEFER_CHUNK];
    size_t expired_count = 0u;

    if (status != A840_KMD_OK)
        return status;
    if (req == NULL || out_desc == NULL)
        return A840_KMD_INVALID_ARG;
    if (req->client_id == 0u ||
        req->requested_bytes == 0u ||
        req->reserved != 0u ||
        (req->flags & ~(uint32_t)A840_KMD_BO_ACQUIRE_KNOWN_MASK) != 0u ||
        req->permissions == 0u ||
        (req->permissions & ~(uint32_t)A840_KMD_SMMU_PERM_READ_WRITE) != 0u ||
        req->cache_mode > A840_KMD_BO_CACHE_COHERENT)
        return A840_KMD_INVALID_ARG;

    effective_align = (req->alignment == 0u) ? A840_KMD_BO_POOL_MIN_ALIGN : req->alignment;
    if (effective_align < A840_KMD_BO_POOL_MIN_ALIGN || !is_power_of_two(effective_align))
        return A840_KMD_ALIGNMENT;

    bucket_idx = a840_kmd_bo_pool_select_bucket(req->requested_bytes, &alloc_bytes);
    if (bucket_idx >= 0) {
        slot_bucket = (uint32_t)bucket_idx;
        if (alloc_bytes < effective_align) {
            if (!align_up_u64(alloc_bytes, effective_align, &alloc_bytes))
                return A840_KMD_RANGE;
            bucket_idx = a840_kmd_bo_pool_select_bucket(alloc_bytes, &alloc_bytes);
            if (bucket_idx >= 0) {
                slot_bucket = (uint32_t)bucket_idx;
            } else {
                slot_bucket = A840_KMD_BO_POOL_BUCKET_COUNT;
                if (!align_up_u64(req->requested_bytes, effective_align, &alloc_bytes))
                    return A840_KMD_RANGE;
            }
        }
    } else {
        slot_bucket = A840_KMD_BO_POOL_BUCKET_COUNT;
        if (!align_up_u64(req->requested_bytes, effective_align, &alloc_bytes))
            return A840_KMD_RANGE;
    }

    if (alloc_bytes == 0u || alloc_bytes >= A840_KMD_GPU_VA_LIMIT)
        return A840_KMD_RANGE;

    memset(out_desc, 0, sizeof(*out_desc));
    memset(&backing_ops, 0, sizeof(backing_ops));

    /* Fast path: try to reuse a matching Parked BO owned by the SAME client. */
    if (slot_bucket < A840_KMD_BO_POOL_BUCKET_COUNT) {
        A840KmdBoPoolSlot *match = NULL;
        int saw_other_client_match = 0;
        uint64_t now = 0u;
        size_t i;

        pool_lock(pool);
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        if (pool->parked_ttl_ns != 0u)
            now = pool_now_ns(pool);

        for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
            A840KmdBoPoolSlot *s = &pool->slots[i];
            if (s->entry_state != A840_KMD_BO_ENTRY_PARKED)
                continue;
            if (pool->parked_ttl_ns != 0u &&
                now >= s->parked_at_ns &&
                (now - s->parked_at_ns) >= pool->parked_ttl_ns) {
                if (expired_count < A840_KMD_BO_POOL_DEFER_CHUNK) {
                    remove_parked_slot_locked(pool, s, &expired_ops[expired_count++]);
                    pool->stats.park_evictions_ttl++;
                }
                continue;
            }
            if (s->bucket_index != slot_bucket ||
                s->bucket_bytes != alloc_bytes ||
                s->permissions != req->permissions ||
                s->cache_mode != req->cache_mode ||
                s->alignment != effective_align)
                continue;
            if (s->client_id != req->client_id) {
                saw_other_client_match = 1;
                continue;
            }
            if (s->smmu_ctx != req->smmu_ctx)
                continue;
            /* Prefer most recently parked (hotter TLB/cache) for reuse. */
            if (match == NULL || s->lru_seq > match->lru_seq)
                match = s;
        }

        if (match != NULL) {
            A840KmdBoPoolSlot saved = *match;
            uint32_t needs_scrub = (!saved.scrubbed) ? 1u : 0u;
            uint64_t new_handle = next_bo_handle_locked(pool);

            if (pool->bucket_parked_count[saved.bucket_index] > 0u)
                pool->bucket_parked_count[saved.bucket_index]--;
            if (pool->stats.parked_bos > 0u)
                pool->stats.parked_bos--;
            if (pool->stats.parked_bytes >= saved.bucket_bytes)
                pool->stats.parked_bytes -= saved.bucket_bytes;
            else
                pool->stats.parked_bytes = 0u;

            if (needs_scrub) {
                /* Detach slot while scrubbing outside lock so no other thread can observe it. */
                memset(match, 0, sizeof(*match));
                pool_unlock(pool);

                run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
                    expired_ops, expired_count, 0u);

                status = run_zero_scrub_unlocked(&backing_ops, has_backing_ops,
                    saved.client_id, saved.phys_addr, saved.cpu_ptr,
                    saved.backing_handle, saved.bucket_bytes);
                if (status != A840_KMD_OK) {
                    BoDeferredOp fail_op;
                    record_deferred_unmap(&saved, &fail_op);
                    run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &fail_op, 1u, 0u);
                    return status;
                }

                pool_lock(pool);
                if (pool->initialized != A840_KMD_BO_POOL_MAGIC) {
                    pool_unlock(pool);
                    {
                        BoDeferredOp fail_op;
                        record_deferred_unmap(&saved, &fail_op);
                        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &fail_op, 1u, 0u);
                    }
                    return A840_KMD_STATE_CONFLICT;
                }
                match = find_free_slot_locked(pool);
                if (match == NULL) {
                    pool_unlock(pool);
                    {
                        BoDeferredOp fail_op;
                        record_deferred_unmap(&saved, &fail_op);
                        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &fail_op, 1u, 0u);
                    }
                    return A840_KMD_CAPACITY;
                }
                *match = saved;
                match->entry_state = A840_KMD_BO_ENTRY_ACTIVE;
                match->bo_handle = new_handle;
                match->requested_bytes = req->requested_bytes;
                match->purgeable_state = A840_PURGE_NONVOLATILE;
                match->in_flight_refs = 0u;
                match->scrubbed = 0u;
                match->pending_destroy = 0u;
                match->parked_at_ns = 0u;
                match->lru_seq = next_lru_seq_locked(pool);

                pool->stats.active_bos++;
                pool->stats.park_hits++;
                pool->stats.smmu_map_saved++;
                pool->stats.zero_scrubs++;
                fill_desc_from_slot(match, 1u, 1u, out_desc);
                pool_unlock(pool);
                return A840_KMD_OK;
            }

            /* Already pre-scrubbed on park: promote in place under lock. */
            match->entry_state = A840_KMD_BO_ENTRY_ACTIVE;
            match->bo_handle = new_handle;
            match->requested_bytes = req->requested_bytes;
            match->purgeable_state = A840_PURGE_NONVOLATILE;
            match->in_flight_refs = 0u;
            match->scrubbed = 0u;
            match->pending_destroy = 0u;
            match->parked_at_ns = 0u;
            match->lru_seq = next_lru_seq_locked(pool);

            pool->stats.active_bos++;
            pool->stats.park_hits++;
            pool->stats.smmu_map_saved++;
            fill_desc_from_slot(match, 1u, 1u, out_desc);
            pool_unlock(pool);

            run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
                expired_ops, expired_count, 0u);
            return A840_KMD_OK;
        }

        if (saw_other_client_match)
            pool->stats.cross_client_park_blocked++;
        pool->stats.park_misses++;
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
            expired_ops, expired_count, 0u);
    } else {
        pool_lock(pool);
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        pool->stats.park_misses++;
        pool_unlock(pool);
    }

    /* Miss path: allocate fresh backing + SMMU mapping strictly OUTSIDE lock. */
    {
        uint64_t gpu_va = 0u;
        uint64_t phys_addr = 0u;
        void *cpu_ptr = NULL;
        void *backing_handle = NULL;
        A840KmdBoPoolSlot *slot;
        BoDeferredOp evicted_op;
        int did_evict = 0;

        if (has_backing_ops) {
            status = backing_ops.alloc_and_map(backing_ops.cookie,
                req->client_id, req->smmu_ctx, alloc_bytes, effective_align,
                req->permissions, req->cache_mode,
                &gpu_va, &phys_addr, &cpu_ptr, &backing_handle);
            if (status != A840_KMD_OK)
                return status;
            if (gpu_va == 0u || (gpu_va % effective_align) != 0u ||
                gpu_va >= A840_KMD_GPU_VA_LIMIT ||
                alloc_bytes > A840_KMD_GPU_VA_LIMIT - gpu_va ||
                phys_addr == 0u || (phys_addr % A840_KMD_BO_POOL_MIN_ALIGN) != 0u) {
                backing_ops.unmap_and_free(backing_ops.cookie,
                    req->client_id, req->smmu_ctx, gpu_va, phys_addr,
                    backing_handle, alloc_bytes);
                return A840_KMD_IO_ERROR;
            }
        }

        pool_lock(pool);
        if (pool->initialized != A840_KMD_BO_POOL_MAGIC) {
            pool_unlock(pool);
            if (has_backing_ops) {
                backing_ops.unmap_and_free(backing_ops.cookie,
                    req->client_id, req->smmu_ctx, gpu_va, phys_addr,
                    backing_handle, alloc_bytes);
            }
            return A840_KMD_STATE_CONFLICT;
        }

        slot = find_free_slot_locked(pool);
        if (slot == NULL) {
            /* Reclaim one oldest parked slot to make room for an active BO. */
            A840KmdBoPoolSlot *victim = find_oldest_parked_locked(pool, -1);
            if (victim != NULL) {
                remove_parked_slot_locked(pool, victim, &evicted_op);
                pool->stats.park_evictions_cap++;
                did_evict = 1;
                slot = victim;
            }
        }
        if (slot == NULL) {
            pool_unlock(pool);
            if (has_backing_ops) {
                backing_ops.unmap_and_free(backing_ops.cookie,
                    req->client_id, req->smmu_ctx, gpu_va, phys_addr,
                    backing_handle, alloc_bytes);
            }
            return A840_KMD_CAPACITY;
        }

        if (!has_backing_ops) {
            uint64_t va_aligned = 0u;
            uint64_t phys_aligned = 0u;
            if (!align_up_u64(pool->next_synthetic_va, effective_align, &va_aligned) ||
                !align_up_u64(pool->next_synthetic_phys, effective_align, &phys_aligned) ||
                va_aligned >= A840_KMD_GPU_VA_LIMIT ||
                alloc_bytes > A840_KMD_GPU_VA_LIMIT - va_aligned) {
                pool_unlock(pool);
                if (did_evict)
                    run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &evicted_op, 1u, 0u);
                return A840_KMD_NO_MEMORY;
            }
            gpu_va = va_aligned;
            phys_addr = phys_aligned;
            pool->next_synthetic_va = va_aligned + alloc_bytes;
            pool->next_synthetic_phys = phys_aligned + alloc_bytes;
        }

        memset(slot, 0, sizeof(*slot));
        slot->entry_state = A840_KMD_BO_ENTRY_ACTIVE;
        slot->bucket_index = slot_bucket;
        slot->permissions = req->permissions;
        slot->cache_mode = req->cache_mode;
        slot->purgeable_state = A840_PURGE_NONVOLATILE;
        slot->in_flight_refs = 0u;
        slot->scrubbed = 0u;
        slot->pending_destroy = 0u;
        slot->bo_handle = next_bo_handle_locked(pool);
        slot->client_id = req->client_id;
        slot->smmu_ctx = req->smmu_ctx;
        slot->gpu_va = gpu_va;
        slot->phys_addr = phys_addr;
        slot->cpu_ptr = cpu_ptr;
        slot->backing_handle = backing_handle;
        slot->requested_bytes = req->requested_bytes;
        slot->bucket_bytes = alloc_bytes;
        slot->alignment = effective_align;
        slot->lru_seq = next_lru_seq_locked(pool);

        pool->stats.active_bos++;
        fill_desc_from_slot(slot, 0u, 1u, out_desc);
        pool_unlock(pool);

        if (did_evict)
            run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &evicted_op, 1u, 0u);
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_release(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t allow_park)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolSlot detached;
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint32_t scrub_on_park = 0u;
    int can_park = 0;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&detached, 0, sizeof(detached));
    memset(&backing_ops, 0, sizeof(backing_ops));

    pool_lock(pool);
    {
        A840KmdBoPoolSlot *slot = find_active_by_handle_locked(pool, bo_handle);
        if (slot == NULL) {
            pool_unlock(pool);
            return A840_KMD_NOT_FOUND;
        }
        if (slot->client_id != client_id) {
            pool_unlock(pool);
            return A840_KMD_PERMISSION_DENIED;
        }
        if (slot->in_flight_refs > 0u) {
            /* Defer destruction until all active GPU fence references retire. */
            slot->pending_destroy = 1u;
            pool_unlock(pool);
            return A840_KMD_OK;
        }

        detached = *slot;
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        scrub_on_park = pool->scrub_on_park;

        can_park = (allow_park != 0u &&
                    slot->bucket_index < A840_KMD_BO_POOL_BUCKET_COUNT &&
                    slot->purgeable_state != A840_PURGE_EMPTY &&
                    slot->bucket_bytes <= pool->max_parked_bytes &&
                    pool->max_parked_per_bucket > 0u);

        remove_active_slot_locked(pool, slot, NULL);
    }
    pool_unlock(pool);

    if (!can_park) {
        BoDeferredOp op;
        record_deferred_unmap(&detached, &op);
        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &op, 1u, 0u);
        return A840_KMD_OK;
    }

    /* Optional background/park scrub strictly OUTSIDE lock before exposing as PARKED. */
    if (scrub_on_park) {
        status = run_zero_scrub_unlocked(&backing_ops, has_backing_ops,
            detached.client_id, detached.phys_addr, detached.cpu_ptr,
            detached.backing_handle, detached.bucket_bytes);
        if (status != A840_KMD_OK) {
            BoDeferredOp op;
            record_deferred_unmap(&detached, &op);
            run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &op, 1u, 0u);
            return status;
        }
    }

    /* Re-acquire lock to enforce bucket/byte caps (LRU eviction) and park. */
    for (;;) {
        BoDeferredOp evict_chunk[A840_KMD_BO_POOL_DEFER_CHUNK];
        size_t evict_count = 0u;
        int need_more_evict = 0;

        pool_lock(pool);
        if (pool->initialized != A840_KMD_BO_POOL_MAGIC) {
            pool_unlock(pool);
            {
                BoDeferredOp op;
                record_deferred_unmap(&detached, &op);
                run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &op, 1u, 0u);
            }
            return A840_KMD_STATE_CONFLICT;
        }

        while (pool->bucket_parked_count[detached.bucket_index] >= pool->max_parked_per_bucket) {
            A840KmdBoPoolSlot *victim;
            if (evict_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                need_more_evict = 1;
                break;
            }
            victim = find_oldest_parked_locked(pool, (int)detached.bucket_index);
            if (victim == NULL)
                break;
            remove_parked_slot_locked(pool, victim, &evict_chunk[evict_count++]);
            pool->stats.park_evictions_cap++;
        }

        while (!need_more_evict &&
               (pool->stats.parked_bytes + detached.bucket_bytes) > pool->max_parked_bytes) {
            A840KmdBoPoolSlot *victim;
            if (evict_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                need_more_evict = 1;
                break;
            }
            victim = find_oldest_parked_locked(pool, -1);
            if (victim == NULL)
                break;
            remove_parked_slot_locked(pool, victim, &evict_chunk[evict_count++]);
            pool->stats.park_evictions_cap++;
        }

        if (!need_more_evict) {
            A840KmdBoPoolSlot *slot = find_free_slot_locked(pool);
            if (slot == NULL) {
                A840KmdBoPoolSlot *victim = find_oldest_parked_locked(pool, -1);
                if (victim != NULL && evict_count < A840_KMD_BO_POOL_DEFER_CHUNK) {
                    remove_parked_slot_locked(pool, victim, &evict_chunk[evict_count++]);
                    pool->stats.park_evictions_cap++;
                    slot = victim;
                }
            }
            if (slot != NULL) {
                *slot = detached;
                slot->entry_state = A840_KMD_BO_ENTRY_PARKED;
                slot->bo_handle = 0u;
                slot->purgeable_state = A840_PURGE_NONVOLATILE;
                slot->in_flight_refs = 0u;
                slot->pending_destroy = 0u;
                slot->scrubbed = scrub_on_park ? 1u : 0u;
                slot->parked_at_ns = pool_now_ns(pool);
                slot->lru_seq = next_lru_seq_locked(pool);

                pool->bucket_parked_count[slot->bucket_index]++;
                pool->stats.parked_bos++;
                pool->stats.parked_bytes += slot->bucket_bytes;
                if (scrub_on_park)
                    pool->stats.zero_scrubs++;
            } else {
                /* Pool slots all active: cannot park, unmap detached BO. */
                if (evict_count < A840_KMD_BO_POOL_DEFER_CHUNK) {
                    record_deferred_unmap(&detached, &evict_chunk[evict_count++]);
                } else {
                    pool_unlock(pool);
                    run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
                        evict_chunk, evict_count, 0u);
                    {
                        BoDeferredOp op;
                        record_deferred_unmap(&detached, &op);
                        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &op, 1u, 0u);
                    }
                    return A840_KMD_OK;
                }
            }
        }
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
            evict_chunk, evict_count, 0u);
        if (!need_more_evict)
            break;
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_set_purgeable(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdPurgeableState requested_state,
    A840KmdPurgeResult *out_result)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint64_t zero_page_phys = 0u;
    BoDeferredOp purge_op;
    int do_purge_callback = 0;
    int do_rehydrate = 0;
    A840KmdBoPoolSlot rehydrate_info;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u || out_result == NULL)
        return A840_KMD_INVALID_ARG;
    if (requested_state != A840_PURGE_KEEP_CURRENT &&
        requested_state != A840_PURGE_NONVOLATILE &&
        requested_state != A840_PURGE_VOLATILE &&
        requested_state != A840_PURGE_EMPTY)
        return A840_KMD_INVALID_ARG;

    memset(out_result, 0, sizeof(*out_result));
    memset(&backing_ops, 0, sizeof(backing_ops));
    memset(&purge_op, 0, sizeof(purge_op));
    memset(&rehydrate_info, 0, sizeof(rehydrate_info));

    if (requested_state == A840_PURGE_EMPTY)
        zero_page_phys = query_zero_page_phys_unlocked(pool->zero_page_mgr);

    pool_lock(pool);
    {
        A840KmdBoPoolSlot *slot = find_active_by_handle_locked(pool, bo_handle);
        uint32_t old_state;

        if (slot == NULL) {
            pool_unlock(pool);
            return A840_KMD_NOT_FOUND;
        }
        if (slot->client_id != client_id) {
            pool_unlock(pool);
            return A840_KMD_PERMISSION_DENIED;
        }
        if (slot->pending_destroy != 0u) {
            pool_unlock(pool);
            return A840_KMD_INVALID_STATE;
        }

        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;

        old_state = slot->purgeable_state;
        out_result->old_state = old_state;

        if (requested_state == A840_PURGE_KEEP_CURRENT) {
            out_result->new_state = old_state;
            out_result->was_purged = (old_state == A840_PURGE_EMPTY) ? 1u : 0u;
            pool_unlock(pool);
            return A840_KMD_OK;
        }

        if (requested_state == A840_PURGE_VOLATILE) {
            if (old_state == A840_PURGE_EMPTY) {
                /* EMPTY remains EMPTY until explicitly restored to NONVOLATILE. */
                out_result->new_state = A840_PURGE_EMPTY;
                out_result->was_purged = 1u;
            } else {
                if (old_state == A840_PURGE_NONVOLATILE)
                    pool->stats.volatile_bos++;
                slot->purgeable_state = A840_PURGE_VOLATILE;
                slot->lru_seq = next_lru_seq_locked(pool);
                out_result->new_state = A840_PURGE_VOLATILE;
                out_result->was_purged = 0u;
            }
            pool_unlock(pool);
            return A840_KMD_OK;
        }

        if (requested_state == A840_PURGE_EMPTY) {
            if (old_state == A840_PURGE_EMPTY) {
                out_result->new_state = A840_PURGE_EMPTY;
                out_result->was_purged = 1u;
                pool_unlock(pool);
                return A840_KMD_OK;
            }
            if (slot->in_flight_refs > 0u) {
                pool->stats.volatile_deferred_inflight++;
                pool_unlock(pool);
                return A840_KMD_STATE_CONFLICT;
            }
            if (old_state == A840_PURGE_VOLATILE && pool->stats.volatile_bos > 0u)
                pool->stats.volatile_bos--;
            record_deferred_unmap(slot, &purge_op);
            purge_op.is_purge = 1u;
            do_purge_callback = 1;

            slot->purgeable_state = A840_PURGE_EMPTY;
            slot->phys_addr = 0u;
            slot->cpu_ptr = NULL;
            slot->backing_handle = NULL;
            pool->stats.empty_bos++;
            pool->stats.volatile_purged_count++;

            out_result->new_state = A840_PURGE_EMPTY;
            out_result->was_purged = 1u;
            pool_unlock(pool);

            if (do_purge_callback) {
                run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
                    &purge_op, 1u, zero_page_phys);
            }
            return A840_KMD_OK;
        }

        /* requested_state == A840_PURGE_NONVOLATILE */
        if (old_state == A840_PURGE_NONVOLATILE) {
            out_result->new_state = A840_PURGE_NONVOLATILE;
            out_result->was_purged = 0u;
            pool_unlock(pool);
            return A840_KMD_OK;
        }
        if (old_state == A840_PURGE_VOLATILE) {
            slot->purgeable_state = A840_PURGE_NONVOLATILE;
            if (pool->stats.volatile_bos > 0u)
                pool->stats.volatile_bos--;
            out_result->new_state = A840_PURGE_NONVOLATILE;
            out_result->was_purged = 0u;
            pool_unlock(pool);
            return A840_KMD_OK;
        }

        /* Transitioning EMPTY -> NONVOLATILE: must report was_purged = 1. */
        if (has_backing_ops && backing_ops.rehydrate_from_empty != NULL) {
            rehydrate_info = *slot;
            do_rehydrate = 1;
        } else {
            if (!has_backing_ops) {
                uint64_t phys_aligned = 0u;
                if (!align_up_u64(pool->next_synthetic_phys, slot->alignment, &phys_aligned)) {
                    pool_unlock(pool);
                    return A840_KMD_NO_MEMORY;
                }
                slot->phys_addr = phys_aligned;
                pool->next_synthetic_phys = phys_aligned + slot->bucket_bytes;
            }
            slot->purgeable_state = A840_PURGE_NONVOLATILE;
            if (pool->stats.empty_bos > 0u)
                pool->stats.empty_bos--;
            out_result->new_state = A840_PURGE_NONVOLATILE;
            out_result->was_purged = 1u;
            pool_unlock(pool);
            return A840_KMD_OK;
        }
    }
    pool_unlock(pool);

    /* Rehydrate physical pages strictly OUTSIDE lock. */
    if (do_rehydrate) {
        uint64_t new_phys = 0u;
        void *new_cpu = NULL;
        void *new_handle = NULL;
        status = backing_ops.rehydrate_from_empty(backing_ops.cookie,
            rehydrate_info.client_id, rehydrate_info.smmu_ctx,
            rehydrate_info.gpu_va, rehydrate_info.bucket_bytes,
            rehydrate_info.permissions, rehydrate_info.cache_mode,
            &new_phys, &new_cpu, &new_handle);
        if (status != A840_KMD_OK)
            return status;
        if (new_phys == 0u || (new_phys % A840_KMD_BO_POOL_MIN_ALIGN) != 0u) {
            if (backing_ops.unmap_and_free != NULL) {
                backing_ops.unmap_and_free(backing_ops.cookie,
                    rehydrate_info.client_id, rehydrate_info.smmu_ctx,
                    rehydrate_info.gpu_va, new_phys, new_handle,
                    rehydrate_info.bucket_bytes);
            }
            return A840_KMD_IO_ERROR;
        }

        pool_lock(pool);
        {
            A840KmdBoPoolSlot *slot = find_active_by_handle_locked(pool, bo_handle);
            if (pool->initialized != A840_KMD_BO_POOL_MAGIC ||
                slot == NULL ||
                slot->client_id != client_id ||
                slot->pending_destroy != 0u ||
                slot->purgeable_state != A840_PURGE_EMPTY) {
                pool_unlock(pool);
                if (backing_ops.unmap_and_free != NULL) {
                    backing_ops.unmap_and_free(backing_ops.cookie,
                        rehydrate_info.client_id, rehydrate_info.smmu_ctx,
                        rehydrate_info.gpu_va, new_phys, new_handle,
                        rehydrate_info.bucket_bytes);
                }
                return A840_KMD_STATE_CONFLICT;
            }
            slot->phys_addr = new_phys;
            slot->cpu_ptr = new_cpu;
            slot->backing_handle = new_handle;
            slot->purgeable_state = A840_PURGE_NONVOLATILE;
            if (pool->stats.empty_bos > 0u)
                pool->stats.empty_bos--;
            out_result->old_state = A840_PURGE_EMPTY;
            out_result->new_state = A840_PURGE_NONVOLATILE;
            out_result->was_purged = 1u;
        }
        pool_unlock(pool);
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_begin_submit(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolSlot *slot;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    pool_lock(pool);
    slot = find_active_by_handle_locked(pool, bo_handle);
    if (slot == NULL) {
        pool_unlock(pool);
        return A840_KMD_NOT_FOUND;
    }
    if (slot->client_id != client_id) {
        pool_unlock(pool);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (slot->pending_destroy != 0u) {
        pool_unlock(pool);
        return A840_KMD_INVALID_STATE;
    }
    if (slot->purgeable_state == A840_PURGE_EMPTY) {
        pool->stats.empty_submit_rejects++;
        pool_unlock(pool);
        return A840_KMD_INVALID_STATE;
    }
    if (slot->in_flight_refs == UINT32_MAX) {
        pool_unlock(pool);
        return A840_KMD_RANGE;
    }
    slot->in_flight_refs++;
    if (out_in_flight_refs != NULL)
        *out_in_flight_refs = slot->in_flight_refs;
    pool_unlock(pool);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_end_submit(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolSlot *slot;
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    BoDeferredOp free_op;
    int destroy_now = 0;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&backing_ops, 0, sizeof(backing_ops));
    memset(&free_op, 0, sizeof(free_op));

    pool_lock(pool);
    slot = find_active_by_handle_locked(pool, bo_handle);
    if (slot == NULL) {
        pool_unlock(pool);
        return A840_KMD_NOT_FOUND;
    }
    if (slot->client_id != client_id) {
        pool_unlock(pool);
        return A840_KMD_PERMISSION_DENIED;
    }
    if (slot->in_flight_refs == 0u) {
        pool_unlock(pool);
        return A840_KMD_INVALID_STATE;
    }
    slot->in_flight_refs--;
    if (out_in_flight_refs != NULL)
        *out_in_flight_refs = slot->in_flight_refs;

    if (slot->in_flight_refs == 0u && slot->pending_destroy != 0u) {
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        remove_active_slot_locked(pool, slot, &free_op);
        destroy_now = 1;
    }
    pool_unlock(pool);

    if (destroy_now)
        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, &free_op, 1u, 0u);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_query_bo(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdBoDesc *out_desc)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolSlot *slot;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u || out_desc == NULL)
        return A840_KMD_INVALID_ARG;

    pool_lock(pool);
    slot = find_active_by_handle_locked(pool, bo_handle);
    if (slot == NULL) {
        pool_unlock(pool);
        return A840_KMD_NOT_FOUND;
    }
    if (slot->client_id != client_id) {
        pool_unlock(pool);
        return A840_KMD_PERMISSION_DENIED;
    }
    fill_desc_from_slot(slot, 0u, slot->scrubbed, out_desc);
    pool_unlock(pool);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_sweep_ttl(
    A840KmdBoPool *pool,
    uint32_t *out_evicted_count)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint32_t total_evicted = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;

    memset(&backing_ops, 0, sizeof(backing_ops));

    for (;;) {
        BoDeferredOp chunk[A840_KMD_BO_POOL_DEFER_CHUNK];
        size_t chunk_count = 0u;
        int more = 0;
        uint64_t now;

        pool_lock(pool);
        if (pool->parked_ttl_ns == 0u) {
            pool_unlock(pool);
            break;
        }
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;
        now = pool_now_ns(pool);

        for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
            A840KmdBoPoolSlot *s = &pool->slots[i];
            if (s->entry_state != A840_KMD_BO_ENTRY_PARKED)
                continue;
            if (now >= s->parked_at_ns && (now - s->parked_at_ns) >= pool->parked_ttl_ns) {
                if (chunk_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                    more = 1;
                    break;
                }
                remove_parked_slot_locked(pool, s, &chunk[chunk_count++]);
                pool->stats.park_evictions_ttl++;
                total_evicted++;
            }
        }
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, chunk, chunk_count, 0u);
        if (!more)
            break;
    }

    if (out_evicted_count != NULL)
        *out_evicted_count = total_evicted;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_memory_pressure(
    A840KmdBoPool *pool,
    uint64_t target_reclaim_bytes,
    A840KmdPressureReport *out_report)
{
    A840KmdStatus status = check_init(pool);
    A840KmdPressureReport report;
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint64_t zero_page_phys;

    if (status != A840_KMD_OK)
        return status;

    memset(&report, 0, sizeof(report));
    memset(&backing_ops, 0, sizeof(backing_ops));
    report.target_reclaim_bytes = target_reclaim_bytes;

    if (target_reclaim_bytes == 0u) {
        if (out_report != NULL)
            *out_report = report;
        return A840_KMD_OK;
    }

    /* Read zero-page physical address outside pool lock to avoid lock nesting. */
    zero_page_phys = query_zero_page_phys_unlocked(pool->zero_page_mgr);

    for (;;) {
        BoDeferredOp chunk[A840_KMD_BO_POOL_DEFER_CHUNK];
        size_t chunk_count = 0u;
        int more = 0;

        pool_lock(pool);
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;

        /* Stage 1: Reclaim Parked BOs first in strict LRU order. */
        while (report.reclaimed_total_bytes < target_reclaim_bytes) {
            A840KmdBoPoolSlot *victim;
            if (chunk_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                more = 1;
                break;
            }
            victim = find_oldest_parked_locked(pool, -1);
            if (victim == NULL)
                break;
            report.reclaimed_parked_bytes += victim->bucket_bytes;
            report.reclaimed_total_bytes += victim->bucket_bytes;
            report.parked_evicted_count++;
            pool->stats.park_evictions_pressure++;
            remove_parked_slot_locked(pool, victim, &chunk[chunk_count++]);
        }

        /* Stage 2: Reclaim VOLATILE BOs with in_flight_refs == 0 in LRU order. */
        while (!more && report.reclaimed_total_bytes < target_reclaim_bytes) {
            A840KmdBoPoolSlot *victim;
            if (chunk_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                more = 1;
                break;
            }
            victim = find_oldest_purgeable_volatile_locked(pool);
            if (victim == NULL)
                break;

            record_deferred_unmap(victim, &chunk[chunk_count]);
            chunk[chunk_count].is_purge = 1u;
            chunk_count++;

            victim->purgeable_state = A840_PURGE_EMPTY;
            victim->phys_addr = 0u;
            victim->cpu_ptr = NULL;
            victim->backing_handle = NULL;

            if (pool->stats.volatile_bos > 0u)
                pool->stats.volatile_bos--;
            pool->stats.empty_bos++;
            pool->stats.volatile_purged_count++;

            report.reclaimed_volatile_bytes += victim->bucket_bytes;
            report.reclaimed_total_bytes += victim->bucket_bytes;
            report.volatile_purged_count++;
        }

        /* Final pass: count VOLATILE BOs safely protected by in_flight_refs > 0. */
        if (!more && report.reclaimed_total_bytes < target_reclaim_bytes) {
            size_t i;
            for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
                const A840KmdBoPoolSlot *s = &pool->slots[i];
                if (s->entry_state == A840_KMD_BO_ENTRY_ACTIVE &&
                    s->purgeable_state == A840_PURGE_VOLATILE &&
                    s->in_flight_refs > 0u) {
                    report.volatile_skipped_inflight++;
                    pool->stats.volatile_deferred_inflight++;
                }
            }
        }
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops,
            chunk, chunk_count, zero_page_phys);
        if (!more)
            break;
    }

    if (out_report != NULL)
        *out_report = report;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_client_teardown(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint32_t *out_reclaimed_count)
{
    A840KmdStatus status = check_init(pool);
    A840KmdBoPoolBackingOps backing_ops;
    uint32_t has_backing_ops = 0u;
    uint32_t reclaimed = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&backing_ops, 0, sizeof(backing_ops));

    for (;;) {
        BoDeferredOp chunk[A840_KMD_BO_POOL_DEFER_CHUNK];
        size_t chunk_count = 0u;
        int more = 0;

        pool_lock(pool);
        has_backing_ops = pool->has_backing_ops;
        if (has_backing_ops)
            backing_ops = pool->backing_ops;

        for (i = 0u; i < A840_KMD_BO_POOL_MAX_SLOTS; ++i) {
            A840KmdBoPoolSlot *s = &pool->slots[i];
            if (s->entry_state == A840_KMD_BO_ENTRY_FREE || s->client_id != client_id)
                continue;
            if (chunk_count >= A840_KMD_BO_POOL_DEFER_CHUNK) {
                more = 1;
                break;
            }
            if (s->entry_state == A840_KMD_BO_ENTRY_PARKED)
                remove_parked_slot_locked(pool, s, &chunk[chunk_count++]);
            else
                remove_active_slot_locked(pool, s, &chunk[chunk_count++]);
            reclaimed++;
        }
        pool_unlock(pool);

        run_deferred_ops_unlocked(&backing_ops, has_backing_ops, chunk, chunk_count, 0u);
        if (!more)
            break;
    }

    if (out_reclaimed_count != NULL)
        *out_reclaimed_count = reclaimed;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_bo_pool_get_stats(
    A840KmdBoPool *pool,
    A840KmdBoPoolStats *out_stats)
{
    A840KmdStatus status = check_init(pool);
    if (status != A840_KMD_OK)
        return status;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    pool_lock(pool);
    *out_stats = pool->stats;
    pool_unlock(pool);
    return A840_KMD_OK;
}
