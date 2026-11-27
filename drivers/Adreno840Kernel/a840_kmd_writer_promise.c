/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_writer_promise.h"
#include <errno.h>
#include <string.h>
#include <time.h>

#define A840_KMD_PROMISE_MAGIC 1347571021u
#define A840_KMD_PROMISE_NS_PER_SEC UINT64_C(1000000000)
#define A840_KMD_PROMISE_MAX_SLEEP_CHUNK_NS (UINT64_C(31536000) * A840_KMD_PROMISE_NS_PER_SEC)

/* Bounded stack batch size for lock-free observer delivery (zero heap alloc). */
#define A840_KMD_PROMISE_EVENT_BATCH_MAX 32u

static A840KmdStatus check_init(const A840KmdWriterPromiseRegistry *reg)
{
    if (reg == NULL)
        return A840_KMD_INVALID_ARG;
    if (reg->initialized != A840_KMD_PROMISE_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static void reg_lock(A840KmdWriterPromiseRegistry *reg)
{
    if (reg->use_custom_sync) {
        reg->sync_ops.lock(reg->sync_ops.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_lock(&reg->lock);
#endif
    }
}

static void reg_unlock(A840KmdWriterPromiseRegistry *reg)
{
    if (reg->use_custom_sync) {
        reg->sync_ops.unlock(reg->sync_ops.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_unlock(&reg->lock);
#endif
    }
}

static uint64_t reg_now_ns(const A840KmdWriterPromiseRegistry *reg)
{
    if (reg->use_custom_sync)
        return reg->sync_ops.now_ns(reg->sync_ops.cookie);
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return 0u;
        return (uint64_t)ts.tv_sec * A840_KMD_PROMISE_NS_PER_SEC + (uint64_t)ts.tv_nsec;
    }
}

static int state_is_terminal(uint32_t state)
{
    return state == (uint32_t)A840_PROMISE_STATE_RETIRED_READY ||
           state == (uint32_t)A840_PROMISE_STATE_CANCELED ||
           state == (uint32_t)A840_PROMISE_STATE_FAILED;
}

static int state_is_display_ready(uint32_t state)
{
    return state == (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY ||
           state == (uint32_t)A840_PROMISE_STATE_RETIRED_READY;
}

static int state_is_in_flight(uint32_t state)
{
    return state == (uint32_t)A840_PROMISE_STATE_PENDING ||
           state == (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY ||
           state == (uint32_t)A840_PROMISE_STATE_DEFERRED_WAITING_FENCE;
}

static int state_is_wait_complete(uint32_t state)
{
    return state == (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY ||
           state == (uint32_t)A840_PROMISE_STATE_RETIRED_READY ||
           state == (uint32_t)A840_PROMISE_STATE_CANCELED ||
           state == (uint32_t)A840_PROMISE_STATE_FAILED;
}

static A840KmdPromiseClientSlot *find_client_locked(
    A840KmdWriterPromiseRegistry *reg, uint64_t client_id)
{
    size_t i;
    if (client_id == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_CLIENTS; ++i) {
        if (reg->clients[i].active && reg->clients[i].client_id == client_id)
            return &reg->clients[i];
    }
    return NULL;
}

static A840KmdPromiseSurfaceAuth *find_auth_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t owner_client,
    uint64_t surface_handle)
{
    size_t i;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_auth[i].active &&
            reg->surface_auth[i].owner_client == owner_client &&
            reg->surface_auth[i].surface_handle == surface_handle)
            return &reg->surface_auth[i];
    }
    return NULL;
}

static int surface_handle_owned_by_other_locked(
    const A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t surface_handle)
{
    size_t i;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_auth[i].active &&
            reg->surface_auth[i].surface_handle == surface_handle &&
            reg->surface_auth[i].owner_client != writer_client)
            return 1;
    }
    return 0;
}

static int surface_identity_owned_by_other_locked(
    const A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t surface_identity)
{
    size_t i;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_auth[i].active &&
            reg->surface_auth[i].surface_identity == surface_identity &&
            reg->surface_auth[i].owner_client != writer_client)
            return 1;
    }
    return 0;
}

static A840KmdPromiseQueueState *find_queue_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue)
{
    size_t i;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_QUEUES; ++i) {
        if (reg->queues[i].active &&
            reg->queues[i].writer_client == writer_client &&
            reg->queues[i].queue == queue)
            return &reg->queues[i];
    }
    return NULL;
}

static A840KmdPromiseQueueState *find_or_alloc_queue_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue)
{
    size_t i;
    A840KmdPromiseQueueState *free_slot = NULL;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_QUEUES; ++i) {
        if (reg->queues[i].active) {
            if (reg->queues[i].writer_client == writer_client &&
                reg->queues[i].queue == queue)
                return &reg->queues[i];
        } else if (free_slot == NULL) {
            free_slot = &reg->queues[i];
        }
    }
    if (free_slot == NULL)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->writer_client = writer_client;
    free_slot->queue = queue;
    return free_slot;
}

static A840KmdSurfaceHead *find_surface_head_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t writer_client)
{
    size_t i;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_heads[i].active &&
            reg->surface_heads[i].surface_identity == surface_identity &&
            reg->surface_heads[i].writer_client == writer_client)
            return &reg->surface_heads[i];
    }
    return NULL;
}

static A840KmdSurfaceHead *find_latest_surface_head_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t writer_client_filter)
{
    size_t i;
    A840KmdSurfaceHead *best = NULL;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        A840KmdSurfaceHead *h = &reg->surface_heads[i];
        if (!h->active || h->surface_identity != surface_identity)
            continue;
        if (writer_client_filter != 0u && h->writer_client != writer_client_filter)
            continue;
        if (best == NULL || h->promise_token > best->promise_token ||
            (h->promise_token == best->promise_token && h->generation > best->generation))
            best = h;
    }
    return best;
}

static A840KmdSurfaceHead *find_or_alloc_surface_head_locked(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t writer_client)
{
    size_t i;
    A840KmdSurfaceHead *free_slot = NULL;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_heads[i].active) {
            if (reg->surface_heads[i].surface_identity == surface_identity &&
                reg->surface_heads[i].writer_client == writer_client)
                return &reg->surface_heads[i];
        } else if (free_slot == NULL) {
            free_slot = &reg->surface_heads[i];
        }
    }
    if (free_slot == NULL)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->surface_identity = surface_identity;
    free_slot->writer_client = writer_client;
    return free_slot;
}

static A840KmdPromiseSlot *find_slot_by_token_locked(
    A840KmdWriterPromiseRegistry *reg, uint64_t promise_token)
{
    size_t i;
    if (promise_token == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        if (reg->slots[i].state != (uint32_t)A840_PROMISE_STATE_FREE &&
            reg->slots[i].promise_token == promise_token)
            return &reg->slots[i];
    }
    return NULL;
}

