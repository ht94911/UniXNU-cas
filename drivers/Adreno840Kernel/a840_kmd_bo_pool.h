/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_BO_POOL_H
#define A840_KMD_BO_POOL_H

#include <stddef.h>
#include <stdint.h>
#include "a840_kmd_zero_page.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status alias for fail-closed EMPTY submission rejection without modifying
 * the frozen A840KmdStatus enum in a840_kmd_zero_page.h.
 */
#ifndef A840_KMD_INVALID_STATE
#define A840_KMD_INVALID_STATE A840_KMD_STATE_CONFLICT
#endif

/* 7 power-of-two size buckets: 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB. */
#define A840_KMD_BO_POOL_BUCKET_COUNT 7u
#define A840_KMD_BO_POOL_MAX_PER_BUCKET 32u
#define A840_KMD_BO_POOL_MAX_SLOTS 256u
#define A840_KMD_BO_POOL_DEFAULT_MAX_BYTES (64u * 1024u * 1024u)
#define A840_KMD_BO_POOL_DEFAULT_BUCKET_LIMIT 16u

/* Cache / coherence attributes that must match exactly on Parked BO reuse. */
typedef enum A840KmdBoCacheMode {
    A840_KMD_BO_CACHE_UNCACHED = 0u,
    A840_KMD_BO_CACHE_WRITE_COMBINE = 1u,
    A840_KMD_BO_CACHE_COHERENT = 2u
} A840KmdBoCacheMode;

/* Purgeable / Volatile Heap states aligned with Metal MTLPurgeableState. */
typedef enum A840KmdPurgeableState {
    A840_PURGE_KEEP_CURRENT = 0u,
    A840_PURGE_NONVOLATILE = 1u,
    A840_PURGE_VOLATILE = 2u,
    A840_PURGE_EMPTY = 3u
} A840KmdPurgeableState;

/* BO residency lifecycle state inside the pool. */
typedef enum A840KmdBoPoolEntryState {
    A840_KMD_BO_ENTRY_FREE = 0u,
    A840_KMD_BO_ENTRY_ACTIVE = 1u,
    A840_KMD_BO_ENTRY_PARKED = 2u
} A840KmdBoPoolEntryState;

/* Acquire request flags. */
enum {
    A840_KMD_BO_ACQUIRE_NONE = 0u,
    A840_KMD_BO_ACQUIRE_PRESCRUBBED_OK = 1u << 0,
    A840_KMD_BO_ACQUIRE_KNOWN_MASK = (1u << 0)
};

/* Optional custom sleepable lock + monotonic clock backend. */
typedef struct A840KmdBoPoolSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    uint64_t (*now_ns)(void *cookie);
} A840KmdBoPoolSyncOps;

/* Optional physical backing / SMMU / scrub callbacks.
 * Every callback is invoked STRICTLY OUTSIDE the pool lock.
 */
typedef struct A840KmdBoPoolBackingOps {
    void *cookie;
    /* Allocate physical memory + initial SMMU mapping for a new BO. */
    A840KmdStatus (*alloc_and_map)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t bytes,
        uint64_t alignment,
        uint32_t permissions,
        uint32_t cache_mode,
        uint64_t *out_gpu_va,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle);
    /* Unmap SMMU VA and free physical backing on eviction / teardown. */
    void (*unmap_and_free)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t phys_addr,
        void *backing_handle,
        uint64_t bytes);
    /* Scrub physical pages to zero before reusing a parked BO. If NULL,
     * the pool zeroes cpu_ptr directly when non-NULL.
     */
    A840KmdStatus (*zero_scrub)(
        void *cookie,
        uint64_t client_id,
        uint64_t phys_addr,
        void *cpu_ptr,
        void *backing_handle,
        uint64_t bytes);
    /* Unbind physical backing (or remap to RO zero page) when a VOLATILE BO
     * is purged into A840_PURGE_EMPTY under memory pressure.
     */
    void (*purge_to_empty)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t phys_addr,
        void *backing_handle,
        uint64_t bytes,
        uint64_t zero_page_phys_addr);
    /* Optional re-backing hook when transitioning A840_PURGE_EMPTY -> NONVOLATILE. */
    A840KmdStatus (*rehydrate_from_empty)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t bytes,
        uint32_t permissions,
        uint32_t cache_mode,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle);
} A840KmdBoPoolBackingOps;

