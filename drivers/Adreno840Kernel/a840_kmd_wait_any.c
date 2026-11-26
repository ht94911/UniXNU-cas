/* SPDX-License-Identifier: MIT */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_wait_any.h"
#include <errno.h>
#include <string.h>
#include <time.h>

#define A840_KMD_WAIT_MAGIC 1463896404u
#define A840_KMD_NS_PER_SEC UINT64_C(1000000000)
#define A840_KMD_MAX_SLEEP_CHUNK_NS (UINT64_C(31536000) * A840_KMD_NS_PER_SEC)

static A840KmdStatus map_abi_status(A840AbiV3Status st)
{
    switch (st) {
    case A840_ABI_V3_OK: return A840_KMD_OK;
    case A840_ABI_V3_INVALID_ARG: return A840_KMD_INVALID_ARG;
    case A840_ABI_V3_RANGE: return A840_KMD_RANGE;
    case A840_ABI_V3_ALIGNMENT: return A840_KMD_ALIGNMENT;
    case A840_ABI_V3_CAPACITY: return A840_KMD_CAPACITY;
    case A840_ABI_V3_NOT_FOUND: return A840_KMD_NOT_FOUND;
    case A840_ABI_V3_STATE_CONFLICT: return A840_KMD_STATE_CONFLICT;
    case A840_ABI_V3_STACK_KEY_MISMATCH: return A840_KMD_STACK_KEY_MISMATCH;
    case A840_ABI_V3_UNSUPPORTED: return A840_KMD_UNSUPPORTED;
    default: return A840_KMD_INVALID_ARG;
    }
}

static A840KmdStatus check_init(const A840KmdWaitAnyEngine *engine)
{
    if (engine == NULL)
        return A840_KMD_INVALID_ARG;
    if (engine->initialized != A840_KMD_WAIT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static void eng_lock(A840KmdWaitAnyEngine *engine)
{
    if (engine->use_custom_backend) {
        engine->backend.lock(engine->backend.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_lock(&engine->lock);
#endif
    }
}

static void eng_unlock(A840KmdWaitAnyEngine *engine)
{
    if (engine->use_custom_backend) {
        engine->backend.unlock(engine->backend.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_unlock(&engine->lock);
#endif
    }
}

static uint64_t eng_now_ns(const A840KmdWaitAnyEngine *engine)
{
    if (engine->use_custom_backend)
        return engine->backend.now_ns(engine->backend.cookie);
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return 0u;
        return (uint64_t)ts.tv_sec * A840_KMD_NS_PER_SEC + (uint64_t)ts.tv_nsec;
    }
}

static uint64_t eng_observed_ticks_locked(const A840KmdWaitAnyEngine *engine)
{
    if (engine->use_custom_backend && engine->backend.read_ticks != NULL) {
        uint64_t hw_ticks = engine->backend.read_ticks(engine->backend.cookie);
        return hw_ticks > engine->latest_ticks ? hw_ticks : engine->latest_ticks;
    }
    return engine->latest_ticks;
}

static A840KmdWaitScope *find_scope_locked(
    A840KmdWaitAnyEngine *engine, uint32_t scope_id)
{
    size_t i;
    if (scope_id == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_SCOPES; ++i) {
        if (engine->scopes[i].active && engine->scopes[i].scope_id == scope_id)
            return &engine->scopes[i];
    }
    return NULL;
}

static A840KmdQueueTimeline *find_queue_locked(
    A840KmdWaitAnyEngine *engine, uint64_t queue)
{
    size_t i;
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active && engine->queues[i].queue_id == queue)
            return &engine->queues[i];
    }
    return NULL;
}

static A840KmdQueueTimeline *find_or_alloc_queue_locked(
    A840KmdWaitAnyEngine *engine, uint64_t queue)
{
    size_t i;
    A840KmdQueueTimeline *free_slot = NULL;
    uint32_t maybe_scope;
    A840KmdWaitScope *scope;

    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active) {
            if (engine->queues[i].queue_id == queue)
                return &engine->queues[i];
        } else if (free_slot == NULL) {
            free_slot = &engine->queues[i];
        }
    }
    if (free_slot == NULL)
        return NULL;

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->queue_id = queue;
    maybe_scope = (uint32_t)(queue >> 32u);
    scope = find_scope_locked(engine, maybe_scope);
    if (scope != NULL) {
        free_slot->scope_id = maybe_scope;
        free_slot->disconnected = scope->disconnected;
    }
    return free_slot;
}

static int waiter_watches_queue(const A840KmdWaiterNode *node, uint64_t queue)
{
    uint32_t i;
    for (i = 0u; i < node->count; ++i) {
        if (node->queues[i] == queue)
            return 1;
    }
    return 0;
}

