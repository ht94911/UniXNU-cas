/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_sparse_va.h"
#include <string.h>

#define A840_KMD_SPARSE_MAGIC 1397768530u
#define A840_KMD_SPARSE_UNBIND_CHUNK 64u
#define A840_KMD_NORMAL_VA_ALIGN UINT64_C(4096)

static void sparse_lock(A840KmdSparseManager *mgr)
{
    if (mgr->has_custom_sync) {
        mgr->sync_ops.lock(mgr->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&mgr->lock);
#endif
}

static void sparse_unlock(A840KmdSparseManager *mgr)
{
    if (mgr->has_custom_sync) {
        mgr->sync_ops.unlock(mgr->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&mgr->lock);
#endif
}

static A840KmdStatus check_init(const A840KmdSparseManager *mgr)
{
    if (mgr == NULL)
        return A840_KMD_INVALID_ARG;
    if (mgr->initialized != A840_KMD_SPARSE_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int ranges_overlap(uint64_t a_va, uint64_t a_len, uint64_t b_va, uint64_t b_len)
{
    uint64_t a_end = a_va + a_len;
    uint64_t b_end = b_va + b_len;
    return (a_va < b_end) && (b_va < a_end);
}

static int bytes_all_zero(const void *ptr, size_t len)
{
    const unsigned char *p = (const unsigned char *)ptr;
    size_t i;
    for (i = 0u; i < len; ++i) {
        if (p[i] != 0u)
            return 0;
    }
    return 1;
}

static A840KmdStatus validate_sparse_range(uint64_t gpu_va, uint64_t length_bytes)
{
    if (gpu_va == 0u || length_bytes == 0u)
        return A840_KMD_INVALID_ARG;
    if ((gpu_va % A840_KMD_SPARSE_TILE_ALIGN) != 0u ||
        (length_bytes % A840_KMD_SPARSE_TILE_BYTES) != 0u)
        return A840_KMD_ALIGNMENT;
    if (gpu_va < A840_KMD_SPARSE_VA_MIN ||
        gpu_va >= A840_KMD_SPARSE_VA_LIMIT ||
        length_bytes > A840_KMD_SPARSE_VA_LIMIT - gpu_va)
        return A840_KMD_RANGE;
    return A840_KMD_OK;
}

static A840KmdStatus validate_normal_range(uint64_t gpu_va, uint64_t length_bytes)
{
    if (gpu_va == 0u || length_bytes == 0u)
        return A840_KMD_INVALID_ARG;
    if ((gpu_va % A840_KMD_NORMAL_VA_ALIGN) != 0u ||
        (length_bytes % A840_KMD_NORMAL_VA_ALIGN) != 0u)
        return A840_KMD_ALIGNMENT;
    if (gpu_va >= A840_KMD_SPARSE_VA_LIMIT ||
        length_bytes > A840_KMD_SPARSE_VA_LIMIT - gpu_va)
        return A840_KMD_RANGE;
    return A840_KMD_OK;
}

static A840KmdStatus validate_permissions(uint32_t permissions)
{
    if (permissions == 0u ||
        (permissions & ~(uint32_t)A840_KMD_SMMU_PERM_READ_WRITE) != 0u)
        return A840_KMD_INVALID_ARG;
    return A840_KMD_OK;
}

static A840KmdSparseBoEntry *find_bo_locked(
    A840KmdSparseManager *mgr, uint64_t client_id, uint64_t bo_handle)
{
    size_t i;
    for (i = 0u; i < A840_KMD_SPARSE_MAX_BOS; ++i) {
        if (mgr->bos[i].active &&
            mgr->bos[i].client_id == client_id &&
            mgr->bos[i].bo_handle == bo_handle)
            return &mgr->bos[i];
    }
    return NULL;
}

static int bo_owned_by_other_client_locked(
    const A840KmdSparseManager *mgr, uint64_t client_id, uint64_t bo_handle)
{
    size_t i;
    for (i = 0u; i < A840_KMD_SPARSE_MAX_BOS; ++i) {
        if (mgr->bos[i].active &&
            mgr->bos[i].bo_handle == bo_handle &&
            mgr->bos[i].client_id != client_id)
            return 1;
    }
    return 0;
}

static A840KmdSparseRegion *find_region_by_id_locked(
    A840KmdSparseManager *mgr, uint64_t region_id)
{
    size_t i;
    if (region_id == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        if (mgr->regions[i].active && mgr->regions[i].region_id == region_id)
            return &mgr->regions[i];
    }
    return NULL;
}

static A840KmdSparseRegion *find_region_containing_locked(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    uint64_t length_bytes)
{
    size_t i;
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        A840KmdSparseRegion *r = &mgr->regions[i];
        if (!r->active || r->client_id != client_id)
            continue;
        if (gpu_va >= r->gpu_va &&
            length_bytes <= r->length_bytes &&
            (gpu_va - r->gpu_va) <= (r->length_bytes - length_bytes))
            return r;
    }
    return NULL;
}

static int region_va_owned_by_other_client_locked(
    const A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    uint64_t length_bytes)
{
    size_t i;
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        const A840KmdSparseRegion *r = &mgr->regions[i];
        if (!r->active || r->client_id == client_id)
            continue;
        if (ranges_overlap(r->gpu_va, r->length_bytes, gpu_va, length_bytes))
            return 1;
    }
    return 0;
}

static void fill_tile_info(
    const A840KmdSparseRegion *region,
    uint32_t tile_idx,
    A840KmdSparseTileInfo *out_info)
{
    const A840KmdSparseTileSlot *slot = &region->tiles[tile_idx];
    memset(out_info, 0, sizeof(*out_info));
    out_info->gpu_va = region->gpu_va + ((uint64_t)tile_idx * A840_KMD_SPARSE_TILE_BYTES);
    out_info->bo_handle = slot->is_zero_page ? 0u : slot->bo_handle;
    out_info->bo_offset = slot->is_zero_page ? 0u : slot->bo_offset;
    out_info->phys_addr = slot->phys_addr;
    out_info->is_zero_page = slot->is_zero_page ? 1u : 0u;
    out_info->permissions = slot->is_zero_page ? A840_KMD_SMMU_PERM_READ_ONLY : slot->permissions;
    out_info->state = slot->is_zero_page ? A840_KMD_SPARSE_TILE_ZERO : A840_KMD_SPARSE_TILE_BOUND;
    out_info->reserved = 0u;
}

static void decrement_bo_ref_locked(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle)
{
    A840KmdSparseBoEntry *bo;
    if (bo_handle == 0u)
        return;
    bo = find_bo_locked(mgr, client_id, bo_handle);
    if (bo != NULL && bo->sparse_refcount > 0u)
        bo->sparse_refcount--;
}

static uint64_t bump_tlb_gen_locked(A840KmdSparseManager *mgr)
{
    mgr->tlb_flush_generation++;
    if (mgr->tlb_flush_generation == 0u)
        mgr->tlb_flush_generation = 1u;
    mgr->stats.tlb_flush_generation = mgr->tlb_flush_generation;
    return mgr->tlb_flush_generation;
}

A840KmdStatus a840_kmd_sparse_init(
    A840KmdSparseManager *mgr,
    const A840KmdSparseConfig *config)
{
    if (mgr == NULL || config == NULL || config->zero_page_mgr == NULL)
        return A840_KMD_INVALID_ARG;

    if (config->sync_ops != NULL) {
        if (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    } else {
#if defined(A840_KMD_CUSTOM_SYNC)
        return A840_KMD_INVALID_ARG;
#endif
    }

    memset(mgr, 0, sizeof(*mgr));
    mgr->zero_page_mgr = config->zero_page_mgr;
    mgr->external_overlap_fn = config->external_overlap_fn;
    mgr->external_overlap_cookie = config->external_overlap_cookie;

    if (config->sync_ops != NULL) {
        mgr->has_custom_sync = 1u;
        mgr->sync_ops = *config->sync_ops;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    else {
        if (pthread_mutex_init(&mgr->lock, NULL) != 0)
            return A840_KMD_IO_ERROR;
    }
#endif

    if (config->smmu_ops != NULL) {
        mgr->has_smmu_ops = 1u;
        mgr->smmu_ops = *config->smmu_ops;
    }

    mgr->next_region_id = 1u;
    mgr->tlb_flush_generation = 0u;
    mgr->initialized = A840_KMD_SPARSE_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_sparse_destroy(A840KmdSparseManager *mgr)
{
    typedef struct PendingUnmap {
        uint64_t client_id;
        void *smmu_ctx;
        A840KmdSparsePteRange range;
    } PendingUnmap;
    PendingUnmap pending[A840_KMD_SPARSE_MAX_REGIONS];
    size_t pending_count = 0u;
    A840KmdSparseSmmuOps smmu_ops;
    uint32_t has_smmu_ops = 0u;
    uint32_t has_custom_sync = 0u;
    uint64_t gen = 0u;
    size_t i;

    if (check_init(mgr) != A840_KMD_OK)
        return;

    memset(&smmu_ops, 0, sizeof(smmu_ops));
    sparse_lock(mgr);
    if (mgr->initialized != A840_KMD_SPARSE_MAGIC) {
        sparse_unlock(mgr);
        return;
    }
    has_smmu_ops = mgr->has_smmu_ops;
    if (has_smmu_ops)
        smmu_ops = mgr->smmu_ops;
    has_custom_sync = mgr->has_custom_sync;

    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        if (mgr->regions[i].active) {
            if (pending_count == 0u)
                gen = bump_tlb_gen_locked(mgr);
            pending[pending_count].client_id = mgr->regions[i].client_id;
            pending[pending_count].smmu_ctx = mgr->regions[i].smmu_ctx;
            memset(&pending[pending_count].range, 0, sizeof(pending[pending_count].range));
            pending[pending_count].range.gpu_va = mgr->regions[i].gpu_va;
            pending[pending_count].range.length_bytes = mgr->regions[i].length_bytes;
            pending[pending_count].range.tlb_generation = gen;
            pending[pending_count].range.unmap_region = 1u;
            pending_count++;
        }
    }

    mgr->initialized = 0u;
    sparse_unlock(mgr);

    if (has_smmu_ops && pending_count > 0u) {
        for (i = 0u; i < pending_count; ++i) {
            if (smmu_ops.commit_ranges != NULL) {
                (void)smmu_ops.commit_ranges(smmu_ops.cookie,
                    pending[i].client_id, pending[i].smmu_ctx,
                    &pending[i].range, 1u);
            }
            if (smmu_ops.tlb_flush != NULL) {
                smmu_ops.tlb_flush(smmu_ops.cookie,
                    pending[i].client_id, pending[i].smmu_ctx, gen);
            }
        }
    }

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!has_custom_sync)
        (void)pthread_mutex_destroy(&mgr->lock);
#else
    (void)has_custom_sync;
#endif
    memset(mgr, 0, sizeof(*mgr));
}

A840KmdStatus a840_kmd_sparse_register_bo(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t phys_base,
    uint64_t bytes,
    uint32_t permissions)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseBoEntry *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u || bytes == 0u || phys_base == 0u)
        return A840_KMD_INVALID_ARG;
    if ((phys_base % A840_KMD_SPARSE_TILE_ALIGN) != 0u ||
        (bytes % A840_KMD_SPARSE_TILE_BYTES) != 0u)
        return A840_KMD_ALIGNMENT;
    if (phys_base >= A840_KMD_SPARSE_VA_LIMIT ||
        bytes > A840_KMD_SPARSE_VA_LIMIT - phys_base)
        return A840_KMD_RANGE;
    status = validate_permissions(permissions);
    if (status != A840_KMD_OK)
        return status;

    sparse_lock(mgr);
    if (find_bo_locked(mgr, client_id, bo_handle) != NULL) {
        sparse_unlock(mgr);
        return A840_KMD_STATE_CONFLICT;
    }
    for (i = 0u; i < A840_KMD_SPARSE_MAX_BOS; ++i) {
        if (!mgr->bos[i].active) {
            free_slot = &mgr->bos[i];
            break;
        }
    }
    if (free_slot == NULL) {
        sparse_unlock(mgr);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->permissions = permissions;
    free_slot->sparse_refcount = 0u;
    free_slot->client_id = client_id;
    free_slot->bo_handle = bo_handle;
    free_slot->phys_base = phys_base;
    free_slot->bytes = bytes;
    mgr->stats.active_bos++;
    sparse_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_get_bo_refcount(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_refcount)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseBoEntry *bo;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u || out_refcount == NULL)
        return A840_KMD_INVALID_ARG;

    sparse_lock(mgr);
    bo = find_bo_locked(mgr, client_id, bo_handle);
    if (bo == NULL) {
        if (bo_owned_by_other_client_locked(mgr, client_id, bo_handle)) {
            mgr->stats.rejected_cross_client++;
            sparse_unlock(mgr);
            return A840_KMD_PERMISSION_DENIED;
        }
        sparse_unlock(mgr);
        return A840_KMD_NOT_FOUND;
    }
    *out_refcount = bo->sparse_refcount;
    sparse_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_register_normal_range(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    uint64_t length_bytes)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseNormalRange *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;
    status = validate_normal_range(gpu_va, length_bytes);
    if (status != A840_KMD_OK)
        return status;

    sparse_lock(mgr);
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        if (mgr->regions[i].active &&
            mgr->regions[i].client_id == client_id &&
            ranges_overlap(mgr->regions[i].gpu_va, mgr->regions[i].length_bytes,
                gpu_va, length_bytes)) {
            sparse_unlock(mgr);
            return A840_KMD_STATE_CONFLICT;
        }
    }
    for (i = 0u; i < A840_KMD_SPARSE_MAX_NORMAL_RANGES; ++i) {
        if (mgr->normal_ranges[i].active) {
            if (mgr->normal_ranges[i].client_id == client_id &&
                ranges_overlap(mgr->normal_ranges[i].gpu_va,
                    mgr->normal_ranges[i].length_bytes, gpu_va, length_bytes)) {
                sparse_unlock(mgr);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &mgr->normal_ranges[i];
        }
    }
    if (free_slot == NULL) {
        sparse_unlock(mgr);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->client_id = client_id;
    free_slot->gpu_va = gpu_va;
    free_slot->length_bytes = length_bytes;
    sparse_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_unregister_normal_range(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va)
{
    A840KmdStatus status = check_init(mgr);
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || gpu_va == 0u)
        return A840_KMD_INVALID_ARG;

    sparse_lock(mgr);
    for (i = 0u; i < A840_KMD_SPARSE_MAX_NORMAL_RANGES; ++i) {
        if (mgr->normal_ranges[i].active &&
            mgr->normal_ranges[i].client_id == client_id &&
            mgr->normal_ranges[i].gpu_va == gpu_va) {
            memset(&mgr->normal_ranges[i], 0, sizeof(mgr->normal_ranges[i]));
            sparse_unlock(mgr);
            return A840_KMD_OK;
        }
    }
    sparse_unlock(mgr);
    return A840_KMD_NOT_FOUND;
}

A840KmdStatus a840_kmd_sparse_region_create(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t gpu_va,
    uint64_t length_bytes,
    uint64_t *out_region_id)
{
    A840KmdStatus status = check_init(mgr);
    struct NativeAdrenoAbiZeroPageReplyV3 zp_reply;
    A840KmdZeroPageStats zp_stats;
    A840KmdSparseRegion *free_slot = NULL;
    A840KmdSparsePteRange init_range;
    A840KmdSparseSmmuOps smmu_ops;
    uint32_t has_smmu_ops = 0u;
    uint64_t tile_count_u64;
    uint32_t tile_count;
    uint64_t region_id = 0u;
    uint64_t gen = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_region_id == NULL)
        return A840_KMD_INVALID_ARG;

    status = validate_sparse_range(gpu_va, length_bytes);
    if (status != A840_KMD_OK) {
        sparse_lock(mgr);
        mgr->stats.rejected_invalid_args++;
        sparse_unlock(mgr);
        return status;
    }

    tile_count_u64 = length_bytes / A840_KMD_SPARSE_TILE_BYTES;
    if (tile_count_u64 == 0u || tile_count_u64 > A840_KMD_SPARSE_MAX_TILES_PER_REGION) {
        sparse_lock(mgr);
        mgr->stats.rejected_invalid_args++;
        sparse_unlock(mgr);
        return A840_KMD_RANGE;
    }
    tile_count = (uint32_t)tile_count_u64;

    /* Read-only zero-page lookup strictly OUTSIDE sparse manager lock. */
    memset(&zp_reply, 0, sizeof(zp_reply));
    memset(&zp_stats, 0, sizeof(zp_stats));
    status = a840_kmd_zero_page_query_client(mgr->zero_page_mgr, client_id, &zp_reply);
    if (status != A840_KMD_OK)
        return status;
    status = a840_kmd_zero_page_get_stats(mgr->zero_page_mgr, &zp_stats);
    if (status != A840_KMD_OK)
        return status;
    if (!zp_stats.backing_allocated ||
        zp_stats.backing_phys_addr == 0u ||
        (zp_stats.backing_phys_addr % A840_KMD_SPARSE_TILE_ALIGN) != 0u ||
        zp_reply.bytes != A840_KMD_SPARSE_TILE_BYTES)
        return A840_KMD_STATE_CONFLICT;

    /* External non-sparse VA overlap check strictly OUTSIDE sparse manager lock. */
    if (mgr->external_overlap_fn != NULL &&
        mgr->external_overlap_fn(mgr->external_overlap_cookie,
            client_id, gpu_va, length_bytes) != 0)
        return A840_KMD_STATE_CONFLICT;

    memset(&smmu_ops, 0, sizeof(smmu_ops));
    memset(&init_range, 0, sizeof(init_range));

    sparse_lock(mgr);
    if (ranges_overlap(zp_reply.gpu_va, zp_reply.bytes, gpu_va, length_bytes)) {
        sparse_unlock(mgr);
        return A840_KMD_STATE_CONFLICT;
    }
    for (i = 0u; i < A840_KMD_SPARSE_MAX_NORMAL_RANGES; ++i) {
        if (mgr->normal_ranges[i].active &&
            mgr->normal_ranges[i].client_id == client_id &&
            ranges_overlap(mgr->normal_ranges[i].gpu_va,
                mgr->normal_ranges[i].length_bytes, gpu_va, length_bytes)) {
            sparse_unlock(mgr);
            return A840_KMD_STATE_CONFLICT;
        }
    }
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        if (mgr->regions[i].active) {
            if (mgr->regions[i].client_id == client_id &&
                ranges_overlap(mgr->regions[i].gpu_va,
                    mgr->regions[i].length_bytes, gpu_va, length_bytes)) {
                sparse_unlock(mgr);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &mgr->regions[i];
        }
    }
    if (free_slot == NULL) {
        sparse_unlock(mgr);
        return A840_KMD_CAPACITY;
    }

    region_id = mgr->next_region_id++;
    if (region_id == 0u)
        region_id = mgr->next_region_id++;

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->tile_count = tile_count;
    free_slot->region_id = region_id;
    free_slot->client_id = client_id;
    free_slot->smmu_ctx = smmu_ctx;
    free_slot->gpu_va = gpu_va;
    free_slot->length_bytes = length_bytes;
    free_slot->zero_page_va = zp_reply.gpu_va;
    free_slot->zero_page_phys = zp_stats.backing_phys_addr;

    for (i = 0u; i < (size_t)tile_count; ++i) {
        free_slot->tiles[i].bo_handle = 0u;
        free_slot->tiles[i].bo_offset = 0u;
        free_slot->tiles[i].phys_addr = zp_stats.backing_phys_addr;
        free_slot->tiles[i].permissions = A840_KMD_SMMU_PERM_READ_ONLY;
        free_slot->tiles[i].is_zero_page = 1u;
    }

    gen = bump_tlb_gen_locked(mgr);
    mgr->stats.active_regions++;
    mgr->stats.active_zero_tiles += tile_count;
    mgr->stats.total_regions_created++;
    mgr->stats.total_tlb_flushes++;

    init_range.gpu_va = gpu_va;
    init_range.length_bytes = length_bytes;
    init_range.phys_addr = zp_stats.backing_phys_addr;
    init_range.tlb_generation = gen;
    init_range.permissions = A840_KMD_SMMU_PERM_READ_ONLY;
    init_range.is_zero_page = 1u;
    init_range.unmap_region = 0u;

    has_smmu_ops = mgr->has_smmu_ops;
    if (has_smmu_ops)
        smmu_ops = mgr->smmu_ops;
    sparse_unlock(mgr);

    if (has_smmu_ops && smmu_ops.commit_ranges != NULL) {
        status = smmu_ops.commit_ranges(smmu_ops.cookie, client_id, smmu_ctx,
            &init_range, 1u);
        if (status != A840_KMD_OK) {
            /* Roll back newly created region if initial zero-page SMMU map failed. */
            sparse_lock(mgr);
            {
                A840KmdSparseRegion *rb = find_region_by_id_locked(mgr, region_id);
                if (rb != NULL && rb->client_id == client_id) {
                    for (i = 0u; i < (size_t)rb->tile_count; ++i) {
                        if (!rb->tiles[i].is_zero_page) {
                            decrement_bo_ref_locked(mgr, client_id, rb->tiles[i].bo_handle);
                            if (mgr->stats.active_bound_tiles > 0u)
                                mgr->stats.active_bound_tiles--;
                        } else if (mgr->stats.active_zero_tiles > 0u) {
                            mgr->stats.active_zero_tiles--;
                        }
                    }
                    if (mgr->stats.active_regions > 0u)
                        mgr->stats.active_regions--;
                    memset(rb, 0, sizeof(*rb));
                }
            }
            sparse_unlock(mgr);
            return status;
        }
    }
    if (has_smmu_ops && smmu_ops.tlb_flush != NULL)
        smmu_ops.tlb_flush(smmu_ops.cookie, client_id, smmu_ctx, gen);

    *out_region_id = region_id;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_region_destroy(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t region_id)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseRegion *r;
    A840KmdSparsePteRange unmap_range;
    A840KmdSparseSmmuOps smmu_ops;
    uint32_t has_smmu_ops = 0u;
    void *smmu_ctx = NULL;
    uint64_t gen = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || region_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&smmu_ops, 0, sizeof(smmu_ops));
    memset(&unmap_range, 0, sizeof(unmap_range));

    sparse_lock(mgr);
    r = find_region_by_id_locked(mgr, region_id);
    if (r == NULL) {
        sparse_unlock(mgr);
        return A840_KMD_NOT_FOUND;
    }
    if (r->client_id != client_id) {
        mgr->stats.rejected_cross_client++;
        sparse_unlock(mgr);
        return A840_KMD_PERMISSION_DENIED;
    }

    for (i = 0u; i < (size_t)r->tile_count; ++i) {
        if (!r->tiles[i].is_zero_page) {
            decrement_bo_ref_locked(mgr, client_id, r->tiles[i].bo_handle);
            if (mgr->stats.active_bound_tiles > 0u)
                mgr->stats.active_bound_tiles--;
        } else if (mgr->stats.active_zero_tiles > 0u) {
            mgr->stats.active_zero_tiles--;
        }
    }

    gen = bump_tlb_gen_locked(mgr);
    smmu_ctx = r->smmu_ctx;
    unmap_range.gpu_va = r->gpu_va;
    unmap_range.length_bytes = r->length_bytes;
    unmap_range.tlb_generation = gen;
    unmap_range.unmap_region = 1u;

    if (mgr->stats.active_regions > 0u)
        mgr->stats.active_regions--;
    mgr->stats.total_regions_destroyed++;
    mgr->stats.total_tlb_flushes++;

    memset(r, 0, sizeof(*r));
    has_smmu_ops = mgr->has_smmu_ops;
    if (has_smmu_ops)
        smmu_ops = mgr->smmu_ops;
    sparse_unlock(mgr);

    if (has_smmu_ops) {
        if (smmu_ops.commit_ranges != NULL) {
            status = smmu_ops.commit_ranges(smmu_ops.cookie, client_id,
                smmu_ctx, &unmap_range, 1u);
            if (status != A840_KMD_OK)
                return status;
        }
        if (smmu_ops.tlb_flush != NULL)
            smmu_ops.tlb_flush(smmu_ops.cookie, client_id, smmu_ctx, gen);
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_dispatch_v3(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    const A840KmdSparseBindReqV3 *req,
    size_t req_bytes,
    A840KmdSparseBindReplyV3 *reply,
    size_t reply_bytes)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseRegion *op_regions[A840_KMD_SPARSE_MAX_BATCH_OPS];
    A840KmdSparseBoEntry *op_bos[A840_KMD_SPARSE_MAX_BATCH_OPS];
    A840KmdSparsePteRange pte_batch[A840_KMD_SPARSE_MAX_BATCH_OPS];
    void *pte_smmu_ctxs[A840_KMD_SPARSE_MAX_BATCH_OPS];
    size_t pte_count = 0u;
    A840KmdSparseSmmuOps smmu_ops;
    uint32_t has_smmu_ops = 0u;
    uint32_t mutated = 0u;
    uint64_t gen = 0u;
    uint64_t zero_page_va = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || req == NULL || reply == NULL)
        return A840_KMD_INVALID_ARG;
    if (req_bytes != sizeof(A840KmdSparseBindReqV3) ||
        reply_bytes != sizeof(A840KmdSparseBindReplyV3) ||
        req_bytes > NativeAdrenoAbiEnvelopeBytes ||
        reply_bytes > NativeAdrenoAbiEnvelopeBytes)
        return A840_KMD_RANGE;

    memset(reply, 0, sizeof(*reply));
    memset(&smmu_ops, 0, sizeof(smmu_ops));

    if (req->version != NativeAdrenoAbiVersionV3 ||
        req->op_count == 0u ||
        req->op_count > A840_KMD_SPARSE_MAX_BATCH_OPS ||
        (req->flags & ~(uint32_t)A840_KMD_SPARSE_REQ_FLAG_KNOWN_MASK) != 0u ||
        req->reserved0 != 0u ||
        req->reserved1 != 0u) {
        sparse_lock(mgr);
        mgr->stats.rejected_invalid_args++;
        sparse_unlock(mgr);
        return A840_KMD_INVALID_ARG;
    }

    if (req->op_count < A840_KMD_SPARSE_MAX_BATCH_OPS) {
        size_t tail_count = (size_t)A840_KMD_SPARSE_MAX_BATCH_OPS - (size_t)req->op_count;
        if (!bytes_all_zero(&req->ops[req->op_count], tail_count * sizeof(req->ops[0]))) {
            sparse_lock(mgr);
            mgr->stats.rejected_invalid_args++;
            sparse_unlock(mgr);
            return A840_KMD_INVALID_ARG;
        }
    }

    sparse_lock(mgr);

    /* If req->region_id is specified, verify ownership up front. */
    if (req->region_id != 0u) {
        A840KmdSparseRegion *target = find_region_by_id_locked(mgr, req->region_id);
        if (target == NULL) {
            sparse_unlock(mgr);
            return A840_KMD_NOT_FOUND;
        }
        if (target->client_id != client_id) {
            mgr->stats.rejected_cross_client++;
            sparse_unlock(mgr);
            return A840_KMD_PERMISSION_DENIED;
        }
    }

    /* Phase 1: Validate every operation fail-closed before mutating any state. */
    for (i = 0u; i < (size_t)req->op_count; ++i) {
        const A840KmdSparseBindOp *op = &req->ops[i];
        A840KmdSparseRegion *region;
        A840KmdSparseBoEntry *bo = NULL;

        if (op->reserved0 != 0u) {
            mgr->stats.rejected_invalid_args++;
            sparse_unlock(mgr);
            return A840_KMD_INVALID_ARG;
        }

        status = validate_sparse_range(op->gpu_va, op->length_bytes);
        if (status != A840_KMD_OK) {
            mgr->stats.rejected_invalid_args++;
            sparse_unlock(mgr);
            return status;
        }

        region = find_region_containing_locked(mgr, client_id, op->gpu_va, op->length_bytes);
        if (region == NULL) {
            if (req->region_id != 0u) {
                A840KmdSparseRegion *target = find_region_by_id_locked(mgr, req->region_id);
                if (target != NULL && target->client_id == client_id) {
                    mgr->stats.rejected_invalid_args++;
                    sparse_unlock(mgr);
                    return A840_KMD_RANGE;
                }
            }
            if (region_va_owned_by_other_client_locked(mgr, client_id,
                    op->gpu_va, op->length_bytes)) {
                mgr->stats.rejected_cross_client++;
                sparse_unlock(mgr);
                return A840_KMD_PERMISSION_DENIED;
            }
            sparse_unlock(mgr);
            return A840_KMD_NOT_FOUND;
        }
        if (req->region_id != 0u && region->region_id != req->region_id) {
            mgr->stats.rejected_invalid_args++;
            sparse_unlock(mgr);
            return A840_KMD_RANGE;
        }

        switch (op->op) {
        case A840_KMD_SPARSE_OP_BIND_BO:
            status = validate_permissions(op->flags);
            if (status != A840_KMD_OK) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return status;
            }
            if (op->bo_handle == 0u) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return A840_KMD_INVALID_ARG;
            }
            if ((op->bo_offset % A840_KMD_SPARSE_TILE_ALIGN) != 0u) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return A840_KMD_ALIGNMENT;
            }
            bo = find_bo_locked(mgr, client_id, op->bo_handle);
            if (bo == NULL) {
                if (bo_owned_by_other_client_locked(mgr, client_id, op->bo_handle)) {
                    mgr->stats.rejected_cross_client++;
                    sparse_unlock(mgr);
                    return A840_KMD_PERMISSION_DENIED;
                }
                sparse_unlock(mgr);
                return A840_KMD_NOT_FOUND;
            }
            if (op->bo_offset > bo->bytes ||
                op->length_bytes > bo->bytes - op->bo_offset) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return A840_KMD_RANGE;
            }
            if ((op->flags & A840_KMD_SMMU_PERM_WRITE) != 0u &&
                (bo->permissions & A840_KMD_SMMU_PERM_WRITE) == 0u) {
                sparse_unlock(mgr);
                return A840_KMD_PERMISSION_DENIED;
            }
            break;

        case A840_KMD_SPARSE_OP_BIND_ZERO_PAGE:
        case A840_KMD_SPARSE_OP_UNBIND_TO_ZERO:
            if ((op->flags & A840_KMD_SMMU_PERM_WRITE) != 0u) {
                mgr->stats.rejected_zero_page_writes++;
                sparse_unlock(mgr);
                return A840_KMD_PERMISSION_DENIED;
            }
            if ((op->flags & ~(uint32_t)A840_KMD_SMMU_PERM_READ_ONLY) != 0u ||
                op->bo_handle != 0u ||
                op->bo_offset != 0u) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return A840_KMD_INVALID_ARG;
            }
            break;

        case A840_KMD_SPARSE_OP_QUERY_TILE:
            if (op->flags != 0u ||
                op->bo_handle != 0u ||
                op->bo_offset != 0u ||
                op->length_bytes != A840_KMD_SPARSE_TILE_BYTES) {
                mgr->stats.rejected_invalid_args++;
                sparse_unlock(mgr);
                return A840_KMD_INVALID_ARG;
            }
            break;

        default:
            mgr->stats.rejected_invalid_args++;
            sparse_unlock(mgr);
            return A840_KMD_INVALID_ARG;
        }

        op_regions[i] = region;
        op_bos[i] = bo;
    }

    /* Phase 2: Commit all validated operations atomically under lock. */
    for (i = 0u; i < (size_t)req->op_count; ++i) {
        const A840KmdSparseBindOp *op = &req->ops[i];
        A840KmdSparseRegion *region = op_regions[i];
        A840KmdSparseBoEntry *bo = op_bos[i];
        uint32_t start_tile = (uint32_t)((op->gpu_va - region->gpu_va) / A840_KMD_SPARSE_TILE_BYTES);
        uint32_t tile_cnt = (uint32_t)(op->length_bytes / A840_KMD_SPARSE_TILE_BYTES);
        uint32_t t;

        if (zero_page_va == 0u)
            zero_page_va = region->zero_page_va;

        if (op->op == A840_KMD_SPARSE_OP_BIND_BO) {
            for (t = 0u; t < tile_cnt; ++t) {
                A840KmdSparseTileSlot *slot = &region->tiles[start_tile + t];
                uint64_t slice_off = op->bo_offset + ((uint64_t)t * A840_KMD_SPARSE_TILE_BYTES);
                if (!slot->is_zero_page) {
                    decrement_bo_ref_locked(mgr, client_id, slot->bo_handle);
                    if (mgr->stats.active_bound_tiles > 0u)
                        mgr->stats.active_bound_tiles--;
                } else if (mgr->stats.active_zero_tiles > 0u) {
                    mgr->stats.active_zero_tiles--;
                }
                slot->bo_handle = bo->bo_handle;
                slot->bo_offset = slice_off;
                slot->phys_addr = bo->phys_base + slice_off;
                slot->permissions = op->flags;
                slot->is_zero_page = 0u;
                bo->sparse_refcount++;
                mgr->stats.active_bound_tiles++;
            }
            mgr->stats.total_tile_binds += tile_cnt;

            memset(&pte_batch[pte_count], 0, sizeof(pte_batch[pte_count]));
            pte_batch[pte_count].gpu_va = op->gpu_va;
            pte_batch[pte_count].length_bytes = op->length_bytes;
            pte_batch[pte_count].phys_addr = bo->phys_base + op->bo_offset;
            pte_batch[pte_count].permissions = op->flags;
            pte_batch[pte_count].is_zero_page = 0u;
            pte_batch[pte_count].unmap_region = 0u;
            pte_smmu_ctxs[pte_count] = region->smmu_ctx;
            pte_count++;
            mutated = 1u;

            fill_tile_info(region, start_tile, &reply->results[i]);
        } else if (op->op == A840_KMD_SPARSE_OP_BIND_ZERO_PAGE ||
                   op->op == A840_KMD_SPARSE_OP_UNBIND_TO_ZERO) {
            for (t = 0u; t < tile_cnt; ++t) {
                A840KmdSparseTileSlot *slot = &region->tiles[start_tile + t];
                if (!slot->is_zero_page) {
                    decrement_bo_ref_locked(mgr, client_id, slot->bo_handle);
                    if (mgr->stats.active_bound_tiles > 0u)
                        mgr->stats.active_bound_tiles--;
                    mgr->stats.active_zero_tiles++;
                }
                slot->bo_handle = 0u;
                slot->bo_offset = 0u;
                slot->phys_addr = region->zero_page_phys;
                slot->permissions = A840_KMD_SMMU_PERM_READ_ONLY;
                slot->is_zero_page = 1u;
            }
            mgr->stats.total_tile_unbinds += tile_cnt;

            memset(&pte_batch[pte_count], 0, sizeof(pte_batch[pte_count]));
            pte_batch[pte_count].gpu_va = op->gpu_va;
            pte_batch[pte_count].length_bytes = op->length_bytes;
            pte_batch[pte_count].phys_addr = region->zero_page_phys;
            pte_batch[pte_count].permissions = A840_KMD_SMMU_PERM_READ_ONLY;
            pte_batch[pte_count].is_zero_page = 1u;
            pte_batch[pte_count].unmap_region = 0u;
            pte_smmu_ctxs[pte_count] = region->smmu_ctx;
            pte_count++;
            mutated = 1u;

            fill_tile_info(region, start_tile, &reply->results[i]);
        } else {
            /* A840_KMD_SPARSE_OP_QUERY_TILE */
            fill_tile_info(region, start_tile, &reply->results[i]);
            reply->queried_count++;
        }
    }

    if (mutated) {
        gen = bump_tlb_gen_locked(mgr);
        mgr->stats.total_tlb_flushes++;
        for (i = 0u; i < pte_count; ++i)
            pte_batch[i].tlb_generation = gen;
    } else {
        gen = mgr->tlb_flush_generation;
    }

    reply->completed_ops = req->op_count;
    reply->flags = 0u;
    reply->reserved0 = 0u;
    reply->tlb_flush_generation = gen;
    reply->zero_page_gpu_va = zero_page_va;

    has_smmu_ops = mgr->has_smmu_ops;
    if (has_smmu_ops)
        smmu_ops = mgr->smmu_ops;
    sparse_unlock(mgr);

    /* Emit coalesced SMMU updates and TLB invalidations strictly OUTSIDE lock. */
    if (mutated && has_smmu_ops && pte_count > 0u) {
        size_t start = 0u;
        while (start < pte_count) {
            void *ctx = pte_smmu_ctxs[start];
            size_t end = start + 1u;
            while (end < pte_count && pte_smmu_ctxs[end] == ctx)
                end++;
            if (smmu_ops.commit_ranges != NULL) {
                status = smmu_ops.commit_ranges(smmu_ops.cookie, client_id, ctx,
                    &pte_batch[start], end - start);
                if (status != A840_KMD_OK)
                    return status;
            }
            start = end;
        }
        if (smmu_ops.tlb_flush != NULL) {
            for (i = 0u; i < pte_count; ++i) {
                size_t prev;
                int seen = 0;
                for (prev = 0u; prev < i; ++prev) {
                    if (pte_smmu_ctxs[prev] == pte_smmu_ctxs[i]) {
                        seen = 1;
                        break;
                    }
                }
                if (!seen)
                    smmu_ops.tlb_flush(smmu_ops.cookie, client_id, pte_smmu_ctxs[i], gen);
            }
        }
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_unbind_bo(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t destroy_bo,
    uint32_t *out_unbound_tiles,
    uint64_t *out_tlb_generation)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparsePteRange chunk[A840_KMD_SPARSE_UNBIND_CHUNK];
    void *chunk_ctxs[A840_KMD_SPARSE_UNBIND_CHUNK];
    void *flush_ctxs[A840_KMD_SPARSE_MAX_REGIONS];
    size_t flush_ctx_count = 0u;
    A840KmdSparseSmmuOps smmu_ops;
    uint32_t has_smmu_ops = 0u;
    uint32_t total_unbound = 0u;
    uint64_t gen = 0u;
    int first_pass = 1;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&smmu_ops, 0, sizeof(smmu_ops));

    for (;;) {
        size_t chunk_count = 0u;
        int more_remaining = 0;
        size_t r_idx;

        sparse_lock(mgr);
        if (first_pass) {
            A840KmdSparseBoEntry *bo = find_bo_locked(mgr, client_id, bo_handle);
            if (bo == NULL) {
                if (bo_owned_by_other_client_locked(mgr, client_id, bo_handle)) {
                    mgr->stats.rejected_cross_client++;
                    sparse_unlock(mgr);
                    return A840_KMD_PERMISSION_DENIED;
                }
                sparse_unlock(mgr);
                return A840_KMD_NOT_FOUND;
            }
            if (destroy_bo != 0u) {
                memset(bo, 0, sizeof(*bo));
                if (mgr->stats.active_bos > 0u)
                    mgr->stats.active_bos--;
            }
            has_smmu_ops = mgr->has_smmu_ops;
            if (has_smmu_ops)
                smmu_ops = mgr->smmu_ops;
            gen = mgr->tlb_flush_generation;
            first_pass = 0;
        }

        for (r_idx = 0u; r_idx < A840_KMD_SPARSE_MAX_REGIONS; ++r_idx) {
            A840KmdSparseRegion *r = &mgr->regions[r_idx];
            uint32_t t = 0u;
            if (!r->active || r->client_id != client_id)
                continue;
            while (t < r->tile_count) {
                uint32_t run_start;
                uint32_t run_len;
                if (r->tiles[t].is_zero_page || r->tiles[t].bo_handle != bo_handle) {
                    t++;
                    continue;
                }
                if (chunk_count >= A840_KMD_SPARSE_UNBIND_CHUNK) {
                    more_remaining = 1;
                    break;
                }
                run_start = t;
                while (t < r->tile_count &&
                       !r->tiles[t].is_zero_page &&
                       r->tiles[t].bo_handle == bo_handle) {
                    r->tiles[t].bo_handle = 0u;
                    r->tiles[t].bo_offset = 0u;
                    r->tiles[t].phys_addr = r->zero_page_phys;
                    r->tiles[t].permissions = A840_KMD_SMMU_PERM_READ_ONLY;
                    r->tiles[t].is_zero_page = 1u;
                    if (destroy_bo == 0u)
                        decrement_bo_ref_locked(mgr, client_id, bo_handle);
                    if (mgr->stats.active_bound_tiles > 0u)
                        mgr->stats.active_bound_tiles--;
                    mgr->stats.active_zero_tiles++;
                    mgr->stats.total_bo_auto_unbinds++;
                    total_unbound++;
                    t++;
                }
                run_len = t - run_start;
                if (total_unbound == run_len)
                    gen = bump_tlb_gen_locked(mgr);

                memset(&chunk[chunk_count], 0, sizeof(chunk[chunk_count]));
                chunk[chunk_count].gpu_va =
                    r->gpu_va + ((uint64_t)run_start * A840_KMD_SPARSE_TILE_BYTES);
                chunk[chunk_count].length_bytes =
                    (uint64_t)run_len * A840_KMD_SPARSE_TILE_BYTES;
                chunk[chunk_count].phys_addr = r->zero_page_phys;
                chunk[chunk_count].tlb_generation = gen;
                chunk[chunk_count].permissions = A840_KMD_SMMU_PERM_READ_ONLY;
                chunk[chunk_count].is_zero_page = 1u;
                chunk[chunk_count].unmap_region = 0u;
                chunk_ctxs[chunk_count] = r->smmu_ctx;
                chunk_count++;

                {
                    size_t k;
                    int seen = 0;
                    for (k = 0u; k < flush_ctx_count; ++k) {
                        if (flush_ctxs[k] == r->smmu_ctx) {
                            seen = 1;
                            break;
                        }
                    }
                    if (!seen && flush_ctx_count < A840_KMD_SPARSE_MAX_REGIONS)
                        flush_ctxs[flush_ctx_count++] = r->smmu_ctx;
                }
            }
            if (more_remaining)
                break;
        }

        if (!more_remaining && total_unbound > 0u)
            mgr->stats.total_tlb_flushes++;
        sparse_unlock(mgr);

        if (has_smmu_ops && chunk_count > 0u && smmu_ops.commit_ranges != NULL) {
            size_t start = 0u;
            while (start < chunk_count) {
                void *ctx = chunk_ctxs[start];
                size_t end = start + 1u;
                while (end < chunk_count && chunk_ctxs[end] == ctx)
                    end++;
                status = smmu_ops.commit_ranges(smmu_ops.cookie, client_id, ctx,
                    &chunk[start], end - start);
                if (status != A840_KMD_OK)
                    return status;
                start = end;
            }
        }

        if (!more_remaining)
            break;
    }

    if (total_unbound > 0u && has_smmu_ops && smmu_ops.tlb_flush != NULL) {
        size_t k;
        for (k = 0u; k < flush_ctx_count; ++k)
            smmu_ops.tlb_flush(smmu_ops.cookie, client_id, flush_ctxs[k], gen);
    }

    if (out_unbound_tiles != NULL)
        *out_unbound_tiles = total_unbound;
    if (out_tlb_generation != NULL)
        *out_tlb_generation = gen;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_query_tile(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    A840KmdSparseTileInfo *out_info)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparseRegion *r;
    uint32_t tile_idx;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_info == NULL)
        return A840_KMD_INVALID_ARG;
    status = validate_sparse_range(gpu_va, A840_KMD_SPARSE_TILE_BYTES);
    if (status != A840_KMD_OK)
        return status;

    sparse_lock(mgr);
    r = find_region_containing_locked(mgr, client_id, gpu_va, A840_KMD_SPARSE_TILE_BYTES);
    if (r == NULL) {
        if (region_va_owned_by_other_client_locked(mgr, client_id, gpu_va,
                A840_KMD_SPARSE_TILE_BYTES)) {
            mgr->stats.rejected_cross_client++;
            sparse_unlock(mgr);
            return A840_KMD_PERMISSION_DENIED;
        }
        sparse_unlock(mgr);
        return A840_KMD_NOT_FOUND;
    }
    tile_idx = (uint32_t)((gpu_va - r->gpu_va) / A840_KMD_SPARSE_TILE_BYTES);
    fill_tile_info(r, tile_idx, out_info);
    sparse_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_client_teardown(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    A840KmdSparseTeardownSummary *out_summary)
{
    A840KmdStatus status = check_init(mgr);
    A840KmdSparsePteRange unmaps[A840_KMD_SPARSE_MAX_REGIONS];
    void *unmap_ctxs[A840_KMD_SPARSE_MAX_REGIONS];
    size_t unmap_count = 0u;
    A840KmdSparseSmmuOps smmu_ops;
    A840KmdSparseTeardownSummary summary;
    uint32_t has_smmu_ops = 0u;
    uint64_t gen = 0u;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&smmu_ops, 0, sizeof(smmu_ops));
    memset(&summary, 0, sizeof(summary));

    sparse_lock(mgr);
    for (i = 0u; i < A840_KMD_SPARSE_MAX_REGIONS; ++i) {
        A840KmdSparseRegion *r = &mgr->regions[i];
        uint32_t t;
        if (!r->active || r->client_id != client_id)
            continue;

        for (t = 0u; t < r->tile_count; ++t) {
            if (!r->tiles[t].is_zero_page) {
                decrement_bo_ref_locked(mgr, client_id, r->tiles[t].bo_handle);
                summary.bo_refs_released++;
                if (mgr->stats.active_bound_tiles > 0u)
                    mgr->stats.active_bound_tiles--;
            } else if (mgr->stats.active_zero_tiles > 0u) {
                mgr->stats.active_zero_tiles--;
            }
        }

        if (unmap_count == 0u)
            gen = bump_tlb_gen_locked(mgr);

        memset(&unmaps[unmap_count], 0, sizeof(unmaps[unmap_count]));
        unmaps[unmap_count].gpu_va = r->gpu_va;
        unmaps[unmap_count].length_bytes = r->length_bytes;
        unmaps[unmap_count].tlb_generation = gen;
        unmaps[unmap_count].unmap_region = 1u;
        unmap_ctxs[unmap_count] = r->smmu_ctx;
        unmap_count++;

        summary.regions_reclaimed++;
        summary.tiles_reclaimed += r->tile_count;
        if (mgr->stats.active_regions > 0u)
            mgr->stats.active_regions--;
        mgr->stats.total_regions_destroyed++;
        memset(r, 0, sizeof(*r));
    }

    for (i = 0u; i < A840_KMD_SPARSE_MAX_BOS; ++i) {
        if (mgr->bos[i].active && mgr->bos[i].client_id == client_id) {
            memset(&mgr->bos[i], 0, sizeof(mgr->bos[i]));
            summary.bos_removed++;
            if (mgr->stats.active_bos > 0u)
                mgr->stats.active_bos--;
        }
    }

    for (i = 0u; i < A840_KMD_SPARSE_MAX_NORMAL_RANGES; ++i) {
        if (mgr->normal_ranges[i].active && mgr->normal_ranges[i].client_id == client_id)
            memset(&mgr->normal_ranges[i], 0, sizeof(mgr->normal_ranges[i]));
    }

    if (unmap_count > 0u) {
        mgr->stats.total_tlb_flushes++;
        summary.tlb_flush_generation = gen;
    } else {
        summary.tlb_flush_generation = mgr->tlb_flush_generation;
    }

    has_smmu_ops = mgr->has_smmu_ops;
    if (has_smmu_ops)
        smmu_ops = mgr->smmu_ops;
    sparse_unlock(mgr);

    if (unmap_count > 0u && has_smmu_ops) {
        if (smmu_ops.commit_ranges != NULL) {
            size_t start = 0u;
            while (start < unmap_count) {
                void *ctx = unmap_ctxs[start];
                size_t end = start + 1u;
                while (end < unmap_count && unmap_ctxs[end] == ctx)
                    end++;
                status = smmu_ops.commit_ranges(smmu_ops.cookie, client_id, ctx,
                    &unmaps[start], end - start);
                if (status != A840_KMD_OK) {
                    if (out_summary != NULL)
                        *out_summary = summary;
                    return status;
                }
                start = end;
            }
        }
        if (smmu_ops.tlb_flush != NULL) {
            for (i = 0u; i < unmap_count; ++i) {
                size_t prev;
                int seen = 0;
                for (prev = 0u; prev < i; ++prev) {
                    if (unmap_ctxs[prev] == unmap_ctxs[i]) {
                        seen = 1;
                        break;
                    }
                }
                if (!seen)
                    smmu_ops.tlb_flush(smmu_ops.cookie, client_id, unmap_ctxs[i], gen);
            }
        }
    }

    if (out_summary != NULL)
        *out_summary = summary;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_sparse_get_stats(
    A840KmdSparseManager *mgr,
    A840KmdSparseStats *out_stats)
{
    A840KmdStatus status = check_init(mgr);
    if (status != A840_KMD_OK)
        return status;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    sparse_lock(mgr);
    *out_stats = mgr->stats;
    sparse_unlock(mgr);
    return A840_KMD_OK;
}