static void fill_snapshot_from_head(
    const A840KmdSurfaceHead *head,
    A840KmdSurfacePromiseSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = 1u;
    out->display_ready = state_is_display_ready(head->state) ? 1u : 0u;
    out->client_role = head->client_role;
    out->policy = head->policy;
    out->state = head->state;
    out->cancel_reason = head->cancel_reason;
    out->fence_retired = head->fence_retired;
    out->surface_identity = head->surface_identity;
    out->writer_client = head->writer_client;
    out->generation = head->generation;
    out->promise_token = head->promise_token;
    out->queue = head->queue;
    out->fence = head->fence;
}

static void fill_event_from_slot(
    const A840KmdPromiseSlot *slot,
    uint32_t surface_index,
    A840KmdPromiseEvent *out)
{
    memset(out, 0, sizeof(*out));
    out->promise_token = slot->promise_token;
    out->writer_client = slot->writer_client;
    out->surface_identity = slot->surface_identities[surface_index];
    out->surface_handle = slot->surface_handles[surface_index];
    out->generation = slot->generations[surface_index];
    out->queue = slot->queue;
    out->fence = slot->fence;
    out->client_role = slot->client_role;
    out->policy = slot->policy;
    out->state = slot->state;
    out->cancel_reason = slot->cancel_reason;
}

