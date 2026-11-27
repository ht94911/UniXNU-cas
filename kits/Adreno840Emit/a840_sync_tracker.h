/* SPDX-License-Identifier: MIT */
/* Adreno 840 (A8XX / Gen8) Stateful Hardware Sync & Cache Coherency Tracker
 * (Phase 5 / Wave 9 Lane 3: 120Hz PM4 Barrier & Redundant WFI/Flush Eliminator)
 *
 * Pure C11, allocation-free, transactional rollback on error, zero raw hex
 * register/opcode literals. Strictly emits real A8XX packets via the existing
 * a840_emit_wfi / a840_emit_wait_mem_writes / a840_emit_cache_flush_inv APIs
 * and never emits Type-4 writes to CP_PROTECT protected registers.
 */
#ifndef A840_SYNC_TRACKER_H
#define A840_SYNC_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_emit_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SYNC_MAX_TRACKED_RESOURCES = 64u,
    A840_SYNC_MAX_PASS_RESOURCES = 64u
};

/* Write-dirty hardware cache / pipeline domains. */
typedef enum A840SyncDirtyBits {
    A840_SYNC_DIRTY_NONE = 0u,
    A840_SYNC_DIRTY_COLOR_CCU = 1u << 0,
    A840_SYNC_DIRTY_DEPTH_CCU = 1u << 1,
    A840_SYNC_DIRTY_UCHE_SHADER_WR = 1u << 2,
    A840_SYNC_DIRTY_MEM_WRITE_CP = 1u << 3,
    A840_SYNC_DIRTY_BLIT_2D_WR = 1u << 4,
    A840_SYNC_DIRTY_GMEM_RESOLVE = 1u << 5,
    A840_SYNC_DIRTY_ALL = (1u << 6) - 1u
} A840SyncDirtyBits;

/* Read-side cache cleanliness / pipeline idle state bits. */
typedef enum A840SyncCleanBits {
    A840_SYNC_CLEAN_NONE = 0u,
    A840_SYNC_CLEAN_UCHE_TEX = 1u << 0,
    A840_SYNC_CLEAN_UCHE_CONST = 1u << 1,
    A840_SYNC_CLEAN_CCU_COLOR = 1u << 2,
    A840_SYNC_CLEAN_WFI = 1u << 3,
    A840_SYNC_CLEAN_MEM_WRITES = 1u << 4,
    A840_SYNC_CLEAN_ALL = (1u << 5) - 1u
} A840SyncCleanBits;

/* Consumer domains that require coherence before executing. */
typedef enum A840SyncConsumerBits {
    A840_SYNC_CONSUME_NONE = 0u,
    A840_SYNC_CONSUME_UCHE_TEX = 1u << 0,
    A840_SYNC_CONSUME_UCHE_CONST = 1u << 1,
    A840_SYNC_CONSUME_COMPUTE_RW = 1u << 2,
    A840_SYNC_CONSUME_COLOR_RT = 1u << 3,
    A840_SYNC_CONSUME_DEPTH_RT = 1u << 4,
    A840_SYNC_CONSUME_BLIT_2D = 1u << 5,
    A840_SYNC_CONSUME_CP_INDIRECT = 1u << 6,
    A840_SYNC_CONSUME_HOST_PRESENT = 1u << 7,
    A840_SYNC_CONSUME_ALL = (1u << 8) - 1u
} A840SyncConsumerBits;

/* Explicit synchronization flags (including conservative fail-safe escapes). */
typedef enum A840SyncFlags {
    A840_SYNC_FLAG_NONE = 0u,
    A840_SYNC_GLOBAL_MEMORY = 1u << 0,
    A840_SYNC_FORCE_FULL_BARRIER = 1u << 1,
    A840_SYNC_EXTERNAL_IMPORT = 1u << 2,
    A840_SYNC_REQUIRE_WFI = 1u << 3,
    A840_SYNC_REQUIRE_WAIT_MEM = 1u << 4,
    A840_SYNC_FLAGS_VALID_MASK = (1u << 5) - 1u
} A840SyncFlags;

typedef enum A840SyncMode {
    A840_SYNC_MODE_TRACKED = 0, /* Stateful deduplication & disjoint-pass elision */
    A840_SYNC_MODE_NAIVE = 1    /* Baseline unoptimized full barrier emission for A/B */
} A840SyncMode;

