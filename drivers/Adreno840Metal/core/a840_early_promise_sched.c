/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_early_promise_sched.h"

#include <string.h>

static bool role_valid(A840EarlyClientRole role)
{
    return role == A840_EARLY_ROLE_APP || role == A840_EARLY_ROLE_RENDER_SERVER;
}

static bool cancel_reason_valid(A840EarlyPromiseCancelReason reason)
{
    return reason == A840_EARLY_CANCEL_EXPLICIT ||
           reason == A840_EARLY_CANCEL_GPU_FAULT ||
           reason == A840_EARLY_CANCEL_CHANNEL_RESET ||
           reason == A840_EARLY_CANCEL_CHUNK_SUBMIT_FAILED;
}

static A840EarlyPromiseHistory *find_history(A840EarlyPromiseSched *sched, uint64_t surface_id)
{
    uint32_t i;
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_HISTORY; ++i) {
        if (sched->history[i].active && sched->history[i].surface_id == surface_id) {
            return &sched->history[i];
        }
    }
    return NULL;
}

static A840EarlyPromiseHistory *upsert_history(A840EarlyPromiseSched *sched, uint64_t surface_id)
{
    uint32_t i;
    A840EarlyPromiseHistory *h = find_history(sched, surface_id);
    if (h) {
        return h;
    }
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_HISTORY; ++i) {
        if (!sched->history[i].active) {
            memset(&sched->history[i], 0, sizeof(sched->history[i]));
            sched->history[i].active = true;
            sched->history[i].surface_id = surface_id;
            return &sched->history[i];
        }
    }
    return NULL;
}

static A840EarlyPromiseSurfacePlan *find_frame_surface(A840EarlyPromiseSched *sched,
                                                       uint64_t surface_id)
{
    uint32_t i;
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_SURFACES; ++i) {
        if (sched->frame_surfaces[i].active &&
            sched->frame_surfaces[i].surface_id == surface_id) {
            return &sched->frame_surfaces[i];
        }
    }
    return NULL;
}

static A840EarlyPromiseSurfacePlan *alloc_frame_surface(A840EarlyPromiseSched *sched,
                                                        uint64_t surface_id)
{
    uint32_t i;
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_SURFACES; ++i) {
        if (!sched->frame_surfaces[i].active) {
            memset(&sched->frame_surfaces[i], 0, sizeof(sched->frame_surfaces[i]));
            sched->frame_surfaces[i].active = true;
            sched->frame_surfaces[i].surface_id = surface_id;
            sched->stats.tracked_frame_surfaces++;
            return &sched->frame_surfaces[i];
        }
    }
    return NULL;
}

static A840EarlyPromiseActiveRecord *alloc_active_record(A840EarlyPromiseSched *sched)
{
    uint32_t i;
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_ACTIVE; ++i) {
        if (!sched->active_records[i].active) {
            memset(&sched->active_records[i], 0, sizeof(sched->active_records[i]));
            sched->active_records[i].active = true;
            sched->stats.active_pending_count++;
            return &sched->active_records[i];
        }
    }
    return NULL;
}

static void release_active_record(A840EarlyPromiseSched *sched,
                                  A840EarlyPromiseActiveRecord *rec)
{
    if (rec->active) {
        rec->active = false;
        if (sched->stats.active_pending_count > 0u) {
            sched->stats.active_pending_count--;
        }
    }
}

static void dispatch_events_unlocked(A840EarlyPromiseCallback cb,
                                     void *ctx,
                                     const A840EarlyPromiseEvent *events,
                                     size_t count)
{
    size_t i;
    if (!cb || count == 0u) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        cb(ctx, &events[i]);
    }
}