static void wake_waiters_for_queue_locked(
    A840KmdWaitAnyEngine *engine, uint64_t queue)
{
    if (engine->use_custom_backend) {
        if (engine->active_waiter_count > 0u) {
            engine->targeted_wakes++;
            engine->backend.wake_all(engine->backend.cookie);
        }
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    {
        A840KmdWaiterNode *node = engine->waiters_head;
        while (node != NULL) {
            if (waiter_watches_queue(node, queue)) {
                engine->targeted_wakes++;
                (void)pthread_cond_signal(&node->cond);
            }
            node = node->next;
        }
    }
#else
    (void)queue;
#endif
}

static void wake_waiters_for_scope_locked(
    A840KmdWaitAnyEngine *engine, uint32_t scope_id)
{
    if (engine->use_custom_backend) {
        if (engine->active_waiter_count > 0u) {
            engine->targeted_wakes++;
            engine->backend.wake_all(engine->backend.cookie);
        }
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    {
        A840KmdWaiterNode *node = engine->waiters_head;
        while (node != NULL) {
            int match = (node->scope_id == scope_id);
            if (!match) {
                uint32_t i;
                for (i = 0u; i < node->count; ++i) {
                    if ((uint32_t)(node->queues[i] >> 32u) == scope_id) {
                        match = 1;
                        break;
                    }
                }
            }
            if (match) {
                engine->targeted_wakes++;
                (void)pthread_cond_signal(&node->cond);
            }
            node = node->next;
        }
    }
#else
    (void)scope_id;
#endif
}

static A840AbiV3Status query_fence_locked(
    void *cookie, uint64_t queue, uint64_t *completed_fence)
{
    A840KmdWaitAnyEngine *engine = (A840KmdWaitAnyEngine *)cookie;
    A840KmdQueueTimeline *slot;
    if (engine == NULL || completed_fence == NULL)
        return A840_ABI_V3_INVALID_ARG;
    slot = find_queue_locked(engine, queue);
    *completed_fence = (slot != NULL) ? slot->completed_fence : 0u;
    return A840_ABI_V3_OK;
}

static A840KmdStatus check_abort_conditions_locked(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id,
    const struct NativeAdrenoAbiWaitAnyV3 *request)
{
    uint32_t i;
    if (scope_id != 0u) {
        A840KmdWaitScope *scope = find_scope_locked(engine, scope_id);
        if (scope == NULL || scope->disconnected != 0u)
            return A840_KMD_DISCONNECTED;
    }
    for (i = 0u; i < request->count; ++i) {
        A840KmdQueueTimeline *slot = find_queue_locked(engine, request->queues[i]);
        if (slot != NULL) {
            if (slot->disconnected != 0u)
                return A840_KMD_DISCONNECTED;
            if (slot->poisoned != 0u)
                return A840_KMD_POISONED;
        }
    }
    return A840_KMD_OK;
}

static int reply_is_satisfied(
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    const struct NativeAdrenoAbiWaitAnyReplyV3 *reply)
{
    uint32_t wanted = (request->count == A840_KMD_WAIT_ANY_MAX_FENCES) ?
        UINT32_MAX : ((UINT32_C(1) << request->count) - 1u);
    if ((request->flags & 1u) != 0u)
        return reply->satisfied_mask == wanted;
    return reply->satisfied_mask != 0u;
}

A840KmdStatus a840_kmd_wait_any_init(
    A840KmdWaitAnyEngine *engine,
    const A840KmdWaitBackend *backend)
{
    A840AbiV3Status abi_st;

    if (engine == NULL)
        return A840_KMD_INVALID_ARG;

    if (backend != NULL) {
        if (backend->lock == NULL || backend->unlock == NULL ||
            backend->wait == NULL || backend->wake_all == NULL ||
            backend->now_ns == NULL)
            return A840_KMD_INVALID_ARG;
    }
#if defined(A840_KMD_CUSTOM_SYNC)
    else {
        return A840_KMD_INVALID_ARG;
    }
#endif

    memset(engine, 0, sizeof(*engine));
    abi_st = a840_abi_v3_init(&engine->default_abi_state,
        NativeAdrenoParamsV3TimelineWaitAny);
    if (abi_st != A840_ABI_V3_OK)
        return map_abi_status(abi_st);

    if (backend != NULL) {
        engine->backend = *backend;
        engine->use_custom_backend = 1u;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&engine->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#endif
        engine->use_custom_backend = 0u;
    }

    engine->initialized = A840_KMD_WAIT_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_wait_any_destroy(A840KmdWaitAnyEngine *engine)
{
    size_t i;
    if (check_init(engine) != A840_KMD_OK)
        return;

    eng_lock(engine);
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_SCOPES; ++i) {
        if (engine->scopes[i].active)
            engine->scopes[i].disconnected = 1u;
    }
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active)
            engine->queues[i].disconnected = 1u;
    }
    if (engine->use_custom_backend) {
        engine->backend.wake_all(engine->backend.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        A840KmdWaiterNode *node = engine->waiters_head;
        while (node != NULL) {
            (void)pthread_cond_signal(&node->cond);
            node = node->next;
        }
#endif
    }
    engine->initialized = 0u;
    eng_unlock(engine);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!engine->use_custom_backend)
        (void)pthread_mutex_destroy(&engine->lock);
#endif
}