typedef struct A840SyncResourceEntry {
    uint64_t resource_id;   /* Nonzero BO handle or surface identifier */
    uint32_t dirty_mask;    /* Bitmask of A840SyncDirtyBits */
    uint32_t read_mask;     /* Bitmask of A840SyncConsumerBits that cached reads */
    uint32_t last_pass_id;
} A840SyncResourceEntry;

typedef struct A840SyncPassAccess {
    const uint64_t *read_ids;
    size_t read_count;
    const uint64_t *write_ids;
    size_t write_count;
    uint32_t consumer_mask; /* Bitmask of A840SyncConsumerBits */
    uint32_t flags;         /* Bitmask of A840SyncFlags */
} A840SyncPassAccess;

typedef struct A840SyncStats {
    uint64_t wfi_emitted;
    uint64_t wfi_elided;
    uint64_t wait_mem_emitted;
    uint64_t wait_mem_elided;
    uint64_t ccu_flush_emitted;
    uint64_t ccu_flush_elided;
    uint64_t uche_inv_emitted;
    uint64_t uche_inv_elided;
    uint64_t unrelated_pass_barriers_elided;
    uint64_t conservative_fallbacks;
    uint64_t dwords_emitted;
    uint64_t dwords_saved;
} A840SyncStats;

typedef struct A840SyncTracker {
    A840SyncMode mode;
    uint32_t dirty_domains; /* Aggregate A840SyncDirtyBits */
    uint32_t clean_state;   /* Aggregate A840SyncCleanBits */
    bool uche_may_hold_stale_reads;
    bool overflow_conservative;
    bool external_hazard_pending;
    bool active_work_since_wfi;
    uint32_t current_pass_id;
    uint32_t resource_count;
    A840SyncResourceEntry resources[A840_SYNC_MAX_TRACKED_RESOURCES];
    A840SyncStats stats;
} A840SyncTracker;

/* Initialize / reset tracker state (zero heap allocation). */
A840EmitStatus a840_sync_tracker_init(A840SyncTracker *tracker, A840SyncMode mode);
void a840_sync_tracker_reset(A840SyncTracker *tracker);
void a840_sync_tracker_set_mode(A840SyncTracker *tracker, A840SyncMode mode);
A840SyncStats a840_sync_tracker_stats(const A840SyncTracker *tracker);

/* Record GPU work producing dirty cache domains and/or touching tracked BOs.
 * If resource_ids == NULL and resource_count == 0, updates global domain state
 * and marks conservative fallback when resource_count > 0 is omitted.
 * Recording > 64 distinct active resources sets overflow_conservative = true
 * so subsequent barriers safely fall back to full conservative flushes. */
A840EmitStatus a840_sync_note_write(A840SyncTracker *tracker,
                                    uint32_t dirty_bits,
                                    const uint64_t *resource_ids,
                                    size_t resource_count);

/* Record read-side cache population (e.g. texture sampling or UBO reads). */
A840EmitStatus a840_sync_note_read(A840SyncTracker *tracker,
                                   uint32_t consumer_bits,
                                   const uint64_t *resource_ids,
                                   size_t resource_count);

/* Mark an external/imported or unknown hazard (forces conservative barrier). */
void a840_sync_note_external_hazard(A840SyncTracker *tracker);

/* Direct deduplicated sync emitters (drop-in replacements for raw calls). */
A840EmitStatus a840_sync_emit_wfi(A840SyncTracker *tracker,
                                  A840CmdStream *cs,
                                  uint32_t flags);

A840EmitStatus a840_sync_emit_wait_mem_writes(A840SyncTracker *tracker,
                                              A840CmdStream *cs,
                                              uint32_t flags);

A840EmitStatus a840_sync_emit_cache_barrier(A840SyncTracker *tracker,
                                            A840CmdStream *cs,
                                            uint32_t requested_cache_mask,
                                            uint32_t consumer_mask,
                                            uint32_t flags);

/* Pass-boundary coherence barrier:
 * Inspects overlap between `next_pass` (read_ids/write_ids) and currently
 * dirty tracked resources. If completely disjoint and no global/force/overflow/
 * external hazard is active, safely elides the pass barrier in TRACKED mode. */
A840EmitStatus a840_sync_emit_pass_barrier(A840SyncTracker *tracker,
                                           A840CmdStream *cs,
                                           const A840SyncPassAccess *next_pass);

#ifdef __cplusplus
}
#endif

#endif /* A840_SYNC_TRACKER_H */