static A840EarlyPromiseStatus publish_surface_locked(A840EarlyPromiseSched *sched,
                                                     A840EarlyPromiseSurfacePlan *plan,
                                                     uint64_t cb_id,
                                                     uint32_t ring_id,
                                                     A840EarlyPromiseEvent *out_events,
                                                     size_t *inout_event_count,
                                                     size_t max_events)
{
    A840EarlyPromiseHistory *hist;
    A840EarlyPromiseActiveRecord *rec;
    A840EarlyPromisePolicy policy;
    A840EarlyPromiseEventKind ev_kind;
    uint64_t next_gen;
    uint64_t token;
    uint32_t display_ready;
    A840EarlyPromiseEvent ev;

    if (plan->published || plan->canceled || plan->last_chunk_seqno == 0u) {
        return A840_PROMISE_SCHED_OK;
    }

    hist = upsert_history(sched, plan->surface_id);
    if (!hist) {
        return A840_PROMISE_SCHED_ERR_CAPACITY;
    }

    if (plan->explicit_generation != 0u) {
        if (plan->explicit_generation <= hist->last_generation) {
            sched->stats.non_monotonic_gen_rejected++;
            return A840_PROMISE_SCHED_ERR_NON_MONOTONIC_GEN;
        }
        next_gen = plan->explicit_generation;
    } else {
        if (hist->last_generation == UINT64_MAX) {
            return A840_PROMISE_SCHED_ERR_CAPACITY;
        }
        next_gen = hist->last_generation + 1u;
    }

    /* Strict privilege enforcement:
     * ONLY RENDER_SERVER with early_requested may emit EARLY_SCHEDULED.
     * APP ALWAYS walks DEFER_UNTIL_RETIRED. */
    if (sched->client_role == A840_EARLY_ROLE_RENDER_SERVER && plan->early_requested) {
        policy = A840_EARLY_POLICY_EARLY_SCHEDULED;
        ev_kind = A840_PROMISE_EVENT_EARLY_SCHEDULED;
        display_ready = 1u;
    } else {
        policy = A840_EARLY_POLICY_DEFER_UNTIL_RETIRED;
        ev_kind = A840_PROMISE_EVENT_DEFERRED_ARMED;
        display_ready = 0u;
    }

    rec = alloc_active_record(sched);
    if (!rec) {
        return A840_PROMISE_SCHED_ERR_CAPACITY;
    }

    token = ++sched->next_promise_token;
    rec->retired = false;
    rec->policy = policy;
    rec->state = ev_kind;
    rec->frame_id = sched->active_frame_id;
    rec->cb_id = cb_id;
    rec->surface_id = plan->surface_id;
    rec->generation = next_gen;
    rec->chunk_seqno = plan->last_chunk_seqno;
    rec->promise_token = token;
    rec->ring_id = ring_id;

    hist->last_generation = next_gen;
    hist->last_chunk_seqno = plan->last_chunk_seqno;
    hist->last_promise_token = token;
    hist->last_ring_id = ring_id;
    hist->last_policy = policy;
    hist->last_event = ev_kind;
    hist->last_cancel_reason = A840_EARLY_CANCEL_NONE;
    hist->display_ready = display_ready;

    plan->published = true;

    if (policy == A840_EARLY_POLICY_EARLY_SCHEDULED) {
        sched->stats.early_scheduled_count++;
    } else {
        sched->stats.deferred_armed_count++;
    }

    memset(&ev, 0, sizeof(ev));
    ev.kind = ev_kind;
    ev.client_role = sched->client_role;
    ev.policy = policy;
    ev.cancel_reason = A840_EARLY_CANCEL_NONE;
    ev.client_id = sched->client_id;
    ev.frame_id = sched->active_frame_id;
    ev.cb_id = cb_id;
    ev.surface_id = plan->surface_id;
    ev.generation = next_gen;
    ev.chunk_seqno = plan->last_chunk_seqno;
    ev.promise_token = token;
    ev.ring_id = ring_id;
    ev.display_ready = display_ready;

    if (*inout_event_count < max_events) {
        out_events[(*inout_event_count)++] = ev;
    }
    return A840_PROMISE_SCHED_OK;
}

A840EarlyPromiseStatus a840_early_promise_init(A840EarlyPromiseSched *sched,
                                               const A840EarlyPromiseConfig *cfg)
{
    if (!sched || !cfg) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    if (!role_valid(cfg->client_role) || cfg->client_id == 0u) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }

    memset(sched, 0, sizeof(*sched));
    if (pthread_mutex_init(&sched->lock, NULL) != 0) {
        return A840_PROMISE_SCHED_ERR_INVALID_STATE;
    }
    sched->lock_initialized = true;
    sched->initialized = true;
    sched->client_id = cfg->client_id;
    sched->client_role = cfg->client_role;
    sched->strict_reject_app_early = cfg->strict_reject_app_early;
    sched->callback = cfg->callback;
    sched->callback_ctx = cfg->callback_ctx;
    return A840_PROMISE_SCHED_OK;
}

