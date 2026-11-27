/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_sync_tracker.h"

#include <string.h>

enum {
    A840_SYNC_VALID_CACHE_MASK = A840_CACHE_UCHE_INV |
                                 A840_CACHE_CCU_FLUSH_COLOR |
                                 A840_CACHE_CCU_FLUSH_DEPTH |
                                 A840_CACHE_CCU_INV_COLOR,
    /* Exact packet dword sizes emitted by a840_emit_sync.c:
     *   a840_emit_wfi:             1 dword  (pkt7 CP_WAIT_FOR_IDLE, count=0)
     *   a840_emit_wait_mem_writes: 1 dword  (pkt7 CP_WAIT_MEM_WRITES, count=0)
     *   each cache event:          2 dwords (pkt7 CP_EVENT_WRITE7, count=1) */
    A840_SYNC_WFI_DWORDS = 1u,
    A840_SYNC_WAIT_MEM_DWORDS = 1u,
    A840_SYNC_EVENT_DWORDS = 2u
};

static uint32_t popcount_u32(uint32_t mask)
{
    uint32_t count = 0u;
    while (mask != 0u) {
        count += mask & 1u;
        mask >>= 1u;
    }
    return count;
}

static bool flags_valid(uint32_t flags)
{
    return (flags & ~(uint32_t)A840_SYNC_FLAGS_VALID_MASK) == 0u;
}

static A840SyncResourceEntry *find_resource(A840SyncTracker *tracker, uint64_t id)
{
    uint32_t i;
    for (i = 0u; i < tracker->resource_count; ++i) {
        if (tracker->resources[i].resource_id == id) {
            return &tracker->resources[i];
        }
    }
    return NULL;
}

static const A840SyncResourceEntry *find_resource_const(const A840SyncTracker *tracker,
                                                        uint64_t id)
{
    uint32_t i;
    for (i = 0u; i < tracker->resource_count; ++i) {
        if (tracker->resources[i].resource_id == id) {
            return &tracker->resources[i];
        }
    }
    return NULL;
}

static A840SyncResourceEntry *upsert_resource(A840SyncTracker *tracker, uint64_t id)
{
    A840SyncResourceEntry *entry = find_resource(tracker, id);
    if (entry) {
        return entry;
    }
    if (tracker->resource_count >= A840_SYNC_MAX_TRACKED_RESOURCES) {
        tracker->overflow_conservative = true;
        return NULL;
    }
    entry = &tracker->resources[tracker->resource_count++];
    memset(entry, 0, sizeof(*entry));
    entry->resource_id = id;
    return entry;
}

static bool validate_resource_ids(const uint64_t *ids, size_t count)
{
    size_t i;
    if (count == 0u) {
        return true;
    }
    if (!ids) {
        return false;
    }
    for (i = 0u; i < count; ++i) {
        if (ids[i] == 0u) {
            return false;
        }
    }
    return true;
}

/* Recompute aggregate dirty_domains from tracked resources when not in
 * conservative overflow mode. */
static void recompute_aggregate_dirty(A840SyncTracker *tracker)
{
    uint32_t agg = 0u;
    uint32_t i;
    if (tracker->overflow_conservative || tracker->external_hazard_pending) {
        return;
    }
    for (i = 0u; i < tracker->resource_count; ++i) {
        agg |= tracker->resources[i].dirty_mask;
    }
    tracker->dirty_domains = agg;
}

A840EmitStatus a840_sync_tracker_init(A840SyncTracker *tracker, A840SyncMode mode)
{
    if (!tracker) {
        return A840_EMIT_INVALID_ARG;
    }
    if (mode != A840_SYNC_MODE_TRACKED && mode != A840_SYNC_MODE_NAIVE) {
        return A840_EMIT_INVALID_ARG;
    }
    memset(tracker, 0, sizeof(*tracker));
    tracker->mode = mode;
    tracker->clean_state = A840_SYNC_CLEAN_ALL;
    return A840_EMIT_OK;
}

void a840_sync_tracker_reset(A840SyncTracker *tracker)
{
    A840SyncMode mode;
    if (!tracker) {
        return;
    }
    mode = tracker->mode;
    memset(tracker, 0, sizeof(*tracker));
    tracker->mode = mode;
    tracker->clean_state = A840_SYNC_CLEAN_ALL;
}