static void wake_waiters_for_surface_locked(
    A840KmdWriterPromiseRegistry *reg, uint64_t surface_identity)
{
    if (reg->use_custom_sync) {
        if (reg->active_waiter_count > 0u)
            reg->sync_ops.wake_all(reg->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    {
        A840KmdPromiseWaiter *w = reg->waiters_head;
        while (w != NULL) {
            if (surface_identity == 0u || w->surface_identity == surface_identity)
                (void)pthread_cond_signal(&w->cond);
            w = w->next;
        }
    }
#else
    (void)surface_identity;
#endif
}

static void wake_all_waiters_locked(A840KmdWriterPromiseRegistry *reg)
{
    wake_waiters_for_surface_locked(reg, 0u);
}

static void sync_surface_heads_from_slot_locked(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdPromiseSlot *slot,
    int wake_waiters)
{
    uint32_t i;
    for (i = 0u; i < slot->surface_count; ++i) {
        A840KmdSurfaceHead *head = find_surface_head_locked(
            reg, slot->surface_identities[i], slot->writer_client);
        if (head != NULL && head->promise_token == slot->promise_token) {
            head->client_role = slot->client_role;
            head->policy = slot->policy;
            head->state = slot->state;
            head->cancel_reason = slot->cancel_reason;
            head->fence_retired = slot->fence_retired;
            head->queue = slot->queue;
            head->fence = slot->fence;
            if (wake_waiters)
                wake_waiters_for_surface_locked(reg, head->surface_identity);
        }
    }
}

static void deliver_events_unlocked(
    A840KmdPromiseObserver observer,
    void *cookie,
    const A840KmdPromiseEvent *events,
    size_t count)
{
    size_t i;
    if (observer == NULL || count == 0u)
        return;
    for (i = 0u; i < count; ++i)
        observer(cookie, &events[i]);
}

static void recompute_pending_locked(A840KmdWriterPromiseRegistry *reg)
{
    size_t i;
    uint32_t count = 0u;
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        if (state_is_in_flight(reg->slots[i].state))
            count++;
    }
    reg->stats.pending_promises = count;
}

static A840KmdStatus resolve_surface_handles(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdWriterPromiseCall *call,
    uint64_t out_identities[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ])
{
    uint32_t i, j;
    A840KmdSurfaceIdentityResolver resolver = NULL;
    void *resolver_cookie = NULL;

    /* Check basic handle uniqueness and non-zero invariant before touching resolver. */
    if (call->abi.surface_count == 0u ||
        call->abi.surface_count > A840_KMD_PROMISE_MAX_SURFACES_PER_REQ)
        return A840_KMD_INVALID_ARG;

    for (i = 0u; i < call->abi.surface_count; ++i) {
        if (call->abi.surface_ids[i] == 0u)
            return A840_KMD_INVALID_ARG;
        for (j = 0u; j < i; ++j) {
            if (call->abi.surface_ids[i] == call->abi.surface_ids[j])
                return A840_KMD_STATE_CONFLICT;
        }
    }
    /* Unused surface handle tail entries must be 0 (fail-closed against garbage). */
    for (i = call->abi.surface_count; i < A840_KMD_PROMISE_MAX_SURFACES_PER_REQ; ++i) {
        if (call->abi.surface_ids[i] != 0u)
            return A840_KMD_INVALID_ARG;
        if (call->has_surface_identities && call->surface_identities[i] != 0u)
            return A840_KMD_INVALID_ARG;
    }

    reg_lock(reg);
    resolver = reg->surface_resolver;
    resolver_cookie = reg->surface_resolver_cookie;
    reg_unlock(reg);

    for (i = 0u; i < call->abi.surface_count; ++i) {
        uint64_t handle = call->abi.surface_ids[i];
        uint64_t resolved_id = 0u;
        A840KmdStatus st = A840_KMD_NOT_FOUND;

        /* Check built-in trusted auth table first. */
        reg_lock(reg);
        {
            A840KmdPromiseSurfaceAuth *auth = find_auth_locked(
                reg, call->writer_client, handle);
            if (auth != NULL) {
                resolved_id = auth->surface_identity;
                st = A840_KMD_OK;
            } else if (surface_handle_owned_by_other_locked(reg, call->writer_client, handle)) {
                st = A840_KMD_PERMISSION_DENIED;
            }
        }
        reg_unlock(reg);

        if (st == A840_KMD_NOT_FOUND && resolver != NULL) {
            /* Call external resolver OUTSIDE reg->lock to avoid lock inversion. */
            st = resolver(resolver_cookie, call->writer_client, handle, &resolved_id);
            if (st == A840_KMD_OK && resolved_id == 0u)
                st = A840_KMD_INVALID_ARG;
        }

        if (st != A840_KMD_OK)
            return st;

        /* If caller also supplied explicit surface_identities, enforce exact match
         * and reject any cross-client forgery attempt.
         */
        if (call->has_surface_identities) {
            if (call->surface_identities[i] == 0u ||
                call->surface_identities[i] != resolved_id)
                return A840_KMD_PERMISSION_DENIED;
        }

        reg_lock(reg);
        if (surface_identity_owned_by_other_locked(reg, call->writer_client, resolved_id)) {
            reg_unlock(reg);
            return A840_KMD_PERMISSION_DENIED;
        }
        reg_unlock(reg);

        /* Reject duplicate resolved identities in the same call. */
        for (j = 0u; j < i; ++j) {
            if (out_identities[j] == resolved_id)
                return A840_KMD_STATE_CONFLICT;
        }
        out_identities[i] = resolved_id;
    }

    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_writer_promise_init(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdPromiseConfig *config)
{
    if (reg == NULL)
        return A840_KMD_INVALID_ARG;

    if (config != NULL && config->sync_ops != NULL) {
        const A840KmdPromiseSyncOps *ops = config->sync_ops;
        if (ops->lock == NULL || ops->unlock == NULL ||
            ops->wait == NULL || ops->wake_all == NULL ||
            ops->now_ns == NULL)
            return A840_KMD_INVALID_ARG;
    }
#if defined(A840_KMD_CUSTOM_SYNC)
    else {
        return A840_KMD_INVALID_ARG;
    }
#endif

    memset(reg, 0, sizeof(*reg));

    if (config != NULL) {
        reg->wait_any = config->wait_any;
        reg->fault_capture = config->fault_capture;
        reg->surface_resolver = config->surface_resolver;
        reg->surface_resolver_cookie = config->surface_resolver_cookie;
        reg->observer = config->observer;
        reg->observer_cookie = config->observer_cookie;
        if (config->sync_ops != NULL) {
            reg->sync_ops = *config->sync_ops;
            reg->use_custom_sync = 1u;
        }
    }

    if (!reg->use_custom_sync) {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&reg->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#endif
    }

    reg->initialized = A840_KMD_PROMISE_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_writer_promise_destroy(A840KmdWriterPromiseRegistry *reg)
{
    size_t i;
    if (check_init(reg) != A840_KMD_OK)
        return;

    reg_lock(reg);
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        if (state_is_in_flight(reg->slots[i].state)) {
            reg->slots[i].state = (uint32_t)A840_PROMISE_STATE_CANCELED;
            reg->slots[i].cancel_reason = (uint32_t)A840_PROMISE_CANCEL_CLIENT_CLOSE;
            sync_surface_heads_from_slot_locked(reg, &reg->slots[i], 0);
        }
    }
    reg->observer = NULL;
    reg->observer_cookie = NULL;
    reg->initialized = 0u;
    wake_all_waiters_locked(reg);
    reg_unlock(reg);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!reg->use_custom_sync)
        (void)pthread_mutex_destroy(&reg->lock);
#endif
}

A840KmdStatus a840_kmd_writer_promise_set_observer(
    A840KmdWriterPromiseRegistry *reg,
    A840KmdPromiseObserver observer,
    void *cookie)
{
    A840KmdStatus st = check_init(reg);
    if (st != A840_KMD_OK)
        return st;
    reg_lock(reg);
    reg->observer = observer;
    reg->observer_cookie = cookie;
    reg_unlock(reg);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_writer_promise_register_client(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t client_id,
    A840ClientRole role)
{
    A840KmdStatus st = check_init(reg);
    A840KmdPromiseClientSlot *slot = NULL;
    A840KmdPromiseClientSlot *free_slot = NULL;
    size_t i;

    if (st != A840_KMD_OK)
        return st;
    if (client_id == 0u ||
        (role != A840_CLIENT_ROLE_APP && role != A840_CLIENT_ROLE_RENDER_SERVER))
        return A840_KMD_INVALID_ARG;

    reg_lock(reg);
    for (i = 0u; i < A840_KMD_PROMISE_MAX_CLIENTS; ++i) {
        if (reg->clients[i].active) {
            if (reg->clients[i].client_id == client_id) {
                slot = &reg->clients[i];
                break;
            }
        } else if (free_slot == NULL) {
            free_slot = &reg->clients[i];
        }
    }

    if (slot != NULL) {
        /* Reject role escalation / conflict on an already-active client id. */
        if (slot->role != (uint32_t)role) {
            reg_unlock(reg);
            return A840_KMD_STATE_CONFLICT;
        }
        reg_unlock(reg);
        return A840_KMD_OK;
    }

    if (free_slot == NULL) {
        reg_unlock(reg);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->client_id = client_id;
    free_slot->role = (uint32_t)role;
    reg->stats.active_clients++;
    reg_unlock(reg);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_writer_promise_bind_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t owner_client,
    uint64_t surface_handle,
    uint64_t surface_identity)
{
    A840KmdStatus st = check_init(reg);
    A840KmdPromiseSurfaceAuth *existing = NULL;
    A840KmdPromiseSurfaceAuth *free_slot = NULL;
    size_t i;

    if (st != A840_KMD_OK)
        return st;
    if (owner_client == 0u || surface_handle == 0u || surface_identity == 0u)
        return A840_KMD_INVALID_ARG;

    reg_lock(reg);
    if (find_client_locked(reg, owner_client) == NULL) {
        reg_unlock(reg);
        return A840_KMD_NOT_FOUND;
    }
    if (surface_identity_owned_by_other_locked(reg, owner_client, surface_identity)) {
        reg_unlock(reg);
        return A840_KMD_PERMISSION_DENIED;
    }

    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_auth[i].active) {
            if (reg->surface_auth[i].owner_client == owner_client &&
                reg->surface_auth[i].surface_handle == surface_handle) {
                existing = &reg->surface_auth[i];
                break;
            }
        } else if (free_slot == NULL) {
            free_slot = &reg->surface_auth[i];
        }
    }

    if (existing != NULL) {
        if (existing->surface_identity != surface_identity) {
            reg_unlock(reg);
            return A840_KMD_STATE_CONFLICT;
        }
        reg_unlock(reg);
        return A840_KMD_OK;
    }

    if (free_slot == NULL) {
        reg_unlock(reg);
        return A840_KMD_CAPACITY;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1u;
    free_slot->owner_client = owner_client;
    free_slot->surface_handle = surface_handle;
    free_slot->surface_identity = surface_identity;
    reg_unlock(reg);
    return A840_KMD_OK;
}

void a840_kmd_writer_promise_unbind_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t owner_client,
    uint64_t surface_handle)
{
    A840KmdPromiseSurfaceAuth *auth;
    if (check_init(reg) != A840_KMD_OK || owner_client == 0u || surface_handle == 0u)
        return;
    reg_lock(reg);
    auth = find_auth_locked(reg, owner_client, surface_handle);
    if (auth != NULL)
        memset(auth, 0, sizeof(*auth));
    reg_unlock(reg);
}

static A840KmdStatus check_channel_healthy(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue)
{
    A840KmdFaultCapture *fc;
    A840KmdWaitAnyEngine *wa;

    reg_lock(reg);
    {
        A840KmdPromiseQueueState *qs = find_queue_locked(reg, writer_client, queue);
        if (qs != NULL && qs->poisoned) {
            reg_unlock(reg);
            return A840_KMD_POISONED;
        }
        fc = reg->fault_capture;
        wa = reg->wait_any;
    }
    reg_unlock(reg);

    if (fc != NULL && a840_kmd_fault_is_channel_poisoned(fc, writer_client, queue))
        return A840_KMD_POISONED;

    if (wa != NULL) {
        int wa_poisoned = 0;
        if (a840_kmd_wait_any_query_queue(wa, queue, NULL, &wa_poisoned) == A840_KMD_OK &&
            wa_poisoned)
            return A840_KMD_POISONED;
    }

    return A840_KMD_OK;
}

static uint64_t query_queue_completed_fence(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue)
{
    uint64_t completed = 0u;
    A840KmdWaitAnyEngine *wa = NULL;

    reg_lock(reg);
    {
        A840KmdPromiseQueueState *qs = find_queue_locked(reg, writer_client, queue);
        if (qs != NULL)
            completed = qs->completed_fence;
        wa = reg->wait_any;
    }
    reg_unlock(reg);

    if (wa != NULL) {
        uint64_t wa_completed = 0u;
        if (a840_kmd_wait_any_query_queue(wa, queue, &wa_completed, NULL) == A840_KMD_OK &&
            wa_completed > completed)
            completed = wa_completed;
    }
    return completed;
}

A840KmdStatus a840_kmd_writer_promise_dispatch(
    A840KmdWriterPromiseRegistry *reg,
    const A840KmdWriterPromiseCall *call,
    struct NativeAdrenoAbiWriterPromiseReplyV3 *reply,
    A840PromiseHandoffPolicy *out_policy)
{
    A840KmdStatus st = check_init(reg);
    uint64_t resolved_ids[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
    A840KmdPromiseEvent events[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ * 2u];
    size_t event_count = 0u;
    A840KmdPromiseObserver obs = NULL;
    void *obs_cookie = NULL;
    struct NativeAdrenoAbiWriterPromiseReplyV3 out_reply;
    A840PromiseHandoffPolicy chosen_policy = A840_PROMISE_POLICY_NONE;
    uint64_t observed_completed = 0u;
    uint32_t i;

    if (st != A840_KMD_OK)
        return st;
    if (call == NULL || reply == NULL || call->writer_client == 0u)
        return A840_KMD_INVALID_ARG;

    /* Strict fail-closed validation of reserved fields and flags. */
    if (call->reserved != 0u || call->reserved2 != 0u ||
        call->has_surface_identities > 1u ||
        (call->flags & ~(uint32_t)A840_PROMISE_FLAG_KNOWN_MASK) != 0u) {
        reg_lock(reg);
        reg->stats.total_rejected_calls++;
        reg_unlock(reg);
        return A840_KMD_INVALID_ARG;
    }

    if (call->abi.op != NativeAdrenoPromiseMake &&
        call->abi.op != NativeAdrenoPromiseFulfill &&
        call->abi.op != NativeAdrenoPromiseCancel) {
        reg_lock(reg);
        reg->stats.total_rejected_calls++;
        reg_unlock(reg);
        return A840_KMD_INVALID_ARG;
    }

    memset(resolved_ids, 0, sizeof(resolved_ids));
    st = resolve_surface_handles(reg, call, resolved_ids);
    if (st != A840_KMD_OK) {
        reg_lock(reg);
        reg->stats.total_rejected_calls++;
        reg_unlock(reg);
        return st;
    }

    if (call->abi.op == NativeAdrenoPromiseMake ||
        call->abi.op == NativeAdrenoPromiseFulfill) {
        st = check_channel_healthy(reg, call->writer_client, call->abi.queue);
        if (st != A840_KMD_OK) {
            reg_lock(reg);
            reg->stats.total_rejected_calls++;
            reg_unlock(reg);
            return st;
        }
        observed_completed = query_queue_completed_fence(
            reg, call->writer_client, call->abi.queue);
    }

    reg_lock(reg);

    {
        A840KmdPromiseClientSlot *client = find_client_locked(reg, call->writer_client);
        if (client == NULL) {
            reg->stats.total_rejected_calls++;
            reg_unlock(reg);
            return A840_KMD_NOT_FOUND;
        }

        if (call->abi.op == NativeAdrenoPromiseMake) {
            A840KmdPromiseSlot *slot = NULL;
            A840KmdPromiseSlot *terminal_recycle = NULL;
            A840KmdSurfaceHead *heads[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
            uint64_t next_gens[A840_KMD_PROMISE_MAX_SURFACES_PER_REQ];
            size_t s;
            size_t new_heads_needed = 0u;
            size_t free_heads_available = 0u;
            A840KmdPromiseQueueState *qs;

            /* ABI Make invariants: promise_token == 0, fence == 0. */
            if (call->abi.promise_token != 0u || call->abi.fence != 0u) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_INVALID_ARG;
            }
            if (reg->last_promise_token == UINT64_MAX) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_CAPACITY;
            }

            /* Privilege split policy check on Make as well (early strict rejection). */
            if (client->role == (uint32_t)A840_CLIENT_ROLE_APP &&
                (call->flags & (uint32_t)A840_PROMISE_FLAG_EARLY_HANDOFF) != 0u &&
                (call->flags & (uint32_t)A840_PROMISE_FLAG_STRICT_REJECT_APP_EARLY) != 0u) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_PERMISSION_DENIED;
            }

            /* Re-check queue poison under lock to eliminate TOCTOU. */
            qs = find_queue_locked(reg, call->writer_client, call->abi.queue);
            if (qs != NULL && qs->poisoned) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_POISONED;
            }
            if (qs == NULL) {
                /* Verify queue table capacity before mutating anything. */
                int has_free_q = 0;
                for (s = 0u; s < A840_KMD_PROMISE_MAX_QUEUES; ++s) {
                    if (!reg->queues[s].active) {
                        has_free_q = 1;
                        break;
                    }
                }
                if (!has_free_q) {
                    reg->stats.total_rejected_calls++;
                    reg_unlock(reg);
                    return A840_KMD_CAPACITY;
                }
            }

            /* Validate generation monotonicity and head capacity BEFORE mutating state. */
            for (s = 0u; s < A840_KMD_PROMISE_MAX_SURFACES; ++s) {
                if (!reg->surface_heads[s].active)
                    free_heads_available++;
            }

            for (i = 0u; i < call->abi.surface_count; ++i) {
                A840KmdSurfaceHead *h = find_surface_head_locked(
                    reg, resolved_ids[i], call->writer_client);
                heads[i] = h;
                if (h == NULL) {
                    new_heads_needed++;
                    next_gens[i] = (call->generation != 0u) ? call->generation : 1u;
                } else {
                    if (call->generation != 0u) {
                        if (call->generation <= h->generation) {
                            reg->stats.total_rejected_calls++;
                            reg_unlock(reg);
                            return A840_KMD_STATE_CONFLICT;
                        }
                        next_gens[i] = call->generation;
                    } else {
                        if (h->generation == UINT64_MAX) {
                            reg->stats.total_rejected_calls++;
                            reg_unlock(reg);
                            return A840_KMD_CAPACITY;
                        }
                        next_gens[i] = h->generation + 1u;
                    }
                }
            }

            if (new_heads_needed > free_heads_available) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_CAPACITY;
            }

            /* Find free or recyclable terminal slot. */
            for (s = 0u; s < A840_KMD_PROMISE_MAX_SLOTS; ++s) {
                if (reg->slots[s].state == (uint32_t)A840_PROMISE_STATE_FREE) {
                    slot = &reg->slots[s];
                    break;
                }
                if (terminal_recycle == NULL && state_is_terminal(reg->slots[s].state))
                    terminal_recycle = &reg->slots[s];
            }
            if (slot == NULL)
                slot = terminal_recycle;
            if (slot == NULL) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_CAPACITY;
            }

            /* All preconditions validated: commit atomically. */
            (void)find_or_alloc_queue_locked(reg, call->writer_client, call->abi.queue);

            if (client->role == (uint32_t)A840_CLIENT_ROLE_RENDER_SERVER) {
                chosen_policy = A840_PROMISE_POLICY_EARLY_SCHEDULED;
            } else {
                chosen_policy = A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED;
            }

            reg->last_promise_token++;
            memset(slot, 0, sizeof(*slot));
            slot->state = (uint32_t)A840_PROMISE_STATE_PENDING;
            slot->client_role = client->role;
            slot->policy = (uint32_t)chosen_policy;
            slot->cancel_reason = (uint32_t)A840_PROMISE_CANCEL_NONE;
            slot->surface_count = call->abi.surface_count;
            slot->promise_token = reg->last_promise_token;
            slot->writer_client = call->writer_client;
            slot->queue = call->abi.queue;
            slot->fence = 0u;

            for (i = 0u; i < call->abi.surface_count; ++i) {
                A840KmdSurfaceHead *h = heads[i];
                if (h == NULL) {
                    h = find_or_alloc_surface_head_locked(
                        reg, resolved_ids[i], call->writer_client);
                } else if (state_is_in_flight(h->state) && h->promise_token != 0u) {
                    /* Supersede older in-flight promise for the same surface when
                     * it only covers this surface or mark old frame superseded on head.
                     */
                    A840KmdPromiseSlot *old_slot = find_slot_by_token_locked(
                        reg, h->promise_token);
                    if (old_slot != NULL && state_is_in_flight(old_slot->state)) {
                        uint32_t k;
                        int still_head_elsewhere = 0;
                        for (k = 0u; k < old_slot->surface_count; ++k) {
                            A840KmdSurfaceHead *oh = find_surface_head_locked(
                                reg, old_slot->surface_identities[k], old_slot->writer_client);
                            if (oh != NULL && oh->promise_token == old_slot->promise_token &&
                                old_slot->surface_identities[k] != resolved_ids[i]) {
                                still_head_elsewhere = 1;
                                break;
                            }
                        }
                        if (!still_head_elsewhere) {
                            old_slot->state = (uint32_t)A840_PROMISE_STATE_CANCELED;
                            old_slot->cancel_reason = (uint32_t)A840_PROMISE_CANCEL_SUPERSEDED;
                            reg->stats.total_superseded++;
                            if (event_count < (sizeof(events) / sizeof(events[0])))
                                fill_event_from_slot(old_slot, 0u, &events[event_count++]);
                        } else {
                            reg->stats.total_superseded++;
                        }
                    }
                }

                slot->surface_handles[i] = call->abi.surface_ids[i];
                slot->surface_identities[i] = resolved_ids[i];
                slot->generations[i] = next_gens[i];

                h->client_role = slot->client_role;
                h->policy = slot->policy;
                h->state = slot->state;
                h->cancel_reason = slot->cancel_reason;
                h->fence_retired = 0u;
                h->generation = next_gens[i];
                h->promise_token = slot->promise_token;
                h->queue = slot->queue;
                h->fence = 0u;

                if (event_count < (sizeof(events) / sizeof(events[0])))
                    fill_event_from_slot(slot, i, &events[event_count++]);
            }

            reg->stats.total_made++;
            recompute_pending_locked(reg);

            out_reply.promise_token = slot->promise_token;
            out_reply.accepted = 1u;
            out_reply.reserved = 0u;
        } else if (call->abi.op == NativeAdrenoPromiseFulfill) {
            A840KmdPromiseSlot *slot;
            A840KmdPromiseQueueState *qs;

            /* ABI Fulfill invariants: nonzero token, nonzero fence. */
            if (call->abi.promise_token == 0u || call->abi.fence == 0u) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_INVALID_ARG;
            }
            slot = find_slot_by_token_locked(reg, call->abi.promise_token);
            if (slot == NULL) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_NOT_FOUND;
            }
            if (slot->writer_client != call->writer_client) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_PERMISSION_DENIED;
            }
            if (slot->state != (uint32_t)A840_PROMISE_STATE_PENDING ||
                slot->surface_count != call->abi.surface_count ||
                slot->queue != call->abi.queue) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_STATE_CONFLICT;
            }
            for (i = 0u; i < call->abi.surface_count; ++i) {
                if (slot->surface_handles[i] != call->abi.surface_ids[i] ||
                    slot->surface_identities[i] != resolved_ids[i]) {
                    reg->stats.total_rejected_calls++;
                    reg_unlock(reg);
                    return A840_KMD_STATE_CONFLICT;
                }
            }
            if (call->generation != 0u) {
                for (i = 0u; i < slot->surface_count; ++i) {
                    if (slot->generations[i] != call->generation) {
                        reg->stats.total_rejected_calls++;
                        reg_unlock(reg);
                        return A840_KMD_STATE_CONFLICT;
                    }
                }
            }

            /* Re-check queue poison under lock. */
            qs = find_queue_locked(reg, call->writer_client, call->abi.queue);
            if (qs != NULL && qs->poisoned) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_POISONED;
            }
            if (qs != NULL && qs->completed_fence > observed_completed)
                observed_completed = qs->completed_fence;

            /* Core privilege split enforcement:
             * - RenderServer: allowed early scheduled handoff on Fulfill.
             * - Regular App: NEVER early-handed-off before GPU fence retires.
             *   If caller passes EARLY_HANDOFF + STRICT_REJECT_APP_EARLY -> deny.
             *   Otherwise force DEFER_UNTIL_RETIRED even if EARLY_HANDOFF was set.
             */
            if (client->role == (uint32_t)A840_CLIENT_ROLE_APP &&
                (call->flags & (uint32_t)A840_PROMISE_FLAG_EARLY_HANDOFF) != 0u &&
                (call->flags & (uint32_t)A840_PROMISE_FLAG_STRICT_REJECT_APP_EARLY) != 0u) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_PERMISSION_DENIED;
            }

            slot->fence = call->abi.fence;
            if (client->role == (uint32_t)A840_CLIENT_ROLE_RENDER_SERVER) {
                chosen_policy = A840_PROMISE_POLICY_EARLY_SCHEDULED;
                slot->policy = (uint32_t)chosen_policy;
                if (observed_completed >= slot->fence) {
                    slot->state = (uint32_t)A840_PROMISE_STATE_RETIRED_READY;
                    slot->fence_retired = 1u;
                    reg->stats.total_retired_ready++;
                } else {
                    slot->state = (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY;
                    slot->fence_retired = 0u;
                    reg->stats.total_early_scheduled++;
                }
            } else {
                chosen_policy = A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED;
                slot->policy = (uint32_t)chosen_policy;
                if (observed_completed >= slot->fence) {
                    slot->state = (uint32_t)A840_PROMISE_STATE_RETIRED_READY;
                    slot->fence_retired = 1u;
                    reg->stats.total_retired_ready++;
                } else {
                    slot->state = (uint32_t)A840_PROMISE_STATE_DEFERRED_WAITING_FENCE;
                    slot->fence_retired = 0u;
                    reg->stats.total_deferred_app++;
                }
            }

            sync_surface_heads_from_slot_locked(reg, slot, 1);
            for (i = 0u; i < slot->surface_count; ++i) {
                if (event_count < (sizeof(events) / sizeof(events[0])))
                    fill_event_from_slot(slot, i, &events[event_count++]);
            }
            recompute_pending_locked(reg);

            out_reply.promise_token = slot->promise_token;
            out_reply.accepted = 1u;
            out_reply.reserved = 0u;
        } else {
            /* NativeAdrenoPromiseCancel */
            A840KmdPromiseSlot *slot;
            if (call->abi.promise_token == 0u || call->abi.fence != 0u) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_INVALID_ARG;
            }
            slot = find_slot_by_token_locked(reg, call->abi.promise_token);
            if (slot == NULL) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_NOT_FOUND;
            }
            if (slot->writer_client != call->writer_client) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_PERMISSION_DENIED;
            }
            if (!state_is_in_flight(slot->state) ||
                slot->surface_count != call->abi.surface_count) {
                reg->stats.total_rejected_calls++;
                reg_unlock(reg);
                return A840_KMD_STATE_CONFLICT;
            }
            for (i = 0u; i < call->abi.surface_count; ++i) {
                if (slot->surface_handles[i] != call->abi.surface_ids[i] ||
                    slot->surface_identities[i] != resolved_ids[i]) {
                    reg->stats.total_rejected_calls++;
                    reg_unlock(reg);
                    return A840_KMD_STATE_CONFLICT;
                }
            }

            chosen_policy = (A840PromiseHandoffPolicy)slot->policy;
            slot->state = (uint32_t)A840_PROMISE_STATE_CANCELED;
            slot->cancel_reason = (uint32_t)A840_PROMISE_CANCEL_EXPLICIT;
            slot->fence = 0u;
            sync_surface_heads_from_slot_locked(reg, slot, 1);
            for (i = 0u; i < slot->surface_count; ++i) {
                if (event_count < (sizeof(events) / sizeof(events[0])))
                    fill_event_from_slot(slot, i, &events[event_count++]);
            }
            reg->stats.total_canceled_explicit++;
            recompute_pending_locked(reg);

            out_reply.promise_token = slot->promise_token;
            out_reply.accepted = 1u;
            out_reply.reserved = 0u;
        }
    }

    obs = reg->observer;
    obs_cookie = reg->observer_cookie;
    reg_unlock(reg);

    *reply = out_reply;
    if (out_policy != NULL)
        *out_policy = chosen_policy;

    deliver_events_unlocked(obs, obs_cookie, events, event_count);
    return A840_KMD_OK;
}