void a840_early_promise_destroy(A840EarlyPromiseSched *sched)
{
    if (!sched) {
        return;
    }
    if (sched->lock_initialized) {
        pthread_mutex_destroy(&sched->lock);
        sched->lock_initialized = false;
    }
    sched->initialized = false;
}

A840EarlyPromiseStatus a840_early_promise_begin_frame(A840EarlyPromiseSched *sched,
                                                      uint64_t frame_id,
                                                      uint64_t cb_id,
                                                      uint32_t ring_id)
{
    if (!sched || !sched->initialized || cb_id == 0u ||
        ring_id > A840_EARLY_PROMISE_MAX_RING_ID) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&sched->lock);
    if (sched->ring_poisoned[ring_id]) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_CHANNEL_POISONED;
    }

    memset(sched->frame_surfaces, 0, sizeof(sched->frame_surfaces));
    sched->stats.tracked_frame_surfaces = 0u;
    sched->frame_active = true;
    sched->active_frame_id = frame_id;
    sched->active_cb_id = cb_id;
    sched->active_ring_id = ring_id;
    pthread_mutex_unlock(&sched->lock);
    return A840_PROMISE_SCHED_OK;
}

A840EarlyPromiseStatus a840_early_promise_plan_surface(A840EarlyPromiseSched *sched,
                                                       uint64_t surface_id,
                                                       uint32_t expected_write_chunks,
                                                       uint64_t explicit_generation,
                                                       bool request_early)
{
    A840EarlyPromiseSurfacePlan *plan;
    A840EarlyPromiseHistory *hist;
    bool effective_early = false;

    if (!sched || !sched->initialized || surface_id == 0u) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&sched->lock);
    if (!sched->frame_active) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_INVALID_STATE;
    }
    if (sched->ring_poisoned[sched->active_ring_id]) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_CHANNEL_POISONED;
    }

    /* Fail-closed privilege gate for APP clients requesting early promise. */
    if (sched->client_role == A840_EARLY_ROLE_APP) {
        if (request_early) {
            if (sched->strict_reject_app_early) {
                sched->stats.app_early_rejected_count++;
                pthread_mutex_unlock(&sched->lock);
                return A840_PROMISE_SCHED_ERR_PERMISSION_DENIED;
            }
            sched->stats.app_early_downgraded_count++;
        }
        effective_early = false;
    } else {
        effective_early = request_early;
    }

    /* Validate monotonic explicit generation before mutating state. */
    if (explicit_generation != 0u) {
        hist = find_history(sched, surface_id);
        if (hist && explicit_generation <= hist->last_generation) {
            sched->stats.non_monotonic_gen_rejected++;
            pthread_mutex_unlock(&sched->lock);
            return A840_PROMISE_SCHED_ERR_NON_MONOTONIC_GEN;
        }
    }

    plan = find_frame_surface(sched, surface_id);
    if (plan) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_DUPLICATE;
    }

    plan = alloc_frame_surface(sched, surface_id);
    if (!plan) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_CAPACITY;
    }

    plan->expected_write_chunks = expected_write_chunks;
    plan->explicit_generation = explicit_generation;
    plan->early_requested = effective_early;
    plan->kicked_write_chunks = 0u;
    plan->last_chunk_seqno = 0u;
    plan->published = false;
    plan->canceled = false;

    pthread_mutex_unlock(&sched->lock);
    return A840_PROMISE_SCHED_OK;
}

