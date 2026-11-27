/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_TIER0_PACK_H
#define A840_KMD_TIER0_PACK_H

#include "a840_kmd_zero_page.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_PACK_MAX_BYTES = 64u * 1024u * 1024u,
    A840_PACK_MAX_CLIENTS = A840_KMD_ZERO_PAGE_MAX_CLIENTS,
    A840_PACK_SHA256_BYTES = 32
};

typedef struct A840KmdPackIdentity {
    uint64_t actual_stack_key, bytes;
    uint8_t sha256[A840_PACK_SHA256_BYTES];
} A840KmdPackIdentity;

typedef struct A840KmdPackImage {
    void *source;
    A840KmdPackIdentity identity;
} A840KmdPackImage;

typedef struct A840KmdPackViews {
    uint64_t gpu_va, cpu_va;
} A840KmdPackViews;

/* One manager per GPU. All callbacks except lock/unlock run under its
 * sleepable mutex; none may reenter. acquire retains an immutable source and
 * returns its loader-verified stack key, exact length and whole-file SHA-256.
 * Holding a source does not wire it. Failure transfers no resource.
 * wire_ro pins those exact bytes, with a private zeroed final-page tail.
 * bind_ro creates or extends this client's mapping atomically. On upgrade
 * the GPU VA stays stable. Errors preserve the old mapping. GPU PTEs and
 * both current and maximum CPU protections must exclude writes.
 * revoke synchronously completes CPU/GPU unmapping, TLB invalidation and
 * outstanding GPU reads; on failure the core retains the wire for retry. */
typedef struct A840KmdPackMapOps {
    void *cookie;
    void (*lock)(void *);
    void (*unlock)(void *);
    A840KmdStatus (*acquire)(void *, void *source, A840KmdPackImage *);
    void (*release)(void *, void *retained_source);
    A840KmdStatus (*wire_ro)(void *, const A840KmdPackImage *, uint32_t perm, void **backing);
    void (*unwire)(void *, void *backing);
    A840KmdStatus (*bind_ro)(void *, void *backing, uint64_t client, uint32_t perm,
        uint32_t map_cpu, A840KmdPackViews *views);
    A840KmdStatus (*revoke)(void *, void *backing, uint64_t client);
} A840KmdPackMapOps;

typedef enum A840KmdWarmState {
    A840_WARM_IDLE, A840_WARM_ARMED, A840_WARM_WARMING,
    A840_WARM_READY, A840_WARM_PAUSED_MEMORY_PRESSURE
} A840KmdWarmState;

typedef struct A840KmdPackClient {
    uint64_t client;
    uint32_t refs, bound, closing;
    A840KmdPackViews views;
} A840KmdPackClient;

typedef struct A840KmdPackManager {
    A840KmdPackMapOps ops;
    A840KmdPackImage active, pending;
    void *backing;
    A840KmdPackClient clients[A840_PACK_MAX_CLIENTS];
    uint64_t total_refs;
    uint32_t bound_clients, boot_complete, pressure;
    A840KmdWarmState warm; /* Unpaused state; pressure is an overlay. */
} A840KmdPackManager;

typedef struct A840KmdPackStats {
    A840KmdPackIdentity active;
    uint64_t total_refs;
    uint32_t backing_wire_count, bound_clients, replacement_pending, client_refs;
    A840KmdWarmState warm;
} A840KmdPackStats;

A840KmdStatus a840_kmd_pack_init(A840KmdPackManager *, const A840KmdPackMapOps *);
/* Quiesce external call dispatch before destroy. Failed revokes are retained;
 * retry destroy or close_client after the backend recovers. */
A840KmdStatus a840_kmd_pack_destroy(A840KmdPackManager *);
/* Nonblocking replacement. While old clients drain no new refs are issued.
 * The final successful revoke activates the pending source automatically. */
A840KmdStatus a840_kmd_pack_publish(A840KmdPackManager *, void *source);
A840KmdStatus a840_kmd_pack_open_client(A840KmdPackManager *, uint64_t client);
A840KmdStatus a840_kmd_pack_close_client(A840KmdPackManager *, uint64_t client);
A840KmdStatus a840_kmd_pack_map_shared_pack(A840KmdPackManager *, uint64_t client,
    const struct NativeAdrenoAbiSharedPackReqV3 *, uint32_t permissions,
    struct NativeAdrenoAbiSharedPackReplyV3 *);
A840KmdStatus a840_kmd_pack_unmap(A840KmdPackManager *, uint64_t client);
A840KmdStatus a840_kmd_pack_stats(A840KmdPackManager *, uint64_t client, A840KmdPackStats *);
void a840_kmd_pack_boot_complete(A840KmdPackManager *);
void a840_kmd_pack_set_params(A840KmdPackManager *, uint32_t params_flags);
/* Scheduler invokes one bounded work unit per budget grant. A pressure
 * notification drains the current unit and prevents any further dispatch. */
typedef A840KmdStatus (*A840KmdWarmUnit)(void *cookie, uint32_t *finished);
A840KmdStatus a840_kmd_pack_warm_step(A840KmdPackManager *, A840KmdWarmUnit, void *cookie);

#ifdef __cplusplus
}
#endif
#endif
