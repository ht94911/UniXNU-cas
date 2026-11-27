/* SPDX-License-Identifier: MIT */
/* Adreno 840 Native Metal Driver — Long CommandBuffer Pipeline Chunking Engine
 * (Phase 5 / Wave 10 Lane 3: 120Hz / 8.333 ms Early-Kick CPU/GPU Overlap)
 *
 * Pure C11, zero steady-state heap allocation. Splits long multi-pass Metal
 * CommandBuffers into hardware-ordered PM4 chunks strictly at safe Pass /
 * Compute / Blit boundaries (never mid-GMEM tile loop), reusing A840FrameArena
 * and A840CmdSlotPool with A840SyncTracker cross-chunk cache coherency.
 *
 * Strictly emits genuine A8XX PM4 packets via kits/Adreno840Emit APIs and
 * never writes CP_PROTECT registers or raw hex register offsets.
 */
#ifndef A840_CB_CHUNKER_H
#define A840_CB_CHUNKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_frame_arena.h"
#include "a840_sync_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_CHUNK_DEFAULT_MAX_PASSES = 2u,
    A840_CHUNK_DEFAULT_MAX_DWORDS = 512u,
    A840_CHUNK_DEFAULT_MAX_EST_GPU_US = 1500u,
    A840_CHUNK_TAIL_RESERVE_DWORDS = 32u,
    A840_CHUNK_DEFAULT_RELOC_CAP = 64u,
    A840_CHUNK_MAX_SURFACES_PER_CHUNK = 32u,
    A840_CHUNK_MAX_RING_ID = 3u
};

typedef enum A840ChunkStatus {
    A840_CHUNK_OK = 0,
    A840_CHUNK_ERR_INVALID_ARG = 1,
    A840_CHUNK_ERR_INVALID_STATE = 2,
    A840_CHUNK_ERR_MID_GMEM_PASS = 3,
    A840_CHUNK_ERR_MID_PASS = 4,
    A840_CHUNK_ERR_PASS_ACTIVE = 5,
    A840_CHUNK_ERR_SLOT_EXHAUSTED = 6,
    A840_CHUNK_ERR_ARENA_OOM = 7,
    A840_CHUNK_ERR_EMIT_FAILED = 8,
    A840_CHUNK_ERR_KICK_FAILED = 9,
    A840_CHUNK_ERR_ABORTED = 10,
    A840_CHUNK_ERR_CAPACITY = 11
} A840ChunkStatus;

typedef enum A840ChunkLifecycleStatus {
    A840_CHUNK_STATUS_IDLE = 0,
    A840_CHUNK_STATUS_RECORDING = 1,
    A840_CHUNK_STATUS_KICKED = 2,
    A840_CHUNK_STATUS_RETIRED = 3,
    A840_CHUNK_STATUS_ABORTED = 4
} A840ChunkLifecycleStatus;

typedef enum A840ChunkFlags {
    A840_CHUNK_FLAG_NONE = 0u,
    A840_CHUNK_FLAG_EARLY_KICK = 1u << 0,
    A840_CHUNK_FLAG_FINAL = 1u << 1,
    A840_CHUNK_FLAG_HAS_ORDER_DEP = 1u << 2,
    A840_CHUNK_FLAG_HAS_SYNC_TAIL = 1u << 3,
    A840_CHUNK_FLAG_HAS_SEQNO_MARKER = 1u << 4,
    A840_CHUNK_FLAG_MANUAL_KICK = 1u << 5,
    A840_CHUNK_FLAG_FINAL_BARRIER_ONLY = 1u << 6
} A840ChunkFlags;

typedef enum A840ChunkPassKind {
    A840_CHUNK_PASS_NONE = 0u,
    A840_CHUNK_PASS_RENDER_GMEM = 1u,
    A840_CHUNK_PASS_RENDER_SYSMEM = 2u,
    A840_CHUNK_PASS_COMPUTE = 3u,
    A840_CHUNK_PASS_BLIT = 4u
} A840ChunkPassKind;

typedef enum A840ChunkKickReason {
    A840_CHUNK_KICK_NONE = 0u,
    A840_CHUNK_KICK_THRESHOLD_PASSES = 1u,
    A840_CHUNK_KICK_THRESHOLD_DWORDS = 2u,
    A840_CHUNK_KICK_THRESHOLD_GPU_US = 3u,
    A840_CHUNK_KICK_SLOT_WATERMARK = 4u,
    A840_CHUNK_KICK_MANUAL = 5u,
    A840_CHUNK_KICK_FINAL = 6u
} A840ChunkKickReason;