size_t a840_kmd_writer_promise_signal_fence(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue,
    uint64_t completed_fence,
    uint64_t ticks)
{
    size_t transitioned_slots = 0u;
    A840KmdWaitAnyEngine *wa = NULL;
    A840KmdPromiseObserver obs = NULL;
    void *obs_cookie = NULL;
    uint32_t batch_id;
    int done = 0;

    if (check_init(reg) != A840_KMD_OK || writer_client == 0u)
        return 0u;

    /* First update queue timeline and transition matching slots under lock. */
    reg_lock(reg);
    wa = reg->wait_any;
    {
        A840KmdPromiseQueueState *qs = find_or_alloc_queue_locked(reg, writer_client, queue);
        if (qs != NULL) {
            if (qs->poisoned) {
                reg_unlock(reg);
                return 0u;
            }
            if (completed_fence > qs->completed_fence)
                qs->completed_fence = completed_fence;
        }
    }

    reg->next_notify_batch++;
    if (reg->next_notify_batch == 0u)
        reg->next_notify_batch = 1u;
    batch_id = reg->next_notify_batch;

    {
        size_t i;
        for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
            A840KmdPromiseSlot *slot = &reg->slots[i];
            if (slot->writer_client != writer_client || slot->queue != queue)
                continue;
            if ((slot->state == (uint32_t)A840_PROMISE_STATE_DEFERRED_WAITING_FENCE ||
                 slot->state == (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY) &&
                slot->fence != 0u && slot->fence <= completed_fence) {
                slot->state = (uint32_t)A840_PROMISE_STATE_RETIRED_READY;
                slot->fence_retired = 1u;
                slot->notify_batch = batch_id;
                slot->notify_index = 0u;
                sync_surface_heads_from_slot_locked(reg, slot, 1);
                reg->stats.total_retired_ready++;
                transitioned_slots++;
            }
        }
        recompute_pending_locked(reg);
    }
    reg_unlock(reg);

    /* Advance WaitAny outside reg->lock to prevent lock-order inversion. */
    if (wa != NULL)
        (void)a840_kmd_wait_any_signal_fence(wa, queue, completed_fence, ticks);

    /* Deliver observer events in bounded stack chunks outside reg->lock. */
    while (!done) {
        A840KmdPromiseEvent batch[A840_KMD_PROMISE_EVENT_BATCH_MAX];
        size_t count = 0u;
        size_t i;

        reg_lock(reg);
        obs = reg->observer;
        obs_cookie = reg->observer_cookie;
        done = 1;
        for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
            A840KmdPromiseSlot *slot = &reg->slots[i];
            if (slot->notify_batch != batch_id)
                continue;
            while (slot->notify_index < slot->surface_count) {
                if (count >= A840_KMD_PROMISE_EVENT_BATCH_MAX) {
                    done = 0;
                    break;
                }
                fill_event_from_slot(slot, slot->notify_index, &batch[count++]);
                slot->notify_index++;
            }
            if (slot->notify_index >= slot->surface_count)
                slot->notify_batch = 0u;
            if (!done)
                break;
        }
        reg_unlock(reg);

        deliver_events_unlocked(obs, obs_cookie, batch, count);
    }

    return transitioned_slots;
}