void a840_sync_tracker_set_mode(A840SyncTracker *tracker, A840SyncMode mode)
{
    if (!tracker) {
        return;
    }
    if (mode == A840_SYNC_MODE_TRACKED || mode == A840_SYNC_MODE_NAIVE) {
        tracker->mode = mode;
    }
}

A840SyncStats a840_sync_tracker_stats(const A840SyncTracker *tracker)
{
    A840SyncStats zero;
    memset(&zero, 0, sizeof(zero));
    if (!tracker) {
        return zero;
    }
    return tracker->stats;
}

A840EmitStatus a840_sync_note_write(A840SyncTracker *tracker,
                                    uint32_t dirty_bits,
                                    const uint64_t *resource_ids,
                                    size_t resource_count)
{
    size_t i;
    if (!tracker) {
        return A840_EMIT_INVALID_ARG;
    }
    if ((dirty_bits & ~(uint32_t)A840_SYNC_DIRTY_ALL) != 0u) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (!validate_resource_ids(resource_ids, resource_count)) {
        return A840_EMIT_INVALID_ARG;
    }
    if (dirty_bits == A840_SYNC_DIRTY_NONE) {
        return A840_EMIT_OK;
    }

    tracker->dirty_domains |= dirty_bits;
    tracker->active_work_since_wfi = true;
    tracker->clean_state &= ~(uint32_t)A840_SYNC_CLEAN_WFI;

    if ((dirty_bits & A840_SYNC_DIRTY_MEM_WRITE_CP) != 0u) {
        tracker->clean_state &= ~(uint32_t)A840_SYNC_CLEAN_MEM_WRITES;
    }
    if ((dirty_bits & (A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_BLIT_2D_WR |
                       A840_SYNC_DIRTY_GMEM_RESOLVE)) != 0u) {
        tracker->clean_state &= ~(uint32_t)A840_SYNC_CLEAN_CCU_COLOR;
    }
    /* Any GPU write can make previously cached UCHE texture/const lines stale. */
    tracker->clean_state &= ~(uint32_t)(A840_SYNC_CLEAN_UCHE_TEX | A840_SYNC_CLEAN_UCHE_CONST);
    tracker->uche_may_hold_stale_reads = true;

    if (resource_count == 0u) {
        /* Untracked/anonymous write forces conservative pass-level tracking. */
        tracker->overflow_conservative = true;
    } else {
        for (i = 0u; i < resource_count; ++i) {
            A840SyncResourceEntry *e = upsert_resource(tracker, resource_ids[i]);
            if (e) {
                e->dirty_mask |= dirty_bits;
                e->last_pass_id = tracker->current_pass_id;
            }
        }
    }
    return A840_EMIT_OK;
}

A840EmitStatus a840_sync_note_read(A840SyncTracker *tracker,
                                   uint32_t consumer_bits,
                                   const uint64_t *resource_ids,
                                   size_t resource_count)
{
    size_t i;
    if (!tracker) {
        return A840_EMIT_INVALID_ARG;
    }
    if ((consumer_bits & ~(uint32_t)A840_SYNC_CONSUME_ALL) != 0u) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (!validate_resource_ids(resource_ids, resource_count)) {
        return A840_EMIT_INVALID_ARG;
    }
    if (consumer_bits == A840_SYNC_CONSUME_NONE) {
        return A840_EMIT_OK;
    }

    tracker->active_work_since_wfi = true;
    tracker->clean_state &= ~(uint32_t)A840_SYNC_CLEAN_WFI;
    if ((consumer_bits & (A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_UCHE_CONST |
                          A840_SYNC_CONSUME_COMPUTE_RW)) != 0u) {
        tracker->uche_may_hold_stale_reads = true;
    }
    for (i = 0u; i < resource_count; ++i) {
        A840SyncResourceEntry *e = upsert_resource(tracker, resource_ids[i]);
        if (e) {
            e->read_mask |= consumer_bits;
            e->last_pass_id = tracker->current_pass_id;
        }
    }
    return A840_EMIT_OK;
}

void a840_sync_note_external_hazard(A840SyncTracker *tracker)
{
    if (!tracker) {
        return;
    }
    tracker->external_hazard_pending = true;
    tracker->dirty_domains |= A840_SYNC_DIRTY_ALL;
    tracker->clean_state = A840_SYNC_CLEAN_NONE;
    tracker->uche_may_hold_stale_reads = true;
    tracker->active_work_since_wfi = true;
}