typedef struct A840ChunkPassDesc {
    A840ChunkPassKind kind;
    A840SyncPassAccess sync_access;
    uint32_t produce_dirty_mask;     /* A840SyncDirtyBits produced by this pass */
    uint32_t est_gpu_us;             /* Estimated GPU execution time (us) */
    uint32_t est_cpu_us;             /* Estimated CPU encoding time (us) */
    const uint64_t *written_surface_ids;
    size_t written_surface_count;
    bool is_final_pass;              /* Optional hint: final pass in CB */
} A840ChunkPassDesc;

typedef struct A840ChunkDesc {
    uint64_t frame_id;
    uint64_t cb_id;
    uint32_t chunk_index;
    uint32_t ring_id;
    uint64_t chunk_seqno;            /* Strictly monotonic chunk sequence number */
    uint64_t dep_chunk_seqno;        /* Prior chunk_seqno in CB (0 for chunk 0) */
    uint32_t flags;                  /* Bitmask of A840ChunkFlags */
    A840ChunkLifecycleStatus status;
    A840ChunkKickReason kick_reason;
    uint32_t pass_count;
    const uint32_t *pm4_words;
    uint32_t pm4_dwords;
    const A840Reloc *relocs;
    uint32_t reloc_count;
    uint32_t est_gpu_us;
    uint32_t est_cpu_us;
    A840CmdSlotHandle slot_handle;
    uint32_t written_surface_count;
    uint64_t written_surface_ids[A840_CHUNK_MAX_SURFACES_PER_CHUNK];
} A840ChunkDesc;

/* Callback invoked whenever a completed chunk is ready to be submitted to GPU.
 * Returning non-OK immediately aborts the chunker so no further chunks launch. */
typedef A840ChunkStatus (*A840ChunkKickFn)(void *ctx, const A840ChunkDesc *chunk);

typedef struct A840ChunkerConfig {
    uint32_t max_passes_per_chunk;   /* 0 disables pass-count trigger */
    uint32_t max_dwords_per_chunk;   /* 0 disables dword-count trigger */
    uint32_t max_est_gpu_us_per_chunk; /* 0 disables GPU-time trigger */
    uint32_t reloc_capacity_per_chunk; /* 0 -> A840_CHUNK_DEFAULT_RELOC_CAP */
    uint32_t ring_id;                /* 0..3 hardware ring */
    A840SyncMode sync_mode;          /* TRACKED (default) or NAIVE for A/B */
    uint64_t marker_bo_handle;       /* Optional BO for CACHE_FLUSH_TS marker */
    uint64_t marker_gpu_va;          /* Optional 8B-aligned 48-bit GPU VA */
    A840ChunkKickFn kick_fn;
    void *kick_ctx;
} A840ChunkerConfig;

typedef struct A840ChunkerStats {
    uint64_t cbs_completed;
    uint64_t cbs_aborted;
    uint64_t chunks_kicked;
    uint64_t early_kicks;
    uint64_t final_kicks;
    uint64_t aborted_chunks;
    uint64_t rejected_mid_gmem_kicks;
    uint64_t rejected_mid_pass_kicks;
    uint64_t threshold_pass_kicks;
    uint64_t threshold_dword_kicks;
    uint64_t threshold_gpu_us_kicks;
    uint64_t manual_kicks;
    uint64_t total_passes_encoded;
    uint64_t total_pm4_dwords;
    uint64_t last_cb_chunks_kicked;
    uint64_t last_cb_early_kicks;
    uint64_t monolithic_cpu_wait_us;
    uint64_t monolithic_total_us;
    uint64_t chunked_critical_path_us;
    uint64_t gpu_overlap_gain_us;
    uint64_t single_cb_encode_ns;
    double single_cb_encode_us;
    uint64_t heap_allocs;
} A840ChunkerStats;

