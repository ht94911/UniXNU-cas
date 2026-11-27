/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_display_handoff.h"
#include <string.h>

#define A840_KMD_DISPLAY_MAGIC 1146111053u

static A840KmdStatus check_init(const A840KmdDisplayHandoff *dh)
{
    if (dh == NULL)
        return A840_KMD_INVALID_ARG;
    if (dh->initialized != A840_KMD_DISPLAY_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static void dh_lock(A840KmdDisplayHandoff *dh)
{
    if (dh->use_custom_sync) {
        dh->sync_ops.lock(dh->sync_ops.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_lock(&dh->lock);
#endif
    }
}

static void dh_unlock(A840KmdDisplayHandoff *dh)
{
    if (dh->use_custom_sync) {
        dh->sync_ops.unlock(dh->sync_ops.cookie);
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_mutex_unlock(&dh->lock);
#endif
    }
}

A840KmdStatus a840_kmd_display_handoff_init(
    A840KmdDisplayHandoff *dh,
    const A840KmdDisplayHandoffConfig *config)
{
    if (dh == NULL || config == NULL ||
        config->surface_mgr == NULL || config->promise_reg == NULL)
        return A840_KMD_INVALID_ARG;

    if (config->sync_ops != NULL) {
        if (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    }
#if defined(A840_KMD_CUSTOM_SYNC)
    else {
        return A840_KMD_INVALID_ARG;
    }
#endif

    memset(dh, 0, sizeof(*dh));
    dh->surface_mgr = config->surface_mgr;
    dh->promise_reg = config->promise_reg;
    dh->commit_fn = config->commit_fn;
    dh->commit_cookie = config->commit_cookie;

    if (config->sync_ops != NULL) {
        dh->sync_ops = *config->sync_ops;
        dh->use_custom_sync = 1u;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&dh->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#endif
        dh->use_custom_sync = 0u;
    }

    dh->initialized = A840_KMD_DISPLAY_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_display_handoff_destroy(A840KmdDisplayHandoff *dh)
{
    if (check_init(dh) != A840_KMD_OK)
        return;

    dh_lock(dh);
    dh->initialized = 0u;
    dh->commit_fn = NULL;
    dh->commit_cookie = NULL;
    dh_unlock(dh);

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!dh->use_custom_sync)
        (void)pthread_mutex_destroy(&dh->lock);
#endif
}

static A840KmdStatus query_surface_scanout(
    A840KmdSurfaceManager *surface_mgr,
    uint64_t consumer_client,
    uint64_t bo_handle,
    const A840KmdMdssCaps *mdss_caps,
    A840KmdSurfaceResult *out_res)
{
    struct NativeAdrenoAbiSurfaceMetaV3 get_req;
    memset(&get_req, 0, sizeof(get_req));
    get_req.bo_handle = bo_handle;
    get_req.op = 0u; /* GET */
    return a840_kmd_surface_meta(
        surface_mgr, consumer_client, &get_req, 1u, mdss_caps, out_res);
}

static void populate_commit_desc(
    uint64_t surface_identity,
    uint64_t bo_handle,
    uint32_t scanout_mode,
    uint32_t sync_mode,
    const A840KmdSurfaceResult *surf,
    const A840KmdSurfacePromiseSnapshot *snap,
    A840KmdMdssCommitDesc *out)
{
    memset(out, 0, sizeof(*out));
    out->surface_identity = surface_identity;
    out->bo_handle = bo_handle;
    out->writer_client = snap->writer_client;
    out->generation = snap->generation;
    out->promise_token = snap->promise_token;
    out->gpu_queue = snap->queue;
    out->gpu_fence = snap->fence;
    out->scanout_mode = scanout_mode;
    out->sync_mode = sync_mode;
    out->ubwc_enabled = surf->meta.ubwc_enabled;
    out->ubwc_version = surf->meta.ubwc_version;
    out->highest_bank_bit = surf->meta.highest_bank_bit;
    out->swizzle_levels = surf->meta.swizzle_levels;
    out->macrotile_mode = surf->meta.macrotile_mode;
    out->meta_pitch = surf->meta.meta_pitch;
    out->meta_offset = surf->meta.meta_offset;
}

A840KmdStatus a840_kmd_display_handoff_submit_frame(
    A840KmdDisplayHandoff *dh,
    const A840KmdDisplayFrameRequest *req,
    A840KmdDisplayHandoffResult *out_result)
{
    A840KmdStatus st = check_init(dh);
    A840KmdDisplayHandoffResult res;
    A840KmdSurfaceResult primary_surf;
    A840KmdSurfaceResult active_surf;
    A840KmdSurfacePromiseSnapshot snap;
    uint64_t active_bo_handle;
    uint64_t active_surface_id;
    uint32_t scanout_mode = (uint32_t)A840_DISPLAY_SCANOUT_NONE;
    uint32_t sync_mode = (uint32_t)A840_DISPLAY_SYNC_NONE;
    uint32_t timed_out = 0u;
    int is_linear_fallback = 0;
    A840KmdMdssCommitFn commit_fn = NULL;
    void *commit_cookie = NULL;

    if (st != A840_KMD_OK)
        return st;
    if (req == NULL || out_result == NULL)
        return A840_KMD_INVALID_ARG;

    memset(&res, 0, sizeof(res));
    memset(&primary_surf, 0, sizeof(primary_surf));
    memset(&active_surf, 0, sizeof(active_surf));
    memset(&snap, 0, sizeof(snap));

    if (req->reserved != 0u ||
        (req->flags & ~(uint32_t)A840_DISPLAY_REQ_KNOWN_MASK) != 0u ||
        req->consumer_client == 0u ||
        req->surface_handle == 0u ||
        req->surface_identity == 0u) {
        dh_lock(dh);
        dh->stats.rejected_handoffs++;
        dh_unlock(dh);
        res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
        *out_result = res;
        return A840_KMD_INVALID_ARG;
    }

    /* Step 1: Negotiate UBWC 6.0 HBB=16 vs linear scanout against current MDSS caps. */
    st = query_surface_scanout(
        dh->surface_mgr, req->consumer_client, req->surface_handle,
        req->mdss_caps, &primary_surf);
    if (st != A840_KMD_OK) {
        dh_lock(dh);
        dh->stats.rejected_handoffs++;
        dh_unlock(dh);
        res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
        *out_result = res;
        return st;
    }

    active_surf = primary_surf;
    active_bo_handle = req->surface_handle;
    active_surface_id = req->surface_identity;

    if (primary_surf.meta.ubwc_enabled) {
        if (primary_surf.scanout_eligible && !primary_surf.fallback_linear_required) {
            scanout_mode = (uint32_t)A840_DISPLAY_SCANOUT_UBWC6_DIRECT;
        } else {
            /* MDSS rejected direct UBWC 6.0 scanout; require separate linear resolve BO. */
            res.surface_result = primary_surf;
            res.fallback_linear_required = 1u;

            if ((req->flags & (uint32_t)A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK) == 0u ||
                req->linear_resolve_handle == 0u) {
                dh_lock(dh);
                dh->stats.rejected_handoffs++;
                dh_unlock(dh);
                res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE;
                *out_result = res;
                return A840_KMD_UNSUPPORTED;
            }

            if (req->linear_resolve_handle == req->surface_handle) {
                dh_lock(dh);
                dh->stats.rejected_handoffs++;
                dh_unlock(dh);
                res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
                *out_result = res;
                return A840_KMD_INVALID_ARG;
            }

            st = query_surface_scanout(
                dh->surface_mgr, req->consumer_client, req->linear_resolve_handle,
                req->mdss_caps, &active_surf);
            if (st != A840_KMD_OK) {
                dh_lock(dh);
                dh->stats.rejected_handoffs++;
                dh_unlock(dh);
                res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
                *out_result = res;
                return st;
            }

            /* Resolve fallback surface MUST actually be linear (ubwc_enabled == 0). */
            if (!active_surf.layout_valid || active_surf.meta.ubwc_enabled != 0u) {
                dh_lock(dh);
                dh->stats.rejected_handoffs++;
                dh_unlock(dh);
                res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE;
                res.surface_result = active_surf;
                *out_result = res;
                return A840_KMD_STATE_CONFLICT;
            }

            active_bo_handle = req->linear_resolve_handle;
            active_surface_id = (req->linear_resolve_identity != 0u) ?
                req->linear_resolve_identity : req->surface_identity;
            scanout_mode = (uint32_t)A840_DISPLAY_SCANOUT_LINEAR_FALLBACK;
            is_linear_fallback = 1;
        }
    } else {
        /* Primary surface is already registered as linear. */
        if (!primary_surf.layout_valid) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return A840_KMD_STATE_CONFLICT;
        }
        scanout_mode = (uint32_t)A840_DISPLAY_SCANOUT_LINEAR_DIRECT;
    }

    res.surface_result = active_surf;
    res.fallback_linear_required = primary_surf.fallback_linear_required;

    /* Step 2: Query or wait for WriterPromise state on the target surface identity. */
    if ((req->flags & (uint32_t)A840_DISPLAY_REQ_WAIT_FOR_PROMISE) != 0u) {
        st = a840_kmd_writer_promise_wait_surface(
            dh->promise_reg, active_surface_id, req->min_generation,
            req->timeout_ns, &snap, &timed_out);
        if (st != A840_KMD_OK) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return st;
        }
    } else {
        st = a840_kmd_writer_promise_query_surface(
            dh->promise_reg, active_surface_id, 0u, &snap);
        if (st != A840_KMD_OK) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return st;
        }
        if (snap.generation < req->min_generation)
            timed_out = 1u;
    }

    res.promise_snapshot = snap;
    res.promise_state = snap.state;
    res.promise_policy = snap.policy;
    res.cancel_reason = snap.cancel_reason;
    res.timed_out = timed_out;

    if (timed_out) {
        dh_lock(dh);
        dh->stats.timed_out_handoffs++;
        dh_unlock(dh);
        res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_TIMED_OUT;
        *out_result = res;
        return A840_KMD_OK;
    }

    if (snap.state == (uint32_t)A840_PROMISE_STATE_FAILED ||
        snap.state == (uint32_t)A840_PROMISE_STATE_CANCELED) {
        dh_lock(dh);
        dh->stats.canceled_on_fault++;
        dh_unlock(dh);
        res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_CANCELED_ON_FAULT;
        *out_result = res;
        return (snap.state == (uint32_t)A840_PROMISE_STATE_FAILED) ?
            A840_KMD_POISONED : A840_KMD_DISCONNECTED;
    }

    /* Enforce privilege boundary defensively on the display side as well:
     * - SCHEDULED_READY is ONLY valid for A840_CLIENT_ROLE_RENDER_SERVER.
     * - Non-compositor Apps MUST have reached RETIRED_READY (fence_retired == 1).
     */
    if (snap.state == (uint32_t)A840_PROMISE_STATE_SCHEDULED_READY) {
        if (snap.client_role != (uint32_t)A840_CLIENT_ROLE_RENDER_SERVER ||
            snap.fence == 0u) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return A840_KMD_PERMISSION_DENIED;
        }
        sync_mode = (uint32_t)A840_DISPLAY_SYNC_EARLY_GPU_FENCE;
    } else if (snap.state == (uint32_t)A840_PROMISE_STATE_RETIRED_READY) {
        if (!snap.fence_retired) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return A840_KMD_STATE_CONFLICT;
        }
        sync_mode = (uint32_t)A840_DISPLAY_SYNC_RETIRED_READY;
    } else {
        /* Still PENDING or DEFERRED_WAITING_FENCE (non-blocking query). */
        dh_lock(dh);
        dh->stats.timed_out_handoffs++;
        dh_unlock(dh);
        res.timed_out = 1u;
        res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_TIMED_OUT;
        *out_result = res;
        return A840_KMD_OK;
    }

    populate_commit_desc(
        active_surface_id, active_bo_handle, scanout_mode, sync_mode,
        &active_surf, &snap, &res.commit);

    dh_lock(dh);
    commit_fn = dh->commit_fn;
    commit_cookie = dh->commit_cookie;
    dh_unlock(dh);

    /* Invoke MDSS hardware commit callback strictly outside all KMD locks. */
    if (commit_fn != NULL) {
        st = commit_fn(commit_cookie, &res.commit);
        if (st != A840_KMD_OK) {
            dh_lock(dh);
            dh->stats.rejected_handoffs++;
            dh_unlock(dh);
            res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_REJECTED;
            *out_result = res;
            return st;
        }
    }

    /* Record deterministic Phase 5 diagnostic counters under handoff mutex. */
    dh_lock(dh);
    if (sync_mode == (uint32_t)A840_DISPLAY_SYNC_EARLY_GPU_FENCE) {
        dh->stats.early_scheduled_handoffs++;
    } else {
        dh->stats.fence_satisfied_handoffs++;
        if (snap.policy == (uint32_t)A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED)
            dh->stats.deferred_app_handoffs++;
    }

    if (is_linear_fallback) {
        dh->stats.linear_fallback_handoffs++;
    } else if (scanout_mode == (uint32_t)A840_DISPLAY_SCANOUT_UBWC6_DIRECT) {
        dh->stats.ubwc_direct_handoffs++;
    } else if (scanout_mode == (uint32_t)A840_DISPLAY_SCANOUT_LINEAR_DIRECT) {
        dh->stats.linear_direct_handoffs++;
    }
    dh_unlock(dh);

    res.scanout_mode = scanout_mode;
    res.sync_mode = sync_mode;
    res.outcome = (uint32_t)A840_DISPLAY_HANDOFF_COMMITTED;
    *out_result = res;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_display_handoff_get_stats(
    A840KmdDisplayHandoff *dh,
    A840KmdDisplayHandoffStats *out_stats)
{
    A840KmdStatus st = check_init(dh);
    if (st != A840_KMD_OK)
        return st;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;
    dh_lock(dh);
    *out_stats = dh->stats;
    dh_unlock(dh);
    return A840_KMD_OK;
}