static bool is_conservative_forced(const A840SyncTracker *tracker, uint32_t flags)
{
    return tracker->overflow_conservative ||
           tracker->external_hazard_pending ||
           ((flags & (A840_SYNC_FORCE_FULL_BARRIER |
                      A840_SYNC_EXTERNAL_IMPORT |
                      A840_SYNC_GLOBAL_MEMORY)) != 0u);
}

static void apply_emitted_cache_mask(A840SyncTracker *tracker,
                                     uint32_t emitted_mask,
                                     bool cleared_all_resources)
{
    uint32_t i;
    if ((emitted_mask & A840_CACHE_CCU_FLUSH_COLOR) != 0u) {
        uint32_t clear_bits = A840_SYNC_DIRTY_COLOR_CCU |
                              A840_SYNC_DIRTY_BLIT_2D_WR |
                              A840_SYNC_DIRTY_GMEM_RESOLVE;
        tracker->dirty_domains &= ~clear_bits;
        if (cleared_all_resources) {
            for (i = 0u; i < tracker->resource_count; ++i) {
                tracker->resources[i].dirty_mask &= ~clear_bits;
            }
        }
    }
    if ((emitted_mask & A840_CACHE_CCU_FLUSH_DEPTH) != 0u) {
        tracker->dirty_domains &= ~(uint32_t)A840_SYNC_DIRTY_DEPTH_CCU;
        if (cleared_all_resources) {
            for (i = 0u; i < tracker->resource_count; ++i) {
                tracker->resources[i].dirty_mask &= ~(uint32_t)A840_SYNC_DIRTY_DEPTH_CCU;
            }
        }
    }
    if ((emitted_mask & A840_CACHE_CCU_INV_COLOR) != 0u) {
        tracker->clean_state |= A840_SYNC_CLEAN_CCU_COLOR;
    }
    if ((emitted_mask & A840_CACHE_UCHE_INV) != 0u) {
        tracker->dirty_domains &= ~(uint32_t)A840_SYNC_DIRTY_UCHE_SHADER_WR;
        tracker->clean_state |= (A840_SYNC_CLEAN_UCHE_TEX | A840_SYNC_CLEAN_UCHE_CONST);
        tracker->uche_may_hold_stale_reads = false;
        if (cleared_all_resources) {
            for (i = 0u; i < tracker->resource_count; ++i) {
                tracker->resources[i].dirty_mask &= ~(uint32_t)A840_SYNC_DIRTY_UCHE_SHADER_WR;
                tracker->resources[i].read_mask &=
                    ~(uint32_t)(A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_UCHE_CONST);
            }
        }
    }
}

A840EmitStatus a840_sync_emit_wfi(A840SyncTracker *tracker,
                                  A840CmdStream *cs,
                                  uint32_t flags)
{
    A840EmitStatus st;
    bool force;
    if (!tracker || !cs) {
        return A840_EMIT_INVALID_ARG;
    }
    if (!flags_valid(flags)) {
        return A840_EMIT_RANGE_ERROR;
    }

    force = (tracker->mode == A840_SYNC_MODE_NAIVE) ||
            ((flags & (A840_SYNC_FORCE_FULL_BARRIER | A840_SYNC_EXTERNAL_IMPORT)) != 0u) ||
            tracker->external_hazard_pending;

    if (!force &&
        !tracker->active_work_since_wfi &&
        (tracker->clean_state & A840_SYNC_CLEAN_WFI) != 0u) {
        tracker->stats.wfi_elided++;
        tracker->stats.dwords_saved += A840_SYNC_WFI_DWORDS;
        return A840_EMIT_OK;
    }

    st = a840_emit_wfi(cs);
    if (st != A840_EMIT_OK) {
        return st;
    }
    tracker->active_work_since_wfi = false;
    tracker->clean_state |= A840_SYNC_CLEAN_WFI;
    tracker->stats.wfi_emitted++;
    tracker->stats.dwords_emitted += A840_SYNC_WFI_DWORDS;
    return A840_EMIT_OK;
}