typedef struct A840CbChunker {
    bool initialized;
    bool cb_active;
    bool in_pass;
    bool in_gmem_pass;
    bool has_open_chunk;
    bool cb_aborted;
    bool cb_final_kicked;
    A840ChunkLifecycleStatus last_chunk_status;

    A840FrameArena *arena;
    A840CmdSlotPool *pool;
    A840ChunkerConfig config;
    A840SyncTracker sync;

    uint64_t frame_id;
    uint64_t cb_id;
    uint64_t cb_start_ns;
    uint64_t next_chunk_seqno;
    uint64_t prev_cb_chunk_seqno;
    uint32_t next_chunk_index;
    uint32_t cb_chunks_kicked;
    uint32_t cb_early_kicks;
    uint32_t cb_total_passes;

    /* Per-CB discrete-event CPU/GPU overlap timeline accumulators */
    uint64_t sim_cpu_accum_us;
    uint64_t sim_gpu_sum_us;
    uint64_t sim_gpu_finish_us;

    /* Active open chunk backing (zero heap allocation: slot + frame arena) */
    A840CmdSlotHandle open_slot_handle;
    A840CmdSlot *open_slot;
    A840Reloc *open_relocs;
    size_t open_reloc_cap;
    A840ChunkDesc *open_desc;
    A840CmdStream cs;
    uint32_t chunk_pass_count;
    uint32_t chunk_est_gpu_us;
    uint32_t chunk_est_cpu_us;
    uint32_t chunk_surface_count;
    uint64_t chunk_surfaces[A840_CHUNK_MAX_SURFACES_PER_CHUNK];

    /* Active pass state */
    A840ChunkPassKind active_pass_kind;
    uint32_t active_pass_dirty_mask;
    uint32_t active_pass_est_gpu_us;
    uint32_t active_pass_est_cpu_us;
    bool active_pass_is_final;
    uint32_t active_pass_write_count;
    uint64_t active_pass_write_ids[A840_SYNC_MAX_PASS_RESOURCES];
    uint32_t active_pass_read_count;
    uint64_t active_pass_read_ids[A840_SYNC_MAX_PASS_RESOURCES];
    uint32_t active_pass_consumer_mask;

    A840ChunkerStats stats;
} A840CbChunker;

void a840_cb_chunker_default_config(A840ChunkerConfig *cfg);

A840ChunkStatus a840_cb_chunker_init(A840CbChunker *chunker,
                                     A840FrameArena *arena,
                                     A840CmdSlotPool *pool,
                                     const A840ChunkerConfig *cfg);

A840ChunkStatus a840_cb_chunker_begin_cb(A840CbChunker *chunker,
                                         uint64_t frame_id,
                                         uint64_t cb_id);

A840ChunkStatus a840_cb_chunker_begin_pass(A840CbChunker *chunker,
                                           const A840ChunkPassDesc *pass);

/* Returns active chunk A840CmdStream while inside an active pass (NULL otherwise). */
A840CmdStream *a840_cb_chunker_cmd_stream(A840CbChunker *chunker);

/* Emit deterministic genuine A8XX CP_NOP workload dwords inside an active pass. */
A840ChunkStatus a840_cb_chunker_emit_pass_nop(A840CbChunker *chunker,
                                              uint32_t payload_dwords,
                                              uint32_t tag);

/* Emit a genuine A8XX CACHE_FLUSH_TS / RB_DONE_TS timestamp packet inside an active pass. */
A840ChunkStatus a840_cb_chunker_emit_pass_timestamp(A840CbChunker *chunker,
                                                    uint32_t event_id,
                                                    uint64_t bo_handle,
                                                    uint64_t gpu_va,
                                                    uint64_t payload);

A840ChunkStatus a840_cb_chunker_end_pass(A840CbChunker *chunker);

/* Explicitly request a chunk kick at a safe boundary.
 * Rejects with A840_CHUNK_ERR_MID_GMEM_PASS if inside a GMEM tile pass, or
 * A840_CHUNK_ERR_MID_PASS if inside any other active pass. */
A840ChunkStatus a840_cb_chunker_force_kick(A840CbChunker *chunker);

/* Immediately abort current CB (e.g. on GPU fault / channel reset), releasing
 * any unsubmitted slot and blocking all subsequent pass/chunk kicks. */
void a840_cb_chunker_abort(A840CbChunker *chunker);

/* Seal final chunk with A840_CHUNK_FLAG_FINAL, finalize overlap stats, and
 * recycle temporary CB state. */
A840ChunkStatus a840_cb_chunker_finish(A840CbChunker *chunker);

A840ChunkerStats a840_cb_chunker_stats(const A840CbChunker *chunker);
A840SyncStats a840_cb_chunker_sync_stats(const A840CbChunker *chunker);
bool a840_cb_chunker_is_in_gmem_pass(const A840CbChunker *chunker);
bool a840_cb_chunker_is_aborted(const A840CbChunker *chunker);

#ifdef __cplusplus
}
#endif

#endif /* A840_CB_CHUNKER_H */
