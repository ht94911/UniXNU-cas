/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_cb_chunker.h"

#include <string.h>
#include <time.h>

#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

enum {
    A840_CHUNK_NOP_BATCH_DWORDS = 64u,
    A840_CHUNK_NS_PER_SEC = 1000000000u
};

static uint64_t monotonic_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * (uint64_t)A840_CHUNK_NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

static bool pass_kind_valid(A840ChunkPassKind kind)
{
    return kind == A840_CHUNK_PASS_RENDER_GMEM ||
           kind == A840_CHUNK_PASS_RENDER_SYSMEM ||
           kind == A840_CHUNK_PASS_COMPUTE ||
           kind == A840_CHUNK_PASS_BLIT;
}

static bool marker_va_valid(uint64_t va)
{
    const uint64_t va_limit = UINT64_C(1) << 48;
    if (va == 0u) {
        return true;
    }
    if (va >= va_limit || (va & 7u) != 0u || sizeof(uint64_t) > va_limit - va) {
        return false;
    }
    return true;
}

static void release_open_slot(A840CbChunker *chunker)
{
    if (chunker->open_slot_handle != A840_CMD_SLOT_INVALID_HANDLE) {
        (void)a840_cmd_slot_release(chunker->pool, chunker->open_slot_handle);
    }
    chunker->has_open_chunk = false;
    chunker->open_slot_handle = A840_CMD_SLOT_INVALID_HANDLE;
    chunker->open_slot = NULL;
    chunker->open_relocs = NULL;
    chunker->open_reloc_cap = 0u;
    chunker->open_desc = NULL;
    memset(&chunker->cs, 0, sizeof(chunker->cs));
    chunker->chunk_pass_count = 0u;
    chunker->chunk_est_gpu_us = 0u;
    chunker->chunk_est_cpu_us = 0u;
    chunker->chunk_surface_count = 0u;
}

static void abort_internal(A840CbChunker *chunker, bool count_aborted_chunk)
{
    if (!chunker->cb_aborted) {
        chunker->cb_aborted = true;
        chunker->stats.cbs_aborted++;
    }
    if (count_aborted_chunk) {
        chunker->stats.aborted_chunks++;
    }
    chunker->in_pass = false;
    chunker->in_gmem_pass = false;
    chunker->active_pass_kind = A840_CHUNK_PASS_NONE;
    chunker->last_chunk_status = A840_CHUNK_STATUS_ABORTED;
    release_open_slot(chunker);
}

static A840ChunkStatus open_next_chunk(A840CbChunker *chunker)
{
    A840ArenaStatus astatus;
    A840EmitStatus estatus;
    size_t reloc_cap;
    size_t usable_dwords;

    if (chunker->has_open_chunk) {
        return A840_CHUNK_OK;
    }

    astatus = a840_cmd_slot_acquire(chunker->pool,
                                    &chunker->open_slot_handle,
                                    &chunker->open_slot);
    if (astatus != A840_ARENA_OK || !chunker->open_slot) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_SLOT_EXHAUSTED;
    }

    if (chunker->open_slot->pm4_capacity_dwords <= A840_CHUNK_TAIL_RESERVE_DWORDS) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_CAPACITY;
    }

    reloc_cap = chunker->config.reloc_capacity_per_chunk
                    ? (size_t)chunker->config.reloc_capacity_per_chunk
                    : (size_t)A840_CHUNK_DEFAULT_RELOC_CAP;

    chunker->open_desc = (A840ChunkDesc *)a840_frame_arena_alloc(
        chunker->arena, sizeof(A840ChunkDesc), A840_FRAME_ARENA_ALIGN, &astatus);
    if (astatus != A840_ARENA_OK || !chunker->open_desc) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_ARENA_OOM;
    }

    chunker->open_relocs = (A840Reloc *)a840_frame_arena_alloc(
        chunker->arena, reloc_cap * sizeof(A840Reloc), A840_FRAME_ARENA_ALIGN, &astatus);
    if (astatus != A840_ARENA_OK || !chunker->open_relocs) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_ARENA_OOM;
    }

    usable_dwords = (size_t)chunker->open_slot->pm4_capacity_dwords -
                    (size_t)A840_CHUNK_TAIL_RESERVE_DWORDS;
    estatus = a840_cs_init(&chunker->cs,
                           chunker->open_slot->pm4_words,
                           usable_dwords,
                           chunker->open_relocs,
                           reloc_cap);
    if (estatus != A840_EMIT_OK) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_EMIT_FAILED;
    }

    chunker->open_reloc_cap = reloc_cap;
    chunker->has_open_chunk = true;
    chunker->chunk_pass_count = 0u;
    chunker->chunk_est_gpu_us = 0u;
    chunker->chunk_est_cpu_us = 0u;
    chunker->chunk_surface_count = 0u;
    chunker->last_chunk_status = A840_CHUNK_STATUS_RECORDING;
    return A840_CHUNK_OK;
}