A840EmitStatus a840_sync_emit_wait_mem_writes(A840SyncTracker *tracker,
                                              A840CmdStream *cs,
                                              uint32_t flags)
{
    A840EmitStatus st;
    bool force;
    uint32_t i;
    if (!tracker || !cs) {
        return A840_EMIT_INVALID_ARG;
    }
    if (!flags_valid(flags)) {
        return A840_EMIT_RANGE_ERROR;
    }

    force = (tracker->mode == A840_SYNC_MODE_NAIVE) ||
            ((flags & (A840_SYNC_FORCE_FULL_BARRIER | A840_SYNC_EXTERNAL_IMPORT)) != 0u) ||
            tracker->external_hazard_pending;

    if (!force &&
        (tracker->dirty_domains & A840_SYNC_DIRTY_MEM_WRITE_CP) == 0u &&
        (tracker->clean_state & A840_SYNC_CLEAN_MEM_WRITES) != 0u) {
        tracker->stats.wait_mem_elided++;
        tracker->stats.dwords_saved += A840_SYNC_WAIT_MEM_DWORDS;
        return A840_EMIT_OK;
    }

    st = a840_emit_wait_mem_writes(cs);
    if (st != A840_EMIT_OK) {
        return st;
    }
    tracker->dirty_domains &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
    tracker->clean_state |= A840_SYNC_CLEAN_MEM_WRITES;
    for (i = 0u; i < tracker->resource_count; ++i) {
        tracker->resources[i].dirty_mask &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
    }
    tracker->stats.wait_mem_emitted++;
    tracker->stats.dwords_emitted += A840_SYNC_WAIT_MEM_DWORDS;
    return A840_EMIT_OK;
}

/* Compute minimal required cache mask in TRACKED mode for a requested mask and
 * consumer domain set. Conservative fallback returns the full requested mask. */
static uint32_t compute_minimal_cache_mask(const A840SyncTracker *tracker,
                                           uint32_t requested_mask,
                                           uint32_t dirty_bits,
                                           uint32_t consumer_mask,
                                           uint32_t flags,
                                           bool *out_conservative)
{
    uint32_t needed = 0u;
    bool color_consumers;
    bool uche_consumers;
    bool conservative = (tracker->mode == A840_SYNC_MODE_NAIVE) ||
                        is_conservative_forced(tracker, flags);
    if (out_conservative) {
        *out_conservative = conservative;
    }
    if (conservative) {
        return requested_mask;
    }

    color_consumers = (consumer_mask == A840_SYNC_CONSUME_NONE) ||
                      ((consumer_mask & (A840_SYNC_CONSUME_UCHE_TEX |
                                         A840_SYNC_CONSUME_UCHE_CONST |
                                         A840_SYNC_CONSUME_COMPUTE_RW |
                                         A840_SYNC_CONSUME_BLIT_2D |
                                         A840_SYNC_CONSUME_CP_INDIRECT |
                                         A840_SYNC_CONSUME_HOST_PRESENT)) != 0u);
    uche_consumers = (consumer_mask == A840_SYNC_CONSUME_NONE) ||
                     ((consumer_mask & (A840_SYNC_CONSUME_UCHE_TEX |
                                        A840_SYNC_CONSUME_UCHE_CONST |
                                        A840_SYNC_CONSUME_COMPUTE_RW |
                                        A840_SYNC_CONSUME_BLIT_2D |
                                        A840_SYNC_CONSUME_CP_INDIRECT)) != 0u);

    /* 1. CCU color flush only when color/2D/resolve domain is dirty and needed. */
    if ((requested_mask & A840_CACHE_CCU_FLUSH_COLOR) != 0u) {
        bool color_dirty = (dirty_bits & (A840_SYNC_DIRTY_COLOR_CCU |
                                          A840_SYNC_DIRTY_BLIT_2D_WR |
                                          A840_SYNC_DIRTY_GMEM_RESOLVE)) != 0u;
        if (color_dirty && color_consumers) {
            needed |= A840_CACHE_CCU_FLUSH_COLOR;
        }
    }

    /* 2. CCU depth flush only when depth CCU is dirty. */
    if ((requested_mask & A840_CACHE_CCU_FLUSH_DEPTH) != 0u) {
        if ((dirty_bits & A840_SYNC_DIRTY_DEPTH_CCU) != 0u) {
            needed |= A840_CACHE_CCU_FLUSH_DEPTH;
        }
    }

    /* 3. CCU color invalidate only when CCU color may be stale vs external writes
     *    (shader/UCHE/CP writes) and CCU color is not already clean. */
    if ((requested_mask & A840_CACHE_CCU_INV_COLOR) != 0u) {
        bool external_color_writer = (dirty_bits & (A840_SYNC_DIRTY_UCHE_SHADER_WR |
                                                    A840_SYNC_DIRTY_MEM_WRITE_CP |
                                                    A840_SYNC_DIRTY_BLIT_2D_WR |
                                                    A840_SYNC_DIRTY_GMEM_RESOLVE)) != 0u;
        bool rt_consumer = (consumer_mask == A840_SYNC_CONSUME_NONE) ||
                           ((consumer_mask & A840_SYNC_CONSUME_COLOR_RT) != 0u);
        if (rt_consumer &&
            (external_color_writer || (tracker->clean_state & A840_SYNC_CLEAN_CCU_COLOR) == 0u)) {
            needed |= A840_CACHE_CCU_INV_COLOR;
        }
    }

    /* 4. UCHE invalidate when UCHE may hold stale reads for UCHE consumers,
     *    OR when UCHE shader writes must be flushed/invalidated out to main
     *    memory before non-UCHE consumers (such as RB/CCU color RT) read/modify it. */
    if ((requested_mask & A840_CACHE_UCHE_INV) != 0u) {
        bool shader_wr_dirty = (dirty_bits & A840_SYNC_DIRTY_UCHE_SHADER_WR) != 0u;
        bool any_writer = (dirty_bits != A840_SYNC_DIRTY_NONE);
        bool uche_already_clean =
            ((tracker->clean_state & (A840_SYNC_CLEAN_UCHE_TEX | A840_SYNC_CLEAN_UCHE_CONST)) ==
             (A840_SYNC_CLEAN_UCHE_TEX | A840_SYNC_CLEAN_UCHE_CONST)) &&
            !tracker->uche_may_hold_stale_reads;
        if (shader_wr_dirty ||
            (uche_consumers && !uche_already_clean && (any_writer || tracker->uche_may_hold_stale_reads))) {
            needed |= A840_CACHE_UCHE_INV;
        }
    }

    return needed;
}

