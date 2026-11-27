/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_SPARSE_VA_H
#define A840_KMD_SPARSE_VA_H

#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoABI_v3.h"
#include "a840_kmd_zero_page.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-capacity limits: zero hot-path heap allocations. */
#define A840_KMD_SPARSE_TILE_BYTES A840_KMD_ZERO_PAGE_BYTES
#define A840_KMD_SPARSE_TILE_ALIGN A840_KMD_ZERO_PAGE_ALIGN
#define A840_KMD_SPARSE_VA_MIN A840_KMD_ZERO_PAGE_BYTES
#define A840_KMD_SPARSE_VA_LIMIT A840_KMD_GPU_VA_LIMIT
#define A840_KMD_SPARSE_MAX_BATCH_OPS 32u
#define A840_KMD_SPARSE_MAX_CLIENTS 64u
#define A840_KMD_SPARSE_MAX_REGIONS 64u
#define A840_KMD_SPARSE_MAX_TILES_PER_REGION 128u
#define A840_KMD_SPARSE_MAX_BOS 256u
#define A840_KMD_SPARSE_MAX_NORMAL_RANGES 128u

/* Method 22 (NativeAdrenoMethodMapSparseV3) opcodes. */
typedef enum A840KmdSparseOpType {
    A840_KMD_SPARSE_OP_NONE = 0u,
    A840_KMD_SPARSE_OP_BIND_BO = 1u,
    A840_KMD_SPARSE_OP_BIND_ZERO_PAGE = 2u,
    A840_KMD_SPARSE_OP_UNBIND_TO_ZERO = 3u,
    A840_KMD_SPARSE_OP_QUERY_TILE = 4u
} A840KmdSparseOpType;

/* Tile backing state. */
typedef enum A840KmdSparseTileState {
    A840_KMD_SPARSE_TILE_ZERO = 0u,
    A840_KMD_SPARSE_TILE_BOUND = 1u
} A840KmdSparseTileState;

/* Request-level flags (unknown bits are rejected fail-closed). */
enum {
    A840_KMD_SPARSE_REQ_FLAG_NONE = 0u,
    A840_KMD_SPARSE_REQ_FLAG_KNOWN_MASK = 0u
};

/* Method 22 fixed-size wire structs (strictly <= NativeAdrenoAbiEnvelopeBytes = 3104 B). */
typedef struct A840KmdSparseBindOp {
    uint32_t op;            /* A840KmdSparseOpType */
    uint32_t flags;         /* A840_KMD_SMMU_PERM_* for BIND_BO; RO or 0 for ZERO/UNBIND; 0 for QUERY */
    uint64_t gpu_va;        /* 64 KiB aligned start GPU VA inside sparse region */
    uint64_t length_bytes;  /* nonzero multiple of 64 KiB (64 KiB for QUERY_TILE) */
    uint64_t bo_handle;     /* client BO handle (nonzero for BIND_BO; 0 otherwise) */
    uint64_t bo_offset;     /* 64 KiB aligned offset in BO (0 unless BIND_BO) */
    uint64_t reserved0;     /* MUST be 0 */
} A840KmdSparseBindOp;

typedef struct A840KmdSparseBindReqV3 {
    uint32_t version;       /* MUST be NativeAdrenoAbiVersionV3 (3u) */
    uint32_t op_count;      /* 1..A840_KMD_SPARSE_MAX_BATCH_OPS */
    uint32_t flags;         /* A840_KMD_SPARSE_REQ_FLAG_* */
    uint32_t reserved0;     /* MUST be 0 */
    uint64_t region_id;     /* 0 = locate by gpu_va; nonzero = must match region */
    uint64_t reserved1;     /* MUST be 0 */
    A840KmdSparseBindOp ops[A840_KMD_SPARSE_MAX_BATCH_OPS];
} A840KmdSparseBindReqV3;

typedef struct A840KmdSparseTileInfo {
    uint64_t gpu_va;
    uint64_t bo_handle;     /* 0 when backed by 64 KiB RO zero page */
    uint64_t bo_offset;     /* 0 when backed by 64 KiB RO zero page */
    uint64_t phys_addr;     /* zero page phys_addr or BO slice phys_addr */
    uint32_t is_zero_page;  /* 1 = RO zero page fallback, 0 = real BO backing */
    uint32_t permissions;   /* A840_KMD_SMMU_PERM_* */
    uint32_t state;         /* A840KmdSparseTileState */
    uint32_t reserved;
} A840KmdSparseTileInfo;

typedef struct A840KmdSparseBindReplyV3 {
    uint32_t completed_ops;
    uint32_t queried_count;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t tlb_flush_generation;
    uint64_t zero_page_gpu_va;
    A840KmdSparseTileInfo results[A840_KMD_SPARSE_MAX_BATCH_OPS];
} A840KmdSparseBindReplyV3;