static A840ChunkStatus add_chunk_surface(A840CbChunker *chunker, uint64_t surface_id)
{
    uint32_t i;
    if (surface_id == 0u) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    for (i = 0u; i < chunker->chunk_surface_count; ++i) {
        if (chunker->chunk_surfaces[i] == surface_id) {
            return A840_CHUNK_OK;
        }
    }
    if (chunker->chunk_surface_count >= A840_CHUNK_MAX_SURFACES_PER_CHUNK) {
        return A840_CHUNK_ERR_CAPACITY;
    }
    chunker->chunk_surfaces[chunker->chunk_surface_count++] = surface_id;
    return A840_CHUNK_OK;
}

static uint32_t compute_tail_cache_mask(const A840SyncTracker *sync, bool is_final)
{
    uint32_t dirty = sync->dirty_domains;
    uint32_t mask = 0u;

    if (sync->mode == A840_SYNC_MODE_NAIVE ||
        sync->overflow_conservative ||
        sync->external_hazard_pending) {
        if (dirty != 0u || is_final) {
            return A840_CACHE_CCU_FLUSH_COLOR |
                   A840_CACHE_CCU_FLUSH_DEPTH |
                   A840_CACHE_UCHE_INV;
        }
        return 0u;
    }

    if ((dirty & (A840_SYNC_DIRTY_COLOR_CCU |
                  A840_SYNC_DIRTY_GMEM_RESOLVE |
                  A840_SYNC_DIRTY_BLIT_2D_WR)) != 0u) {
        mask |= A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV;
    }
    if ((dirty & A840_SYNC_DIRTY_DEPTH_CCU) != 0u) {
        mask |= A840_CACHE_CCU_FLUSH_DEPTH | A840_CACHE_UCHE_INV;
    }
    if ((dirty & A840_SYNC_DIRTY_UCHE_SHADER_WR) != 0u) {
        mask |= A840_CACHE_UCHE_INV;
    }
    return mask;
}