size_t a840_kmd_writer_promise_fail_channel(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client,
    uint64_t queue,
    A840PromiseCancelReason reason)
{
    size_t failed_slots = 0u;
    A840KmdWaitAnyEngine *wa = NULL;
    uint32_t batch_id;
    uint32_t effective_reason;
    int done = 0;

    if (check_init(reg) != A840_KMD_OK || writer_client == 0u)
        return 0u;

    effective_reason = (reason == A840_PROMISE_CANCEL_NONE) ?
        (uint32_t)A840_PROMISE_CANCEL_GPU_FAULT : (uint32_t)reason;

    reg_lock(reg);
    wa = reg->wait_any;
    {
        A840KmdPromiseQueueState *qs = find_or_alloc_queue_locked(reg, writer_client, queue);
        if (qs != NULL)
            qs->poisoned = 1u;
    }

    reg->next_notify_batch++;
    if (reg->next_notify_batch == 0u)
        reg->next_notify_batch = 1u;
    batch_id = reg->next_notify_batch;

    {
        size_t i;
        for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
            A840KmdPromiseSlot *slot = &reg->slots[i];
            if (slot->writer_client != writer_client || slot->queue != queue)
                continue;
            if (state_is_in_flight(slot->state)) {
                slot->state = (uint32_t)A840_PROMISE_STATE_FAILED;
                slot->cancel_reason = effective_reason;
                slot->fence_retired = 0u;
                slot->notify_batch = batch_id;
                slot->notify_index = 0u;
                sync_surface_heads_from_slot_locked(reg, slot, 1);
                reg->stats.total_canceled_on_fault++;
                failed_slots++;
            }
        }
        recompute_pending_locked(reg);
    }
    reg_unlock(reg);

    /* Poison WaitAny queue outside reg->lock so WaitAny waiters wake immediately. */
    if (wa != NULL)
        (void)a840_kmd_wait_any_poison_queue(wa, queue);

    while (!done) {
        A840KmdPromiseEvent batch[A840_KMD_PROMISE_EVENT_BATCH_MAX];
        size_t count = 0u;
        A840KmdPromiseObserver obs = NULL;
        void *obs_cookie = NULL;
        size_t i;

        reg_lock(reg);
        obs = reg->observer;
        obs_cookie = reg->observer_cookie;
        done = 1;
        for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
            A840KmdPromiseSlot *slot = &reg->slots[i];
            if (slot->notify_batch != batch_id)
                continue;
            while (slot->notify_index < slot->surface_count) {
                if (count >= A840_KMD_PROMISE_EVENT_BATCH_MAX) {
                    done = 0;
                    break;
                }
                fill_event_from_slot(slot, slot->notify_index, &batch[count++]);
                slot->notify_index++;
            }
            if (slot->notify_index >= slot->surface_count)
                slot->notify_batch = 0u;
            if (!done)
                break;
        }
        reg_unlock(reg);

        deliver_events_unlocked(obs, obs_cookie, batch, count);
    }

    return failed_slots;
}