#if defined(__cplusplus)
static_assert(sizeof(A840KmdSparseBindOp) == 48u, "SparseBindOp layout locked");
static_assert(sizeof(A840KmdSparseTileInfo) == 48u, "SparseTileInfo layout locked");
static_assert(sizeof(A840KmdSparseBindReqV3) == 1568u, "SparseBindReqV3 layout locked");
static_assert(sizeof(A840KmdSparseBindReplyV3) == 1568u, "SparseBindReplyV3 layout locked");
static_assert(sizeof(A840KmdSparseBindReqV3) <= NativeAdrenoAbiEnvelopeBytes,
              "SparseBindReqV3 fits 3104-byte envelope");
static_assert(sizeof(A840KmdSparseBindReplyV3) <= NativeAdrenoAbiEnvelopeBytes,
              "SparseBindReplyV3 fits 3104-byte envelope");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(A840KmdSparseBindOp) == 48u, "SparseBindOp layout locked");
_Static_assert(sizeof(A840KmdSparseTileInfo) == 48u, "SparseTileInfo layout locked");
_Static_assert(sizeof(A840KmdSparseBindReqV3) == 1568u, "SparseBindReqV3 layout locked");
_Static_assert(sizeof(A840KmdSparseBindReplyV3) == 1568u, "SparseBindReplyV3 layout locked");
_Static_assert(sizeof(A840KmdSparseBindReqV3) <= NativeAdrenoAbiEnvelopeBytes,
               "SparseBindReqV3 fits 3104-byte envelope");
_Static_assert(sizeof(A840KmdSparseBindReplyV3) <= NativeAdrenoAbiEnvelopeBytes,
               "SparseBindReplyV3 fits 3104-byte envelope");
#endif

/* Optional custom sleepable lock backend (NULL selects POSIX pthread_mutex).
 * Intended for XNU IOLock integration.
 */
typedef struct A840KmdSparseSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
} A840KmdSparseSyncOps;

/* Coalesced SMMU page-table update descriptor submitted STRICTLY OUTSIDE
 * the sparse manager lock to avoid lock-order inversion.
 */
typedef struct A840KmdSparsePteRange {
    uint64_t gpu_va;
    uint64_t length_bytes;
    uint64_t phys_addr;     /* When is_zero_page==1, repeated 64 KiB zero page phys_addr */
    uint64_t tlb_generation;
    uint32_t permissions;   /* Guaranteed PERM_READ_ONLY whenever is_zero_page==1 */
    uint32_t is_zero_page;  /* 1 = replicate single 64 KiB zero page across length_bytes */
    uint32_t unmap_region;  /* 1 = unmap entire VA range on region/client teardown */
    uint32_t reserved;
} A840KmdSparsePteRange;

/* Optional SMMU page-table and TLB invalidation callbacks.
 * Every callback is invoked STRICTLY OUTSIDE the sparse manager lock.
 */
typedef struct A840KmdSparseSmmuOps {
    void *cookie;
    A840KmdStatus (*commit_ranges)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        const A840KmdSparsePteRange *ranges,
        size_t range_count);
    void (*tlb_flush)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t tlb_flush_generation);
} A840KmdSparseSmmuOps;

/* Optional external check for overlap against non-sparse client GPU VA ranges.
 * Invoked OUTSIDE the sparse manager lock; return 1 if [gpu_va, gpu_va+len)
 * overlaps an existing normal mapping, 0 otherwise.
 */
typedef int (*A840KmdSparseExternalVaOverlapFn)(
    void *cookie,
    uint64_t client_id,
    uint64_t gpu_va,
    uint64_t length_bytes);

typedef struct A840KmdSparseConfig {
    A840KmdZeroPageManager *zero_page_mgr; /* Required read-only zero page dependency */
    const A840KmdSparseSyncOps *sync_ops;  /* NULL -> POSIX pthread_mutex */
    const A840KmdSparseSmmuOps *smmu_ops;  /* Optional SMMU PTE/TLB callbacks */
    A840KmdSparseExternalVaOverlapFn external_overlap_fn;
    void *external_overlap_cookie;
} A840KmdSparseConfig;

typedef struct A840KmdSparseTeardownSummary {
    uint32_t regions_reclaimed;
    uint32_t tiles_reclaimed;
    uint32_t bo_refs_released;
    uint32_t bos_removed;
    uint64_t tlb_flush_generation;
} A840KmdSparseTeardownSummary;

typedef struct A840KmdSparseStats {
    uint32_t active_regions;
    uint32_t active_bos;
    uint32_t active_bound_tiles;
    uint32_t active_zero_tiles;
    uint64_t tlb_flush_generation;
    uint64_t total_regions_created;
    uint64_t total_regions_destroyed;
    uint64_t total_tile_binds;
    uint64_t total_tile_unbinds;
    uint64_t total_bo_auto_unbinds;
    uint64_t total_tlb_flushes;
    uint64_t rejected_zero_page_writes;
    uint64_t rejected_cross_client;
    uint64_t rejected_invalid_args;
} A840KmdSparseStats;

/* Internal fixed-capacity tables. */
typedef struct A840KmdSparseTileSlot {
    uint64_t bo_handle;
    uint64_t bo_offset;
    uint64_t phys_addr;
    uint32_t permissions;
    uint32_t is_zero_page;
} A840KmdSparseTileSlot;