static A840ChunkStatus seal_and_kick_chunk(A840CbChunker *chunker,
                                           bool is_final,
                                           bool barrier_only_final,
                                           A840ChunkKickReason reason)
{
    uint64_t seqno;
    uint64_t dep_seqno;
    uint32_t flags = A840_CHUNK_FLAG_NONE;
    uint32_t tail_cache_mask;
    uint32_t consumer_mask;
    size_t dwords_before_tail;
    A840EmitStatus estatus;
    A840ArenaStatus astatus;
    A840ChunkStatus kstatus;
    A840ChunkDesc *desc;
    A840CmdSlotHandle slot_handle;

    if (!chunker->has_open_chunk || !chunker->open_slot || !chunker->open_desc) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }

    /* Unlock reserved tail headroom so coherency and seqno marker always fit. */
    chunker->cs.capacity_dwords = (size_t)chunker->open_slot->pm4_capacity_dwords;

    seqno = chunker->next_chunk_seqno + 1u;
    dep_seqno = chunker->prev_cb_chunk_seqno;

    if (is_final) {
        flags |= A840_CHUNK_FLAG_FINAL;
    } else {
        flags |= A840_CHUNK_FLAG_EARLY_KICK;
    }
    if (dep_seqno != 0u) {
        flags |= A840_CHUNK_FLAG_HAS_ORDER_DEP;
    }
    if (reason == A840_CHUNK_KICK_MANUAL) {
        flags |= A840_CHUNK_FLAG_MANUAL_KICK;
    }
    if (barrier_only_final) {
        flags |= A840_CHUNK_FLAG_FINAL_BARRIER_ONLY;
    }

    dwords_before_tail = chunker->cs.used_dwords;
    tail_cache_mask = compute_tail_cache_mask(&chunker->sync, is_final);
    consumer_mask = is_final
                        ? (A840_SYNC_CONSUME_UCHE_TEX |
                           A840_SYNC_CONSUME_COLOR_RT |
                           A840_SYNC_CONSUME_HOST_PRESENT)
                        : (A840_SYNC_CONSUME_UCHE_TEX |
                           A840_SYNC_CONSUME_COMPUTE_RW |
                           A840_SYNC_CONSUME_COLOR_RT);

    if (tail_cache_mask != 0u) {
        estatus = a840_sync_emit_cache_barrier(&chunker->sync,
                                               &chunker->cs,
                                               tail_cache_mask,
                                               consumer_mask,
                                               A840_SYNC_FLAG_NONE);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
    }

    if ((chunker->sync.dirty_domains & A840_SYNC_DIRTY_MEM_WRITE_CP) != 0u ||
        tail_cache_mask != 0u) {
        estatus = a840_sync_emit_wait_mem_writes(&chunker->sync,
                                                 &chunker->cs,
                                                 A840_SYNC_FLAG_NONE);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
    }

    if (chunker->cs.used_dwords > dwords_before_tail) {
        flags |= A840_CHUNK_FLAG_HAS_SYNC_TAIL;
    }

    if (chunker->config.marker_gpu_va != 0u) {
        estatus = a840_emit_timestamp_event(&chunker->cs,
                                            A840_EVENT_CACHE_FLUSH_TS,
                                            chunker->config.marker_bo_handle,
                                            chunker->config.marker_gpu_va,
                                            seqno);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
        flags |= A840_CHUNK_FLAG_HAS_SEQNO_MARKER;
    } else {
        uint32_t marker_words[2];
        marker_words[0] = (uint32_t)(seqno & UINT32_MAX);
        marker_words[1] = (uint32_t)(dep_seqno & UINT32_MAX);
        estatus = a840_cs_emit_pkt7(&chunker->cs, A840_CP_NOP, marker_words, 2u);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
        flags |= A840_CHUNK_FLAG_HAS_SEQNO_MARKER;
    }

    chunker->open_slot->pm4_used_dwords = (uint32_t)chunker->cs.used_dwords;
    slot_handle = chunker->open_slot_handle;

    astatus = a840_cmd_slot_mark_submitted(chunker->pool, slot_handle, seqno);
    if (astatus != A840_ARENA_OK) {
        abort_internal(chunker, true);
        return A840_CHUNK_ERR_INVALID_STATE;
    }

    desc = chunker->open_desc;
    memset(desc, 0, sizeof(*desc));
    desc->frame_id = chunker->frame_id;
    desc->cb_id = chunker->cb_id;
    desc->chunk_index = chunker->next_chunk_index;
    desc->ring_id = chunker->config.ring_id;
    desc->chunk_seqno = seqno;
    desc->dep_chunk_seqno = dep_seqno;
    desc->flags = flags;
    desc->status = A840_CHUNK_STATUS_KICKED;
    desc->kick_reason = reason;
    desc->pass_count = chunker->chunk_pass_count;
    desc->pm4_words = chunker->open_slot->pm4_words;
    desc->pm4_dwords = (uint32_t)chunker->cs.used_dwords;
    desc->relocs = chunker->open_relocs;
    desc->reloc_count = (uint32_t)chunker->cs.reloc_count;
    desc->est_gpu_us = chunker->chunk_est_gpu_us;
    desc->est_cpu_us = chunker->chunk_est_cpu_us;
    desc->slot_handle = slot_handle;
    desc->written_surface_count = chunker->chunk_surface_count;
    if (chunker->chunk_surface_count > 0u) {
        memcpy(desc->written_surface_ids,
               chunker->chunk_surfaces,
               (size_t)chunker->chunk_surface_count * sizeof(uint64_t));
    }

    if (chunker->config.kick_fn) {
        kstatus = chunker->config.kick_fn(chunker->config.kick_ctx, desc);
        if (kstatus != A840_CHUNK_OK) {
            desc->status = A840_CHUNK_STATUS_ABORTED;
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_KICK_FAILED;
        }
    }

    /* Commit chunk sequence and update discrete-event CPU/GPU overlap model. */
    chunker->next_chunk_seqno = seqno;
    chunker->prev_cb_chunk_seqno = seqno;
    chunker->next_chunk_index++;
    chunker->cb_chunks_kicked++;
    chunker->stats.chunks_kicked++;
    chunker->stats.total_pm4_dwords += (uint64_t)desc->pm4_dwords;

    if (is_final) {
        chunker->cb_final_kicked = true;
        chunker->stats.final_kicks++;
    } else {
        chunker->cb_early_kicks++;
        chunker->stats.early_kicks++;
    }

    if (reason == A840_CHUNK_KICK_THRESHOLD_PASSES) {
        chunker->stats.threshold_pass_kicks++;
    } else if (reason == A840_CHUNK_KICK_THRESHOLD_DWORDS) {
        chunker->stats.threshold_dword_kicks++;
    } else if (reason == A840_CHUNK_KICK_THRESHOLD_GPU_US) {
        chunker->stats.threshold_gpu_us_kicks++;
    } else if (reason == A840_CHUNK_KICK_MANUAL) {
        chunker->stats.manual_kicks++;
    }

    chunker->sim_cpu_accum_us += (uint64_t)desc->est_cpu_us;
    chunker->sim_gpu_sum_us += (uint64_t)desc->est_gpu_us;
    if (chunker->sim_cpu_accum_us > chunker->sim_gpu_finish_us) {
        chunker->sim_gpu_finish_us = chunker->sim_cpu_accum_us + (uint64_t)desc->est_gpu_us;
    } else {
        chunker->sim_gpu_finish_us += (uint64_t)desc->est_gpu_us;
    }

    /* Slot is now owned by GPU pipeline until fence retire; clear local ref. */
    chunker->has_open_chunk = false;
    chunker->open_slot_handle = A840_CMD_SLOT_INVALID_HANDLE;
    chunker->open_slot = NULL;
    chunker->open_relocs = NULL;
    chunker->open_reloc_cap = 0u;
    chunker->open_desc = NULL;
    memset(&chunker->cs, 0, sizeof(chunker->cs));
    chunker->chunk_pass_count = 0u;
    chunker->chunk_est_gpu_us = 0u;
    chunker->chunk_est_cpu_us = 0u;
    chunker->chunk_surface_count = 0u;
    chunker->last_chunk_status = A840_CHUNK_STATUS_KICKED;
    return A840_CHUNK_OK;
}