typedef struct A840KmdBoPoolConfig {
    uint64_t max_parked_bytes;        /* 0 -> A840_KMD_BO_POOL_DEFAULT_MAX_BYTES */
    uint32_t max_parked_per_bucket;   /* 0 -> A840_KMD_BO_POOL_DEFAULT_BUCKET_LIMIT */
    uint32_t scrub_on_park;           /* 1 = scrub during release/park, 0 = scrub on acquire */
    uint64_t parked_ttl_ns;           /* 0 = no TTL expiry */
    A840KmdZeroPageManager *zero_page_mgr; /* Optional RO zero page source for EMPTY purge */
    const A840KmdBoPoolSyncOps *sync_ops;
    const A840KmdBoPoolBackingOps *backing_ops;
} A840KmdBoPoolConfig;

typedef struct A840KmdBoAcquireReq {
    uint64_t client_id;
    void *smmu_ctx;
    uint64_t requested_bytes;
    uint64_t alignment;               /* 0 -> 4096; must be power-of-two >= 4096 */
    uint32_t permissions;             /* A840_KMD_SMMU_PERM_* */
    uint32_t cache_mode;              /* A840KmdBoCacheMode */
    uint32_t flags;                   /* A840_KMD_BO_ACQUIRE_* */
    uint32_t reserved;                /* MUST be 0 */
} A840KmdBoAcquireReq;

typedef struct A840KmdBoDesc {
    uint64_t bo_handle;
    uint64_t client_id;
    uint64_t gpu_va;
    uint64_t phys_addr;
    void *cpu_ptr;
    void *backing_handle;
    uint64_t requested_bytes;
    uint64_t bucket_bytes;
    uint64_t alignment;
    uint32_t permissions;
    uint32_t cache_mode;
    uint32_t purgeable_state;         /* A840KmdPurgeableState */
    uint32_t in_flight_refs;
    uint32_t from_parked_pool;        /* 1 = hit parked pool (SMMU map saved) */
    uint32_t is_scrubbed;
} A840KmdBoDesc;

typedef struct A840KmdPurgeResult {
    uint32_t old_state;               /* A840KmdPurgeableState */
    uint32_t new_state;               /* A840KmdPurgeableState */
    uint32_t was_purged;              /* 1 when EMPTY observed or transitioning EMPTY -> NONVOLATILE */
    uint32_t reserved;
} A840KmdPurgeResult;

typedef struct A840KmdPressureReport {
    uint64_t target_reclaim_bytes;
    uint64_t reclaimed_total_bytes;
    uint64_t reclaimed_parked_bytes;
    uint64_t reclaimed_volatile_bytes;
    uint32_t parked_evicted_count;
    uint32_t volatile_purged_count;
    uint32_t volatile_skipped_inflight;
    uint32_t reserved;
} A840KmdPressureReport;

typedef struct A840KmdBoPoolStats {
    uint32_t active_bos;
    uint32_t parked_bos;
    uint32_t volatile_bos;
    uint32_t empty_bos;
    uint64_t parked_bytes;
    uint64_t park_hits;
    uint64_t park_misses;
    uint64_t park_evictions_cap;
    uint64_t park_evictions_pressure;
    uint64_t park_evictions_ttl;
    uint64_t smmu_map_saved;
    uint64_t zero_scrubs;
    uint64_t volatile_purged_count;
    uint64_t volatile_deferred_inflight;
    uint64_t empty_submit_rejects;
    uint64_t cross_client_park_blocked;
} A840KmdBoPoolStats;

typedef struct A840KmdBoPoolSlot {
    uint32_t entry_state;             /* A840KmdBoPoolEntryState */
    uint32_t bucket_index;            /* 0..6, or A840_KMD_BO_POOL_BUCKET_COUNT for >16 MiB */
    uint32_t permissions;
    uint32_t cache_mode;
    uint32_t purgeable_state;         /* A840KmdPurgeableState */
    uint32_t in_flight_refs;
    uint32_t scrubbed;
    uint32_t pending_destroy;
    uint64_t bo_handle;
    uint64_t client_id;
    void *smmu_ctx;
    uint64_t gpu_va;
    uint64_t phys_addr;
    void *cpu_ptr;
    void *backing_handle;
    uint64_t requested_bytes;
    uint64_t bucket_bytes;
    uint64_t alignment;
    uint64_t parked_at_ns;
    uint64_t lru_seq;
} A840KmdBoPoolSlot;

