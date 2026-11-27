/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_preempt_ring.h"

#include <string.h>

#define A840_PREEMPT_INIT_MAGIC 1347568980u /* 'PRMT' decimal; avoids hex heuristic */

typedef struct PreemptDoorbellAction {
    uint32_t emit;
    uint32_t ring_id;
    uint32_t wptr;
    uint32_t emit_latency;
    uint64_t seqno;
    uint64_t latency_ns;
    uint64_t now_ns;
} PreemptDoorbellAction;

typedef struct PreemptTriggerAction {
    uint32_t emit;
    uint32_t from_ring;
    uint32_t to_ring;
    uint32_t preempt_level;
    uint64_t preempt_record_va;
    uint64_t preempt_record_bytes;
} PreemptTriggerAction;

typedef struct PreemptPiEvent {
    uint32_t ring_id;
    uint32_t boosted;
    uint32_t boosted_by_ring;
    uint32_t reserved;
    uint64_t boost_until_seqno;
    uint64_t now_ns;
} PreemptPiEvent;

typedef struct PreemptTimeoutAction {
    uint32_t emit;
    uint32_t from_ring;
    uint32_t to_ring;
    uint32_t reserved;
    uint64_t elapsed_ns;
} PreemptTimeoutAction;

typedef struct PreemptLatencyAction {
    uint32_t emit;
    uint32_t kind;
    uint32_t ring_id;
    uint32_t reserved;
    uint64_t latency_ns;
    uint64_t now_ns;
} PreemptLatencyAction;

typedef struct PreemptActionBatch {
    PreemptDoorbellAction doorbell;
    PreemptTriggerAction trigger;
    PreemptTimeoutAction timeout;
    PreemptLatencyAction latency;
    uint32_t retire_emit;
    uint32_t retire_ring;
    uint64_t retire_seqno;
    uint64_t retire_now_ns;
    uint32_t pi_count;
    PreemptPiEvent pi_events[A840_RING_COUNT];
    A840PreemptHwOps ops;
    uint32_t has_ops;
} PreemptActionBatch;

