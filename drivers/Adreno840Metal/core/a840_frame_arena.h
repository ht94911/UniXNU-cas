/* SPDX-License-Identifier: MIT */
/* Adreno 840 Native Metal Driver — Zero-Allocation Per-Frame Infrastructure
 * (Phase 5 / Wave 9 Lane 3: 120Hz / 8.333 ms CPU Overhead Reduction)
 *
 * Provides three steady-state zero-heap-allocation primitives:
 *   1. A840StateHashTable — Open-addressing generational hash table (power-of-two
 *      capacity 256/512/1024+, linear probing, 64-bit mixing hash, O(1) frame
 *      reset via generation bump with UINT32_MAX -> 1 wraparound protection).
 *   2. A840FrameArena     — N-in-flight (2..8, default 3) ring of 64B-aligned
 *      bump allocators with O(1) cursor reset upon frame retirement.
 *   3. A840CmdSlotPool    — Fixed-capacity thread-safe command-buffer/encoder
 *      slot reuse pool with generation-tagged handles (ABA/UAF protection) and
 *      fence-ordered batch retirement (`retire_fence_seqno`).
 *
 * Supports injectable allocator hooks (`A840ArenaAllocator`) so tests can hard-assert
 * zero heap allocations (`heap_alloc_count == 0`, `heap_free_count == 0`) across
 * 1,000+ consecutive 120Hz frames in steady state.
 */
#ifndef A840_FRAME_ARENA_H
#define A840_FRAME_ARENA_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_FRAME_ARENA_ALIGN = 64u,
    A840_FRAME_ARENA_MIN_FRAMES = 2u,
    A840_FRAME_ARENA_DEFAULT_FRAMES = 3u,
    A840_FRAME_ARENA_MAX_FRAMES = 8u,
    A840_STATE_TABLE_MIN_CAP = 16u,
    A840_STATE_TABLE_DEFAULT_CAP = 256u,
    A840_STATE_TABLE_MAX_LOAD_PCT = 75u,
    A840_CMD_SLOT_MAX_CAP = 1024u
};

typedef enum A840ArenaStatus {
    A840_ARENA_OK = 0,
    A840_ARENA_INVALID_ARG = 1,
    A840_ARENA_OOM = 2,
    A840_ARENA_FULL = 3,
    A840_ARENA_NOT_FOUND = 4,
    A840_ARENA_STALE_HANDLE = 5,
    A840_ARENA_BUSY = 6,
    A840_ARENA_ALIGNMENT = 7,
    A840_ARENA_RANGE_ERROR = 8
} A840ArenaStatus;

/* Injectable heap allocator hook for steady-state zero-malloc verification.
 * All allocations happen ONLY during init/create (and frees only during destroy). */
typedef void *(*A840ArenaAllocFn)(void *ctx, size_t size, size_t alignment);
typedef void (*A840ArenaFreeFn)(void *ctx, void *ptr);

typedef struct A840ArenaAllocator {
    void *ctx;
    A840ArenaAllocFn alloc_fn;
    A840ArenaFreeFn free_fn;
} A840ArenaAllocator;

/* ============================================================================
 * 1. Generational Open-Addressing State Hash Table (Zero Steady-State Malloc)
 * ============================================================================ */

typedef struct A840StateSlot {
    uint64_t key;        /* 64-bit pipeline/sampler/viewport/descriptor hash */
    uint64_t value;      /* Associated state cookie / binding signature */
    uint32_t generation; /* Active when == table->generation (0 = empty) */
    uint32_t reserved;
} A840StateSlot;

typedef struct A840StateTableStats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t inserts;
    uint64_t updates;
    uint64_t full_rejects;
    uint64_t frame_resets;
    uint64_t generation_wraps;
    uint32_t probe_steps_max;
} A840StateTableStats;

typedef struct A840StateHashTable {
    A840StateSlot *slots;
    uint32_t capacity;     /* Power of two >= A840_STATE_TABLE_MIN_CAP */
    uint32_t mask;         /* capacity - 1 */
    uint32_t max_entries;  /* 75% load factor cap */
    uint32_t count;        /* Live entries in current generation */
    uint32_t generation;   /* Current generation (1..UINT32_MAX) */
    bool owns_storage;
    A840ArenaAllocator allocator;
    A840StateTableStats stats;
} A840StateHashTable;