static A840ChunkKickReason evaluate_early_kick_trigger(const A840CbChunker *chunker)
{
    uint32_t max_passes = chunker->config.max_passes_per_chunk;
    uint32_t max_dwords = chunker->config.max_dwords_per_chunk;
    uint32_t max_gpu_us = chunker->config.max_est_gpu_us_per_chunk;

    if (max_passes > 0u && chunker->chunk_pass_count >= max_passes) {
        return A840_CHUNK_KICK_THRESHOLD_PASSES;
    }
    if (max_dwords > 0u && chunker->cs.used_dwords >= (size_t)max_dwords) {
        return A840_CHUNK_KICK_THRESHOLD_DWORDS;
    }
    if (max_gpu_us > 0u && chunker->chunk_est_gpu_us >= max_gpu_us) {
        return A840_CHUNK_KICK_THRESHOLD_GPU_US;
    }
    if (chunker->open_slot != NULL) {
        size_t usable = (size_t)chunker->open_slot->pm4_capacity_dwords -
                        (size_t)A840_CHUNK_TAIL_RESERVE_DWORDS;
        if (usable > 0u && chunker->cs.used_dwords >= (usable * 3u) / 4u) {
            return A840_CHUNK_KICK_SLOT_WATERMARK;
        }
    }
    return A840_CHUNK_KICK_NONE;
}