static void sched_lock(A840KmdPreemptScheduler *sched)
{
    if (sched->has_custom_sync) {
        sched->sync_ops.lock(sched->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&sched->lock);
#endif
}

static void sched_unlock(A840KmdPreemptScheduler *sched)
{
    if (sched->has_custom_sync) {
        sched->sync_ops.unlock(sched->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&sched->lock);
#endif
}

static A840KmdStatus check_init(const A840KmdPreemptScheduler *sched)
{
    if (sched == NULL)
        return A840_KMD_INVALID_ARG;
    if (sched->initialized != A840_PREEMPT_INIT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static int is_power_of_two_u32(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static int valid_preempt_level(uint32_t level)
{
    return level == A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY ||
           level == A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY;
}

static void init_action_batch(const A840KmdPreemptScheduler *sched, PreemptActionBatch *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->has_ops = sched->has_hw_ops;
    if (sched->has_hw_ops)
        batch->ops = sched->hw_ops;
}

static void record_pi_event(PreemptActionBatch *batch, uint32_t ring_id,
    uint32_t boosted, uint32_t boosted_by_ring, uint64_t boost_until_seqno, uint64_t now_ns)
{
    if (batch->pi_count < A840_RING_COUNT) {
        PreemptPiEvent *ev = &batch->pi_events[batch->pi_count++];
        ev->ring_id = ring_id;
        ev->boosted = boosted;
        ev->boosted_by_ring = boosted_by_ring;
        ev->boost_until_seqno = boost_until_seqno;
        ev->now_ns = now_ns;
    }
}

/* Dispatch all collected hardware/observer callbacks strictly outside lock. */
static A840KmdStatus dispatch_actions(const PreemptActionBatch *batch)
{
    A840KmdStatus first_err = A840_KMD_OK;

    if (!batch->has_ops)
        return A840_KMD_OK;

    if (batch->retire_emit && batch->ops.on_retire != NULL) {
        batch->ops.on_retire(
            batch->ops.cookie,
            batch->retire_ring,
            batch->retire_seqno,
            batch->retire_now_ns);
    }

    for (uint32_t i = 0u; i < batch->pi_count; ++i) {
        if (batch->ops.on_pi_boost_change != NULL) {
            const PreemptPiEvent *ev = &batch->pi_events[i];
            batch->ops.on_pi_boost_change(
                batch->ops.cookie,
                ev->ring_id,
                ev->boosted,
                ev->boosted_by_ring,
                ev->boost_until_seqno,
                ev->now_ns);
        }
    }

    if (batch->timeout.emit && batch->ops.on_timeout_recovery != NULL) {
        batch->ops.on_timeout_recovery(
            batch->ops.cookie,
            batch->timeout.from_ring,
            batch->timeout.to_ring,
            batch->timeout.elapsed_ns);
    }

    if (batch->doorbell.emit && batch->ops.write_doorbell != NULL) {
        A840KmdStatus st = batch->ops.write_doorbell(
            batch->ops.cookie,
            batch->doorbell.ring_id,
            batch->doorbell.wptr,
            batch->doorbell.seqno,
            A840_REG_CP_RB_WPTR);
        if (st != A840_KMD_OK && first_err == A840_KMD_OK)
            first_err = st;
    }

    if (batch->doorbell.emit_latency && batch->ops.on_latency_sample != NULL) {
        batch->ops.on_latency_sample(
            batch->ops.cookie,
            A840_PREEMPT_LAT_ENQUEUE_TO_DOORBELL,
            batch->doorbell.ring_id,
            batch->doorbell.latency_ns,
            batch->doorbell.now_ns);
    }

    if (batch->latency.emit && batch->ops.on_latency_sample != NULL) {
        batch->ops.on_latency_sample(
            batch->ops.cookie,
            batch->latency.kind,
            batch->latency.ring_id,
            batch->latency.latency_ns,
            batch->latency.now_ns);
    }

    if (batch->trigger.emit && batch->ops.trigger_preempt != NULL) {
        A840KmdStatus st = batch->ops.trigger_preempt(
            batch->ops.cookie,
            batch->trigger.from_ring,
            batch->trigger.to_ring,
            batch->trigger.preempt_level,
            batch->trigger.preempt_record_va,
            batch->trigger.preempt_record_bytes,
            A840_REG_CP_CONTEXT_SWITCH_CNTL);
        if (st != A840_KMD_OK && first_err == A840_KMD_OK)
            first_err = st;
    }

    return first_err;
}

/* Recompute effective priorities across all 4 rings (supports transitive PI). */
static void recompute_effective_priorities_locked(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns,
    PreemptActionBatch *batch)
{
    uint32_t old_boosted[A840_RING_COUNT];
    uint32_t old_by[A840_RING_COUNT];
    uint64_t old_until[A840_RING_COUNT];

    uint32_t eff[A840_RING_COUNT];
    uint32_t by[A840_RING_COUNT];
    uint64_t until[A840_RING_COUNT];

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        old_boosted[r] = sched->rings[r].pi_boost_active;
        old_by[r] = sched->rings[r].boosted_by_ring;
        old_until[r] = sched->rings[r].boost_until_seqno;

        eff[r] = r;
        by[r] = A840_RING_NONE;
        until[r] = 0u;
    }

    /* Clear satisfied dep_max_seqno edges first. */
    for (uint32_t src = 0u; src < A840_RING_COUNT; ++src) {
        for (uint32_t dst = 0u; dst < A840_RING_COUNT; ++dst) {
            uint64_t need = sched->rings[src].dep_max_seqno[dst];
            if (need != 0u && sched->rings[dst].completed_seqno >= need) {
                sched->rings[src].dep_max_seqno[dst] = 0u;
            }
        }
    }

    /* Propagate effective priority along active dependency edges (max 4 hops). */
    for (uint32_t iter = 0u; iter < A840_RING_COUNT; ++iter) {
        for (uint32_t src = 0u; src < A840_RING_COUNT; ++src) {
            if (sched->rings[src].health_state != A840_RING_STATE_ACTIVE)
                continue;
            if (sched->rings[src].inflight_count == 0u)
                continue;
            for (uint32_t dst = 0u; dst < A840_RING_COUNT; ++dst) {
                uint64_t need = sched->rings[src].dep_max_seqno[dst];
                if (need == 0u || sched->rings[dst].health_state != A840_RING_STATE_ACTIVE)
                    continue;
                if (sched->rings[dst].completed_seqno >= need)
                    continue;
                if (eff[src] < eff[dst]) {
                    eff[dst] = eff[src];
                    by[dst] = (by[src] != A840_RING_NONE) ? by[src] : src;
                }
                if (eff[src] < dst && need > until[dst]) {
                    until[dst] = need;
                }
            }
        }
    }

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        uint32_t new_boosted = (eff[r] < r) ? 1u : 0u;
        uint32_t new_by = new_boosted ? by[r] : A840_RING_NONE;
        uint64_t new_until = new_boosted ? until[r] : 0u;

        sched->rings[r].effective_priority = eff[r];
        sched->rings[r].pi_boost_active = new_boosted;
        sched->rings[r].boosted_by_ring = new_by;
        sched->rings[r].boost_until_seqno = new_until;

        if (new_boosted != old_boosted[r] ||
            (new_boosted && (new_by != old_by[r] || new_until != old_until[r]))) {
            if (new_boosted && !old_boosted[r])
                ++sched->total_pi_boosts;
            record_pi_event(batch, r, new_boosted, new_by, new_until, now_ns);
        }
    }
}

/* Check if adding edge (from_ring -> dep_ring) creates a cycle in active deps. */
static int would_create_dep_cycle_locked(
    const A840KmdPreemptScheduler *sched,
    uint32_t from_ring,
    uint32_t dep_ring)
{
    if (from_ring == dep_ring)
        return 1;

    uint32_t visited[A840_RING_COUNT];
    uint32_t queue[A840_RING_COUNT];
    uint32_t qh = 0u, qt = 0u;
    memset(visited, 0, sizeof(visited));

    visited[dep_ring] = 1u;
    queue[qt++] = dep_ring;

    while (qh < qt) {
        uint32_t cur = queue[qh++];
        for (uint32_t nxt = 0u; nxt < A840_RING_COUNT; ++nxt) {
            uint64_t need = sched->rings[cur].dep_max_seqno[nxt];
            if (need == 0u || sched->rings[nxt].completed_seqno >= need)
                continue;
            if (nxt == from_ring)
                return 1;
            if (!visited[nxt]) {
                visited[nxt] = 1u;
                queue[qt++] = nxt;
            }
        }
    }
    return 0;
}

static int ring_is_runnable_locked(const A840KmdPreemptScheduler *sched, uint32_t r)
{
    const A840PreemptRingState *ring = &sched->rings[r];
    if (ring->health_state != A840_RING_STATE_ACTIVE)
        return 0;
    if (ring->inflight_count == 0u)
        return 0;

    /* If the ring has an unsatisfied dependency on another active ring, that
     * dependency ring should run first (already PI-boosted to at least our
     * effective priority).
     */
    for (uint32_t d = 0u; d < A840_RING_COUNT; ++d) {
        uint64_t need = ring->dep_max_seqno[d];
        if (need != 0u && sched->rings[d].completed_seqno < need &&
            sched->rings[d].health_state == A840_RING_STATE_ACTIVE &&
            sched->rings[d].inflight_count > 0u) {
            return 0;
        }
    }
    return 1;
}

/* Pick highest-priority runnable ring. Lower effective_priority wins; on tie,
 * a PI-boosted ring that unblocks a higher ring beats the blocked ring, and
 * otherwise lower raw ring_id wins.
 */
static uint32_t pick_best_runnable_ring_locked(const A840KmdPreemptScheduler *sched)
{
    uint32_t best = A840_RING_NONE;
    uint32_t best_eff = A840_RING_COUNT;

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        if (!ring_is_runnable_locked(sched, r))
            continue;
        uint32_t eff = sched->rings[r].effective_priority;
        if (best == A840_RING_NONE || eff < best_eff ||
            (eff == best_eff && r < best)) {
            best = r;
            best_eff = eff;
        }
    }
    return best;
}

/* Check whether ring r has any inflight submission with urgent 120Hz deadline. */
static int ring_has_urgent_deadline_locked(
    const A840KmdPreemptScheduler *sched,
    uint32_t r,
    uint64_t now_ns)
{
    const A840PreemptRingState *ring = &sched->rings[r];
    for (uint32_t i = 0u; i < ring->inflight_count; ++i) {
        uint32_t idx = (ring->rptr + i) & (ring->capacity - 1u);
        const A840PreemptSlot *slot = &ring->slots[idx];
        if (!slot->valid || slot->frame_deadline_ns == 0u)
            continue;
        if (slot->frame_deadline_ns <= now_ns)
            return 1;
        uint64_t remaining = slot->frame_deadline_ns - now_ns;
        if (remaining <= sched->deadline_urgent_ns ||
            remaining <= sched->frame_budget_ns) {
            return 1;
        }
    }
    return 0;
}

static void prepare_doorbell_locked(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t now_ns,
    PreemptActionBatch *batch)
{
    A840PreemptRingState *ring = &sched->rings[ring_id];
    if (ring->inflight_count == 0u)
        return;

    /* Find newest slot to measure enqueue->doorbell latency if not yet sent. */
    uint32_t newest_idx = (ring->wptr + ring->capacity - 1u) & (ring->capacity - 1u);
    A840PreemptSlot *newest = &ring->slots[newest_idx];

    if (ring->last_doorbell_seqno < ring->submitted_seqno) {
        ring->last_doorbell_wptr = ring->wptr;
        ring->last_doorbell_seqno = ring->submitted_seqno;
        ++sched->total_doorbells;

        batch->doorbell.emit = 1u;
        batch->doorbell.ring_id = ring_id;
        batch->doorbell.wptr = ring->wptr;
        batch->doorbell.seqno = ring->submitted_seqno;

        if (newest->valid && !newest->doorbell_sent) {
            uint64_t lat = (now_ns >= newest->enqueue_ns) ? (now_ns - newest->enqueue_ns) : 0u;
            batch->doorbell.emit_latency = 1u;
            batch->doorbell.latency_ns = lat;
            batch->doorbell.now_ns = now_ns;
        }
        /* Mark all inflight slots as doorbell-submitted. */
        for (uint32_t i = 0u; i < ring->inflight_count; ++i) {
            uint32_t idx = (ring->rptr + i) & (ring->capacity - 1u);
            ring->slots[idx].doorbell_sent = 1u;
        }
    }
}

/* Evaluate whether hardware should activate a ring or initiate preemption. */
static void evaluate_schedule_locked(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns,
    PreemptActionBatch *batch,
    uint32_t *out_preempt_triggered,
    uint32_t *out_deferred_min_slice,
    uint32_t *out_deadline_exempted)
{
    if (out_preempt_triggered)
        *out_preempt_triggered = 0u;
    if (out_deferred_min_slice)
        *out_deferred_min_slice = 0u;
    if (out_deadline_exempted)
        *out_deadline_exempted = 0u;

    /* If a hardware switch is already in flight or awaiting timeout recovery,
     * update target_ring if an even higher-priority ring becomes runnable
     * while still TRIGGERED, but never issue a second overlapping trigger.
     */
    if (sched->state == A840_PREEMPT_TRIGGERED ||
        sched->state == A840_PREEMPT_IN_PROGRESS ||
        sched->state == A840_PREEMPT_TIMEOUT_RECOVERY) {
        uint32_t best = pick_best_runnable_ring_locked(sched);
        if (sched->state == A840_PREEMPT_TRIGGERED &&
            best != A840_RING_NONE &&
            sched->target_ring < A840_RING_COUNT &&
            sched->rings[best].effective_priority < sched->rings[sched->target_ring].effective_priority) {
            sched->target_ring = best;
        }
        return;
    }

    /* Auto-settle COMPLETE -> IDLE when scheduling new work. */
    if (sched->state == A840_PREEMPT_COMPLETE) {
        sched->state = A840_PREEMPT_IDLE;
        sched->from_ring = A840_RING_NONE;
        sched->target_ring = A840_RING_NONE;
    }

    uint32_t best = pick_best_runnable_ring_locked(sched);
    if (best == A840_RING_NONE) {
        sched->deferred_preempt_pending = 0u;
        return;
    }

    /* Case 1: Hardware is idle -> activate best ring directly and ring doorbell. */
    if (sched->active_ring == A840_RING_NONE) {
        sched->active_ring = best;
        sched->rings[best].active_since_ns = now_ns;
        sched->deferred_preempt_pending = 0u;
        prepare_doorbell_locked(sched, best, now_ns, batch);
        return;
    }

    /* Case 2: Best runnable ring is already active -> ring its doorbell if new work. */
    if (best == sched->active_ring) {
        sched->deferred_preempt_pending = 0u;
        prepare_doorbell_locked(sched, best, now_ns, batch);
        return;
    }

    /* Case 3: Active ring is no longer healthy or has no remaining inflight work
     * -> switch immediately via preemption/context switch or direct handoff.
     */
    A840PreemptRingState *cur = &sched->rings[sched->active_ring];
    A840PreemptRingState *cand = &sched->rings[best];

    if (cur->health_state != A840_RING_STATE_ACTIVE || cur->inflight_count == 0u) {
        sched->active_ring = best;
        cand->active_since_ns = now_ns;
        sched->deferred_preempt_pending = 0u;
        prepare_doorbell_locked(sched, best, now_ns, batch);
        return;
    }

    /* Case 4: Active ring is still busy; only preempt if candidate has strictly
     * higher effective priority (smaller numeric value).
     */
    if (cand->effective_priority >= cur->effective_priority) {
        sched->deferred_preempt_pending = 0u;
        return;
    }

    /* Anti-thrash min_slice_ns check vs RING_0_COMPOSITOR 120Hz exemption. */
    uint64_t elapsed = (now_ns >= cur->active_since_ns) ? (now_ns - cur->active_since_ns) : 0u;
    int is_compositor_eff = (best == A840_RING_0_COMPOSITOR ||
                             cand->effective_priority == A840_RING_0_COMPOSITOR);
    int urgent_deadline = ring_has_urgent_deadline_locked(sched, best, now_ns);

    if (sched->min_slice_ns > 0u && elapsed < sched->min_slice_ns) {
        if (is_compositor_eff || urgent_deadline) {
            ++sched->total_deadline_exemptions;
            if (out_deadline_exempted)
                *out_deadline_exempted = 1u;
        } else {
            sched->deferred_preempt_pending = 1u;
            ++sched->total_min_slice_deferrals;
            if (out_deferred_min_slice)
                *out_deferred_min_slice = 1u;
            return;
        }
    }

    /* Trigger hardware preemption from active_ring -> best. */
    sched->state = A840_PREEMPT_TRIGGERED;
    sched->from_ring = sched->active_ring;
    sched->target_ring = best;
    sched->active_preempt_level = cur->preempt_level;
    sched->trigger_ns = now_ns;
    sched->deferred_preempt_pending = 0u;
    ++sched->preempt_generation;
    ++sched->total_preempt_triggers;

    batch->trigger.emit = 1u;
    batch->trigger.from_ring = sched->from_ring;
    batch->trigger.to_ring = sched->target_ring;
    batch->trigger.preempt_level = sched->active_preempt_level;
    batch->trigger.preempt_record_va = cand->preempt_record_va;
    batch->trigger.preempt_record_bytes = cand->preempt_record_bytes;

    if (out_preempt_triggered)
        *out_preempt_triggered = 1u;
}

A840KmdStatus a840_kmd_preempt_get_hw_reg_map(A840PreemptHwRegMap *out_map)
{
    if (out_map == NULL)
        return A840_KMD_INVALID_ARG;

    memset(out_map, 0, sizeof(*out_map));
    out_map->rb_base_lo = A840_REG_CP_RB_BASE;
    out_map->rb_base_hi = A840_REG_CP_RB_BASE_HI;
    out_map->rb_cntl = A840_REG_CP_RB_CNTL;
    out_map->rb_rptr = A840_REG_CP_RB_RPTR;
    out_map->rb_wptr = A840_REG_CP_RB_WPTR;
    out_map->context_switch_cntl = A840_REG_CP_CONTEXT_SWITCH_CNTL;
    out_map->context_switch_level_status = A840_REG_CP_CONTEXT_SWITCH_LEVEL_STATUS;
    out_map->smmu_info_lo = A840_REG_CP_CONTEXT_SWITCH_SMMU_INFO;
    out_map->smmu_info_hi = A840_REG_CP_CONTEXT_SWITCH_SMMU_INFO_HI;
    out_map->priv_non_secure_restore_lo = A840_REG_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR;
    out_map->priv_non_secure_restore_hi = A840_REG_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR_HI;
    out_map->non_priv_restore_lo = A840_REG_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR;
    out_map->non_priv_restore_hi = A840_REG_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR_HI;
    out_map->preempt_threshold = A840_REG_CP_PREEMPT_THRESHOLD;
    out_map->pc_gfx_preempt_mode = A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE;
    out_map->sp_gfx_preempt_safe_mode = A840_REG_SP_CONTEXT_SWITCH_GFX_PREEMPTION_SAFE_MODE;
    out_map->rbbm_int_0_status = A840_REG_RBBM_INT_0_STATUS;
    out_map->rbbm_int_0_mask = A840_REG_RBBM_INT_0_MASK;
    out_map->rbbm_int_clear_cmd = A840_REG_RBBM_INT_CLEAR_CMD;
    out_map->always_on_counter_lo = A840_REG_CP_ALWAYS_ON_COUNTER;
    out_map->always_on_counter_hi = A840_REG_CP_ALWAYS_ON_COUNTER_HI;
    out_map->cp_in_preempt_opcode = A840_CP_IN_PREEMPT;
    out_map->abi_preempt_cap_flag = NativeAdrenoParamsV3PreemptRings;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_priority_to_ring(
    uint32_t priority,
    uint32_t *out_ring_id)
{
    if (out_ring_id == NULL)
        return A840_KMD_INVALID_ARG;

    switch (priority) {
    case A840_PRIORITY_COMPOSITOR:
        *out_ring_id = A840_RING_0_COMPOSITOR;
        return A840_KMD_OK;
    case A840_PRIORITY_HIGH:
        *out_ring_id = A840_RING_1_INTERACTIVE;
        return A840_KMD_OK;
    case A840_PRIORITY_NORMAL:
        *out_ring_id = A840_RING_2_CLIENT_3D;
        return A840_KMD_OK;
    case A840_PRIORITY_BACKGROUND:
        *out_ring_id = A840_RING_3_BG_COMPUTE;
        return A840_KMD_OK;
    default:
        return A840_KMD_RANGE;
    }
}

A840KmdStatus a840_kmd_preempt_ring_to_priority(
    uint32_t ring_id,
    uint32_t *out_priority)
{
    if (out_priority == NULL)
        return A840_KMD_INVALID_ARG;

    switch (ring_id) {
    case A840_RING_0_COMPOSITOR:
        *out_priority = A840_PRIORITY_COMPOSITOR;
        return A840_KMD_OK;
    case A840_RING_1_INTERACTIVE:
        *out_priority = A840_PRIORITY_HIGH;
        return A840_KMD_OK;
    case A840_RING_2_CLIENT_3D:
        *out_priority = A840_PRIORITY_NORMAL;
        return A840_KMD_OK;
    case A840_RING_3_BG_COMPUTE:
        *out_priority = A840_PRIORITY_BACKGROUND;
        return A840_KMD_OK;
    default:
        return A840_KMD_RANGE;
    }
}

A840KmdStatus a840_kmd_preempt_validate_record(
    uint64_t record_va,
    uint64_t record_bytes)
{
    if (record_va == 0u || record_bytes == 0u)
        return A840_KMD_INVALID_ARG;
    if ((record_va & (A840_PREEMPT_RECORD_ALIGN_BYTES - 1u)) != 0u)
        return A840_KMD_ALIGNMENT;
    if ((record_bytes & (A840_PREEMPT_RECORD_ALIGN_BYTES - 1u)) != 0u)
        return A840_KMD_ALIGNMENT;
    if (record_bytes < A840_PREEMPT_RECORD_MIN_BYTES)
        return A840_KMD_RANGE;
    if (record_va > A840_PREEMPT_CANONICAL_VA_MAX ||
        record_bytes > (A840_PREEMPT_CANONICAL_VA_MAX - record_va + 1u))
        return A840_KMD_RANGE;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_init(
    A840KmdPreemptScheduler *sched,
    const A840PreemptConfig *config)
{
    if (sched == NULL || config == NULL)
        return A840_KMD_INVALID_ARG;

    if (config->sync_ops != NULL) {
        if (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    }
#if defined(A840_KMD_CUSTOM_SYNC)
    if (config->sync_ops == NULL)
        return A840_KMD_INVALID_ARG;
#endif

    /* Validate all 4 ring configs before mutating sched. */
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        const A840PreemptRingInit *ri = &config->rings[r];
        A840KmdStatus st = a840_kmd_preempt_validate_record(
            ri->preempt_record_va, ri->preempt_record_bytes);
        if (st != A840_KMD_OK)
            return st;

        if (ri->rb_base_va != 0u) {
            if ((ri->rb_base_va & (A840_PREEMPT_RECORD_ALIGN_BYTES - 1u)) != 0u)
                return A840_KMD_ALIGNMENT;
            if (ri->rb_base_va > A840_PREEMPT_CANONICAL_VA_MAX)
                return A840_KMD_RANGE;
        }

        if (ri->slot_capacity != 0u) {
            if (ri->slot_capacity > A840_PREEMPT_RING_MAX_SLOTS ||
                !is_power_of_two_u32(ri->slot_capacity))
                return A840_KMD_RANGE;
        }

        if (ri->preempt_level != 0u && !valid_preempt_level(ri->preempt_level))
            return A840_KMD_INVALID_ARG;

        /* Reject overlapping preemption records between distinct rings. */
        uint64_t a0 = ri->preempt_record_va;
        uint64_t a1 = a0 + ri->preempt_record_bytes;
        for (uint32_t prev = 0u; prev < r; ++prev) {
            uint64_t b0 = config->rings[prev].preempt_record_va;
            uint64_t b1 = b0 + config->rings[prev].preempt_record_bytes;
            if (a0 < b1 && b0 < a1)
                return A840_KMD_STATE_CONFLICT;
        }
    }

    memset(sched, 0, sizeof(*sched));
    sched->state = A840_PREEMPT_IDLE;
    sched->active_ring = A840_RING_NONE;
    sched->target_ring = A840_RING_NONE;
    sched->from_ring = A840_RING_NONE;
    sched->active_preempt_level = A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;

    sched->min_slice_ns = (config->min_slice_ns == 0u && !config->allow_zero_min_slice)
                              ? A840_PREEMPT_DEFAULT_MIN_SLICE_NS
                              : config->min_slice_ns;
    sched->preempt_timeout_ns = (config->preempt_timeout_ns == 0u)
                                    ? A840_PREEMPT_DEFAULT_TIMEOUT_NS
                                    : config->preempt_timeout_ns;
    sched->frame_budget_ns = (config->frame_budget_ns == 0u)
                                 ? A840_PREEMPT_FRAME_BUDGET_120HZ_NS
                                 : config->frame_budget_ns;
    sched->deadline_urgent_ns = (config->deadline_urgent_ns == 0u)
                                    ? A840_PREEMPT_DEFAULT_URGENT_BUDGET_NS
                                    : config->deadline_urgent_ns;

    if (config->sync_ops != NULL) {
        sched->has_custom_sync = 1u;
        sched->sync_ops = *config->sync_ops;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&sched->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#endif
    }

    if (config->hw_ops != NULL) {
        sched->has_hw_ops = 1u;
        sched->hw_ops = *config->hw_ops;
    }

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        const A840PreemptRingInit *ri = &config->rings[r];
        A840PreemptRingState *rs = &sched->rings[r];
        rs->ring_id = r;
        rs->health_state = A840_RING_STATE_ACTIVE;
        rs->capacity = (ri->slot_capacity != 0u) ? ri->slot_capacity : A840_PREEMPT_RING_MAX_SLOTS;
        rs->preempt_level = (ri->preempt_level != 0u)
                                ? ri->preempt_level
                                : A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
        rs->effective_priority = r;
        rs->boosted_by_ring = A840_RING_NONE;
        rs->preempt_record_va = ri->preempt_record_va;
        rs->preempt_record_bytes = ri->preempt_record_bytes;
        rs->rb_base_va = ri->rb_base_va;
    }

    sched->initialized = A840_PREEMPT_INIT_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_preempt_destroy(A840KmdPreemptScheduler *sched)
{
    if (sched == NULL || sched->initialized != A840_PREEMPT_INIT_MAGIC)
        return;

    sched->initialized = 0u;
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!sched->has_custom_sync)
        (void)pthread_mutex_destroy(&sched->lock);
#endif
    memset(sched, 0, sizeof(*sched));
}

A840KmdStatus a840_kmd_preempt_set_ring_level(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint32_t preempt_level)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (!valid_preempt_level(preempt_level))
        return A840_KMD_INVALID_ARG;

    sched_lock(sched);
    sched->rings[ring_id].preempt_level = preempt_level;
    sched_unlock(sched);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_submit(
    A840KmdPreemptScheduler *sched,
    const A840PreemptSubmitReq *req,
    A840PreemptSubmitResult *out_res)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (req == NULL)
        return A840_KMD_INVALID_ARG;
    if (req->ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (req->ib_va != 0u && ((req->ib_va & 3u) != 0u || req->ib_va > A840_PREEMPT_CANONICAL_VA_MAX))
        return A840_KMD_ALIGNMENT;
    if (req->has_dependency != 0u) {
        if (req->dep_ring >= A840_RING_COUNT)
            return A840_KMD_RANGE;
        if (req->dep_seqno == 0u)
            return A840_KMD_INVALID_ARG;
    }

    PreemptActionBatch batch;
    A840PreemptSubmitResult res;
    memset(&res, 0, sizeof(res));

    sched_lock(sched);
    init_action_batch(sched, &batch);

    A840PreemptRingState *ring = &sched->rings[req->ring_id];
    if (ring->health_state != A840_RING_STATE_ACTIVE) {
        sched_unlock(sched);
        return A840_KMD_POISONED;
    }
    if (ring->inflight_count >= ring->capacity) {
        sched_unlock(sched);
        return A840_KMD_CAPACITY;
    }

    if (req->has_dependency != 0u) {
        if (would_create_dep_cycle_locked(sched, req->ring_id, req->dep_ring)) {
            ++sched->total_cycle_rejects;
            sched_unlock(sched);
            return A840_KMD_STATE_CONFLICT;
        }
        /* Reject dependency on a faulted/resetting ring unless already completed. */
        if (sched->rings[req->dep_ring].completed_seqno < req->dep_seqno &&
            sched->rings[req->dep_ring].health_state != A840_RING_STATE_ACTIVE) {
            sched_unlock(sched);
            return A840_KMD_POISONED;
        }
    }

    /* Enqueue slot into ring circular buffer. */
    uint64_t seqno = ++ring->submitted_seqno;
    uint32_t slot_idx = ring->wptr;
    A840PreemptSlot *slot = &ring->slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->seqno = seqno;
    slot->ib_va = req->ib_va;
    slot->ib_dwords = req->ib_dwords;
    slot->enqueue_ns = req->enqueue_ns;
    slot->frame_deadline_ns = req->frame_deadline_ns;
    slot->has_dep = req->has_dependency ? 1u : 0u;
    slot->dep_ring = req->has_dependency ? req->dep_ring : A840_RING_NONE;
    slot->dep_seqno = req->has_dependency ? req->dep_seqno : 0u;
    slot->valid = 1u;

    ring->wptr = (ring->wptr + 1u) & (ring->capacity - 1u);
    ++ring->inflight_count;
    ++sched->total_submits;

    if (req->has_dependency != 0u &&
        sched->rings[req->dep_ring].completed_seqno < req->dep_seqno) {
        if (req->dep_seqno > ring->dep_max_seqno[req->dep_ring])
            ring->dep_max_seqno[req->dep_ring] = req->dep_seqno;
    }

    uint32_t prev_pi_total = (uint32_t)sched->total_pi_boosts;
    recompute_effective_priorities_locked(sched, req->enqueue_ns, &batch);
    if (req->has_dependency != 0u &&
        sched->rings[req->dep_ring].pi_boost_active != 0u) {
        res.pi_boost_applied = 1u;
    } else if ((uint32_t)sched->total_pi_boosts != prev_pi_total) {
        res.pi_boost_applied = 1u;
    }

    uint32_t preempt_triggered = 0u;
    uint32_t deferred_min_slice = 0u;
    uint32_t deadline_exempted = 0u;
    evaluate_schedule_locked(sched, req->enqueue_ns, &batch,
        &preempt_triggered, &deferred_min_slice, &deadline_exempted);

    res.seqno = seqno;
    res.wptr = ring->wptr;
    res.doorbell_emitted = batch.doorbell.emit;
    res.preempt_triggered = preempt_triggered;
    res.preempt_deferred_min_slice = deferred_min_slice;
    res.deadline_exempted = deadline_exempted;

    sched_unlock(sched);

    if (out_res != NULL)
        *out_res = res;

    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_submit_priority(
    A840KmdPreemptScheduler *sched,
    uint32_t priority,
    const A840PreemptSubmitReq *req_template,
    A840PreemptSubmitResult *out_res)
{
    if (req_template == NULL)
        return A840_KMD_INVALID_ARG;
    uint32_t ring_id = A840_RING_NONE;
    A840KmdStatus st = a840_kmd_preempt_priority_to_ring(priority, &ring_id);
    if (st != A840_KMD_OK)
        return st;

    A840PreemptSubmitReq req = *req_template;
    req.ring_id = ring_id;
    return a840_kmd_preempt_submit(sched, &req, out_res);
}

A840KmdStatus a840_kmd_preempt_on_hw_in_progress(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns)
{
    (void)now_ns;
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;

    sched_lock(sched);
    if (sched->state != A840_PREEMPT_TRIGGERED) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }
    sched->state = A840_PREEMPT_IN_PROGRESS;
    sched_unlock(sched);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_on_switch_done(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;

    PreemptActionBatch batch;

    sched_lock(sched);
    if (sched->state != A840_PREEMPT_IN_PROGRESS) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }

    init_action_batch(sched, &batch);

    uint32_t target = sched->target_ring;
    if (target >= A840_RING_COUNT ||
        sched->rings[target].health_state != A840_RING_STATE_ACTIVE) {
        target = pick_best_runnable_ring_locked(sched);
    }

    uint64_t switch_lat = (now_ns >= sched->trigger_ns) ? (now_ns - sched->trigger_ns) : 0u;
    sched->state = A840_PREEMPT_COMPLETE;
    sched->active_ring = target;
    ++sched->total_preempt_completes;

    if (target < A840_RING_COUNT) {
        sched->rings[target].active_since_ns = now_ns;
        batch.latency.emit = 1u;
        batch.latency.kind = A840_PREEMPT_LAT_TRIGGER_TO_ACTIVE;
        batch.latency.ring_id = target;
        batch.latency.latency_ns = switch_lat;
        batch.latency.now_ns = now_ns;

        prepare_doorbell_locked(sched, target, now_ns, &batch);
    }

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_ack_complete(
    A840KmdPreemptScheduler *sched)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;

    sched_lock(sched);
    if (sched->state != A840_PREEMPT_COMPLETE) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }
    sched->state = A840_PREEMPT_IDLE;
    sched->from_ring = A840_RING_NONE;
    sched->target_ring = A840_RING_NONE;
    sched_unlock(sched);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_check_timeout(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns,
    uint32_t *out_timed_out)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (out_timed_out != NULL)
        *out_timed_out = 0u;

    PreemptActionBatch batch;

    sched_lock(sched);
    init_action_batch(sched, &batch);

    if (sched->state == A840_PREEMPT_TRIGGERED ||
        sched->state == A840_PREEMPT_IN_PROGRESS) {
        uint64_t elapsed = (now_ns >= sched->trigger_ns) ? (now_ns - sched->trigger_ns) : 0u;
        if (elapsed >= sched->preempt_timeout_ns) {
            sched->state = A840_PREEMPT_TIMEOUT_RECOVERY;
            ++sched->total_preempt_timeouts;

            batch.timeout.emit = 1u;
            batch.timeout.from_ring = sched->from_ring;
            batch.timeout.to_ring = sched->target_ring;
            batch.timeout.elapsed_ns = elapsed;

            if (out_timed_out != NULL)
                *out_timed_out = 1u;
        }
    }

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_recover_from_timeout(
    A840KmdPreemptScheduler *sched,
    uint32_t fault_stalled_from_ring,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;

    PreemptActionBatch batch;

    sched_lock(sched);
    if (sched->state != A840_PREEMPT_TIMEOUT_RECOVERY) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }

    init_action_batch(sched, &batch);

    uint32_t stalled = sched->from_ring;
    if (fault_stalled_from_ring != 0u && stalled < A840_RING_COUNT &&
        stalled != A840_RING_0_COMPOSITOR) {
        A840PreemptRingState *rs = &sched->rings[stalled];
        if (rs->health_state == A840_RING_STATE_ACTIVE) {
            rs->health_state = A840_RING_STATE_FAULTED;
            ++rs->fault_count;
            rs->inflight_count = 0u;
            rs->rptr = rs->wptr;
            memset(rs->slots, 0, sizeof(rs->slots));
            memset(rs->dep_max_seqno, 0, sizeof(rs->dep_max_seqno));
        }
    }

    sched->state = A840_PREEMPT_IDLE;
    sched->active_ring = A840_RING_NONE;
    sched->from_ring = A840_RING_NONE;
    sched->target_ring = A840_RING_NONE;

    recompute_effective_priorities_locked(sched, now_ns, &batch);
    evaluate_schedule_locked(sched, now_ns, &batch, NULL, NULL, NULL);

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_on_ring_retire(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t completed_seqno,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;

    PreemptActionBatch batch;

    sched_lock(sched);
    A840PreemptRingState *ring = &sched->rings[ring_id];
    if (completed_seqno > ring->submitted_seqno) {
        sched_unlock(sched);
        return A840_KMD_RANGE;
    }
    if (completed_seqno < ring->completed_seqno) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }

    init_action_batch(sched, &batch);

    ring->completed_seqno = completed_seqno;
    while (ring->inflight_count > 0u) {
        A840PreemptSlot *head = &ring->slots[ring->rptr];
        if (!head->valid || head->seqno > completed_seqno)
            break;
        memset(head, 0, sizeof(*head));
        ring->rptr = (ring->rptr + 1u) & (ring->capacity - 1u);
        --ring->inflight_count;
        ++sched->total_retires;
    }

    /* Rebuild dep_max_seqno for ring_id from remaining inflight slots. */
    memset(ring->dep_max_seqno, 0, sizeof(ring->dep_max_seqno));
    for (uint32_t i = 0u; i < ring->inflight_count; ++i) {
        uint32_t idx = (ring->rptr + i) & (ring->capacity - 1u);
        const A840PreemptSlot *s = &ring->slots[idx];
        if (s->valid && s->has_dep && s->dep_ring < A840_RING_COUNT) {
            if (s->dep_seqno > ring->dep_max_seqno[s->dep_ring])
                ring->dep_max_seqno[s->dep_ring] = s->dep_seqno;
        }
    }

    batch.retire_emit = 1u;
    batch.retire_ring = ring_id;
    batch.retire_seqno = completed_seqno;
    batch.retire_now_ns = now_ns;

    if (sched->active_ring == ring_id && ring->inflight_count == 0u &&
        (sched->state == A840_PREEMPT_IDLE || sched->state == A840_PREEMPT_COMPLETE)) {
        sched->active_ring = A840_RING_NONE;
    }

    recompute_effective_priorities_locked(sched, now_ns, &batch);
    evaluate_schedule_locked(sched, now_ns, &batch, NULL, NULL, NULL);

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_tick(
    A840KmdPreemptScheduler *sched,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;

    PreemptActionBatch batch;

    sched_lock(sched);
    init_action_batch(sched, &batch);

    if (sched->state == A840_PREEMPT_TRIGGERED ||
        sched->state == A840_PREEMPT_IN_PROGRESS) {
        uint64_t elapsed = (now_ns >= sched->trigger_ns) ? (now_ns - sched->trigger_ns) : 0u;
        if (elapsed >= sched->preempt_timeout_ns) {
            sched->state = A840_PREEMPT_TIMEOUT_RECOVERY;
            ++sched->total_preempt_timeouts;
            batch.timeout.emit = 1u;
            batch.timeout.from_ring = sched->from_ring;
            batch.timeout.to_ring = sched->target_ring;
            batch.timeout.elapsed_ns = elapsed;
        }
    } else {
        evaluate_schedule_locked(sched, now_ns, &batch, NULL, NULL, NULL);
    }

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_mark_ring_faulted(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;

    PreemptActionBatch batch;

    sched_lock(sched);
    A840PreemptRingState *ring = &sched->rings[ring_id];
    if (ring->health_state != A840_RING_STATE_ACTIVE) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }

    init_action_batch(sched, &batch);

    ring->health_state = A840_RING_STATE_FAULTED;
    ++ring->fault_count;
    ring->inflight_count = 0u;
    ring->rptr = ring->wptr;
    memset(ring->slots, 0, sizeof(ring->slots));
    memset(ring->dep_max_seqno, 0, sizeof(ring->dep_max_seqno));

    /* Clear any other ring's dependency on the faulted ring so RING_0 is never
     * deadlocked waiting on an isolated faulted ring.
     */
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r)
        sched->rings[r].dep_max_seqno[ring_id] = 0u;

    if (sched->active_ring == ring_id)
        sched->active_ring = A840_RING_NONE;

    if ((sched->state == A840_PREEMPT_TRIGGERED ||
         sched->state == A840_PREEMPT_IN_PROGRESS) &&
        (sched->from_ring == ring_id || sched->target_ring == ring_id)) {
        sched->state = A840_PREEMPT_IDLE;
        sched->from_ring = A840_RING_NONE;
        sched->target_ring = A840_RING_NONE;
    }

    recompute_effective_priorities_locked(sched, now_ns, &batch);
    evaluate_schedule_locked(sched, now_ns, &batch, NULL, NULL, NULL);

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_begin_ring_reset(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;

    sched_lock(sched);
    A840PreemptRingState *ring = &sched->rings[ring_id];
    if (ring->health_state != A840_RING_STATE_FAULTED) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }
    ring->health_state = A840_RING_STATE_RESETTING;
    sched_unlock(sched);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_preempt_complete_ring_reset(
    A840KmdPreemptScheduler *sched,
    uint32_t ring_id,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;

    PreemptActionBatch batch;

    sched_lock(sched);
    A840PreemptRingState *ring = &sched->rings[ring_id];
    if (ring->health_state != A840_RING_STATE_RESETTING) {
        sched_unlock(sched);
        return A840_KMD_STATE_CONFLICT;
    }

    init_action_batch(sched, &batch);

    ring->health_state = A840_RING_STATE_ACTIVE;
    ring->rptr = 0u;
    ring->wptr = 0u;
    ring->inflight_count = 0u;
    ring->completed_seqno = ring->submitted_seqno;
    ring->last_doorbell_wptr = 0u;
    ring->last_doorbell_seqno = ring->submitted_seqno;
    ring->effective_priority = ring_id;
    ring->pi_boost_active = 0u;
    ring->boosted_by_ring = A840_RING_NONE;
    ring->boost_until_seqno = 0u;
    memset(ring->slots, 0, sizeof(ring->slots));
    memset(ring->dep_max_seqno, 0, sizeof(ring->dep_max_seqno));
    ++ring->reset_count;

    recompute_effective_priorities_locked(sched, now_ns, &batch);
    evaluate_schedule_locked(sched, now_ns, &batch, NULL, NULL, NULL);

    sched_unlock(sched);
    return dispatch_actions(&batch);
}

A840KmdStatus a840_kmd_preempt_get_snapshot(
    A840KmdPreemptScheduler *sched,
    A840PreemptSnapshot *out_snapshot)
{
    A840KmdStatus st = check_init(sched);
    if (st != A840_KMD_OK)
        return st;
    if (out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    memset(out_snapshot, 0, sizeof(*out_snapshot));

    sched_lock(sched);
    out_snapshot->state = sched->state;
    out_snapshot->active_ring = sched->active_ring;
    out_snapshot->target_ring = sched->target_ring;
    out_snapshot->from_ring = sched->from_ring;
    out_snapshot->active_preempt_level = sched->active_preempt_level;
    out_snapshot->deferred_preempt_pending = sched->deferred_preempt_pending;
    out_snapshot->preempt_generation = sched->preempt_generation;
    out_snapshot->trigger_ns = sched->trigger_ns;
    out_snapshot->total_submits = sched->total_submits;
    out_snapshot->total_retires = sched->total_retires;
    out_snapshot->total_doorbells = sched->total_doorbells;
    out_snapshot->total_preempt_triggers = sched->total_preempt_triggers;
    out_snapshot->total_preempt_completes = sched->total_preempt_completes;
    out_snapshot->total_preempt_timeouts = sched->total_preempt_timeouts;
    out_snapshot->total_min_slice_deferrals = sched->total_min_slice_deferrals;
    out_snapshot->total_deadline_exemptions = sched->total_deadline_exemptions;
    out_snapshot->total_pi_boosts = sched->total_pi_boosts;
    out_snapshot->total_cycle_rejects = sched->total_cycle_rejects;

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        const A840PreemptRingState *src = &sched->rings[r];
        A840PreemptRingSnapshot *dst = &out_snapshot->rings[r];
        dst->ring_id = src->ring_id;
        dst->health_state = src->health_state;
        dst->capacity = src->capacity;
        dst->rptr = src->rptr;
        dst->wptr = src->wptr;
        dst->inflight_count = src->inflight_count;
        dst->preempt_level = src->preempt_level;
        dst->effective_priority = src->effective_priority;
        dst->pi_boost_active = src->pi_boost_active;
        dst->boosted_by_ring = src->boosted_by_ring;
        dst->preempt_record_va = src->preempt_record_va;
        dst->preempt_record_bytes = src->preempt_record_bytes;
        dst->submitted_seqno = src->submitted_seqno;
        dst->completed_seqno = src->completed_seqno;
        dst->boost_until_seqno = src->boost_until_seqno;
        dst->active_since_ns = src->active_since_ns;
        dst->fault_count = src->fault_count;
        dst->reset_count = src->reset_count;
    }
    sched_unlock(sched);
    return A840_KMD_OK;
}