/* Initialize over caller-owned (or arena-sliced) backing slot array: ZERO malloc. */
A840ArenaStatus a840_state_table_init_external(A840StateHashTable *table,
                                               A840StateSlot *backing_slots,
                                               uint32_t capacity);

/* Initialize with a single upfront allocation via allocator (or default calloc). */
A840ArenaStatus a840_state_table_init(A840StateHashTable *table,
                                      uint32_t capacity,
                                      const A840ArenaAllocator *allocator);

void a840_state_table_destroy(A840StateHashTable *table);

/* O(1) frame reset: increments generation stamp (no memset in steady state).
 * On generation wraparound (UINT32_MAX -> 1), safely clears all slot stamps. */
void a840_state_table_reset_frame(A840StateHashTable *table);

/* Lookup key in current generation. Returns A840_ARENA_OK on hit or
 * A840_ARENA_NOT_FOUND on miss. */
A840ArenaStatus a840_state_table_lookup(A840StateHashTable *table,
                                        uint64_t key,
                                        uint64_t *out_value);

/* Insert or update (key, value). If already present with identical value,
 * sets *out_unchanged = true so caller can skip redundant PM4 state emission.
 * Returns A840_ARENA_FULL if load cap (75%) would be exceeded on a new key. */
A840ArenaStatus a840_state_table_insert_or_update(A840StateHashTable *table,
                                                  uint64_t key,
                                                  uint64_t value,
                                                  bool *out_unchanged);

/* Convenience helper for encoder bind deduplication: returns true if binding
 * (slot_domain, state_hash) is already active in current frame and can be
 * skipped; otherwise records it and returns false. */
bool a840_state_table_dedup_bind(A840StateHashTable *table,
                                 uint32_t bind_domain,
                                 uint64_t state_hash,
                                 A840ArenaStatus *out_status);

A840StateTableStats a840_state_table_stats(const A840StateHashTable *table);

/* 64-bit SplitMix64 mixing helper for state hashing. */
uint64_t a840_state_mix64(uint64_t x);

/* ============================================================================
 * 2. N-In-Flight Per-Frame 64B-Aligned Bump Arena (Zero Steady-State Malloc)
 * ============================================================================ */

typedef struct A840FrameSlice {
    uint8_t *base;
    size_t capacity_bytes;
    size_t used_bytes;
    size_t peak_bytes;
    uint64_t frame_id;
    bool active;
} A840FrameSlice;

typedef struct A840FrameArenaStats {
    uint64_t frames_begun;
    uint64_t frames_retired;
    uint64_t allocations;
    uint64_t oom_rejects;
    size_t high_water_bytes;
} A840FrameArenaStats;

typedef struct A840FrameArena {
    uint8_t *backing_mem;
    size_t total_bytes;
    size_t per_frame_bytes;
    uint32_t frame_count;    /* 2..8 (default 3 for triple buffering) */
    uint32_t active_slot;
    bool has_active_frame;
    bool owns_storage;
    A840ArenaAllocator allocator;
    A840FrameSlice frames[A840_FRAME_ARENA_MAX_FRAMES];
    A840FrameArenaStats stats;
} A840FrameArena;

/* Initialize arena with a single upfront contiguous allocation (zero runtime alloc). */
A840ArenaStatus a840_frame_arena_init(A840FrameArena *arena,
                                      uint32_t frame_count,
                                      size_t per_frame_bytes,
                                      const A840ArenaAllocator *allocator);

/* Initialize arena over caller-provided 64B-aligned backing buffer. */
A840ArenaStatus a840_frame_arena_init_external(A840FrameArena *arena,
                                               uint32_t frame_count,
                                               size_t per_frame_bytes,
                                               void *backing_mem,
                                               size_t backing_bytes);

void a840_frame_arena_destroy(A840FrameArena *arena);

/* Acquire next retired frame slice in ring for `frame_id`. Returns A840_ARENA_BUSY
 * if all N-in-flight frames are still active/unretired. */
A840ArenaStatus a840_frame_arena_begin_frame(A840FrameArena *arena,
                                             uint64_t frame_id,
                                             uint32_t *out_slot_index);

/* Retire a completed frame slice by slot_index or frame_id (O(1) cursor reset). */
A840ArenaStatus a840_frame_arena_retire_frame(A840FrameArena *arena, uint64_t frame_id);