A840EarlyPromiseStatus a840_early_promise_on_chunk_kicked(A840EarlyPromiseSched *sched,
                                                          uint64_t cb_id,
                                                          uint64_t chunk_seqno,
                                                          uint32_t ring_id,
                                                          const uint64_t *written_surface_ids,
                                                          size_t written_surface_count,
                                                          bool is_final_chunk)
{
    A840EarlyPromiseEvent events[A840_EARLY_PROMISE_MAX_SURFACES];
    size_t event_count = 0u;
    A840EarlyPromiseCallback cb = NULL;
    void *cb_ctx = NULL;
    A840EarlyPromiseStatus status = A840_PROMISE_SCHED_OK;
    size_t i;
    uint32_t s;

    if (!sched || !sched->initialized || cb_id == 0u || chunk_seqno == 0u ||
        ring_id > A840_EARLY_PROMISE_MAX_RING_ID) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    if (written_surface_count > 0u && !written_surface_ids) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    if (written_surface_count > A840_EARLY_PROMISE_MAX_SURFACES) {
        return A840_PROMISE_SCHED_ERR_CAPACITY;
    }
    for (i = 0u; i < written_surface_count; ++i) {
        if (written_surface_ids[i] == 0u) {
            return A840_PROMISE_SCHED_ERR_INVALID_ARG;
        }
    }

    pthread_mutex_lock(&sched->lock);
    if (!sched->frame_active || sched->active_cb_id != cb_id || sched->active_ring_id != ring_id) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_INVALID_STATE;
    }
    if (sched->ring_poisoned[ring_id]) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_CHANNEL_POISONED;
    }

    for (i = 0u; i < written_surface_count; ++i) {
        uint64_t sid = written_surface_ids[i];
        A840EarlyPromiseSurfacePlan *plan = find_frame_surface(sched, sid);
        if (!plan) {
            /* Dynamic unplanned surface: auto-track and hold until FINAL chunk. */
            plan = alloc_frame_surface(sched, sid);
            if (!plan) {
                status = A840_PROMISE_SCHED_ERR_CAPACITY;
                break;
            }
            plan->expected_write_chunks = 0u;
            plan->explicit_generation = 0u;
            plan->early_requested = (sched->client_role == A840_EARLY_ROLE_RENDER_SERVER);
        }

        if (plan->canceled) {
            continue;
        }

        /* Deduplicate multiple writes to the same surface within the same chunk. */
        if (plan->last_chunk_seqno == chunk_seqno) {
            sched->stats.duplicate_writes_deduped++;
            continue;
        }

        plan->last_chunk_seqno = chunk_seqno;
        plan->kicked_write_chunks++;

        /* If exact write-chunk count is known and reached, publish immediately! */
        if (!plan->published &&
            plan->expected_write_chunks > 0u &&
            plan->kicked_write_chunks >= plan->expected_write_chunks) {
            status = publish_surface_locked(sched,
                                            plan,
                                            cb_id,
                                            ring_id,
                                            events,
                                            &event_count,
                                            A840_EARLY_PROMISE_MAX_SURFACES);
            if (status != A840_PROMISE_SCHED_OK) {
                break;
            }
        }
    }

    /* On FINAL chunk, publish all remaining touched & unpublished surfaces
     * (never publishing a surface twice, and never publishing untouched ones). */
    if (status == A840_PROMISE_SCHED_OK && is_final_chunk) {
        for (s = 0u; s < A840_EARLY_PROMISE_MAX_SURFACES; ++s) {
            A840EarlyPromiseSurfacePlan *plan = &sched->frame_surfaces[s];
            if (plan->active && !plan->published && !plan->canceled &&
                plan->kicked_write_chunks > 0u) {
                status = publish_surface_locked(sched,
                                                plan,
                                                cb_id,
                                                ring_id,
                                                events,
                                                &event_count,
                                                A840_EARLY_PROMISE_MAX_SURFACES);
                if (status != A840_PROMISE_SCHED_OK) {
                    break;
                }
            }
        }
        if (status == A840_PROMISE_SCHED_OK) {
            sched->frame_active = false;
        }
    }

    cb = sched->callback;
    cb_ctx = sched->callback_ctx;
    if (cb && event_count > 0u) {
        sched->stats.callbacks_invoked += (uint64_t)event_count;
    }
    pthread_mutex_unlock(&sched->lock);

    dispatch_events_unlocked(cb, cb_ctx, events, event_count);
    return status;
}