A840KmdStatus a840_kmd_wait_any_signal_fence(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue,
    uint64_t fence,
    uint64_t ticks)
{
    A840KmdStatus status = check_init(engine);
    A840KmdQueueTimeline *slot;

    if (status != A840_KMD_OK)
        return status;

    eng_lock(engine);
    slot = find_or_alloc_queue_locked(engine, queue);
    if (slot == NULL) {
        eng_unlock(engine);
        return A840_KMD_CAPACITY;
    }
    if (slot->disconnected != 0u) {
        eng_unlock(engine);
        return A840_KMD_DISCONNECTED;
    }
    if (slot->poisoned != 0u) {
        eng_unlock(engine);
        return A840_KMD_POISONED;
    }

    if (fence > slot->completed_fence)
        slot->completed_fence = fence;
    if (ticks > slot->last_ticks)
        slot->last_ticks = ticks;
    if (ticks > engine->latest_ticks)
        engine->latest_ticks = ticks;
    engine->irq_signals++;

    wake_waiters_for_queue_locked(engine, queue);
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_poison_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue)
{
    A840KmdStatus status = check_init(engine);
    A840KmdQueueTimeline *slot;

    if (status != A840_KMD_OK)
        return status;

    eng_lock(engine);
    slot = find_or_alloc_queue_locked(engine, queue);
    if (slot == NULL) {
        eng_unlock(engine);
        return A840_KMD_CAPACITY;
    }
    slot->poisoned = 1u;
    wake_waiters_for_queue_locked(engine, queue);
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_disconnect_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue)
{
    A840KmdStatus status = check_init(engine);
    A840KmdQueueTimeline *slot;

    if (status != A840_KMD_OK)
        return status;

    eng_lock(engine);
    slot = find_or_alloc_queue_locked(engine, queue);
    if (slot == NULL) {
        eng_unlock(engine);
        return A840_KMD_CAPACITY;
    }
    slot->disconnected = 1u;
    wake_waiters_for_queue_locked(engine, queue);
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_register_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id)
{
    A840KmdStatus status = check_init(engine);
    A840KmdWaitScope *free_slot = NULL;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (scope_id == 0u)
        return A840_KMD_INVALID_ARG;

    eng_lock(engine);
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_SCOPES; ++i) {
        if (engine->scopes[i].active) {
            if (engine->scopes[i].scope_id == scope_id) {
                eng_unlock(engine);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &engine->scopes[i];
        }
    }
    if (free_slot == NULL) {
        eng_unlock(engine);
        return A840_KMD_CAPACITY;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->scope_id = scope_id;
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_disconnect_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id)
{
    A840KmdStatus status = check_init(engine);
    A840KmdWaitScope *scope;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (scope_id == 0u)
        return A840_KMD_INVALID_ARG;

    eng_lock(engine);
    scope = find_scope_locked(engine, scope_id);
    if (scope == NULL) {
        eng_unlock(engine);
        return A840_KMD_NOT_FOUND;
    }
    scope->disconnected = 1u;
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active &&
            (engine->queues[i].scope_id == scope_id ||
             (uint32_t)(engine->queues[i].queue_id >> 32u) == scope_id)) {
            engine->queues[i].disconnected = 1u;
        }
    }
    wake_waiters_for_scope_locked(engine, scope_id);
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_reclaim_scope(
    A840KmdWaitAnyEngine *engine,
    uint32_t scope_id)
{
    A840KmdStatus status = check_init(engine);
    A840KmdWaitScope *scope;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (scope_id == 0u)
        return A840_KMD_INVALID_ARG;

    eng_lock(engine);
    scope = find_scope_locked(engine, scope_id);
    if (scope != NULL)
        memset(scope, 0, sizeof(*scope));
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active &&
            (engine->queues[i].scope_id == scope_id ||
             (uint32_t)(engine->queues[i].queue_id >> 32u) == scope_id)) {
            memset(&engine->queues[i], 0, sizeof(engine->queues[i]));
        }
    }
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_wait_scoped(
    A840KmdWaitAnyEngine *engine,
    const A840AbiV3State *abi_state,
    uint32_t scope_id,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *reply)
{
    A840KmdStatus status = check_init(engine);
    const A840AbiV3State *eval_state;
    struct NativeAdrenoAbiWaitAnyReplyV3 candidate;
    A840AbiV3Status abi_st;
    A840KmdWaiterNode node;
    uint64_t start_ns;
    int cond_initialized = 0;
    int saw_timedout = 0;

    if (status != A840_KMD_OK)
        return status;
    if (request == NULL || reply == NULL)
        return A840_KMD_INVALID_ARG;

    eval_state = (abi_state != NULL) ? abi_state : &engine->default_abi_state;
    if (request->count == 0u || request->count > A840_KMD_WAIT_ANY_MAX_FENCES ||
        (request->flags & ~UINT32_C(1)) != 0u)
        return A840_KMD_INVALID_ARG;

    eng_lock(engine);

    /* Fast path: immediate check without initializing condvar or sleeping. */
    status = check_abort_conditions_locked(engine, scope_id, request);
    if (status != A840_KMD_OK) {
        eng_unlock(engine);
        return status;
    }

    memset(&candidate, 0, sizeof(candidate));
    abi_st = a840_abi_v3_eval_wait_any(eval_state, request, query_fence_locked,
        engine, 0u, eng_observed_ticks_locked(engine), &candidate);
    if (abi_st != A840_ABI_V3_OK) {
        eng_unlock(engine);
        return map_abi_status(abi_st);
    }

    if (reply_is_satisfied(request, &candidate) || request->timeout_ns == 0u) {
        *reply = candidate;
        engine->fast_path_hits++;
        eng_unlock(engine);
        return A840_KMD_OK;
    }

    /* Blocking path: zero busy-wait; sleep on condvar / OS wait-queue. */
    memset(&node, 0, sizeof(node));
    node.scope_id = scope_id;
    node.count = request->count;
    memcpy(node.queues, request->queues, (size_t)request->count * sizeof(uint64_t));

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!engine->use_custom_backend) {
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) != 0) {
            eng_unlock(engine);
            return A840_KMD_NO_MEMORY;
        }