void a840_cb_chunker_default_config(A840ChunkerConfig *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_passes_per_chunk = A840_CHUNK_DEFAULT_MAX_PASSES;
    cfg->max_dwords_per_chunk = A840_CHUNK_DEFAULT_MAX_DWORDS;
    cfg->max_est_gpu_us_per_chunk = A840_CHUNK_DEFAULT_MAX_EST_GPU_US;
    cfg->reloc_capacity_per_chunk = A840_CHUNK_DEFAULT_RELOC_CAP;
    cfg->ring_id = 0u;
    cfg->sync_mode = A840_SYNC_MODE_TRACKED;
}

A840ChunkStatus a840_cb_chunker_init(A840CbChunker *chunker,
                                     A840FrameArena *arena,
                                     A840CmdSlotPool *pool,
                                     const A840ChunkerConfig *cfg)
{
    A840ChunkerConfig local_cfg;
    A840EmitStatus estatus;

    if (!chunker || !arena || !pool) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (cfg) {
        local_cfg = *cfg;
    } else {
        a840_cb_chunker_default_config(&local_cfg);
    }
    if (local_cfg.ring_id > A840_CHUNK_MAX_RING_ID) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (local_cfg.sync_mode != A840_SYNC_MODE_TRACKED &&
        local_cfg.sync_mode != A840_SYNC_MODE_NAIVE) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!marker_va_valid(local_cfg.marker_gpu_va)) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (pool->pm4_dwords_per_slot <= A840_CHUNK_TAIL_RESERVE_DWORDS) {
        return A840_CHUNK_ERR_CAPACITY;
    }

    memset(chunker, 0, sizeof(*chunker));
    estatus = a840_sync_tracker_init(&chunker->sync, local_cfg.sync_mode);
    if (estatus != A840_EMIT_OK) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }

    chunker->arena = arena;
    chunker->pool = pool;
    chunker->config = local_cfg;
    chunker->open_slot_handle = A840_CMD_SLOT_INVALID_HANDLE;
    chunker->last_chunk_status = A840_CHUNK_STATUS_IDLE;
    chunker->initialized = true;
    return A840_CHUNK_OK;
}

A840ChunkStatus a840_cb_chunker_begin_cb(A840CbChunker *chunker,
                                         uint64_t frame_id,
                                         uint64_t cb_id)
{
    if (!chunker || !chunker->initialized || cb_id == 0u) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }

    a840_sync_tracker_reset(&chunker->sync);
    a840_sync_tracker_set_mode(&chunker->sync, chunker->config.sync_mode);

    chunker->cb_active = true;
    chunker->in_pass = false;
    chunker->in_gmem_pass = false;
    chunker->has_open_chunk = false;
    chunker->cb_aborted = false;
    chunker->cb_final_kicked = false;
    chunker->last_chunk_status = A840_CHUNK_STATUS_RECORDING;

    chunker->frame_id = frame_id;
    chunker->cb_id = cb_id;
    chunker->cb_start_ns = monotonic_now_ns();
    chunker->prev_cb_chunk_seqno = 0u;
    chunker->next_chunk_index = 0u;
    chunker->cb_chunks_kicked = 0u;
    chunker->cb_early_kicks = 0u;
    chunker->cb_total_passes = 0u;
    chunker->sim_cpu_accum_us = 0u;
    chunker->sim_gpu_sum_us = 0u;
    chunker->sim_gpu_finish_us = 0u;
    chunker->active_pass_kind = A840_CHUNK_PASS_NONE;
    return A840_CHUNK_OK;
}