size_t a840_early_promise_on_chunk_retired(A840EarlyPromiseSched *sched,
                                           uint32_t ring_id,
                                           uint64_t completed_chunk_seqno)
{
    A840EarlyPromiseEvent events[A840_EARLY_PROMISE_MAX_ACTIVE];
    size_t event_count = 0u;
    size_t retired_total = 0u;
    A840EarlyPromiseCallback cb = NULL;
    void *cb_ctx = NULL;
    uint32_t i;

    if (!sched || !sched->initialized || ring_id > A840_EARLY_PROMISE_MAX_RING_ID ||
        completed_chunk_seqno == 0u) {
        return 0u;
    }

    pthread_mutex_lock(&sched->lock);
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_ACTIVE; ++i) {
        A840EarlyPromiseActiveRecord *rec = &sched->active_records[i];
        A840EarlyPromiseHistory *hist;
        A840EarlyPromiseEvent ev;

        if (!rec->active || rec->ring_id != ring_id || rec->chunk_seqno > completed_chunk_seqno) {
            continue;
        }

        retired_total++;
        sched->stats.retired_ready_count++;

        hist = find_history(sched, rec->surface_id);
        if (hist && hist->last_promise_token == rec->promise_token) {
            hist->last_event = A840_PROMISE_EVENT_RETIRED_READY;
            hist->display_ready = 1u;
        }

        memset(&ev, 0, sizeof(ev));
        ev.kind = A840_PROMISE_EVENT_RETIRED_READY;
        ev.client_role = sched->client_role;
        ev.policy = rec->policy;
        ev.cancel_reason = A840_EARLY_CANCEL_NONE;
        ev.client_id = sched->client_id;
        ev.frame_id = rec->frame_id;
        ev.cb_id = rec->cb_id;
        ev.surface_id = rec->surface_id;
        ev.generation = rec->generation;
        ev.chunk_seqno = rec->chunk_seqno;
        ev.promise_token = rec->promise_token;
        ev.ring_id = rec->ring_id;
        ev.display_ready = 1u;

        if (event_count < A840_EARLY_PROMISE_MAX_ACTIVE) {
            events[event_count++] = ev;
        }

        release_active_record(sched, rec);
    }

    cb = sched->callback;
    cb_ctx = sched->callback_ctx;
    if (cb && event_count > 0u) {
        sched->stats.callbacks_invoked += (uint64_t)event_count;
    }
    pthread_mutex_unlock(&sched->lock);

    dispatch_events_unlocked(cb, cb_ctx, events, event_count);
    return retired_total;
}