typedef struct A840KmdBoPool {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_backing_ops;
    uint32_t scrub_on_park;
    uint32_t max_parked_per_bucket;
    uint32_t reserved;
    uint64_t max_parked_bytes;
    uint64_t parked_ttl_ns;
    uint64_t next_bo_handle;
    uint64_t next_synthetic_va;
    uint64_t next_synthetic_phys;
    uint64_t next_lru_seq;
    A840KmdZeroPageManager *zero_page_mgr;
    A840KmdBoPoolSyncOps sync_ops;
    A840KmdBoPoolBackingOps backing_ops;
    A840KmdBoPoolStats stats;
    uint32_t bucket_parked_count[A840_KMD_BO_POOL_BUCKET_COUNT];
    A840KmdBoPoolSlot slots[A840_KMD_BO_POOL_MAX_SLOTS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdBoPool;

/* Helper: return bucket size in bytes for index 0..6 (0 if out of range). */
uint64_t a840_kmd_bo_pool_bucket_size(uint32_t bucket_index);

/* Helper: round requested bytes up to power-of-two bucket index (returns -1 if > 16 MiB). */
int a840_kmd_bo_pool_select_bucket(uint64_t bytes, uint64_t *out_bucket_bytes);

/* Lifecycle */
A840KmdStatus a840_kmd_bo_pool_init(
    A840KmdBoPool *pool,
    const A840KmdBoPoolConfig *config);

void a840_kmd_bo_pool_destroy(A840KmdBoPool *pool);

/* Acquire a BO for `req->client_id`. Reuses a Parked BO ONLY when client_id,
 * bucket_bytes, permissions, cache_mode, and alignment match completely, and
 * ensures the physical pages are zero-scrubbed before handoff.
 */
A840KmdStatus a840_kmd_bo_pool_acquire(
    A840KmdBoPool *pool,
    const A840KmdBoAcquireReq *req,
    A840KmdBoDesc *out_desc);

/* Release an active BO owned by `client_id`.
 * If `allow_park != 0`, bucket-eligible, non-EMPTY, and `in_flight_refs == 0`,
 * parks the BO while keeping its SMMU mapping intact (evicting oldest parked
 * entries if bucket or byte caps would be exceeded). Otherwise unmaps & frees
 * immediately (or marks pending_destroy if in_flight_refs > 0).
 */
A840KmdStatus a840_kmd_bo_pool_release(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t allow_park);

/* Purgeable / Volatile Heap state transition or query (KEEP_CURRENT).
 * - Setting VOLATILE records LRU timestamp; in-flight BOs stay protected from purge.
 * - Setting NONVOLATILE on an EMPTY BO rehydrates (if backing_ops.rehydrate_from_empty
 *   is provided) and reports `out_result->was_purged = 1`.
 */
A840KmdStatus a840_kmd_bo_pool_set_purgeable(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdPurgeableState requested_state,
    A840KmdPurgeResult *out_result);

/* GPU submit guard & in-flight fence reference tracking:
 * - `begin_submit` rejects EMPTY BOs fail-closed with A840_KMD_INVALID_STATE.
 * - While `in_flight_refs > 0`, memory pressure MUST NOT purge a VOLATILE BO.
 */
A840KmdStatus a840_kmd_bo_pool_begin_submit(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs);

A840KmdStatus a840_kmd_bo_pool_end_submit(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_in_flight_refs);

/* Query current state of a client-owned BO. */
A840KmdStatus a840_kmd_bo_pool_query_bo(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint64_t bo_handle,
    A840KmdBoDesc *out_desc);

/* Sweep expired Parked BOs whose age exceeds parked_ttl_ns. */
A840KmdStatus a840_kmd_bo_pool_sweep_ttl(
    A840KmdBoPool *pool,
    uint32_t *out_evicted_count);

/* Memory pressure handler:
 * 1) Reclaims Parked BOs first (LRU order across buckets).
 * 2) If target_reclaim_bytes is still unmet, reclaims VOLATILE BOs with
 *    in_flight_refs == 0 (LRU order), transitioning them atomically to EMPTY
 *    and invoking purge_to_empty outside the lock.
 *    VOLATILE BOs with in_flight_refs > 0 are skipped and never purged.
 */
A840KmdStatus a840_kmd_bo_pool_memory_pressure(
    A840KmdBoPool *pool,
    uint64_t target_reclaim_bytes,
    A840KmdPressureReport *out_report);

/* Reclaim all active and parked BOs owned by `client_id` on client close. */
A840KmdStatus a840_kmd_bo_pool_client_teardown(
    A840KmdBoPool *pool,
    uint64_t client_id,
    uint32_t *out_reclaimed_count);

A840KmdStatus a840_kmd_bo_pool_get_stats(
    A840KmdBoPool *pool,
    A840KmdBoPoolStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_BO_POOL_H */
