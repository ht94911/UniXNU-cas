/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_ZERO_PAGE_H
#define A840_KMD_ZERO_PAGE_H

#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoABI_v3.h"
#include "a840_abi_v3_host.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifndef A840_KMD_STATUS_DEFINED
#define A840_KMD_STATUS_DEFINED
typedef enum A840KmdStatus {
    A840_KMD_OK = 0,
    A840_KMD_PASS_TO_V2 = 1,
    A840_KMD_INVALID_ARG = 2,
    A840_KMD_RANGE = 3,
    A840_KMD_ALIGNMENT = 4,
    A840_KMD_PERMISSION_DENIED = 5,
    A840_KMD_CAPACITY = 6,
    A840_KMD_NOT_FOUND = 7,
    A840_KMD_STATE_CONFLICT = 8,
    A840_KMD_STACK_KEY_MISMATCH = 9,
    A840_KMD_UNSUPPORTED = 10,
    A840_KMD_POISONED = 11,
    A840_KMD_DISCONNECTED = 12,
    A840_KMD_IO_ERROR = 13,
    A840_KMD_NO_MEMORY = 14
} A840KmdStatus;
#endif

#define A840_KMD_ZERO_PAGE_BYTES A840_ABI_V3_ZERO_PAGE_BYTES
#define A840_KMD_ZERO_PAGE_ALIGN A840_ABI_V3_ZERO_PAGE_BYTES
#define A840_KMD_GPU_VA_LIMIT A840_ABI_V3_GPU_VA_LIMIT
#define A840_KMD_ZERO_PAGE_MAX_CLIENTS 128u

/* SMMU mapping permission flags aligned with NativeAdrenoABI v2 BoMap bits.
 * ZeroPage mappings strictly require A840_KMD_SMMU_PERM_READ_ONLY; any write
 * permission bit is unconditionally rejected.
 */
enum {
    A840_KMD_SMMU_PERM_READ_ONLY = 1u << 0,
    A840_KMD_SMMU_PERM_WRITE = 1u << 1,
    A840_KMD_SMMU_PERM_READ_WRITE = (1u << 0) | (1u << 1)
};

/* Abstract SMMU and physical backing-page operations.
 * Wave 4 XNU glue binds these to IOBufferMemoryDescriptor + NativeAdrenoVm.
 */
typedef struct A840KmdSmmuOps {
    void *cookie;
    /* Allocate a single global 64 KiB physical backing page. If out_cpu_ptr
     * receives a non-NULL kernel/host mapping, the manager explicitly zeroes
     * all `bytes` bytes before exposing the page to any client SMMU context.
     */
    A840KmdStatus (*alloc_backing_page)(
        void *cookie,
        uint64_t bytes,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle);
    /* Release the global 64 KiB physical backing page once refcount hits 0. */
    void (*free_backing_page)(
        void *cookie,
        uint64_t phys_addr,
        void *backing_handle,
        uint64_t bytes);
    /* Map the shared physical zero page into one client's SMMU context.
     * `permissions` is guaranteed by the manager to be PERM_READ_ONLY.
     */
    A840KmdStatus (*map_client_ro)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t phys_addr,
        uint64_t bytes,
        uint32_t permissions);
    /* Unmap the zero page from one client's SMMU context on client close. */
    A840KmdStatus (*unmap_client)(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t bytes);
} A840KmdSmmuOps;

typedef struct A840KmdZeroPageClientSlot {
    uint32_t active;
    uint32_t reserved;
    uint64_t client_id;
    void *smmu_ctx;
    uint64_t gpu_va;
} A840KmdZeroPageClientSlot;

typedef struct A840KmdZeroPageStats {
    uint32_t active_clients;
    uint32_t backing_allocated;
    uint64_t backing_phys_addr;
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_maps;
    uint64_t total_unmaps;
    uint64_t rejected_write_maps;
} A840KmdZeroPageStats;

typedef struct A840KmdZeroPageManager {
    uint32_t initialized;
    uint32_t backing_allocated;
    uint32_t refcount;
    uint32_t reserved;
    uint64_t default_gpu_va;
    uint64_t backing_phys_addr;
    void *backing_cpu_ptr;
    void *backing_handle;
    A840KmdSmmuOps ops;
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_maps;
    uint64_t total_unmaps;
    uint64_t rejected_write_maps;
    A840KmdZeroPageClientSlot clients[A840_KMD_ZERO_PAGE_MAX_CLIENTS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdZeroPageManager;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize manager with SMMU callbacks and canonical 64 KiB-aligned 48-bit
 * default GPU VA. Backing physical memory is lazily allocated on first bind.
 */
A840KmdStatus a840_kmd_zero_page_init(
    A840KmdZeroPageManager *mgr,
    const A840KmdSmmuOps *ops,
    uint64_t default_gpu_va);

/* Unmap any lingering clients, release backing page if live, and destroy lock. */
void a840_kmd_zero_page_destroy(A840KmdZeroPageManager *mgr);

/* Bind the shared 64 KiB RO zero page to `client_id`.
 * - `permissions` MUST be A840_KMD_SMMU_PERM_READ_ONLY (PERM_WRITE -> DENIED).
 * - `requested_gpu_va == 0` uses `default_gpu_va`; nonzero must be 64 KiB
 *   aligned within 48-bit canonical GPU VA space.
 * - Repeated calls for an already-bound client with matching VA (or 0) are
 *   idempotent and return the existing mapping without bumping refcount.
 */
A840KmdStatus a840_kmd_zero_page_bind_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t requested_gpu_va,
    uint32_t permissions,
    struct NativeAdrenoAbiZeroPageReplyV3 *out_reply);

/* Idempotent query for an already-bound client. */
A840KmdStatus a840_kmd_zero_page_query_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id,
    struct NativeAdrenoAbiZeroPageReplyV3 *out_reply);

/* Unmap zero page from `client_id` and decrement global refcount.
 * When the last client unbinds (refcount -> 0), the shared backing page is freed.
 */
A840KmdStatus a840_kmd_zero_page_unbind_client(
    A840KmdZeroPageManager *mgr,
    uint64_t client_id);

/* Snapshot current refcount and lifecycle statistics. */
A840KmdStatus a840_kmd_zero_page_get_stats(
    A840KmdZeroPageManager *mgr,
    A840KmdZeroPageStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_ZERO_PAGE_H */