void a840_kmd_writer_promise_on_fault_poison(
    void *cookie,
    uint64_t client_id,
    uint64_t queue_id,
    const A840KmdFaultSnapshot *snapshot)
{
    A840KmdWriterPromiseRegistry *reg = (A840KmdWriterPromiseRegistry *)cookie;
    (void)snapshot;
    if (reg == NULL)
        return;
    (void)a840_kmd_writer_promise_fail_channel(
        reg, client_id, queue_id, A840_PROMISE_CANCEL_GPU_FAULT);
}

size_t a840_kmd_writer_promise_close_client(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client)
{
    size_t canceled_slots = 0u;
    uint64_t queues_to_disconnect[A840_KMD_PROMISE_MAX_QUEUES];
    size_t disconnect_count = 0u;
    A840KmdWaitAnyEngine *wa = NULL;
    uint32_t batch_id;
    size_t i;
    int done = 0;

    if (check_init(reg) != A840_KMD_OK || writer_client == 0u)
        return 0u;

    reg_lock(reg);
    wa = reg->wait_any;
    reg->next_notify_batch++;
    if (reg->next_notify_batch == 0u)
        reg->next_notify_batch = 1u;
    batch_id = reg->next_notify_batch;

    /* Atomically cancel all in-flight promises owned by writer_client. */
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        A840KmdPromiseSlot *slot = &reg->slots[i];
        if (slot->writer_client != writer_client)
            continue;
        if (state_is_in_flight(slot->state)) {
            slot->state = (uint32_t)A840_PROMISE_STATE_CANCELED;
            slot->cancel_reason = (uint32_t)A840_PROMISE_CANCEL_CLIENT_CLOSE;
            slot->fence_retired = 0u;
            slot->notify_batch = batch_id;
            slot->notify_index = 0u;
            sync_surface_heads_from_slot_locked(reg, slot, 1);
            reg->stats.total_canceled_on_close++;
            canceled_slots++;
        }
    }

    /* Collect and clear client queues so WaitAny can disconnect outside lock. */
    for (i = 0u; i < A840_KMD_PROMISE_MAX_QUEUES; ++i) {
        if (reg->queues[i].active && reg->queues[i].writer_client == writer_client) {
            if (disconnect_count < A840_KMD_PROMISE_MAX_QUEUES)
                queues_to_disconnect[disconnect_count++] = reg->queues[i].queue;
            memset(&reg->queues[i], 0, sizeof(reg->queues[i]));
        }
    }

    /* Remove trusted surface ownership bindings owned by closing client. */
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SURFACES; ++i) {
        if (reg->surface_auth[i].active &&
            reg->surface_auth[i].owner_client == writer_client)
            memset(&reg->surface_auth[i], 0, sizeof(reg->surface_auth[i]));
    }

    /* Remove client registration slot. */
    for (i = 0u; i < A840_KMD_PROMISE_MAX_CLIENTS; ++i) {
        if (reg->clients[i].active && reg->clients[i].client_id == writer_client) {
            memset(&reg->clients[i], 0, sizeof(reg->clients[i]));
            if (reg->stats.active_clients > 0u)
                reg->stats.active_clients--;
            break;
        }
    }

    recompute_pending_locked(reg);
    reg_unlock(reg);

    if (wa != NULL) {
        for (i = 0u; i < disconnect_count; ++i)
            (void)a840_kmd_wait_any_disconnect_queue(wa, queues_to_disconnect[i]);
    }

    while (!done) {
        A840KmdPromiseEvent batch[A840_KMD_PROMISE_EVENT_BATCH_MAX];
        size_t count = 0u;
        A840KmdPromiseObserver obs = NULL;
        void *obs_cookie = NULL;

        reg_lock(reg);
        obs = reg->observer;
        obs_cookie = reg->observer_cookie;
        done = 1;
        for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
            A840KmdPromiseSlot *slot = &reg->slots[i];
            if (slot->notify_batch != batch_id)
                continue;
            while (slot->notify_index < slot->surface_count) {
                if (count >= A840_KMD_PROMISE_EVENT_BATCH_MAX) {
                    done = 0;
                    break;
                }
                fill_event_from_slot(slot, slot->notify_index, &batch[count++]);
                slot->notify_index++;
            }
            if (slot->notify_index >= slot->surface_count)
                slot->notify_batch = 0u;
            if (!done)
                break;
        }
        reg_unlock(reg);

        deliver_events_unlocked(obs, obs_cookie, batch, count);
    }

    return canceled_slots;
}

