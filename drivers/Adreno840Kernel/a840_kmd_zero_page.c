/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_zero_page.h"
#include <string.h>

#define A840_KMD_ZERO_PAGE_MAGIC 1413567056u

#if !defined(A840_KMD_CUSTOM_SYNC)
static void zp_lock(A840KmdZeroPageManager *mgr)
{
    (void)pthread_mutex_lock(&mgr->lock);
}

static void zp_unlock(A840KmdZeroPageManager *mgr)
{
    (void)pthread_mutex_unlock(&mgr->lock);
}
#else
static void zp_lock(A840KmdZeroPageManager *mgr) { (void)mgr; }
static void zp_unlock(A840KmdZeroPageManager *mgr) { (void)mgr; }
#endif

static A840KmdStatus validate_gpu_va(uint64_t gpu_va)
{
    if (gpu_va == 0u)
        return A840_KMD_INVALID_ARG;
    if (gpu_va >= A840_KMD_GPU_VA_LIMIT ||
        A840_KMD_ZERO_PAGE_BYTES > A840_KMD_GPU_VA_LIMIT - gpu_va)
        return A840_KMD_RANGE;
    if ((gpu_va % A840_KMD_ZERO_PAGE_ALIGN) != 0u)
        return A840_KMD_ALIGNMENT;
    return A840_KMD_OK;
}

static A840KmdStatus validate_phys_addr(uint64_t phys_addr)
{
    if (phys_addr == 0u)
        return A840_KMD_IO_ERROR;
    if (phys_addr >= A840_KMD_GPU_VA_LIMIT ||
        A840_KMD_ZERO_PAGE_BYTES > A840_KMD_GPU_VA_LIMIT - phys_addr)
        return A840_KMD_RANGE;
    if ((phys_addr % A840_KMD_ZERO_PAGE_ALIGN) != 0u)
        return A840_KMD_ALIGNMENT;
    return A840_KMD_OK;
}

