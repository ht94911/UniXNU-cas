/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_USERCLIENT_V3_H
#define A840_KMD_USERCLIENT_V3_H

#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoABI_v3.h"
#include "a840_abi_v3_host.h"
#include "a840_kmd_zero_page.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_fault_capture.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#define A840_KMD_V2_METHOD_MIN 1u
#define A840_KMD_V2_METHOD_MAX 19u
#define A840_KMD_MAX_CLIENTS 64u

#define A840_KMD_V3_DEFAULT_CAPS \
    (NativeAdrenoParamsV2HostImport | \
     NativeAdrenoParamsV2OwnedCoherent | \
     NativeAdrenoParamsV2SubmitBatch | \
     NativeAdrenoParamsV3TimelineWaitAny | \
     NativeAdrenoParamsV3SharedZeroPage | \
     NativeAdrenoParamsV3SharedTier0Pack | \
     NativeAdrenoParamsV3WriterPromise | \
     NativeAdrenoParamsV3UbwcSurfaceMeta)

/* Callback querying the byte size of an owned BO for Method 25 (SurfaceMetaV3).
 * Must return A840_KMD_OK and set *out_bo_bytes if `client_id` owns `bo_handle`.
 */
typedef A840KmdStatus (*A840KmdBoSizeLookup)(
    void *cookie,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t *out_bo_bytes);

typedef struct A840KmdUserClientConfig {
    uint32_t cap_flags;              /* 0 selects A840_KMD_V3_DEFAULT_CAPS */
    uint32_t reserved;
    uint64_t zero_page_default_va;   /* 64 KiB aligned 48-bit canonical GPU VA */
    A840KmdSmmuOps smmu_ops;         /* required when SharedZeroPage is enabled */
    const A840KmdWaitBackend *wait_backend; /* NULL uses POSIX condvar backend */
    A840KmdBoSizeLookup bo_size_lookup;
    void *bo_size_cookie;
    A840AbiV3PromiseObserver promise_observer;
    void *promise_observer_cookie;
} A840KmdUserClientConfig;

enum {
    A840_KMD_CLIENT_FREE = 0u,
    A840_KMD_CLIENT_OPEN = 1u,
    A840_KMD_CLIENT_CLOSING = 2u
};

typedef struct A840KmdClientSession {
    uint32_t state;
    uint32_t scope_id;
    uint32_t active_waiters;
    uint32_t zero_page_bound;
    uint64_t client_id;
    void *smmu_ctx;
    struct NativeAdrenoAbiZeroPageReplyV3 zero_page;
} A840KmdClientSession;

typedef struct A840KmdClientCloseSummary {
    size_t canceled_promises;
    size_t cleared_poisoned_channels;
    uint32_t zero_page_unbound;
    uint32_t reserved;
} A840KmdClientCloseSummary;

typedef struct A840KmdUserClientV3 {
    uint32_t initialized;
    uint32_t open_client_count;
    uint32_t next_scope_id;
    uint32_t zero_page_enabled;
    A840AbiV3State abi_state;
    A840AbiV3State wait_abi_state;
    A840KmdZeroPageManager zero_page;
    A840KmdWaitAnyEngine wait_any;
    A840KmdFaultCapture fault_capture;
    A840KmdBoSizeLookup bo_size_lookup;
    void *bo_size_cookie;
    A840KmdClientSession clients[A840_KMD_MAX_CLIENTS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
    pthread_cond_t drain_cond;
#endif
} A840KmdUserClientV3;

#ifdef __cplusplus
extern "C" {
#endif

A840KmdStatus a840_kmd_userclient_v3_init(
    A840KmdUserClientV3 *uc,
    const A840KmdUserClientConfig *config);

void a840_kmd_userclient_v3_destroy(A840KmdUserClientV3 *uc);

uint32_t a840_kmd_userclient_v3_advertised_caps(const A840KmdUserClientV3 *uc);

A840KmdStatus a840_kmd_userclient_v3_configure_shared_pack(
    A840KmdUserClientV3 *uc,
    const struct NativeAdrenoAbiSharedPackReplyV3 *pack,
    uint32_t gpu_readonly,
    uint32_t cpu_readonly);

A840KmdStatus a840_kmd_userclient_v3_set_promise_observer(
    A840KmdUserClientV3 *uc,
    A840AbiV3PromiseObserver observer,
    void *cookie);

/* Open a client session, allocate a unique WaitAny queue scope, and bind the
 * shared 64 KiB RO zero page when SharedZeroPage capability is active.
 * `requested_zero_page_va == 0` uses config->zero_page_default_va.
 */
A840KmdStatus a840_kmd_userclient_v3_open_client(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t requested_zero_page_va);

/* One-stop client close: wakes and drains all in-flight WaitAny threads,
 * cancels all pending WriterPromises (notifying observer once each), clears
 * SurfaceMeta entries, unbinds ZeroPage mapping, clears active poison state,
 * and reclaims WaitAny queue timelines.
 */
A840KmdStatus a840_kmd_userclient_v3_close_client(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    A840KmdClientCloseSummary *out_summary);

/* External-Method v3 3104-byte envelope dispatcher:
 * - Rejects input_bytes > 3104 or output_bytes > 3104 with A840_KMD_RANGE.
 * - Returns A840_KMD_PASS_TO_V2 for v2 methods 1..19 unchanged.
 * - Returns A840_KMD_UNSUPPORTED for Methods 22, 26, 0, and >26.
 * - Dispatches v3 methods 20, 21, 23, 24, 25 with exact record size checks.
 */
A840KmdStatus a840_kmd_userclient_v3_dispatch(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint32_t method,
    const void *input,
    size_t input_bytes,
    void *output,
    size_t output_bytes,
    size_t *out_written_bytes);

/* Hardware IRQ completion hook for one client's local queue. */
A840KmdStatus a840_kmd_userclient_v3_signal_fence(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue,
    uint64_t fence,
    uint64_t ticks);

/* Pre-submission channel health guard: returns A840_KMD_POISONED if the
 * channel was isolated by a GPU hang or SMMU fault, else A840_KMD_OK.
 */
A840KmdStatus a840_kmd_userclient_v3_check_submit_channel(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue);

/* Report a GPU hang or SMMU fault: captures snapshot into circular ring,
 * isolates ONLY (client_id, queue_id) as POISONED, and wakes blocked waiters
 * on that channel without affecting any other healthy client/channel.
 */
A840KmdStatus a840_kmd_userclient_v3_report_fault(
    A840KmdUserClientV3 *uc,
    const A840KmdFaultInput *input,
    A840KmdFaultSnapshot *out_snapshot);

/* Query the unique internal WaitAny queue key for an open client's local queue. */
A840KmdStatus a840_kmd_userclient_v3_queue_key(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue,
    uint64_t *out_scoped_queue);

size_t a840_kmd_userclient_v3_pending_promises(
    A840KmdUserClientV3 *uc,
    uint64_t client_id);

uint32_t a840_kmd_userclient_v3_open_client_count(A840KmdUserClientV3 *uc);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_USERCLIENT_V3_H */