static void record_cache_stats(A840SyncTracker *tracker,
                               uint32_t requested_mask,
                               uint32_t emitted_mask)
{
    uint32_t ccu_bits = A840_CACHE_CCU_FLUSH_COLOR |
                        A840_CACHE_CCU_FLUSH_DEPTH |
                        A840_CACHE_CCU_INV_COLOR;
    uint32_t ccu_req = popcount_u32(requested_mask & ccu_bits);
    uint32_t ccu_emit = popcount_u32(emitted_mask & ccu_bits);
    uint32_t uche_req = (requested_mask & A840_CACHE_UCHE_INV) ? 1u : 0u;
    uint32_t uche_emit = (emitted_mask & A840_CACHE_UCHE_INV) ? 1u : 0u;

    tracker->stats.ccu_flush_emitted += ccu_emit;
    tracker->stats.ccu_flush_elided += (ccu_req - ccu_emit);
    tracker->stats.uche_inv_emitted += uche_emit;
    tracker->stats.uche_inv_elided += (uche_req - uche_emit);
    tracker->stats.dwords_emitted += (uint64_t)(ccu_emit + uche_emit) * A840_SYNC_EVENT_DWORDS;
    tracker->stats.dwords_saved +=
        (uint64_t)((ccu_req - ccu_emit) + (uche_req - uche_emit)) * A840_SYNC_EVENT_DWORDS;
}