static A840KmdStatus check_init(const A840KmdZeroPageManager *mgr)
{
    if (mgr == NULL)
        return A840_KMD_INVALID_ARG;
    if (mgr->initialized != A840_KMD_ZERO_PAGE_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_zero_page_init(
    A840KmdZeroPageManager *mgr,
    const A840KmdSmmuOps *ops,
    uint64_t default_gpu_va)
{
    A840KmdStatus status;
    if (mgr == NULL || ops == NULL ||
        ops->alloc_backing_page == NULL ||
        ops->free_backing_page == NULL ||
        ops->map_client_ro == NULL ||
        ops->unmap_client == NULL)
        return A840_KMD_INVALID_ARG;

    status = validate_gpu_va(default_gpu_va);
    if (status != A840_KMD_OK)
        return status;

    memset(mgr, 0, sizeof(*mgr));
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (pthread_mutex_init(&mgr->lock, NULL) != 0)
        return A840_KMD_NO_MEMORY;
#endif
    mgr->ops = *ops;
    mgr->default_gpu_va = default_gpu_va;
    mgr->initialized = A840_KMD_ZERO_PAGE_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_zero_page_destroy(A840KmdZeroPageManager *mgr)
{
    size_t i;
    if (check_init(mgr) != A840_KMD_OK)
        return;

    zp_lock(mgr);
    for (i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        A840KmdZeroPageClientSlot *slot = &mgr->clients[i];
        if (!slot->active)
            continue;
        (void)mgr->ops.unmap_client(mgr->ops.cookie, slot->client_id,
            slot->smmu_ctx, slot->gpu_va, A840_KMD_ZERO_PAGE_BYTES);
        mgr->total_unmaps++;
        memset(slot, 0, sizeof(*slot));
        if (mgr->refcount > 0u)
            mgr->refcount--;
    }
    if (mgr->backing_allocated) {
        mgr->ops.free_backing_page(mgr->ops.cookie, mgr->backing_phys_addr,
            mgr->backing_handle, A840_KMD_ZERO_PAGE_BYTES);
        mgr->total_frees++;
        mgr->backing_allocated = 0u;
        mgr->backing_phys_addr = 0u;
        mgr->backing_cpu_ptr = NULL;
        mgr->backing_handle = NULL;
    }
    mgr->refcount = 0u;
    mgr->initialized = 0u;
    zp_unlock(mgr);

#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_destroy(&mgr->lock);
#endif
}

A840KmdStatus a840_kmd_zero_page_bind_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t requested_gpu_va,
    uint32_t permissions,
    struct NativeAdrenoAbiZeroPageReplyV3 *out_reply)
{
    A840KmdStatus status;
    uint64_t target_va;
    A840KmdZeroPageClientSlot *existing = NULL;
    A840KmdZeroPageClientSlot *free_slot = NULL;
    uint32_t allocated_here = 0u;
    size_t i;

    status = check_init(mgr);
    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_reply == NULL)
        return A840_KMD_INVALID_ARG;

    zp_lock(mgr);

    /* Strictly enforce PERM_READ_ONLY; reject any write bit or invalid perm. */
    if ((permissions & A840_KMD_SMMU_PERM_WRITE) != 0u ||
        permissions != A840_KMD_SMMU_PERM_READ_ONLY) {
        if ((permissions & A840_KMD_SMMU_PERM_WRITE) != 0u)
            mgr->rejected_write_maps++;
        zp_unlock(mgr);
        return A840_KMD_PERMISSION_DENIED;
    }

    target_va = (requested_gpu_va != 0u) ? requested_gpu_va : mgr->default_gpu_va;
    status = validate_gpu_va(target_va);
    if (status != A840_KMD_OK) {
        zp_unlock(mgr);
        return status;
    }

    for (i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (mgr->clients[i].active) {
            if (mgr->clients[i].client_id == client_id) {
                existing = &mgr->clients[i];
                break;
            }
        } else if (free_slot == NULL) {
            free_slot = &mgr->clients[i];
        }
    }

    if (existing != NULL) {
        if (requested_gpu_va != 0u && existing->gpu_va != requested_gpu_va) {
            zp_unlock(mgr);
            return A840_KMD_STATE_CONFLICT;
        }
        out_reply->gpu_va = existing->gpu_va;
        out_reply->bytes = A840_KMD_ZERO_PAGE_BYTES;
        zp_unlock(mgr);
        return A840_KMD_OK;
    }

    if (free_slot == NULL) {
        zp_unlock(mgr);
        return A840_KMD_CAPACITY;
    }

    /* Lazily allocate and zero-fill the single global physical 64 KiB page. */
    if (!mgr->backing_allocated) {
        uint64_t phys = 0u;
        void *cpu_ptr = NULL;
        void *handle = NULL;
        status = mgr->ops.alloc_backing_page(mgr->ops.cookie,
            A840_KMD_ZERO_PAGE_BYTES, &phys, &cpu_ptr, &handle);
        if (status != A840_KMD_OK) {
            zp_unlock(mgr);
            return status;
        }
        status = validate_phys_addr(phys);
        if (status != A840_KMD_OK) {
            mgr->ops.free_backing_page(mgr->ops.cookie, phys, handle,
                A840_KMD_ZERO_PAGE_BYTES);
            zp_unlock(mgr);
            return status;
        }
        if (cpu_ptr != NULL)
            memset(cpu_ptr, 0, (size_t)A840_KMD_ZERO_PAGE_BYTES);
        mgr->backing_phys_addr = phys;
        mgr->backing_cpu_ptr = cpu_ptr;
        mgr->backing_handle = handle;
        mgr->backing_allocated = 1u;
        mgr->total_allocs++;
        allocated_here = 1u;
    }

    status = mgr->ops.map_client_ro(mgr->ops.cookie, client_id, smmu_ctx,
        target_va, mgr->backing_phys_addr, A840_KMD_ZERO_PAGE_BYTES,
        A840_KMD_SMMU_PERM_READ_ONLY);
    if (status != A840_KMD_OK) {
        if (allocated_here && mgr->refcount == 0u) {
            mgr->ops.free_backing_page(mgr->ops.cookie, mgr->backing_phys_addr,
                mgr->backing_handle, A840_KMD_ZERO_PAGE_BYTES);
            mgr->total_frees++;
            mgr->backing_allocated = 0u;
            mgr->backing_phys_addr = 0u;
            mgr->backing_cpu_ptr = NULL;
            mgr->backing_handle = NULL;
        }
        zp_unlock(mgr);
        return status;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->client_id = client_id;
    free_slot->smmu_ctx = smmu_ctx;
    free_slot->gpu_va = target_va;
    mgr->refcount++;
    mgr->total_maps++;

    out_reply->gpu_va = target_va;
    out_reply->bytes = A840_KMD_ZERO_PAGE_BYTES;
    zp_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_zero_page_query_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id,
    struct NativeAdrenoAbiZeroPageReplyV3 *out_reply)
{
    A840KmdStatus status;
    size_t i;

    status = check_init(mgr);
    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_reply == NULL)
        return A840_KMD_INVALID_ARG;

    zp_lock(mgr);
    for (i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (mgr->clients[i].active && mgr->clients[i].client_id == client_id) {
            out_reply->gpu_va = mgr->clients[i].gpu_va;
            out_reply->bytes = A840_KMD_ZERO_PAGE_BYTES;
            zp_unlock(mgr);
            return A840_KMD_OK;
        }
    }
    zp_unlock(mgr);
    return A840_KMD_NOT_FOUND;
}

A840KmdStatus a840_kmd_zero_page_unbind_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id)
{
    A840KmdStatus status;
    A840KmdZeroPageClientSlot *slot = NULL;
    size_t i;

    status = check_init(mgr);
    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    zp_lock(mgr);
    for (i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (mgr->clients[i].active && mgr->clients[i].client_id == client_id) {
            slot = &mgr->clients[i];
            break;
        }
    }
    if (slot == NULL) {
        zp_unlock(mgr);
        return A840_KMD_NOT_FOUND;
    }

    status = mgr->ops.unmap_client(mgr->ops.cookie, slot->client_id,
        slot->smmu_ctx, slot->gpu_va, A840_KMD_ZERO_PAGE_BYTES);
    if (status != A840_KMD_OK) {
        /* Do not drop backing page while SMMU mapping could still be live. */
        zp_unlock(mgr);
        return status;
    }

    memset(slot, 0, sizeof(*slot));
    mgr->total_unmaps++;
    if (mgr->refcount > 0u)
        mgr->refcount--;

    if (mgr->refcount == 0u && mgr->backing_allocated) {
        mgr->ops.free_backing_page(mgr->ops.cookie, mgr->backing_phys_addr,
            mgr->backing_handle, A840_KMD_ZERO_PAGE_BYTES);
        mgr->total_frees++;
        mgr->backing_allocated = 0u;
        mgr->backing_phys_addr = 0u;
        mgr->backing_cpu_ptr = NULL;
        mgr->backing_handle = NULL;
    }

    zp_unlock(mgr);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_zero_page_get_stats(
    A840KmdZeroPageManager *mgr,
    A840KmdZeroPageStats *out_stats)
{
    A840KmdStatus status;
    status = check_init(mgr);
    if (status != A840_KMD_OK)
        return status;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    zp_lock(mgr);
    out_stats->active_clients = mgr->refcount;
    out_stats->backing_allocated = mgr->backing_allocated;
    out_stats->backing_phys_addr = mgr->backing_phys_addr;
    out_stats->total_allocs = mgr->total_allocs;
    out_stats->total_frees = mgr->total_frees;
    out_stats->total_maps = mgr->total_maps;
    out_stats->total_unmaps = mgr->total_unmaps;
    out_stats->rejected_write_maps = mgr->rejected_write_maps;
    zp_unlock(mgr);
    return A840_KMD_OK;
}