A840KmdStatus a840_kmd_writer_promise_query_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t writer_client_filter,
    A840KmdSurfacePromiseSnapshot *out_snapshot)
{
    A840KmdStatus st = check_init(reg);
    A840KmdSurfaceHead *head;

    if (st != A840_KMD_OK)
        return st;
    if (surface_identity == 0u || out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    reg_lock(reg);
    head = find_latest_surface_head_locked(reg, surface_identity, writer_client_filter);
    if (head == NULL) {
        reg_unlock(reg);
        return A840_KMD_NOT_FOUND;
    }
    fill_snapshot_from_head(head, out_snapshot);
    reg_unlock(reg);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_writer_promise_query_token(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t promise_token,
    A840KmdPromiseEvent *out_event)
{
    A840KmdStatus st = check_init(reg);
    A840KmdPromiseSlot *slot;

    if (st != A840_KMD_OK)
        return st;
    if (promise_token == 0u || out_event == NULL)
        return A840_KMD_INVALID_ARG;

    reg_lock(reg);
    slot = find_slot_by_token_locked(reg, promise_token);
    if (slot == NULL) {
        reg_unlock(reg);
        return A840_KMD_NOT_FOUND;
    }
    fill_event_from_slot(slot, 0u, out_event);
    reg_unlock(reg);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_writer_promise_wait_surface(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t surface_identity,
    uint64_t min_generation,
    uint64_t timeout_ns,
    A840KmdSurfacePromiseSnapshot *out_snapshot,
    uint32_t *out_timed_out)
{
    A840KmdStatus st = check_init(reg);
    A840KmdPromiseWaiter waiter;
    uint64_t start_ns;
    int cond_initialized = 0;
    int saw_timedout = 0;

    if (st != A840_KMD_OK)
        return st;
    if (surface_identity == 0u || out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    memset(&waiter, 0, sizeof(waiter));
    waiter.surface_identity = surface_identity;
    waiter.min_generation = min_generation;
    if (out_timed_out != NULL)
        *out_timed_out = 0u;

    /* Fast path under lock before initializing condvar. */
    reg_lock(reg);
    {
        A840KmdSurfaceHead *head = find_latest_surface_head_locked(
            reg, surface_identity, 0u);
        if (head != NULL && head->generation >= min_generation &&
            state_is_wait_complete(head->state)) {
            fill_snapshot_from_head(head, out_snapshot);
            reg_unlock(reg);
            return A840_KMD_OK;
        }
        if (timeout_ns == 0u) {
            if (head != NULL)
                fill_snapshot_from_head(head, out_snapshot);
            else
                memset(out_snapshot, 0, sizeof(*out_snapshot));
            if (out_timed_out != NULL)
                *out_timed_out = 1u;
            reg_unlock(reg);
            return A840_KMD_OK;
        }
    }
    reg_unlock(reg);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!reg->use_custom_sync) {
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) != 0)
            return A840_KMD_NO_MEMORY;
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
            (void)pthread_condattr_destroy(&attr);
            return A840_KMD_IO_ERROR;
        }
        if (pthread_cond_init(&waiter.cond, &attr) != 0) {
            (void)pthread_condattr_destroy(&attr);
            return A840_KMD_NO_MEMORY;
        }
        (void)pthread_condattr_destroy(&attr);
        cond_initialized = 1;
    }
#endif

    start_ns = reg_now_ns(reg);

    reg_lock(reg);
    /* Link stack waiter node under lock. */
    waiter.prev = NULL;
    waiter.next = reg->waiters_head;
    if (reg->waiters_head != NULL)
        reg->waiters_head->prev = &waiter;
    reg->waiters_head = &waiter;
    reg->active_waiter_count++;
    reg->stats.active_waiters = reg->active_waiter_count;

    for (;;) {
        A840KmdSurfaceHead *head;
        uint64_t now_ns;
        uint64_t elapsed_ns;

        if (reg->initialized != A840_KMD_PROMISE_MAGIC) {
            st = A840_KMD_DISCONNECTED;
            break;
        }

        head = find_latest_surface_head_locked(reg, surface_identity, 0u);
        if (head != NULL && head->generation >= min_generation &&
            state_is_wait_complete(head->state)) {
            fill_snapshot_from_head(head, out_snapshot);
            if (out_timed_out != NULL)
                *out_timed_out = 0u;
            st = A840_KMD_OK;
            break;
        }

        now_ns = reg_now_ns(reg);
        elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0u;
        if (timeout_ns != A840_KMD_WAIT_INFINITE &&
            (saw_timedout || elapsed_ns >= timeout_ns)) {
            if (head != NULL)
                fill_snapshot_from_head(head, out_snapshot);
            else
                memset(out_snapshot, 0, sizeof(*out_snapshot));
            if (out_timed_out != NULL)
                *out_timed_out = 1u;
            st = A840_KMD_OK;
            break;
        }

        if (reg->use_custom_sync) {
            int infinite = (timeout_ns == A840_KMD_WAIT_INFINITE) ? 1 : 0;
            uint64_t deadline_ns = UINT64_MAX;
            int wait_rc;
            if (!infinite) {
                uint64_t rem = timeout_ns - elapsed_ns;
                deadline_ns = (rem > UINT64_MAX - now_ns) ? UINT64_MAX : (now_ns + rem);
            }
            wait_rc = reg->sync_ops.wait(reg->sync_ops.cookie, deadline_ns, infinite);
            if (!infinite && wait_rc != 0)
                saw_timedout = 1;
        } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
            if (timeout_ns == A840_KMD_WAIT_INFINITE) {
                (void)pthread_cond_wait(&waiter.cond, &reg->lock);
            } else {
                struct timespec ts;
                uint64_t rem = timeout_ns - elapsed_ns;
                uint64_t chunk = (rem > A840_KMD_PROMISE_MAX_SLEEP_CHUNK_NS) ?
                    A840_KMD_PROMISE_MAX_SLEEP_CHUNK_NS : rem;
                uint64_t add_sec = chunk / A840_KMD_PROMISE_NS_PER_SEC;
                uint64_t add_nsec = chunk % A840_KMD_PROMISE_NS_PER_SEC;
                int rc;

                if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
                    st = A840_KMD_IO_ERROR;
                    break;
                }
                ts.tv_sec += (time_t)add_sec;
                ts.tv_nsec += (long)add_nsec;
                if (ts.tv_nsec >= (long)A840_KMD_PROMISE_NS_PER_SEC) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= (long)A840_KMD_PROMISE_NS_PER_SEC;
                }
                rc = pthread_cond_timedwait(&waiter.cond, &reg->lock, &ts);
                if (rc == ETIMEDOUT && rem <= A840_KMD_PROMISE_MAX_SLEEP_CHUNK_NS)
                    saw_timedout = 1;
            }
