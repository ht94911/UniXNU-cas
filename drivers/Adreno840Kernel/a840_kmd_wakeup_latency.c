/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_wakeup_latency.h"

#include <string.h>

#define A840_WAKEUP_INIT_MAGIC 1463896912u /* 'WAKP' decimal */

typedef struct WakeDispatchItem {
    uint64_t waiter_handle;
    uint32_t ring_id;
    uint32_t reason;
    uint64_t target_seqno;
    uint64_t completed_seqno;
    uint64_t irq_to_wake_ns;
    A840WakeupCallback wake_cb;
    void *wake_cookie;
} WakeDispatchItem;

typedef struct BudgetAlertItem {
    uint32_t metric_kind;
    uint32_t ring_id;
    uint64_t latency_ns;
    uint64_t threshold_ns;
    uint64_t now_ns;
} BudgetAlertItem;

typedef struct WakeDispatchBatch {
    uint32_t wake_count;
    uint32_t alert_count;
    WakeDispatchItem wakes[A840_WAKEUP_MAX_WAITERS];
    BudgetAlertItem alerts[A840_WAKEUP_MAX_WAITERS];
    A840WakeupBudgetAlertCallback on_budget_alert;
    void *budget_alert_cookie;
} WakeDispatchBatch;

static void wake_lock(A840KmdWakeupEngine *engine)
{
    if (engine->has_custom_sync) {
        engine->sync_ops.lock(engine->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_lock(&engine->lock);
#endif
}

static void wake_unlock(A840KmdWakeupEngine *engine)
{
    if (engine->has_custom_sync) {
        engine->sync_ops.unlock(engine->sync_ops.cookie);
        return;
    }
#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_mutex_unlock(&engine->lock);
#endif
}

static A840KmdStatus check_init(const A840KmdWakeupEngine *engine)
{
    if (engine == NULL)
        return A840_KMD_INVALID_ARG;
    if (engine->initialized != A840_WAKEUP_INIT_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static uint64_t resolve_now_ns(const A840KmdWakeupEngine *engine, uint64_t explicit_now_ns)
{
    if (explicit_now_ns == 0u && engine->has_custom_sync && engine->sync_ops.now_ns != NULL)
        return engine->sync_ops.now_ns(engine->sync_ops.cookie);
    return explicit_now_ns;
}

static uint32_t latency_hist_bucket(uint64_t latency_ns)
{
    if (latency_ns == 0u)
        return 0u;
    uint32_t b = 0u;
    uint64_t v = latency_ns;
    while (v > 1u && b + 1u < A840_WAKEUP_HIST_BUCKETS) {
        v >>= 1u;
        ++b;
    }
    return b;
}

static uint64_t make_waiter_handle(uint32_t slot_idx, uint32_t generation)
{
    return (((uint64_t)generation) << 16u) | (uint64_t)(slot_idx + 1u);
}

static void record_latency_locked(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t now_ns,
    WakeDispatchBatch *batch)
{
    A840LatencyMetricState *m = &engine->metrics[metric_kind];

    if (m->count == 0u) {
        m->min_ns = latency_ns;
        m->max_ns = latency_ns;
    } else {
        if (latency_ns < m->min_ns)
            m->min_ns = latency_ns;
        if (latency_ns > m->max_ns)
            m->max_ns = latency_ns;
    }
    ++m->count;
    if (UINT64_MAX - m->sum_ns >= latency_ns)
        m->sum_ns += latency_ns;
    else
        m->sum_ns = UINT64_MAX;

    m->window_samples[m->window_cursor] = latency_ns;
    m->window_cursor = (m->window_cursor + 1u) % A840_WAKEUP_EXACT_WINDOW_SAMPLES;
    if (m->window_count < A840_WAKEUP_EXACT_WINDOW_SAMPLES)
        ++m->window_count;

    ++m->hist_buckets[latency_hist_bucket(latency_ns)];

    if (m->budget_threshold_ns > 0u && latency_ns > m->budget_threshold_ns) {
        ++m->budget_breach_count;
        if (ring_id == A840_RING_0_COMPOSITOR)
            ++m->compositor_budget_breach_count;
        if (batch != NULL && batch->alert_count < A840_WAKEUP_MAX_WAITERS) {
            BudgetAlertItem *a = &batch->alerts[batch->alert_count++];
            a->metric_kind = metric_kind;
            a->ring_id = ring_id;
            a->latency_ns = latency_ns;
            a->threshold_ns = m->budget_threshold_ns;
            a->now_ns = now_ns;
        }
    }
}

/* Simple iterative heapsort (zero recursion, zero malloc) for exact percentiles. */
static void heapsort_u64(uint64_t *arr, uint32_t n)
{
    if (n < 2u)
        return;

    for (uint32_t start = n / 2u; start > 0u; --start) {
        uint32_t root = start - 1u;
        for (;;) {
            uint32_t child = root * 2u + 1u;
            if (child >= n)
                break;
            if (child + 1u < n && arr[child] < arr[child + 1u])
                ++child;
            if (arr[root] < arr[child]) {
                uint64_t tmp = arr[root];
                arr[root] = arr[child];
                arr[child] = tmp;
                root = child;
            } else {
                break;
            }
        }
    }

    for (uint32_t end = n - 1u; end > 0u; --end) {
        uint64_t tmp = arr[end];
        arr[end] = arr[0];
        arr[0] = tmp;

        uint32_t root = 0u;
        for (;;) {
            uint32_t child = root * 2u + 1u;
            if (child >= end)
                break;
            if (child + 1u < end && arr[child] < arr[child + 1u])
                ++child;
            if (arr[root] < arr[child]) {
                uint64_t t2 = arr[root];
                arr[root] = arr[child];
                arr[child] = t2;
                root = child;
            } else {
                break;
            }
        }
    }
}

static uint64_t nearest_rank_percentile(const uint64_t *sorted, uint32_t n, uint32_t p)
{
    if (n == 0u)
        return 0u;
    uint64_t rank = ((uint64_t)p * (uint64_t)n + 99u) / 100u;
    uint32_t idx = (rank > 0u) ? (uint32_t)(rank - 1u) : 0u;
    if (idx >= n)
        idx = n - 1u;
    return sorted[idx];
}

static void compute_metric_stats_locked(
    const A840LatencyMetricState *m,
    A840LatencyStats *out)
{
    memset(out, 0, sizeof(*out));
    out->count = m->count;
    out->min_ns = m->min_ns;
    out->max_ns = m->max_ns;
    out->sum_ns = m->sum_ns;
    out->budget_threshold_ns = m->budget_threshold_ns;
    out->budget_breach_count = m->budget_breach_count;
    out->compositor_budget_breach_count = m->compositor_budget_breach_count;

    if (m->window_count > 0u) {
        uint64_t tmp[A840_WAKEUP_EXACT_WINDOW_SAMPLES];
        memcpy(tmp, m->window_samples, (size_t)m->window_count * sizeof(uint64_t));
        heapsort_u64(tmp, m->window_count);
        out->p50_ns = nearest_rank_percentile(tmp, m->window_count, 50u);
        out->p95_ns = nearest_rank_percentile(tmp, m->window_count, 95u);
        out->p99_ns = nearest_rank_percentile(tmp, m->window_count, 99u);
    }
}

static void dispatch_wake_batch(const WakeDispatchBatch *batch)
{
    for (uint32_t i = 0u; i < batch->wake_count; ++i) {
        const WakeDispatchItem *w = &batch->wakes[i];
        if (w->wake_cb != NULL) {
            w->wake_cb(
                w->wake_cookie,
                w->waiter_handle,
                w->ring_id,
                w->target_seqno,
                w->completed_seqno,
                w->reason,
                w->irq_to_wake_ns);
        }
    }
    if (batch->on_budget_alert != NULL) {
        for (uint32_t i = 0u; i < batch->alert_count; ++i) {
            const BudgetAlertItem *a = &batch->alerts[i];
            batch->on_budget_alert(
                batch->budget_alert_cookie,
                a->metric_kind,
                a->ring_id,
                a->latency_ns,
                a->threshold_ns,
                a->now_ns);
        }
    }
}

static void init_batch(const A840KmdWakeupEngine *engine, WakeDispatchBatch *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->on_budget_alert = engine->on_budget_alert;
    batch->budget_alert_cookie = engine->budget_alert_cookie;
}

static void update_ring_effective_mode_locked(A840WakeupRingState *ring)
{
    if (ring->ring_id == A840_RING_0_COMPOSITOR ||
        ring->ring_id == A840_RING_1_INTERACTIVE ||
        ring->pi_boost_active != 0u) {
        ring->effective_mode = A840_WAKE_MODE_IMMEDIATE;
    } else {
        ring->effective_mode = ring->base_mode;
    }
}

/* Collect and release all satisfied waiters on ring_id. */
static uint32_t collect_satisfied_waiters_locked(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t wake_now_ns,
    uint32_t reason,
    uint32_t drain_all_for_fault,
    WakeDispatchBatch *batch)
{
    A840WakeupRingState *ring = &engine->rings[ring_id];
    uint32_t woken = 0u;

    for (uint32_t i = 0u; i < A840_WAKEUP_MAX_WAITERS; ++i) {
        A840WakeupWaiterSlot *slot = &engine->waiters[i];
        if (!slot->in_use || slot->ring_id != ring_id)
            continue;

        if (!drain_all_for_fault && ring->completed_seqno < slot->target_seqno)
            continue;

        uint64_t irq_ts = slot->satisfied_pending ? slot->satisfied_irq_ns : wake_now_ns;
        uint64_t lat_ns = (wake_now_ns >= irq_ts) ? (wake_now_ns - irq_ts) : 0u;

        if (batch->wake_count < A840_WAKEUP_MAX_WAITERS) {
            WakeDispatchItem *item = &batch->wakes[batch->wake_count++];
            item->waiter_handle = slot->waiter_handle;
            item->ring_id = ring_id;
            item->reason = reason;
            item->target_seqno = slot->target_seqno;
            item->completed_seqno = ring->completed_seqno;
            item->irq_to_wake_ns = lat_ns;
            item->wake_cb = slot->wake_cb;
            item->wake_cookie = slot->wake_cookie;
        }

        if (!drain_all_for_fault) {
            record_latency_locked(engine, A840_LATENCY_IRQ_TO_WAITER_WAKE,
                ring_id, lat_ns, wake_now_ns, batch);
        }

        uint32_t prev_gen = slot->generation;
        memset(slot, 0, sizeof(*slot));
        slot->generation = prev_gen;

        if (ring->active_waiters > 0u)
            --ring->active_waiters;
        if (engine->active_waiter_count > 0u)
            --engine->active_waiter_count;
        ++ring->total_waiters_woken;
        ++woken;
    }

    return woken;
}

static A840KmdStatus apply_ring_config_locked(
    A840WakeupRingState *ring,
    uint32_t ring_id,
    const A840WakeupRingConfig *cfg)
{
    if (cfg->base_mode != A840_WAKE_MODE_IMMEDIATE &&
        cfg->base_mode != A840_WAKE_MODE_COALESCED)
        return A840_KMD_INVALID_ARG;

    /* Fail-closed: RING_0_COMPOSITOR and RING_1_INTERACTIVE NEVER coalesce. */
    if ((ring_id == A840_RING_0_COMPOSITOR || ring_id == A840_RING_1_INTERACTIVE) &&
        cfg->base_mode == A840_WAKE_MODE_COALESCED) {
        return A840_KMD_INVALID_ARG;
    }

    uint32_t default_batch = (ring_id == A840_RING_2_CLIENT_3D)
                                 ? A840_WAKEUP_DEFAULT_R2_MAX_BATCH
                                 : A840_WAKEUP_DEFAULT_R3_MAX_BATCH;
    uint64_t default_win = (ring_id == A840_RING_2_CLIENT_3D)
                               ? A840_WAKEUP_DEFAULT_R2_COALESCE_NS
                               : A840_WAKEUP_DEFAULT_R3_COALESCE_NS;

    ring->base_mode = cfg->base_mode;
    if (cfg->base_mode == A840_WAKE_MODE_COALESCED) {
        ring->coalesce_max_batch = (cfg->coalesce_max_batch != 0u)
                                       ? cfg->coalesce_max_batch
                                       : default_batch;
        ring->coalesce_window_ns = (cfg->coalesce_window_ns == 0u && !cfg->allow_zero_window)
                                       ? default_win
                                       : cfg->coalesce_window_ns;
    } else {
        ring->coalesce_max_batch = 1u;
        ring->coalesce_window_ns = 0u;
    }
    update_ring_effective_mode_locked(ring);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_init(
    A840KmdWakeupEngine *engine,
    const A840WakeupConfig *config)
{
    if (engine == NULL)
        return A840_KMD_INVALID_ARG;

    if (config != NULL && config->sync_ops != NULL) {
        if (config->sync_ops->lock == NULL || config->sync_ops->unlock == NULL)
            return A840_KMD_INVALID_ARG;
    }
#if defined(A840_KMD_CUSTOM_SYNC)
    if (config == NULL || config->sync_ops == NULL)
        return A840_KMD_INVALID_ARG;
#endif

    /* Pre-validate custom ring configs before mutating engine. */
    if (config != NULL && config->use_custom_ring_cfg != 0u) {
        for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
            A840WakeupRingState scratch;
            memset(&scratch, 0, sizeof(scratch));
            scratch.ring_id = r;
            A840KmdStatus st = apply_ring_config_locked(&scratch, r, &config->rings[r]);
            if (st != A840_KMD_OK)
                return st;
        }
    }

    memset(engine, 0, sizeof(*engine));
    engine->next_generation = 1u;

    if (config != NULL && config->sync_ops != NULL) {
        engine->has_custom_sync = 1u;
        engine->sync_ops = *config->sync_ops;
    } else {
#if !defined(A840_KMD_CUSTOM_SYNC)
        if (pthread_mutex_init(&engine->lock, NULL) != 0)
            return A840_KMD_NO_MEMORY;
#endif
    }

    if (config != NULL) {
        engine->on_budget_alert = config->on_budget_alert;
        engine->budget_alert_cookie = config->budget_alert_cookie;
    }

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        A840WakeupRingState *rs = &engine->rings[r];
        rs->ring_id = r;
        if (config != NULL && config->use_custom_ring_cfg != 0u) {
            (void)apply_ring_config_locked(rs, r, &config->rings[r]);
        } else {
            A840WakeupRingConfig def_cfg;
            memset(&def_cfg, 0, sizeof(def_cfg));
            if (r == A840_RING_0_COMPOSITOR || r == A840_RING_1_INTERACTIVE) {
                def_cfg.base_mode = A840_WAKE_MODE_IMMEDIATE;
            } else {
                def_cfg.base_mode = A840_WAKE_MODE_COALESCED;
            }
            (void)apply_ring_config_locked(rs, r, &def_cfg);
        }
    }

    for (uint32_t m = 0u; m < A840_LATENCY_METRIC_COUNT; ++m) {
        uint64_t thresh = A840_WAKEUP_DEFAULT_BUDGET_ALERT_NS;
        if (config != NULL && config->budget_alert_ns[m] != 0u)
            thresh = config->budget_alert_ns[m];
        engine->metrics[m].budget_threshold_ns = thresh;
    }

    engine->initialized = A840_WAKEUP_INIT_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_wakeup_destroy(A840KmdWakeupEngine *engine)
{
    if (engine == NULL || engine->initialized != A840_WAKEUP_INIT_MAGIC)
        return;

    engine->initialized = 0u;
#if !defined(A840_KMD_CUSTOM_SYNC)
    if (!engine->has_custom_sync)
        (void)pthread_mutex_destroy(&engine->lock);
#endif
    memset(engine, 0, sizeof(*engine));
}

A840KmdStatus a840_kmd_wakeup_configure_ring(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    const A840WakeupRingConfig *ring_cfg)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (ring_cfg == NULL)
        return A840_KMD_INVALID_ARG;

    wake_lock(engine);
    st = apply_ring_config_locked(&engine->rings[ring_id], ring_id, ring_cfg);
    wake_unlock(engine);
    return st;
}

A840KmdStatus a840_kmd_wakeup_set_budget_threshold(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    uint64_t threshold_ns)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (metric_kind >= A840_LATENCY_METRIC_COUNT)
        return A840_KMD_RANGE;

    wake_lock(engine);
    engine->metrics[metric_kind].budget_threshold_ns = threshold_ns;
    wake_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_register_waiter(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t target_seqno,
    uint64_t now_ns,
    A840WakeupCallback wake_cb,
    void *wake_cookie,
    A840WakeupRegisterResult *out_res)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (target_seqno == 0u || wake_cb == NULL)
        return A840_KMD_INVALID_ARG;

    A840WakeupRegisterResult res;
    memset(&res, 0, sizeof(res));
    WakeDispatchBatch batch;
    int fast_path = 0;

    wake_lock(engine);
    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, now_ns);
    A840WakeupRingState *ring = &engine->rings[ring_id];

    ++engine->total_registrations;

    /* Fast-path: already satisfied -> invoke wake_cb outside lock, zero slot used. */
    if (ring->completed_seqno >= target_seqno) {
        ++engine->total_fast_path_hits;
        ++ring->total_waiters_woken;
        res.waiter_handle = 0u;
        res.already_satisfied = 1u;
        res.slot_index = A840_WAKEUP_MAX_WAITERS;

        WakeDispatchItem *item = &batch.wakes[batch.wake_count++];
        item->waiter_handle = 0u;
        item->ring_id = ring_id;
        item->reason = A840_WAKE_REASON_FAST_PATH;
        item->target_seqno = target_seqno;
        item->completed_seqno = ring->completed_seqno;
        item->irq_to_wake_ns = 0u;
        item->wake_cb = wake_cb;
        item->wake_cookie = wake_cookie;

        record_latency_locked(engine, A840_LATENCY_IRQ_TO_WAITER_WAKE,
            ring_id, 0u, ts, &batch);
        fast_path = 1;
        wake_unlock(engine);

        if (out_res != NULL)
            *out_res = res;
        if (fast_path)
            dispatch_wake_batch(&batch);
        return A840_KMD_OK;
    }

    /* Allocate from fixed zero-heap pool. */
    uint32_t free_idx = A840_WAKEUP_MAX_WAITERS;
    for (uint32_t i = 0u; i < A840_WAKEUP_MAX_WAITERS; ++i) {
        if (!engine->waiters[i].in_use) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == A840_WAKEUP_MAX_WAITERS) {
        ++engine->total_pool_rejects;
        wake_unlock(engine);
        return A840_KMD_CAPACITY;
    }

    uint32_t gen = engine->next_generation++;
    if (gen == 0u)
        gen = engine->next_generation++;

    A840WakeupWaiterSlot *slot = &engine->waiters[free_idx];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1u;
    slot->ring_id = ring_id;
    slot->generation = gen;
    slot->waiter_handle = make_waiter_handle(free_idx, gen);
    slot->target_seqno = target_seqno;
    slot->register_ns = ts;
    slot->wake_cb = wake_cb;
    slot->wake_cookie = wake_cookie;

    ++ring->active_waiters;
    ++engine->active_waiter_count;
    if (engine->active_waiter_count > engine->peak_waiter_count)
        engine->peak_waiter_count = engine->active_waiter_count;

    res.waiter_handle = slot->waiter_handle;
    res.already_satisfied = 0u;
    res.slot_index = free_idx;

    wake_unlock(engine);
    if (out_res != NULL)
        *out_res = res;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_cancel_waiter(
    A840KmdWakeupEngine *engine,
    uint64_t waiter_handle)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (waiter_handle == 0u)
        return A840_KMD_INVALID_ARG;

    uint32_t idx_plus_1 = (uint32_t)(waiter_handle & 65535u);
    if (idx_plus_1 == 0u || idx_plus_1 > A840_WAKEUP_MAX_WAITERS)
        return A840_KMD_NOT_FOUND;
    uint32_t slot_idx = idx_plus_1 - 1u;

    wake_lock(engine);
    A840WakeupWaiterSlot *slot = &engine->waiters[slot_idx];
    if (!slot->in_use || slot->waiter_handle != waiter_handle) {
        wake_unlock(engine);
        return A840_KMD_NOT_FOUND;
    }

    uint32_t rid = slot->ring_id;
    uint32_t prev_gen = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = prev_gen;

    if (rid < A840_RING_COUNT && engine->rings[rid].active_waiters > 0u)
        --engine->rings[rid].active_waiters;
    if (engine->active_waiter_count > 0u)
        --engine->active_waiter_count;
    ++engine->total_cancellations;

    wake_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_on_irq(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t completed_seqno,
    uint64_t irq_ns,
    uint32_t *out_woken_count)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (out_woken_count != NULL)
        *out_woken_count = 0u;

    WakeDispatchBatch batch;
    uint32_t woken = 0u;

    wake_lock(engine);
    A840WakeupRingState *ring = &engine->rings[ring_id];
    if (completed_seqno < ring->completed_seqno) {
        wake_unlock(engine);
        return A840_KMD_STATE_CONFLICT;
    }

    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, irq_ns);

    ring->completed_seqno = completed_seqno;
    ++ring->total_irqs;
    update_ring_effective_mode_locked(ring);

    /* Stamp satisfied_irq_ns on any newly-satisfied waiters. */
    uint32_t newly_satisfied = 0u;
    for (uint32_t i = 0u; i < A840_WAKEUP_MAX_WAITERS; ++i) {
        A840WakeupWaiterSlot *slot = &engine->waiters[i];
        if (!slot->in_use || slot->ring_id != ring_id)
            continue;
        if (completed_seqno >= slot->target_seqno && !slot->satisfied_pending) {
            slot->satisfied_pending = 1u;
            slot->satisfied_irq_ns = ts;
            ++newly_satisfied;
        }
    }

    if (ring->effective_mode == A840_WAKE_MODE_IMMEDIATE) {
        woken = collect_satisfied_waiters_locked(
            engine, ring_id, ts, A840_WAKE_REASON_SEQNO_REACHED, 0u, &batch);
        ring->coalesce_pending_irqs = 0u;
        ring->coalesce_window_start_ns = 0u;
        if (woken > 0u)
            ++ring->total_immediate_wakes;
    } else {
        /* COALESCED mode for non-boosted RING_2 / RING_3. */
        if (ring->coalesce_pending_irqs == 0u)
            ring->coalesce_window_start_ns = ts;
        ++ring->coalesce_pending_irqs;

        uint64_t elapsed = (ts >= ring->coalesce_window_start_ns)
                               ? (ts - ring->coalesce_window_start_ns)
                               : 0u;
        int batch_full = (ring->coalesce_max_batch > 0u &&
                          ring->coalesce_pending_irqs >= ring->coalesce_max_batch);
        int window_hit = (elapsed >= ring->coalesce_window_ns);
        (void)newly_satisfied;

        if (batch_full || window_hit) {
            woken = collect_satisfied_waiters_locked(
                engine, ring_id, ts, A840_WAKE_REASON_SEQNO_REACHED, 0u, &batch);
            ring->coalesce_pending_irqs = 0u;
            ring->coalesce_window_start_ns = 0u;
            if (woken > 0u)
                ++ring->total_coalesced_flushes;
        }
    }

    wake_unlock(engine);

    dispatch_wake_batch(&batch);
    if (out_woken_count != NULL)
        *out_woken_count = woken;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_flush_coalesced(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t now_ns,
    uint32_t force_flush,
    uint32_t *out_woken_count)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (out_woken_count != NULL)
        *out_woken_count = 0u;

    WakeDispatchBatch batch;
    uint32_t woken = 0u;

    wake_lock(engine);
    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, now_ns);
    A840WakeupRingState *ring = &engine->rings[ring_id];

    int should_flush = (force_flush != 0u);
    if (!should_flush && ring->coalesce_pending_irqs > 0u) {
        uint64_t elapsed = (ts >= ring->coalesce_window_start_ns)
                               ? (ts - ring->coalesce_window_start_ns)
                               : 0u;
        if (elapsed >= ring->coalesce_window_ns)
            should_flush = 1;
    }

    if (should_flush) {
        woken = collect_satisfied_waiters_locked(
            engine, ring_id, ts, A840_WAKE_REASON_SEQNO_REACHED, 0u, &batch);
        ring->coalesce_pending_irqs = 0u;
        ring->coalesce_window_start_ns = 0u;
        if (woken > 0u)
            ++ring->total_coalesced_flushes;
    }

    wake_unlock(engine);
    dispatch_wake_batch(&batch);
    if (out_woken_count != NULL)
        *out_woken_count = woken;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_poll_timers(
    A840KmdWakeupEngine *engine,
    uint64_t now_ns,
    uint32_t *out_total_woken)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;

    uint32_t total = 0u;
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        uint32_t w = 0u;
        st = a840_kmd_wakeup_flush_coalesced(engine, r, now_ns, 0u, &w);
        if (st != A840_KMD_OK)
            return st;
        total += w;
    }
    if (out_total_woken != NULL)
        *out_total_woken = total;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_set_pi_boost(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint32_t boosted,
    uint64_t now_ns,
    uint32_t *out_flushed_woken)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (out_flushed_woken != NULL)
        *out_flushed_woken = 0u;

    WakeDispatchBatch batch;
    uint32_t flushed = 0u;

    wake_lock(engine);
    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, now_ns);
    A840WakeupRingState *ring = &engine->rings[ring_id];

    ring->pi_boost_active = boosted ? 1u : 0u;
    update_ring_effective_mode_locked(ring);

    if (ring->pi_boost_active != 0u) {
        flushed = collect_satisfied_waiters_locked(
            engine, ring_id, ts, A840_WAKE_REASON_PI_FLUSH, 0u, &batch);
        ring->coalesce_pending_irqs = 0u;
        ring->coalesce_window_start_ns = 0u;
        if (flushed > 0u)
            ++ring->total_immediate_wakes;
    }

    wake_unlock(engine);
    dispatch_wake_batch(&batch);
    if (out_flushed_woken != NULL)
        *out_flushed_woken = flushed;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_fault_ring(
    A840KmdWakeupEngine *engine,
    uint32_t ring_id,
    uint64_t now_ns,
    uint32_t *out_woken_count)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;
    if (out_woken_count != NULL)
        *out_woken_count = 0u;

    WakeDispatchBatch batch;
    uint32_t woken = 0u;

    wake_lock(engine);
    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, now_ns);
    A840WakeupRingState *ring = &engine->rings[ring_id];

    woken = collect_satisfied_waiters_locked(
        engine, ring_id, ts, A840_WAKE_REASON_RING_FAULTED, 1u, &batch);
    ring->coalesce_pending_irqs = 0u;
    ring->coalesce_window_start_ns = 0u;

    wake_unlock(engine);
    dispatch_wake_batch(&batch);
    if (out_woken_count != NULL)
        *out_woken_count = woken;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_record_latency(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t now_ns)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (metric_kind >= A840_LATENCY_METRIC_COUNT || ring_id >= A840_RING_COUNT)
        return A840_KMD_RANGE;

    WakeDispatchBatch batch;

    wake_lock(engine);
    init_batch(engine, &batch);
    uint64_t ts = resolve_now_ns(engine, now_ns);
    record_latency_locked(engine, metric_kind, ring_id, latency_ns, ts, &batch);
    wake_unlock(engine);

    dispatch_wake_batch(&batch);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_get_latency_stats(
    A840KmdWakeupEngine *engine,
    uint32_t metric_kind,
    A840LatencyStats *out_stats)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (metric_kind >= A840_LATENCY_METRIC_COUNT)
        return A840_KMD_RANGE;
    if (out_stats == NULL)
        return A840_KMD_INVALID_ARG;

    wake_lock(engine);
    compute_metric_stats_locked(&engine->metrics[metric_kind], out_stats);
    wake_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_reset_latency_stats(
    A840KmdWakeupEngine *engine)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;

    wake_lock(engine);
    for (uint32_t m = 0u; m < A840_LATENCY_METRIC_COUNT; ++m) {
        uint64_t saved_thresh = engine->metrics[m].budget_threshold_ns;
        memset(&engine->metrics[m], 0, sizeof(engine->metrics[m]));
        engine->metrics[m].budget_threshold_ns = saved_thresh;
    }
    wake_unlock(engine);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_wakeup_get_snapshot(
    A840KmdWakeupEngine *engine,
    A840WakeupSnapshot *out_snapshot)
{
    A840KmdStatus st = check_init(engine);
    if (st != A840_KMD_OK)
        return st;
    if (out_snapshot == NULL)
        return A840_KMD_INVALID_ARG;

    memset(out_snapshot, 0, sizeof(*out_snapshot));

    wake_lock(engine);
    out_snapshot->active_waiter_count = engine->active_waiter_count;
    out_snapshot->peak_waiter_count = engine->peak_waiter_count;
    out_snapshot->total_registrations = engine->total_registrations;
    out_snapshot->total_fast_path_hits = engine->total_fast_path_hits;
    out_snapshot->total_cancellations = engine->total_cancellations;
    out_snapshot->total_pool_rejects = engine->total_pool_rejects;

    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        const A840WakeupRingState *src = &engine->rings[r];
        A840WakeupRingSnapshot *dst = &out_snapshot->rings[r];
        dst->ring_id = src->ring_id;
        dst->base_mode = src->base_mode;
        dst->effective_mode = src->effective_mode;
        dst->pi_boost_active = src->pi_boost_active;
        dst->coalesce_max_batch = src->coalesce_max_batch;
        dst->coalesce_pending_irqs = src->coalesce_pending_irqs;
        dst->active_waiters = src->active_waiters;
        dst->coalesce_window_ns = src->coalesce_window_ns;
        dst->coalesce_window_start_ns = src->coalesce_window_start_ns;
        dst->completed_seqno = src->completed_seqno;
        dst->total_irqs = src->total_irqs;
        dst->total_immediate_wakes = src->total_immediate_wakes;
        dst->total_coalesced_flushes = src->total_coalesced_flushes;
        dst->total_waiters_woken = src->total_waiters_woken;
    }

    for (uint32_t m = 0u; m < A840_LATENCY_METRIC_COUNT; ++m) {
        compute_metric_stats_locked(&engine->metrics[m], &out_snapshot->metrics[m]);
    }
    wake_unlock(engine);
    return A840_KMD_OK;
}