A840ChunkStatus a840_cb_chunker_begin_pass(A840CbChunker *chunker,
                                           const A840ChunkPassDesc *pass)
{
    A840ChunkStatus cstatus;
    A840EmitStatus estatus;
    size_t i;

    if (!chunker || !chunker->initialized || !pass) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->cb_aborted) {
        return A840_CHUNK_ERR_ABORTED;
    }
    if (chunker->cb_final_kicked) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->in_pass) {
        return A840_CHUNK_ERR_PASS_ACTIVE;
    }
    if (!pass_kind_valid(pass->kind)) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if ((pass->sync_access.read_count > A840_SYNC_MAX_PASS_RESOURCES) ||
        (pass->sync_access.write_count > A840_SYNC_MAX_PASS_RESOURCES) ||
        (pass->written_surface_count > A840_CHUNK_MAX_SURFACES_PER_CHUNK)) {
        return A840_CHUNK_ERR_CAPACITY;
    }
    if ((pass->written_surface_count > 0u && !pass->written_surface_ids) ||
        (pass->sync_access.read_count > 0u && !pass->sync_access.read_ids) ||
        (pass->sync_access.write_count > 0u && !pass->sync_access.write_ids)) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }

    cstatus = open_next_chunk(chunker);
    if (cstatus != A840_CHUNK_OK) {
        return cstatus;
    }

    /* Validate written surfaces before mutating chunk/sync state. */
    for (i = 0u; i < pass->written_surface_count; ++i) {
        if (pass->written_surface_ids[i] == 0u) {
            return A840_CHUNK_ERR_INVALID_ARG;
        }
    }

    /* Emit only required pass-entry coherence barriers via A840SyncTracker. */
    if (pass->sync_access.read_count > 0u ||
        pass->sync_access.write_count > 0u ||
        pass->sync_access.consumer_mask != 0u ||
        pass->sync_access.flags != 0u) {
        estatus = a840_sync_emit_pass_barrier(&chunker->sync,
                                              &chunker->cs,
                                              &pass->sync_access);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
    }

    for (i = 0u; i < pass->written_surface_count; ++i) {
        cstatus = add_chunk_surface(chunker, pass->written_surface_ids[i]);
        if (cstatus != A840_CHUNK_OK) {
            return cstatus;
        }
    }

    chunker->active_pass_kind = pass->kind;
    chunker->active_pass_dirty_mask = pass->produce_dirty_mask;
    chunker->active_pass_est_gpu_us = pass->est_gpu_us;
    chunker->active_pass_est_cpu_us = pass->est_cpu_us;
    chunker->active_pass_is_final = pass->is_final_pass;
    chunker->active_pass_consumer_mask = pass->sync_access.consumer_mask;

    chunker->active_pass_write_count = (uint32_t)pass->sync_access.write_count;
    if (pass->sync_access.write_count > 0u) {
        memcpy(chunker->active_pass_write_ids,
               pass->sync_access.write_ids,
               pass->sync_access.write_count * sizeof(uint64_t));
    }
    chunker->active_pass_read_count = (uint32_t)pass->sync_access.read_count;
    if (pass->sync_access.read_count > 0u) {
        memcpy(chunker->active_pass_read_ids,
               pass->sync_access.read_ids,
               pass->sync_access.read_count * sizeof(uint64_t));
    }

    chunker->in_pass = true;
    chunker->in_gmem_pass = (pass->kind == A840_CHUNK_PASS_RENDER_GMEM);
    return A840_CHUNK_OK;
}

A840CmdStream *a840_cb_chunker_cmd_stream(A840CbChunker *chunker)
{
    if (!chunker || !chunker->initialized || !chunker->cb_active ||
        chunker->cb_aborted || !chunker->in_pass || !chunker->has_open_chunk) {
        return NULL;
    }
    return &chunker->cs;
}

A840ChunkStatus a840_cb_chunker_emit_pass_nop(A840CbChunker *chunker,
                                              uint32_t payload_dwords,
                                              uint32_t tag)
{
    uint32_t batch[A840_CHUNK_NOP_BATCH_DWORDS];
    uint32_t remaining = payload_dwords;
    uint32_t offset = 0u;

    if (!chunker || !chunker->initialized) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->cb_aborted) {
        return A840_CHUNK_ERR_ABORTED;
    }
    if (!chunker->in_pass || !chunker->has_open_chunk) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }

    if (payload_dwords == 0u) {
        A840EmitStatus st = a840_cs_emit_pkt7(&chunker->cs, A840_CP_NOP, NULL, 0u);
        return (st == A840_EMIT_OK) ? A840_CHUNK_OK : A840_CHUNK_ERR_EMIT_FAILED;
    }

    while (remaining > 0u) {
        uint32_t n = remaining > A840_CHUNK_NOP_BATCH_DWORDS
                         ? A840_CHUNK_NOP_BATCH_DWORDS
                         : remaining;
        uint32_t i;
        A840EmitStatus st;
        for (i = 0u; i < n; ++i) {
            batch[i] = tag + offset + i;
        }
        st = a840_cs_emit_pkt7(&chunker->cs, A840_CP_NOP, batch, n);
        if (st != A840_EMIT_OK) {
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
        remaining -= n;
        offset += n;
    }
    return A840_CHUNK_OK;
}