#endif
        }
    }

    /* Unlink waiter node while still holding reg->lock. */
    if (waiter.prev != NULL)
        waiter.prev->next = waiter.next;
    else
        reg->waiters_head = waiter.next;
    if (waiter.next != NULL)
        waiter.next->prev = waiter.prev;
    if (reg->active_waiter_count > 0u)
        reg->active_waiter_count--;
    reg->stats.active_waiters = reg->active_waiter_count;
    reg_unlock(reg);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (cond_initialized)
        (void)pthread_cond_destroy(&waiter.cond);
#else
    (void)cond_initialized;
#endif

    return st;
}

size_t a840_kmd_writer_promise_pending_count(
    A840KmdWriterPromiseRegistry *reg,
    uint64_t writer_client)
{
    size_t i;
    size_t count = 0u;
    if (check_init(reg) != A840_KMD_OK)
        return 0u;
    reg_lock(reg);
    for (i = 0u; i < A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        if (state_is_in_flight(reg->slots[i].state) &&
            (writer_client == 0u || reg->slots[i].writer_client == writer_client))
            count++;
    }
    reg_unlock(reg);
    return count;
}

A840KmdStatus a840_kmd_writer_promise_get_stats(
    A840KmdWriterPromiseRegistry *reg,
    A840KmdPromiseStats *out_stats)
{
    A840KmdStatus st = check_init(reg);
    if (st != A840_KMD_OK)
        return st;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;
    reg_lock(reg);
    *out_stats = reg->stats;
    reg_unlock(reg);
    return A840_KMD_OK;
}