void a840_kmd_wakeup_bridge_pi_boost(
    void *wakeup_engine_cookie,
    uint32_t ring_id,
    uint32_t boosted,
    uint32_t boosted_by_ring,
    uint64_t boost_until_seqno,
    uint64_t now_ns)
{
    (void)boosted_by_ring;
    (void)boost_until_seqno;
    A840KmdWakeupEngine *engine = (A840KmdWakeupEngine *)wakeup_engine_cookie;
    if (engine == NULL)
        return;
    (void)a840_kmd_wakeup_set_pi_boost(engine, ring_id, boosted, now_ns, NULL);
}

void a840_kmd_wakeup_bridge_latency(
    void *wakeup_engine_cookie,
    uint32_t preempt_latency_kind,
    uint32_t ring_id,
    uint64_t latency_ns,
    uint64_t now_ns)
{
    A840KmdWakeupEngine *engine = (A840KmdWakeupEngine *)wakeup_engine_cookie;
    if (engine == NULL)
        return;
    uint32_t metric = A840_LATENCY_ENQUEUE_TO_DOORBELL;
    if (preempt_latency_kind == A840_PREEMPT_LAT_TRIGGER_TO_ACTIVE)
        metric = A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE;
    (void)a840_kmd_wakeup_record_latency(engine, metric, ring_id, latency_ns, now_ns);
}