A840EmitStatus a840_sync_emit_cache_barrier(A840SyncTracker *tracker,
                                            A840CmdStream *cs,
                                            uint32_t requested_cache_mask,
                                            uint32_t consumer_mask,
                                            uint32_t flags)
{
    uint32_t emit_mask;
    bool conservative = false;
    bool need_wait_mem;
    bool need_wfi;
    uint32_t saved_dwords;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus st;

    if (!tracker || !cs) {
        return A840_EMIT_INVALID_ARG;
    }
    if ((requested_cache_mask & ~(uint32_t)A840_SYNC_VALID_CACHE_MASK) != 0u ||
        (consumer_mask & ~(uint32_t)A840_SYNC_CONSUME_ALL) != 0u ||
        !flags_valid(flags)) {
        return A840_EMIT_RANGE_ERROR;
    }

    emit_mask = compute_minimal_cache_mask(tracker, requested_cache_mask,
                                           tracker->dirty_domains, consumer_mask,
                                           flags, &conservative);
    need_wait_mem = ((flags & A840_SYNC_REQUIRE_WAIT_MEM) != 0u);
    need_wfi = ((flags & A840_SYNC_REQUIRE_WFI) != 0u) ||
               ((flags & A840_SYNC_FORCE_FULL_BARRIER) != 0u);

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    if (emit_mask != 0u) {
        st = a840_emit_cache_flush_inv(cs, emit_mask);
        if (st != A840_EMIT_OK) {
            cs->used_dwords = start_dwords;
            cs->reloc_count = start_relocs;
            return st;
        }
    }

    /* Emit optional wait_mem_writes / WFI transactionally. */
    {
        bool emit_wait_mem = false;
        bool emit_wfi = false;

        if (need_wait_mem) {
            emit_wait_mem = conservative ||
                            (tracker->dirty_domains & A840_SYNC_DIRTY_MEM_WRITE_CP) != 0u ||
                            (tracker->clean_state & A840_SYNC_CLEAN_MEM_WRITES) == 0u;
        }
        if (need_wfi) {
            emit_wfi = conservative ||
                       tracker->active_work_since_wfi ||
                       (tracker->clean_state & A840_SYNC_CLEAN_WFI) == 0u;
        }

        if (emit_wait_mem) {
            st = a840_emit_wait_mem_writes(cs);
            if (st != A840_EMIT_OK) {
                cs->used_dwords = start_dwords;
                cs->reloc_count = start_relocs;
                return st;
            }
        }
        if (emit_wfi) {
            st = a840_emit_wfi(cs);
            if (st != A840_EMIT_OK) {
                cs->used_dwords = start_dwords;
                cs->reloc_count = start_relocs;
                return st;
            }
        }

        /* Commit tracker state & statistics only after all emissions succeed. */
        if (conservative && tracker->mode == A840_SYNC_MODE_TRACKED) {
            tracker->stats.conservative_fallbacks++;
        }
        record_cache_stats(tracker, requested_cache_mask, emit_mask);
        apply_emitted_cache_mask(tracker, emit_mask, true);

        if (need_wait_mem) {
            if (emit_wait_mem) {
                uint32_t i;
                tracker->dirty_domains &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
                tracker->clean_state |= A840_SYNC_CLEAN_MEM_WRITES;
                for (i = 0u; i < tracker->resource_count; ++i) {
                    tracker->resources[i].dirty_mask &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
                }
                tracker->stats.wait_mem_emitted++;
                tracker->stats.dwords_emitted += A840_SYNC_WAIT_MEM_DWORDS;
            } else {
                tracker->stats.wait_mem_elided++;
                tracker->stats.dwords_saved += A840_SYNC_WAIT_MEM_DWORDS;
            }
        }
        if (need_wfi) {
            if (emit_wfi) {
                tracker->active_work_since_wfi = false;
                tracker->clean_state |= A840_SYNC_CLEAN_WFI;
                tracker->stats.wfi_emitted++;
                tracker->stats.dwords_emitted += A840_SYNC_WFI_DWORDS;
            } else {
                tracker->stats.wfi_elided++;
                tracker->stats.dwords_saved += A840_SYNC_WFI_DWORDS;
            }
        }
        /* Full conservative barrier retires external/overflow hazard state. */
        if (conservative &&
            emit_mask == requested_cache_mask &&
            (requested_cache_mask & (A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV)) ==
                (A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV) &&
            emit_wfi) {
            tracker->external_hazard_pending = false;
            if (tracker->overflow_conservative && tracker->dirty_domains == A840_SYNC_DIRTY_NONE) {
                tracker->overflow_conservative = false;
                tracker->resource_count = 0u;
            }
        }
        (void)saved_dwords;
    }

    return A840_EMIT_OK;
}

