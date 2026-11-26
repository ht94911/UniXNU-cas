/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_WAIT_ANY_H
#define A840_KMD_WAIT_ANY_H

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

#define A840_KMD_WAIT_ANY_MAX_FENCES 32u
#define A840_KMD_WAIT_ANY_MAX_QUEUES 512u
#define A840_KMD_WAIT_ANY_MAX_SCOPES 128u
#define A840_KMD_WAIT_INFINITE UINT64_MAX

/* Optional OS sync & clock backend for zero-modification XNU integration
 * (Wave 4 binds lock/unlock/wait/wake_all to IOLock + thread_wakeup).
 * Passing NULL to a840_kmd_wait_any_init() selects the built-in POSIX
 * pthread_mutex + per-waiter CLOCK_MONOTONIC pthread_cond backend.
 */
typedef struct A840KmdWaitBackend {
    void *cookie;
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    /* Atomically release lock, sleep until wake_all() or deadline_ns (unless
     * infinite != 0), then re-acquire lock. Returns 0 on wake, nonzero on timeout.
     */
    int (*wait)(void *cookie, uint64_t deadline_ns, int infinite);
    void (*wake_all)(void *cookie);
    uint64_t (*now_ns)(void *cookie);
    /* Optional hardware always-on ticks reader; NULL uses latest IRQ ticks. */
    uint64_t (*read_ticks)(void *cookie);
} A840KmdWaitBackend;

typedef struct A840KmdQueueTimeline {
    uint32_t active;
    uint32_t poisoned;
    uint32_t disconnected;
    uint32_t scope_id;
    uint64_t queue_id;
    uint64_t completed_fence;
    uint64_t last_ticks;
} A840KmdQueueTimeline;

typedef struct A840KmdWaitScope {
    uint32_t active;
    uint32_t disconnected;
    uint32_t scope_id;
    uint32_t reserved;
} A840KmdWaitScope;

typedef struct A840KmdWaiterNode {
    struct A840KmdWaiterNode *prev;
    struct A840KmdWaiterNode *next;
    uint32_t scope_id;
    uint32_t count;
    uint64_t queues[A840_KMD_WAIT_ANY_MAX_FENCES];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_cond_t cond;
#endif
} A840KmdWaiterNode;

typedef struct A840KmdWaitAnyStats {
    uint32_t active_waiters;
    uint32_t active_queues;
    uint32_t poisoned_queues;
    uint32_t reserved;
    uint64_t latest_ticks;
    uint64_t fast_path_hits;
    uint64_t sleep_entries;
    uint64_t irq_signals;
    uint64_t targeted_wakes;
} A840KmdWaitAnyStats;

typedef struct A840KmdWaitAnyEngine {
    uint32_t initialized;
    uint32_t use_custom_backend;
    uint32_t active_waiter_count;
    uint32_t reserved;
    uint64_t latest_ticks;
    uint64_t fast_path_hits;
    uint64_t sleep_entries;
    uint64_t irq_signals;
    uint64_t targeted_wakes;
    A840AbiV3State default_abi_state;
    A840KmdWaitBackend backend;
    A840KmdWaiterNode *waiters_head;
    A840KmdQueueTimeline queues[A840_KMD_WAIT_ANY_MAX_QUEUES];
    A840KmdWaitScope scopes[A840_KMD_WAIT_ANY_MAX_SCOPES];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdWaitAnyEngine;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize WaitAny engine. Pass backend=NULL for built-in POSIX condvar backend. */
A840KmdStatus a840_kmd_wait_any_init(
    A840KmdWaitAnyEngine *engine,
    const A840KmdWaitBackend *backend);

void a840_kmd_wait_any_destroy(A840KmdWaitAnyEngine *engine);

/* Hardware IRQ / completion thread advances queue's monotonic timeline fence
 * and immediately wakes all blocked waiters observing `queue`.
 */
A840KmdStatus a840_kmd_wait_any_signal_fence(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue,
    uint64_t fence,
    uint64_t ticks);

/* Mark a single queue POISONED and immediately wake all waiters observing it. */
A840KmdStatus a840_kmd_wait_any_poison_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue);

/* Mark a standalone queue DISCONNECTED and wake all waiters observing it. */
A840KmdStatus a840_kmd_wait_any_disconnect_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue);

/* Per-client scope lifecycle used by UserClient v3 for race-free close(). */
A840KmdStatus a840_kmd_wait_any_register_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id);

A840KmdStatus a840_kmd_wait_any_disconnect_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id);

A840KmdStatus a840_kmd_wait_any_reclaim_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id);

/* Standalone blocking/fast-path WAIT_ANY / WAIT_ALL (Method 20). */
A840KmdStatus a840_kmd_wait_any_wait(
    A840KmdWaitAnyEngine *engine,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *reply);

/* Scoped WAIT_ANY / WAIT_ALL reusing a validated A840AbiV3State. */
A840KmdStatus a840_kmd_wait_any_wait_scoped(
    A840KmdWaitAnyEngine *engine,
    const A840AbiV3State *abi_state,
    uint32_t scope_id,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *reply);

A840KmdStatus a840_kmd_wait_any_query_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue,
    uint64_t *out_completed_fence,
    int *out_poisoned);

A840KmdStatus a840_kmd_wait_any_get_stats(
    A840KmdWaitAnyEngine *engine,
    A840KmdWaitAnyStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_WAIT_ANY_H */
