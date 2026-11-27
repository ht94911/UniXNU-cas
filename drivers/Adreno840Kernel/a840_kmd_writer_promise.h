/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_WRITER_PROMISE_H
#define A840_KMD_WRITER_PROMISE_H

#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoABI_v3.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_fault_capture.h"

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-capacity registry bounds: zero hot-path heap allocations. */
#define A840_KMD_PROMISE_MAX_CLIENTS 128u
#define A840_KMD_PROMISE_MAX_SURFACES 256u
#define A840_KMD_PROMISE_MAX_SLOTS 128u
#define A840_KMD_PROMISE_MAX_QUEUES 256u
#define A840_KMD_PROMISE_MAX_SURFACES_PER_REQ 16u

/* Client privilege roles for Method 24 handoff policy. */
typedef enum A840ClientRole {
    A840_CLIENT_ROLE_APP = 0u,
    A840_CLIENT_ROLE_RENDER_SERVER = 1u
} A840ClientRole;

/* Method 24 caller policy flags (kernel-internal policy knob / ioctl flags). */
enum {
    A840_PROMISE_FLAG_NONE = 0u,
    A840_PROMISE_FLAG_EARLY_HANDOFF = 1u << 0,
    A840_PROMISE_FLAG_STRICT_REJECT_APP_EARLY = 1u << 1,
    A840_PROMISE_FLAG_KNOWN_MASK =
        (A840_PROMISE_FLAG_EARLY_HANDOFF |
         A840_PROMISE_FLAG_STRICT_REJECT_APP_EARLY)
};

/* Promise publication mode decided by KMD privilege policy. */
typedef enum A840PromiseHandoffPolicy {
    A840_PROMISE_POLICY_NONE = 0u,
    A840_PROMISE_POLICY_EARLY_SCHEDULED = 1u,
    A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED = 2u
} A840PromiseHandoffPolicy;

/* Promise slot lifecycle state. */
typedef enum A840PromiseState {
    A840_PROMISE_STATE_FREE = 0u,
    A840_PROMISE_STATE_PENDING = 1u,
    A840_PROMISE_STATE_SCHEDULED_READY = 2u,
    A840_PROMISE_STATE_DEFERRED_WAITING_FENCE = 3u,
    A840_PROMISE_STATE_RETIRED_READY = 4u,
    A840_PROMISE_STATE_CANCELED = 5u,
    A840_PROMISE_STATE_FAILED = 6u
} A840PromiseState;

/* Reason attached to terminal failure/cancellation events. */
typedef enum A840PromiseCancelReason {
    A840_PROMISE_CANCEL_NONE = 0u,
    A840_PROMISE_CANCEL_EXPLICIT = 1u,
    A840_PROMISE_CANCEL_GPU_FAULT = 2u,
    A840_PROMISE_CANCEL_CHANNEL_RESET = 3u,
    A840_PROMISE_CANCEL_CLIENT_CLOSE = 4u,
    A840_PROMISE_CANCEL_SUPERSEDED = 5u
} A840PromiseCancelReason;

/* Optional external sleepable mutex + clock backend (NULL -> POSIX pthread). */
typedef struct A840KmdPromiseSyncOps {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    int (*wait)(void *cookie, uint64_t deadline_ns, int infinite);
    void (*wake_all)(void *cookie);
    uint64_t (*now_ns)(void *cookie);
} A840KmdPromiseSyncOps;

/* Trusted surface ownership / identity resolver callback.
 * Maps a client-local surface_handle to a kernel-verified surface_identity.
 * Returns A840_KMD_OK on authorized ownership, A840_KMD_PERMISSION_DENIED if
 * owned by another client, or A840_KMD_NOT_FOUND if unknown.
 */
typedef A840KmdStatus (*A840KmdSurfaceIdentityResolver)(
    void *cookie,
    uint64_t writer_client,
    uint64_t surface_handle,
    uint64_t *out_surface_identity);

/* Immutable snapshot delivered to lock-free observer callbacks and queries. */
typedef struct A840KmdPromiseEvent {
    uint64_t promise_token;
    uint64_t writer_client;
    uint64_t surface_identity;
    uint64_t surface_handle;
    uint64_t generation;
    uint64_t queue;
    uint64_t fence;
    uint32_t client_role;
    uint32_t policy;
    uint32_t state;
    uint32_t cancel_reason;
} A840KmdPromiseEvent;

/* Invoked STRICTLY OUTSIDE the registry mutex so display / compositor
 * observers can safely inspect or wake downstream queues without inversion.
 */
typedef void (*A840KmdPromiseObserver)(
    void *cookie,
    const A840KmdPromiseEvent *event);

/* Method 24 extended call context carrying trusted kernel privilege metadata
 * alongside the frozen 160-byte ABI request.
 */