A840EmitStatus a840_sync_emit_pass_barrier(A840SyncTracker *tracker,
                                           A840CmdStream *cs,
                                           const A840SyncPassAccess *next_pass)
{
    const uint32_t naive_cache_mask = A840_CACHE_CCU_FLUSH_COLOR |
                                      A840_CACHE_CCU_FLUSH_DEPTH |
                                      A840_CACHE_CCU_INV_COLOR |
                                      A840_CACHE_UCHE_INV;
    uint32_t overlap_dirty = 0u;
    bool has_overlap = false;
    bool conservative;
    size_t i;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus st;

    if (!tracker || !cs || !next_pass) {
        return A840_EMIT_INVALID_ARG;
    }
    if ((next_pass->consumer_mask & ~(uint32_t)A840_SYNC_CONSUME_ALL) != 0u ||
        !flags_valid(next_pass->flags)) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (!validate_resource_ids(next_pass->read_ids, next_pass->read_count) ||
        !validate_resource_ids(next_pass->write_ids, next_pass->write_count)) {
        return A840_EMIT_INVALID_ARG;
    }

    tracker->current_pass_id++;

    /* NAIVE baseline: always emit full pass barrier (CCU flush/inv + UCHE inv +
     * wait_mem_writes + WFI) for direct A/B comparison. */
    if (tracker->mode == A840_SYNC_MODE_NAIVE) {
        uint32_t flags = next_pass->flags | A840_SYNC_REQUIRE_WAIT_MEM | A840_SYNC_REQUIRE_WFI;
        return a840_sync_emit_cache_barrier(tracker, cs, naive_cache_mask,
                                            next_pass->consumer_mask, flags);
    }

    /* Conservative fallback on force/global/external/overflow or anonymous pass. */
    conservative = is_conservative_forced(tracker, next_pass->flags) ||
                   (next_pass->read_count == 0u && next_pass->write_count == 0u);
    if (conservative) {
        uint32_t flags = next_pass->flags | A840_SYNC_FORCE_FULL_BARRIER |
                         A840_SYNC_REQUIRE_WAIT_MEM | A840_SYNC_REQUIRE_WFI;
        return a840_sync_emit_cache_barrier(tracker, cs, naive_cache_mask,
                                            next_pass->consumer_mask, flags);
    }

    /* Check resource-level RAW / WAW / WAR hazards against tracked entries. */
    for (i = 0u; i < next_pass->read_count; ++i) {
        const A840SyncResourceEntry *e = find_resource_const(tracker, next_pass->read_ids[i]);
        if (e && e->dirty_mask != A840_SYNC_DIRTY_NONE) {
            has_overlap = true;
            overlap_dirty |= e->dirty_mask;
        }
    }
    for (i = 0u; i < next_pass->write_count; ++i) {
        const A840SyncResourceEntry *e = find_resource_const(tracker, next_pass->write_ids[i]);
        if (e && (e->dirty_mask != A840_SYNC_DIRTY_NONE || e->read_mask != A840_SYNC_CONSUME_NONE)) {
            has_overlap = true;
            overlap_dirty |= e->dirty_mask;
        }
    }

    /* Disjoint resource sets with no global/external/overflow hazard:
     * safely elide the entire inter-pass barrier (0 PM4 dwords emitted). */
    if (!has_overlap) {
        tracker->stats.unrelated_pass_barriers_elided++;
        tracker->stats.ccu_flush_elided += 3u;
        tracker->stats.uche_inv_elided += 1u;
        tracker->stats.wait_mem_elided += 1u;
        tracker->stats.wfi_elided += 1u;
        tracker->stats.dwords_saved +=
            (4u * A840_SYNC_EVENT_DWORDS) + A840_SYNC_WAIT_MEM_DWORDS + A840_SYNC_WFI_DWORDS;
        return A840_EMIT_OK;
    }

    /* Overlapping resources detected: emit only the exact subset of cache
     * flushes/invalidates + waits needed for the overlapping dirty domains. */
    {
        uint32_t req_cache = 0u;
        uint32_t emit_cache = 0u;
        bool emit_wait_mem = false;
        bool emit_wfi = false;

        if ((overlap_dirty & (A840_SYNC_DIRTY_COLOR_CCU |
                              A840_SYNC_DIRTY_BLIT_2D_WR |
                              A840_SYNC_DIRTY_GMEM_RESOLVE)) != 0u) {
            req_cache |= A840_CACHE_CCU_FLUSH_COLOR;
        }
        if ((overlap_dirty & A840_SYNC_DIRTY_DEPTH_CCU) != 0u) {
            req_cache |= A840_CACHE_CCU_FLUSH_DEPTH;
        }
        if ((next_pass->consumer_mask & A840_SYNC_CONSUME_COLOR_RT) != 0u &&
            (overlap_dirty & (A840_SYNC_DIRTY_UCHE_SHADER_WR |
                              A840_SYNC_DIRTY_MEM_WRITE_CP |
                              A840_SYNC_DIRTY_BLIT_2D_WR)) != 0u) {
            req_cache |= A840_CACHE_CCU_INV_COLOR;
        }
        if ((next_pass->consumer_mask & (A840_SYNC_CONSUME_UCHE_TEX |
                                         A840_SYNC_CONSUME_UCHE_CONST |
                                         A840_SYNC_CONSUME_COMPUTE_RW |
                                         A840_SYNC_CONSUME_BLIT_2D |
                                         A840_SYNC_CONSUME_CP_INDIRECT)) != 0u ||
            (overlap_dirty & A840_SYNC_DIRTY_UCHE_SHADER_WR) != 0u) {
            req_cache |= A840_CACHE_UCHE_INV;
        }

        emit_cache = compute_minimal_cache_mask(tracker, req_cache, overlap_dirty,
                                                next_pass->consumer_mask, next_pass->flags, NULL);
        emit_wait_mem = ((overlap_dirty & A840_SYNC_DIRTY_MEM_WRITE_CP) != 0u) ||
                        ((next_pass->flags & A840_SYNC_REQUIRE_WAIT_MEM) != 0u &&
                         (tracker->dirty_domains & A840_SYNC_DIRTY_MEM_WRITE_CP) != 0u);
        /* WFI is required when ordering overlapping producer->consumer passes
         * unless a WFI has already drained all active work. */
        emit_wfi = tracker->active_work_since_wfi ||
                   ((next_pass->flags & A840_SYNC_REQUIRE_WFI) != 0u &&
                    (tracker->clean_state & A840_SYNC_CLEAN_WFI) == 0u);

        start_dwords = cs->used_dwords;
        start_relocs = cs->reloc_count;

        if (emit_cache != 0u) {
            st = a840_emit_cache_flush_inv(cs, emit_cache);
            if (st != A840_EMIT_OK) {
                cs->used_dwords = start_dwords;
                cs->reloc_count = start_relocs;
                return st;
            }
        }
        if (emit_wait_mem) {
            st = a840_emit_wait_mem_writes(cs);
            if (st != A840_EMIT_OK) {
                cs->used_dwords = start_dwords;
                cs->reloc_count = start_relocs;
                return st;
            }
        }
        if (emit_wfi) {
            st = a840_emit_wfi(cs);
            if (st != A840_EMIT_OK) {
                cs->used_dwords = start_dwords;
                cs->reloc_count = start_relocs;
                return st;
            }
        }

        /* Account stats against the naive full pass barrier baseline. */
        record_cache_stats(tracker, naive_cache_mask, emit_cache);
        apply_emitted_cache_mask(tracker, emit_cache, true);

        if (emit_wait_mem) {
            for (i = 0u; i < tracker->resource_count; ++i) {
                tracker->resources[i].dirty_mask &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
            }
            tracker->dirty_domains &= ~(uint32_t)A840_SYNC_DIRTY_MEM_WRITE_CP;
            tracker->clean_state |= A840_SYNC_CLEAN_MEM_WRITES;
            tracker->stats.wait_mem_emitted++;
            tracker->stats.dwords_emitted += A840_SYNC_WAIT_MEM_DWORDS;
        } else {
            tracker->stats.wait_mem_elided++;
            tracker->stats.dwords_saved += A840_SYNC_WAIT_MEM_DWORDS;
        }

        if (emit_wfi) {
            tracker->active_work_since_wfi = false;
            tracker->clean_state |= A840_SYNC_CLEAN_WFI;
            tracker->stats.wfi_emitted++;
            tracker->stats.dwords_emitted += A840_SYNC_WFI_DWORDS;
        } else {
            tracker->stats.wfi_elided++;
            tracker->stats.dwords_saved += A840_SYNC_WFI_DWORDS;
        }

        /* Clear WAR read marks on resources about to be overwritten. */
        for (i = 0u; i < next_pass->write_count; ++i) {
            A840SyncResourceEntry *e = find_resource(tracker, next_pass->write_ids[i]);
            if (e) {
                e->read_mask = A840_SYNC_CONSUME_NONE;
            }
        }
        recompute_aggregate_dirty(tracker);
    }

    return A840_EMIT_OK;
}