A840ChunkStatus a840_cb_chunker_emit_pass_timestamp(A840CbChunker *chunker,
                                                    uint32_t event_id,
                                                    uint64_t bo_handle,
                                                    uint64_t gpu_va,
                                                    uint64_t payload)
{
    A840EmitStatus st;
    if (!chunker || !chunker->initialized) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->cb_aborted) {
        return A840_CHUNK_ERR_ABORTED;
    }
    if (!chunker->in_pass || !chunker->has_open_chunk) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    st = a840_emit_timestamp_event(&chunker->cs, event_id, bo_handle, gpu_va, payload);
    return (st == A840_EMIT_OK) ? A840_CHUNK_OK : A840_CHUNK_ERR_EMIT_FAILED;
}

A840ChunkStatus a840_cb_chunker_end_pass(A840CbChunker *chunker)
{
    A840EmitStatus estatus;
    A840ChunkKickReason reason;
    bool is_final_pass;

    if (!chunker || !chunker->initialized) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->cb_aborted) {
        return A840_CHUNK_ERR_ABORTED;
    }
    if (!chunker->in_pass || !chunker->has_open_chunk) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }

    if (chunker->active_pass_read_count > 0u && chunker->active_pass_consumer_mask != 0u) {
        estatus = a840_sync_note_read(&chunker->sync,
                                      chunker->active_pass_consumer_mask,
                                      chunker->active_pass_read_ids,
                                      chunker->active_pass_read_count);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
    }

    if (chunker->active_pass_dirty_mask != 0u) {
        estatus = a840_sync_note_write(&chunker->sync,
                                       chunker->active_pass_dirty_mask,
                                       chunker->active_pass_write_count > 0u
                                           ? chunker->active_pass_write_ids
                                           : NULL,
                                       chunker->active_pass_write_count);
        if (estatus != A840_EMIT_OK) {
            abort_internal(chunker, true);
            return A840_CHUNK_ERR_EMIT_FAILED;
        }
    }

    is_final_pass = chunker->active_pass_is_final;
    chunker->chunk_pass_count++;
    chunker->chunk_est_gpu_us += chunker->active_pass_est_gpu_us;
    chunker->chunk_est_cpu_us += chunker->active_pass_est_cpu_us;
    chunker->cb_total_passes++;
    chunker->stats.total_passes_encoded++;

    /* Safe pass boundary reached: close GMEM/pass guard BEFORE evaluating kick. */
    chunker->in_pass = false;
    chunker->in_gmem_pass = false;
    chunker->active_pass_kind = A840_CHUNK_PASS_NONE;
    chunker->active_pass_is_final = false;

    if (is_final_pass) {
        return seal_and_kick_chunk(chunker, true, false, A840_CHUNK_KICK_FINAL);
    }

    reason = evaluate_early_kick_trigger(chunker);
    if (reason != A840_CHUNK_KICK_NONE) {
        return seal_and_kick_chunk(chunker, false, false, reason);
    }

    return A840_CHUNK_OK;
}

A840ChunkStatus a840_cb_chunker_force_kick(A840CbChunker *chunker)
{
    if (!chunker || !chunker->initialized) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->cb_aborted) {
        return A840_CHUNK_ERR_ABORTED;
    }
    if (chunker->in_gmem_pass) {
        chunker->stats.rejected_mid_gmem_kicks++;
        return A840_CHUNK_ERR_MID_GMEM_PASS;
    }
    if (chunker->in_pass) {
        chunker->stats.rejected_mid_pass_kicks++;
        return A840_CHUNK_ERR_MID_PASS;
    }
    if (!chunker->has_open_chunk || chunker->chunk_pass_count == 0u) {
        return A840_CHUNK_OK;
    }
    return seal_and_kick_chunk(chunker, false, false, A840_CHUNK_KICK_MANUAL);
}