#if !defined(__APPLE__)
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
            (void)pthread_condattr_destroy(&attr);
            eng_unlock(engine);
            return A840_KMD_IO_ERROR;
        }
#endif
        if (pthread_cond_init(&node.cond, &attr) != 0) {
            (void)pthread_condattr_destroy(&attr);
            eng_unlock(engine);
            return A840_KMD_NO_MEMORY;
        }
        (void)pthread_condattr_destroy(&attr);
        cond_initialized = 1;
    }
#endif

    node.prev = NULL;
    node.next = engine->waiters_head;
    if (engine->waiters_head != NULL)
        engine->waiters_head->prev = &node;
    engine->waiters_head = &node;
    engine->active_waiter_count++;

    start_ns = eng_now_ns(engine);
    status = A840_KMD_OK;

    for (;;) {
        uint64_t now_ns = eng_now_ns(engine);
        uint64_t elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0u;
        uint64_t eval_elapsed_ns = elapsed_ns;

        if (saw_timedout && request->timeout_ns != A840_KMD_WAIT_INFINITE &&
            eval_elapsed_ns < request->timeout_ns)
            eval_elapsed_ns = request->timeout_ns;

        status = check_abort_conditions_locked(engine, scope_id, request);
        if (status != A840_KMD_OK)
            break;

        memset(&candidate, 0, sizeof(candidate));
        abi_st = a840_abi_v3_eval_wait_any(eval_state, request,
            query_fence_locked, engine, eval_elapsed_ns,
            eng_observed_ticks_locked(engine), &candidate);
        if (abi_st != A840_ABI_V3_OK) {
            status = map_abi_status(abi_st);
            break;
        }

        if (reply_is_satisfied(request, &candidate)) {
            *reply = candidate;
            status = A840_KMD_OK;
            break;
        }

        if (request->timeout_ns != A840_KMD_WAIT_INFINITE && candidate.timed_out) {
            *reply = candidate;
            status = A840_KMD_OK;
            break;
        }

        engine->sleep_entries++;

        if (engine->use_custom_backend) {
            int infinite = (request->timeout_ns == A840_KMD_WAIT_INFINITE) ? 1 : 0;
            uint64_t deadline_ns = UINT64_MAX;
            int wait_rc;
            if (!infinite) {
                uint64_t rem = request->timeout_ns - elapsed_ns;
                deadline_ns = (rem > UINT64_MAX - now_ns) ? UINT64_MAX : (now_ns + rem);
            }
            wait_rc = engine->backend.wait(engine->backend.cookie, deadline_ns, infinite);
            if (!infinite && wait_rc != 0)
                saw_timedout = 1;
        } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
            if (request->timeout_ns == A840_KMD_WAIT_INFINITE) {
                (void)pthread_cond_wait(&node.cond, &engine->lock);
            } else {
                struct timespec ts;
                uint64_t rem = request->timeout_ns - elapsed_ns;
                uint64_t chunk = (rem > A840_KMD_MAX_SLEEP_CHUNK_NS) ?
                    A840_KMD_MAX_SLEEP_CHUNK_NS : rem;
                uint64_t add_sec = chunk / A840_KMD_NS_PER_SEC;
                uint64_t add_nsec = chunk % A840_KMD_NS_PER_SEC;
                int rc;

#if defined(__APPLE__)
                ts.tv_sec = (time_t)add_sec;
                ts.tv_nsec = (long)add_nsec;
                rc = pthread_cond_timedwait_relative_np(&node.cond, &engine->lock, &ts);
#else
                if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
                    status = A840_KMD_IO_ERROR;
                    break;
                }
                ts.tv_sec += (time_t)add_sec;
                ts.tv_nsec += (long)add_nsec;
                if (ts.tv_nsec >= (long)A840_KMD_NS_PER_SEC) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= (long)A840_KMD_NS_PER_SEC;
                }
                rc = pthread_cond_timedwait(&node.cond, &engine->lock, &ts);