/* Bump-allocate `bytes` aligned to `alignment` (power of two; 0 -> 64B) from
 * the currently active frame slice. Zero heap allocation. */
void *a840_frame_arena_alloc(A840FrameArena *arena,
                             size_t bytes,
                             size_t alignment,
                             A840ArenaStatus *out_status);

A840FrameArenaStats a840_frame_arena_stats(const A840FrameArena *arena);

/* ============================================================================
 * 3. Generation-Tagged Command Slot Reuse Pool (ABA/UAF-Safe, Thread-Safe)
 * ============================================================================ */

/* Opaque 64-bit generation-tagged handle: [63:32]=generation, [31:0]=index+1.
 * Zero is always invalid (A840_CMD_SLOT_INVALID_HANDLE). */
typedef uint64_t A840CmdSlotHandle;
#define A840_CMD_SLOT_INVALID_HANDLE UINT64_C(0)

typedef enum A840CmdSlotState {
    A840_CMD_SLOT_FREE = 0,
    A840_CMD_SLOT_ACQUIRED = 1,
    A840_CMD_SLOT_SUBMITTED = 2
} A840CmdSlotState;

typedef struct A840CmdSlot {
    uint32_t index;
    uint32_t generation;     /* Incremented on every release/retire to prevent ABA */
    A840CmdSlotState state;
    uint32_t next_free;      /* Intrusive freelist index (UINT32_MAX = end) */
    uint64_t fence_seqno;    /* Nonzero when submitted awaiting GPU fence retire */
    uint32_t *pm4_words;     /* Pre-allocated per-slot PM4 command storage */
    uint32_t pm4_capacity_dwords;
    uint32_t pm4_used_dwords;
    void *user_payload;
} A840CmdSlot;

typedef struct A840CmdSlotPoolStats {
    uint64_t acquires;
    uint64_t releases;
    uint64_t submits;
    uint64_t retired_slots;
    uint64_t pool_empty_rejects;
    uint64_t stale_handle_rejects;
    uint32_t in_use_current;
    uint32_t in_use_peak;
} A840CmdSlotPoolStats;

typedef struct A840CmdSlotPool {
    pthread_mutex_t lock;
    bool lock_initialized;
    A840CmdSlot *slots;
    uint32_t *pm4_storage;
    uint32_t capacity;
    uint32_t pm4_dwords_per_slot;
    uint32_t free_head;
    uint32_t in_use_count;
    bool owns_storage;
    A840ArenaAllocator allocator;
    A840CmdSlotPoolStats stats;
} A840CmdSlotPool;

/* Create fixed-capacity command slot pool with single upfront allocation. */
A840ArenaStatus a840_cmd_slot_pool_init(A840CmdSlotPool *pool,
                                        uint32_t capacity,
                                        uint32_t pm4_dwords_per_slot,
                                        const A840ArenaAllocator *allocator);

void a840_cmd_slot_pool_destroy(A840CmdSlotPool *pool);

/* Acquire a free command slot (thread-safe, zero heap allocation). */
A840ArenaStatus a840_cmd_slot_acquire(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle *out_handle,
                                      A840CmdSlot **out_slot);

/* Validate handle without mutating state (rejects ABA/UAF stale handles). */
A840ArenaStatus a840_cmd_slot_resolve(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle handle,
                                      A840CmdSlot **out_slot);

/* Mark an acquired slot as submitted with a monotonic GPU fence sequence number. */
A840ArenaStatus a840_cmd_slot_mark_submitted(A840CmdSlotPool *pool,
                                             A840CmdSlotHandle handle,
                                             uint64_t fence_seqno);

/* Release an acquired (unsubmitted) or submitted slot back to the freelist,
 * bumping its generation so any stale handle copy fails with STALE_HANDLE. */
A840ArenaStatus a840_cmd_slot_release(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle handle);

/* Batch-retire all SUBMITTED slots with fence_seqno <= completed_fence_seqno,
 * returning them to the freelist with bumped generation stamps. */
A840ArenaStatus a840_cmd_slot_retire_fence_seqno(A840CmdSlotPool *pool,
                                                 uint64_t completed_fence_seqno,
                                                 uint32_t *out_retired_count);

A840CmdSlotPoolStats a840_cmd_slot_pool_stats(A840CmdSlotPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* A840_FRAME_ARENA_H */