void a840_cb_chunker_abort(A840CbChunker *chunker)
{
    if (!chunker || !chunker->initialized || !chunker->cb_active) {
        return;
    }
    abort_internal(chunker, chunker->has_open_chunk);
}

A840ChunkStatus a840_cb_chunker_finish(A840CbChunker *chunker)
{
    A840ChunkStatus status = A840_CHUNK_OK;
    uint64_t end_ns;
    uint64_t elapsed_ns;
    uint64_t mono_total;

    if (!chunker || !chunker->initialized) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (!chunker->cb_active) {
        return A840_CHUNK_ERR_INVALID_STATE;
    }
    if (chunker->in_gmem_pass) {
        chunker->stats.rejected_mid_gmem_kicks++;
        return A840_CHUNK_ERR_MID_GMEM_PASS;
    }
    if (chunker->in_pass) {
        chunker->stats.rejected_mid_pass_kicks++;
        return A840_CHUNK_ERR_MID_PASS;
    }
    if (chunker->cb_aborted) {
        release_open_slot(chunker);
        chunker->cb_active = false;
        return A840_CHUNK_ERR_ABORTED;
    }

    if (chunker->has_open_chunk && chunker->chunk_pass_count > 0u) {
        status = seal_and_kick_chunk(chunker, true, false, A840_CHUNK_KICK_FINAL);
        if (status != A840_CHUNK_OK) {
            chunker->cb_active = false;
            return status;
        }
    } else if (!chunker->cb_final_kicked && chunker->cb_chunks_kicked > 0u) {
        /* Last pass triggered an early kick without is_final_pass hint; seal a
         * minimal valid A8XX finalization chunk with A840_CHUNK_FLAG_FINAL. */
        status = open_next_chunk(chunker);
        if (status != A840_CHUNK_OK) {
            chunker->cb_active = false;
            return status;
        }
        status = seal_and_kick_chunk(chunker, true, true, A840_CHUNK_KICK_FINAL);
        if (status != A840_CHUNK_OK) {
            chunker->cb_active = false;
            return status;
        }
    } else {
        release_open_slot(chunker);
    }

    end_ns = monotonic_now_ns();
    elapsed_ns = (end_ns >= chunker->cb_start_ns) ? (end_ns - chunker->cb_start_ns) : 0u;

    mono_total = chunker->sim_cpu_accum_us + chunker->sim_gpu_sum_us;
    chunker->stats.last_cb_chunks_kicked = chunker->cb_chunks_kicked;
    chunker->stats.last_cb_early_kicks = chunker->cb_early_kicks;
    chunker->stats.monolithic_cpu_wait_us = chunker->sim_cpu_accum_us;
    chunker->stats.monolithic_total_us = mono_total;
    chunker->stats.chunked_critical_path_us = chunker->sim_gpu_finish_us;
    chunker->stats.gpu_overlap_gain_us =
        (mono_total >= chunker->sim_gpu_finish_us)
            ? (mono_total - chunker->sim_gpu_finish_us)
            : 0u;
    chunker->stats.single_cb_encode_ns = elapsed_ns;
    chunker->stats.single_cb_encode_us = (double)elapsed_ns / 1000.0;
    chunker->stats.heap_allocs = 0u;
    chunker->stats.cbs_completed++;

    chunker->cb_active = false;
    return A840_CHUNK_OK;
}

A840ChunkerStats a840_cb_chunker_stats(const A840CbChunker *chunker)
{
    A840ChunkerStats empty;
    memset(&empty, 0, sizeof(empty));
    if (!chunker) {
        return empty;
    }
    return chunker->stats;
}

A840SyncStats a840_cb_chunker_sync_stats(const A840CbChunker *chunker)
{
    A840SyncStats empty;
    memset(&empty, 0, sizeof(empty));
    if (!chunker) {
        return empty;
    }
    return a840_sync_tracker_stats(&chunker->sync);
}

bool a840_cb_chunker_is_in_gmem_pass(const A840CbChunker *chunker)
{
    return chunker != NULL && chunker->in_gmem_pass;
}

bool a840_cb_chunker_is_aborted(const A840CbChunker *chunker)
{
    return chunker != NULL && chunker->cb_aborted;
}