typedef struct A840KmdSparseRegion {
    uint32_t active;
    uint32_t tile_count;
    uint64_t region_id;
    uint64_t client_id;
    void *smmu_ctx;
    uint64_t gpu_va;
    uint64_t length_bytes;
    uint64_t zero_page_va;
    uint64_t zero_page_phys;
    A840KmdSparseTileSlot tiles[A840_KMD_SPARSE_MAX_TILES_PER_REGION];
} A840KmdSparseRegion;

typedef struct A840KmdSparseBoEntry {
    uint32_t active;
    uint32_t permissions;
    uint32_t sparse_refcount;
    uint32_t reserved;
    uint64_t client_id;
    uint64_t bo_handle;
    uint64_t phys_base;
    uint64_t bytes;
} A840KmdSparseBoEntry;

typedef struct A840KmdSparseNormalRange {
    uint32_t active;
    uint32_t reserved;
    uint64_t client_id;
    uint64_t gpu_va;
    uint64_t length_bytes;
} A840KmdSparseNormalRange;

typedef struct A840KmdSparseManager {
    uint32_t initialized;
    uint32_t has_custom_sync;
    uint32_t has_smmu_ops;
    uint32_t reserved;
    uint64_t next_region_id;
    uint64_t tlb_flush_generation;
    A840KmdZeroPageManager *zero_page_mgr;
    A840KmdSparseSyncOps sync_ops;
    A840KmdSparseSmmuOps smmu_ops;
    A840KmdSparseExternalVaOverlapFn external_overlap_fn;
    void *external_overlap_cookie;
    A840KmdSparseStats stats;
    A840KmdSparseRegion regions[A840_KMD_SPARSE_MAX_REGIONS];
    A840KmdSparseBoEntry bos[A840_KMD_SPARSE_MAX_BOS];
    A840KmdSparseNormalRange normal_ranges[A840_KMD_SPARSE_MAX_NORMAL_RANGES];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdSparseManager;

/* Lifecycle */
A840KmdStatus a840_kmd_sparse_init(
    A840KmdSparseManager *mgr,
    const A840KmdSparseConfig *config);

void a840_kmd_sparse_destroy(A840KmdSparseManager *mgr);

/* Trusted BO ownership registration (64 KiB aligned phys_base and size). */
A840KmdStatus a840_kmd_sparse_register_bo(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t phys_base,
    uint64_t bytes,
    uint32_t permissions);

/* Query current sparse tile reference count for a client-owned BO. */
A840KmdStatus a840_kmd_sparse_get_bo_refcount(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t *out_refcount);

/* Track non-sparse VA mappings per client so sparse region creation rejects overlaps. */
A840KmdStatus a840_kmd_sparse_register_normal_range(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    uint64_t length_bytes);

A840KmdStatus a840_kmd_sparse_unregister_normal_range(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va);

/* Create a sparse VA region for `client_id`.
 * All tiles are initialized to the client's 64 KiB read-only GPU zero page.
 */
A840KmdStatus a840_kmd_sparse_region_create(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t gpu_va,
    uint64_t length_bytes,
    uint64_t *out_region_id);

/* Destroy one sparse VA region owned by `client_id`, decrementing all bound
 * BO reference counts and triggering a TLB flush outside the lock.
 */
A840KmdStatus a840_kmd_sparse_region_destroy(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t region_id);

/* Method 22 (NativeAdrenoMethodMapSparseV3) fixed-envelope batch dispatcher.
 * Validates all operations fail-closed before mutating state.
 */
A840KmdStatus a840_kmd_sparse_dispatch_v3(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    const A840KmdSparseBindReqV3 *req,
    size_t req_bytes,
    A840KmdSparseBindReplyV3 *reply,
    size_t reply_bytes);

/* Atomically unbind all tiles backed by (client_id, bo_handle) back to the
 * 64 KiB read-only zero page, bump tlb_flush_generation, and invoke SMMU
 * callbacks outside the lock. When `destroy_bo != 0`, also removes the BO
 * registration so subsequent binds to this handle fail closed.
 */
A840KmdStatus a840_kmd_sparse_unbind_bo(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t bo_handle,
    uint32_t destroy_bo,
    uint32_t *out_unbound_tiles,
    uint64_t *out_tlb_generation);

/* Query a single 64 KiB tile owned by `client_id`. */
A840KmdStatus a840_kmd_sparse_query_tile(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    uint64_t gpu_va,
    A840KmdSparseTileInfo *out_info);

/* Atomically reclaim all sparse regions, BO bindings, and normal ranges fo
 * `client_id`, emitting one coalesced TLB invalidation outside the lock.
 */
A840KmdStatus a840_kmd_sparse_client_teardown(
    A840KmdSparseManager *mgr,
    uint64_t client_id,
    A840KmdSparseTeardownSummary *out_summary);

A840KmdStatus a840_kmd_sparse_get_stats(
    A840KmdSparseManager *mgr,
    A840KmdSparseStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_SPARSE_VA_H */