typedef struct A840KmdWriterPromiseCall {
    uint64_t writer_client;
    uint64_t generation;     /* 0 = auto-monotonic next; nonzero must strictly increase */
    uint32_t flags;          /* A840_PROMISE_FLAG_* */
    uint32_t reserved;       /* MUST be 0 (fail-closed) */
    uint32_t has_surface_identities; /* 0 = use resolver/table; 1 = verify explicit ids */
    uint32_t reserved2;      /* MUST be 0 (fail-closed) */
    uint64_t surface_identities[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
    struct NativeAdrenoAbiWriterPromiseV3 abi;
} A840KmdWriterPromiseCall;

/* Snapshot returned when querying or waiting for a surface writer promise. */
typedef struct A840KmdSurfacePromiseSnapshot {
    uint32_t valid;
    uint32_t display_ready;
    uint32_t client_role;
    uint32_t policy;
    uint32_t state;
    uint32_t cancel_reason;
    uint32_t fence_retired;
    uint32_t reserved;
    uint64_t surface_identity;
    uint64_t writer_client;
    uint64_t generation;
    uint64_t promise_token;
    uint64_t queue;
    uint64_t fence;
} A840KmdSurfacePromiseSnapshot;

typedef struct A840KmdPromiseClientSlot {
    uint32_t active;
    uint32_t role;
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t client_id;
} A840KmdPromiseClientSlot;

typedef struct A840KmdPromiseSurfaceAuth {
    uint32_t active;
    uint32_t reserved;
    uint64_t owner_client;
    uint64_t surface_handle;
    uint64_t surface_identity;
} A840KmdPromiseSurfaceAuth;

typedef struct A840KmdPromiseQueueState {
    uint32_t active;
    uint32_t poisoned;
    uint64_t writer_client;
    uint64_t queue;
    uint64_t completed_fence;
} A840KmdPromiseQueueState;

typedef struct A840KmdPromiseSlot {
    uint32_t state;
    uint32_t client_role;
    uint32_t policy;
    uint32_t cancel_reason;
    uint32_t surface_count;
    uint32_t fence_retired;
    uint32_t notify_batch;
    uint32_t notify_index;
    uint64_t promise_token;
    uint64_t writer_client;
    uint64_t queue;
    uint64_t fence;
    uint64_t surface_handles[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
    uint64_t surface_identities[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
    uint64_t generations[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
} A840KmdPromiseSlot;

typedef struct A840KmdSurfaceHead {
    uint32_t active;
    uint32_t client_role;
    uint32_t policy;
    uint32_t state;
    uint32_t cancel_reason;
    uint32_t fence_retired;
    uint64_t surface_identity;
    uint64_t writer_client;
    uint64_t generation;
    uint64_t promise_token;
    uint64_t queue;
    uint64_t fence;
} A840KmdSurfaceHead;

typedef struct A840KmdPromiseWaiter {
    struct A840KmdPromiseWaiter *prev;
    struct A840KmdPromiseWaiter *next;
    uint64_t surface_identity;
    uint64_t min_generation;
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_cond_t cond;
#endif
} A840KmdPromiseWaiter;

typedef struct A840KmdPromiseStats {
    uint64_t total_made;
    uint64_t total_early_scheduled;
    uint64_t total_deferred_app;
    uint64_t total_retired_ready;
    uint64_t total_canceled_explicit;
    uint64_t total_canceled_on_fault;
    uint64_t total_canceled_on_close;
    uint64_t total_superseded;
    uint64_t total_rejected_calls;
    uint32_t active_clients;
    uint32_t pending_promises;
    uint32_t active_waiters;
    uint32_t reserved;
} A840KmdPromiseStats;

typedef struct A840KmdPromiseConfig {
    const A840KmdPromiseSyncOps *sync_ops; /* NULL = built-in POSIX mutex+cond */
    A840KmdWaitAnyEngine *wait_any;        /* Optional WaitAny engine for IRQ/poison linkage */
    A840KmdFaultCapture *fault_capture;    /* Optional FaultCapture for poisoned-channel guard */
    A840KmdSurfaceIdentityResolver surface_resolver;
    void *surface_resolver_cookie;
    A840KmdPromiseObserver observer;
    void *observer_cookie;
} A840KmdPromiseConfig;

typedef struct A840KmdWriterPromiseRegistry {
    uint32_t initialized;
    uint32_t use_custom_sync;
    uint32_t active_waiter_count;
    uint32_t next_notify_batch;
    uint64_t last_promise_token;
    A840KmdPromiseSyncOps sync_ops;
    A840KmdWaitAnyEngine *wait_any;
    A840KmdFaultCapture *fault_capture;
    A840KmdSurfaceIdentityResolver surface_resolver;
    void *surface_resolver_cookie;
    A840KmdPromiseObserver observer;
    void *observer_cookie;
    A840KmdPromiseWaiter *waiters_head;
    A840KmdPromiseStats stats;
    A840KmdPromiseClientSlot clients[A840_KMD_PROMISE_MAX_CLIENTS];
    A840KmdPromiseSurfaceAuth surface_auth[A840_KMD_PROMISE_MAX_SURFACES];
    A840KmdPromiseQueueState queues[A840_KMD_PROMISE_MAX_QUEUES];
    A840KmdSurfaceHead surface_heads[A840_KMD_PROMISE_MAX_SURFACES];
    A840KmdPromiseSlot slots[A840_KMD_PROMISE_MAX_SLOTS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdWriterPromiseRegistry;

/* Lifecycle */
A840KmdStatus a840_kmd_writer_promise_init(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdPromiseConfig *config);

void a840_kmd_writer_promise_destroy(A840KmdWriterPromiseRegistry *reg);

A840KmdStatus a840_kmd_writer_promise_set_observer(
    A840KmdWriterPromiseRegistry *reg,
    A840KmdPromiseObserver observer,
    void *cookie);

/* Trusted client registration with explicit role (RenderServer vs App). */
A840KmdStatus a840_kmd_writer_promise_register_client(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t client_id,
    A840ClientRole role);

/* Trusted surface ownership registration.
 * Maps (owner_client, surface_handle) -> surface_identity.
 */
A840KmdStatus a840_kmd_writer_promise_bind_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t owner_client,
    uint64_t surface_handle,
    uint64_t surface_identity);

void a840_kmd_writer_promise_unbind_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t owner_client,
    uint64_t surface_handle);

/* Method 24 core entry point (Make / Fulfill / Cancel).
 * Validates frozen ABI constraints, client ownership, monotonic generation,
 * reserved==0, poisoned channel guard, and RenderServer vs App handoff policy.
 * On error, guarantees zero state mutation (fail-closed) and untouched reply.
 */
A840KmdStatus a840_kmd_writer_promise_dispatch(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdWriterPromiseCall *call,
    struct NativeAdrenoAbiWriterPromiseReplyV3 *reply,
    A840PromiseHandoffPolicy *out_policy);

/* Hardware IRQ / timeline retirement hook: marks all deferred/scheduled promises
 * on (writer_client, queue) with fence <= completed_fence as retired-ready,
 * advances WaitAny if linked, wakes blocked display waiters, and fires observer
 * callbacks strictly outside the lock.
 */
size_t a840_kmd_writer_promise_signal_fence(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue,
    uint64_t completed_fence,
    uint64_t ticks);

/* Fault / channel reset cancellation: atomically transitions all non-retired
 * promises on (writer_client, queue) to FAILED, wakes all promise waiters,
 * poisons the linked WaitAny queue, and invokes observer callbacks outside lock.
 */
size_t a840_kmd_writer_promise_fail_channel(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue,
    A840PromiseCancelReason reason);

/* Adapter callback matching A840KmdFaultPoisonCallback signature for direct
 * wiring to a840_kmd_fault_set_poison_callback(fc, ..., reg).
 */
void a840_kmd_writer_promise_on_fault_poison(
    void *cookie,
    uint64_t client_id,
    uint64_t queue_id,
    const A840KmdFaultSnapshot *snapshot);

/* Client close cleanup: atomically cancels all in-flight promises owned by
 * writer_client, wakes all waiters, disconnects linked WaitAny queues, removes
 * client auth bindings, and notifies observers outside the lock.
 */
size_t a840_kmd_writer_promise_close_client(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client);

/* Query latest promise state for a surface_identity (writer_client_filter == 0 matches any). */
A840KmdStatus a840_kmd_writer_promise_query_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t writer_client_filter,
    A840KmdSurfacePromiseSnapshot *out_snapshot);

/* Query a specific promise by token. */
A840KmdStatus a840_kmd_writer_promise_query_token(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t promise_token,
    A840KmdPromiseEvent *out_event);

/* Wait until the latest promise for (surface_identity, generation >= min_generation)
 * becomes display-ready (SCHEDULED_READY for RenderServer or RETIRED_READY for retired)
 * or terminates in FAILED/CANCELED.
 */
A840KmdStatus a840_kmd_writer_promise_wait_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t min_generation,
    uint64_t timeout_ns,
    A840KmdSurfacePromiseSnapshot *out_snapshot,
    uint32_t *out_timed_out);

size_t a840_kmd_writer_promise_pending_count(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client);

A840KmdStatus a840_kmd_writer_promise_get_stats(
    A840KmdWriterPromiseRegistry *reg,
    A840KmdPromiseStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_WRITER_PROMISE_H */