static size_t cancel_matching_locked(A840EarlyPromiseSched *sched,
                                     bool match_cb,
                                     uint64_t cb_id,
                                     bool match_ring,
                                     uint32_t ring_id,
                                     A840EarlyPromiseCancelReason reason,
                                     A840EarlyPromiseEvent *out_events,
                                     size_t max_events)
{
    size_t count = 0u;
    uint32_t i;

    /* 1. Cancel all active non-retired published/deferred records matching filter. */
    for (i = 0u; i < A840_EARLY_PROMISE_MAX_ACTIVE; ++i) {
        A840EarlyPromiseActiveRecord *rec = &sched->active_records[i];
        A840EarlyPromiseHistory *hist;
        A840EarlyPromiseEvent ev;

        if (!rec->active) {
            continue;
        }
        if (match_cb && rec->cb_id != cb_id) {
            continue;
        }
        if (match_ring && rec->ring_id != ring_id) {
            continue;
        }

        hist = find_history(sched, rec->surface_id);
        if (hist && hist->last_promise_token == rec->promise_token) {
            hist->last_event = A840_PROMISE_EVENT_PROMISE_CANCELED;
            hist->last_cancel_reason = reason;
            hist->display_ready = 0u;
        }

        memset(&ev, 0, sizeof(ev));
        ev.kind = A840_PROMISE_EVENT_PROMISE_CANCELED;
        ev.client_role = sched->client_role;
        ev.policy = rec->policy;
        ev.cancel_reason = reason;
        ev.client_id = sched->client_id;
        ev.frame_id = rec->frame_id;
        ev.cb_id = rec->cb_id;
        ev.surface_id = rec->surface_id;
        ev.generation = rec->generation;
        ev.chunk_seqno = rec->chunk_seqno;
        ev.promise_token = rec->promise_token;
        ev.ring_id = rec->ring_id;
        ev.display_ready = 0u;

        if (count < max_events) {
            out_events[count++] = ev;
        }
        sched->stats.canceled_broadcast_count++;
        release_active_record(sched, rec);
    }

    /* 2. Cancel any in-progress unpublished surfaces in the active frame if matching. */
    if (sched->frame_active &&
        (!match_cb || sched->active_cb_id == cb_id) &&
        (!match_ring || sched->active_ring_id == ring_id)) {
        for (i = 0u; i < A840_EARLY_PROMISE_MAX_SURFACES; ++i) {
            A840EarlyPromiseSurfacePlan *plan = &sched->frame_surfaces[i];
            A840EarlyPromiseHistory *hist;
            A840EarlyPromiseEvent ev;

            if (!plan->active || plan->published || plan->canceled) {
                continue;
            }
            plan->canceled = true;

            hist = upsert_history(sched, plan->surface_id);
            if (hist) {
                hist->last_event = A840_PROMISE_EVENT_PROMISE_CANCELED;
                hist->last_cancel_reason = reason;
                hist->display_ready = 0u;
            }

            memset(&ev, 0, sizeof(ev));
            ev.kind = A840_PROMISE_EVENT_PROMISE_CANCELED;
            ev.client_role = sched->client_role;
            ev.policy = (sched->client_role == A840_EARLY_ROLE_RENDER_SERVER && plan->early_requested)
                            ? A840_EARLY_POLICY_EARLY_SCHEDULED
                            : A840_EARLY_POLICY_DEFER_UNTIL_RETIRED;
            ev.cancel_reason = reason;
            ev.client_id = sched->client_id;
            ev.frame_id = sched->active_frame_id;
            ev.cb_id = sched->active_cb_id;
            ev.surface_id = plan->surface_id;
            ev.generation = hist ? hist->last_generation : 0u;
            ev.chunk_seqno = plan->last_chunk_seqno;
            ev.promise_token = 0u;
            ev.ring_id = sched->active_ring_id;
            ev.display_ready = 0u;

            if (count < max_events) {
                out_events[count++] = ev;
            }
            sched->stats.canceled_broadcast_count++;
        }
        sched->frame_active = false;
    }

    return count;
}

size_t a840_early_promise_cancel_cb(A840EarlyPromiseSched *sched,
                                    uint64_t cb_id,
                                    A840EarlyPromiseCancelReason reason)
{
    A840EarlyPromiseEvent events[A840_EARLY_PROMISE_MAX_ACTIVE + A840_EARLY_PROMISE_MAX_SURFACES];
    size_t count = 0u;
    A840EarlyPromiseCallback cb = NULL;
    void *cb_ctx = NULL;

    if (!sched || !sched->initialized || cb_id == 0u || !cancel_reason_valid(reason)) {
        return 0u;
    }

    pthread_mutex_lock(&sched->lock);
    count = cancel_matching_locked(sched,
                                   true,
                                   cb_id,
                                   false,
                                   0u,
                                   reason,
                                   events,
                                   A840_EARLY_PROMISE_MAX_ACTIVE + A840_EARLY_PROMISE_MAX_SURFACES);
    cb = sched->callback;
    cb_ctx = sched->callback_ctx;
    if (cb && count > 0u) {
        sched->stats.callbacks_invoked += (uint64_t)count;
    }
    pthread_mutex_unlock(&sched->lock);

    dispatch_events_unlocked(cb, cb_ctx, events, count);
    return count;
}