#endif
                if (rc == ETIMEDOUT && rem <= A840_KMD_MAX_SLEEP_CHUNK_NS)
                    saw_timedout = 1;
            }
#endif
        }
    }

    /* Unlink stack node while still holding engine->lock so no signaler can
     * observe node.cond after unlock/destroy.
     */
    if (node.prev != NULL)
        node.prev->next = node.next;
    else
        engine->waiters_head = node.next;
    if (node.next != NULL)
        node.next->prev = node.prev;
    if (engine->active_waiter_count > 0u)
        engine->active_waiter_count--;

    eng_unlock(engine);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (cond_initialized)
        (void)pthread_cond_destroy(&node.cond);
#else
    (void)cond_initialized;
#endif

    return status;
}

A840KmdStatus a840_kmd_wait_any_wait(
    A840KmdWaitAnyEngine *engine,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *reply)
{
    return a840_kmd_wait_any_wait_scoped(engine, NULL, 0u, request, reply);
}

A840KmdStatus a840_kmd_wait_any_query_queue(
    A840KmdWaitAnyEngine *engine,
    uint64_t queue,
    uint64_t *out_completed_fence,
    int *out_poisoned)
{
    A840KmdStatus status = check_init(engine);
    A840KmdQueueTimeline *slot;

    if (status != A840_KMD_OK)
        return status;

    eng_lock(engine);
    slot = find_queue_locked(engine, queue);
    if (out_completed_fence != NULL)
        *out_completed_fence = (slot != NULL) ? slot->completed_fence : 0u;
    if (out_poisoned != NULL)
        *out_poisoned = (slot != NULL && slot->poisoned != 0u) ? 1 : 0;
    eng_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wait_any_get_stats(
    A840KmdWaitAnyEngine *engine,
    A840KmdWaitAnyStats *out_stats)
{
    A840KmdStatus status = check_init(engine);
    size_t i;
    uint32_t active_q = 0u;
    uint32_t poisoned_q = 0u;

    if (status != A840_KMD_OK)
        return status;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    eng_lock(engine);
    for (i = 0u; i < A840_KMD_WAIT_ANY_MAX_QUEUES; ++i) {
        if (engine->queues[i].active) {
            active_q++;
            if (engine->queues[i].poisoned)
                poisoned_q++;
        }
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->active_waiters = engine->active_waiter_count;
    out_stats->active_queues = active_q;
    out_stats->poisoned_queues = poisoned_q;
    out_stats->latest_ticks = eng_observed_ticks_locked(engine);
    out_stats->fast_path_hits = engine->fast_path_hits;
    out_stats->sleep_entries = engine->sleep_entries;
    out_stats->irq_signals = engine->irq_signals;
    out_stats->targeted_wakes = engine->targeted_wakes;
    eng_unlock(engine);
    return A840_KMD_OK;
}