size_t a840_early_promise_cancel_ring(A840EarlyPromiseSched *sched,
                                      uint32_t ring_id,
                                      A840EarlyPromiseCancelReason reason,
                                      bool poison_ring)
{
    A840EarlyPromiseEvent events[A840_EARLY_PROMISE_MAX_ACTIVE + A840_EARLY_PROMISE_MAX_SURFACES];
    size_t count = 0u;
    A840EarlyPromiseCallback cb = NULL;
    void *cb_ctx = NULL;

    if (!sched || !sched->initialized || ring_id > A840_EARLY_PROMISE_MAX_RING_ID ||
        !cancel_reason_valid(reason)) {
        return 0u;
    }

    pthread_mutex_lock(&sched->lock);
    if (poison_ring) {
        sched->ring_poisoned[ring_id] = true;
    }
    count = cancel_matching_locked(sched,
                                   false,
                                   0u,
                                   true,
                                   ring_id,
                                   reason,
                                   events,
                                   A840_EARLY_PROMISE_MAX_ACTIVE + A840_EARLY_PROMISE_MAX_SURFACES);
    cb = sched->callback;
    cb_ctx = sched->callback_ctx;
    if (cb && count > 0u) {
        sched->stats.callbacks_invoked += (uint64_t)count;
    }
    pthread_mutex_unlock(&sched->lock);

    dispatch_events_unlocked(cb, cb_ctx, events, count);
    return count;
}

A840EarlyPromiseStatus a840_early_promise_reset_ring(A840EarlyPromiseSched *sched,
                                                     uint32_t ring_id)
{
    if (!sched || !sched->initialized || ring_id > A840_EARLY_PROMISE_MAX_RING_ID) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&sched->lock);
    sched->ring_poisoned[ring_id] = false;
    pthread_mutex_unlock(&sched->lock);
    return A840_PROMISE_SCHED_OK;
}

A840EarlyPromiseStatus a840_early_promise_query_surface(A840EarlyPromiseSched *sched,
                                                        uint64_t surface_id,
                                                        A840EarlyPromiseSurfaceSnapshot *out_snapshot)
{
    A840EarlyPromiseHistory *hist;

    if (!sched || !sched->initialized || surface_id == 0u || !out_snapshot) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    pthread_mutex_lock(&sched->lock);
    hist = find_history(sched, surface_id);
    if (!hist) {
        pthread_mutex_unlock(&sched->lock);
        return A840_PROMISE_SCHED_ERR_NOT_FOUND;
    }
    out_snapshot->found = true;
    out_snapshot->surface_id = hist->surface_id;
    out_snapshot->generation = hist->last_generation;
    out_snapshot->chunk_seqno = hist->last_chunk_seqno;
    out_snapshot->promise_token = hist->last_promise_token;
    out_snapshot->ring_id = hist->last_ring_id;
    out_snapshot->policy = hist->last_policy;
    out_snapshot->last_event = hist->last_event;
    out_snapshot->last_cancel_reason = hist->last_cancel_reason;
    out_snapshot->display_ready = hist->display_ready;
    pthread_mutex_unlock(&sched->lock);
    return A840_PROMISE_SCHED_OK;
}

A840EarlyPromiseStats a840_early_promise_get_stats(A840EarlyPromiseSched *sched)
{
    A840EarlyPromiseStats out;
    memset(&out, 0, sizeof(out));
    if (!sched || !sched->initialized) {
        return out;
    }
    pthread_mutex_lock(&sched->lock);
    out = sched->stats;
    pthread_mutex_unlock(&sched->lock);
    return out;
}

A840EarlyPromiseStatus a840_early_promise_fill_abi_v3(
    const A840EarlyPromiseEvent *ev,
    struct NativeAdrenoAbiWriterPromiseV3 *out_abi)
{
    if (!ev || !out_abi || ev->surface_id == 0u) {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    memset(out_abi, 0, sizeof(*out_abi));
    if (ev->kind == A840_PROMISE_EVENT_PROMISE_CANCELED) {
        out_abi->op = NativeAdrenoPromiseCancel;
    } else if (ev->kind == A840_PROMISE_EVENT_EARLY_SCHEDULED ||
               ev->kind == A840_PROMISE_EVENT_DEFERRED_ARMED ||
               ev->kind == A840_PROMISE_EVENT_RETIRED_READY) {
        out_abi->op = NativeAdrenoPromiseFulfill;
    } else {
        return A840_PROMISE_SCHED_ERR_INVALID_ARG;
    }
    out_abi->surface_count = 1u;
    out_abi->promise_token = ev->promise_token;
    out_abi->queue = (uint64_t)ev->ring_id;
    out_abi->fence = ev->chunk_seqno;
    out_abi->surface_ids[0] = ev->surface_id;
    return A840_PROMISE_SCHED_OK;
}
