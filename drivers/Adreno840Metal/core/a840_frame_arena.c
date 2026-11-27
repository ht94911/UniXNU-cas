/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_frame_arena.h"

#include <stdlib.h>
#include <string.h>

#define A840_FREELIST_END UINT32_MAX

static bool is_power_of_two_size(size_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static size_t align_up_size(size_t v, size_t align)
{
    size_t mask = align - 1u;
    if (v > SIZE_MAX - mask) {
        return SIZE_MAX;
    }
    return (v + mask) & ~mask;
}

static void *default_alloc(void *ctx, size_t size, size_t alignment)
{
    void *ptr = NULL;
    size_t eff_align = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    (void)ctx;
    if (!is_power_of_two_size(eff_align) || size == 0u) {
        return NULL;
    }
    if (posix_memalign(&ptr, eff_align, size) != 0) {
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

static void default_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static A840ArenaAllocator resolve_allocator(const A840ArenaAllocator *in)
{
    A840ArenaAllocator out;
    if (in && in->alloc_fn && in->free_fn) {
        out = *in;
    } else {
        out.ctx = NULL;
        out.alloc_fn = default_alloc;
        out.free_fn = default_free;
    }
    return out;
}

static bool allocator_arg_valid(const A840ArenaAllocator *in)
{
    if (!in) {
        return true;
    }
    return (in->alloc_fn == NULL && in->free_fn == NULL) ||
           (in->alloc_fn != NULL && in->free_fn != NULL);
}

uint64_t a840_state_mix64(uint64_t x)
{
    /* SplitMix64 finalizer (derived from MurmurHash3 / Stafford Mix13). */
    x ^= x >> 30u;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27u;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31u;
    return x;
}

/* ============================================================================
 * 1. Generational Open-Addressing State Hash Table
 * ============================================================================ */

A840ArenaStatus a840_state_table_init_external(A840StateHashTable *table,
                                               A840StateSlot *backing_slots,
                                               uint32_t capacity)
{
    if (!table || !backing_slots || capacity < A840_STATE_TABLE_MIN_CAP ||
        !is_power_of_two_size((size_t)capacity)) {
        return A840_ARENA_INVALID_ARG;
    }
    memset(table, 0, sizeof(*table));
    memset(backing_slots, 0, (size_t)capacity * sizeof(*backing_slots));
    table->slots = backing_slots;
    table->capacity = capacity;
    table->mask = capacity - 1u;
    table->max_entries = (capacity * A840_STATE_TABLE_MAX_LOAD_PCT) / 100u;
    if (table->max_entries == 0u) {
        table->max_entries = 1u;
    }
    table->generation = 1u;
    table->owns_storage = false;
    return A840_ARENA_OK;
}

A840ArenaStatus a840_state_table_init(A840StateHashTable *table,
                                      uint32_t capacity,
                                      const A840ArenaAllocator *allocator)
{
    A840ArenaAllocator alloc;
    A840StateSlot *mem;
    A840ArenaStatus st;

    if (!table || !allocator_arg_valid(allocator)) {
        return A840_ARENA_INVALID_ARG;
    }
    if (capacity == 0u) {
        capacity = A840_STATE_TABLE_DEFAULT_CAP;
    }
    if (capacity < A840_STATE_TABLE_MIN_CAP || !is_power_of_two_size((size_t)capacity) ||
        capacity > (1u << 24)) {
        return A840_ARENA_INVALID_ARG;
    }

    alloc = resolve_allocator(allocator);
    mem = (A840StateSlot *)alloc.alloc_fn(alloc.ctx, (size_t)capacity * sizeof(*mem),
                                          A840_FRAME_ARENA_ALIGN);
    if (!mem) {
        return A840_ARENA_OOM;
    }
    st = a840_state_table_init_external(table, mem, capacity);
    if (st != A840_ARENA_OK) {
        alloc.free_fn(alloc.ctx, mem);
        return st;
    }
    table->owns_storage = true;
    table->allocator = alloc;
    return A840_ARENA_OK;
}

void a840_state_table_destroy(A840StateHashTable *table)
{
    if (!table) {
        return;
    }
    if (table->owns_storage && table->slots && table->allocator.free_fn) {
        table->allocator.free_fn(table->allocator.ctx, table->slots);
    }
    memset(table, 0, sizeof(*table));
}

void a840_state_table_reset_frame(A840StateHashTable *table)
{
    uint32_t i;
    if (!table || !table->slots) {
        return;
    }
    table->count = 0u;
    table->stats.frame_resets++;
    if (table->generation == UINT32_MAX) {
        /* Wraparound protection: clear all slot generation stamps before rolling to 1. */
        for (i = 0u; i < table->capacity; ++i) {
            table->slots[i].generation = 0u;
        }
        table->generation = 1u;
        table->stats.generation_wraps++;
    } else {
        table->generation++;
    }
}

A840ArenaStatus a840_state_table_lookup(A840StateHashTable *table,
                                        uint64_t key,
                                        uint64_t *out_value)
{
    uint32_t idx;
    uint32_t step;
    if (!table || !table->slots) {
        return A840_ARENA_INVALID_ARG;
    }
    table->stats.lookups++;
    idx = (uint32_t)(a840_state_mix64(key) & (uint64_t)table->mask);

    for (step = 0u; step < table->capacity; ++step) {
        A840StateSlot *slot = &table->slots[(idx + step) & table->mask];
        if (slot->generation != table->generation) {
            if (step + 1u > table->stats.probe_steps_max) {
                table->stats.probe_steps_max = step + 1u;
            }
            table->stats.misses++;
            return A840_ARENA_NOT_FOUND;
        }
        if (slot->key == key) {
            if (step + 1u > table->stats.probe_steps_max) {
                table->stats.probe_steps_max = step + 1u;
            }
            table->stats.hits++;
            if (out_value) {
                *out_value = slot->value;
            }
            return A840_ARENA_OK;
        }
    }

    if (table->capacity > table->stats.probe_steps_max) {
        table->stats.probe_steps_max = table->capacity;
    }
    table->stats.misses++;
    return A840_ARENA_NOT_FOUND;
}

A840ArenaStatus a840_state_table_insert_or_update(A840StateHashTable *table,
                                                  uint64_t key,
                                                  uint64_t value,
                                                  bool *out_unchanged)
{
    uint32_t idx;
    uint32_t step;
    if (out_unchanged) {
        *out_unchanged = false;
    }
    if (!table || !table->slots) {
        return A840_ARENA_INVALID_ARG;
    }

    idx = (uint32_t)(a840_state_mix64(key) & (uint64_t)table->mask);

    for (step = 0u; step < table->capacity; ++step) {
        A840StateSlot *slot = &table->slots[(idx + step) & table->mask];
        if (slot->generation != table->generation) {
            if (table->count >= table->max_entries) {
                table->stats.full_rejects++;
                return A840_ARENA_FULL;
            }
            slot->key = key;
            slot->value = value;
            slot->generation = table->generation;
            table->count++;
            table->stats.inserts++;
            if (step + 1u > table->stats.probe_steps_max) {
                table->stats.probe_steps_max = step + 1u;
            }
            return A840_ARENA_OK;
        }
        if (slot->key == key) {
            if (step + 1u > table->stats.probe_steps_max) {
                table->stats.probe_steps_max = step + 1u;
            }
            if (slot->value == value) {
                table->stats.hits++;
                if (out_unchanged) {
                    *out_unchanged = true;
                }
            } else {
                slot->value = value;
                table->stats.updates++;
            }
            return A840_ARENA_OK;
        }
    }

    table->stats.full_rejects++;
    return A840_ARENA_FULL;
}

bool a840_state_table_dedup_bind(A840StateHashTable *table,
                                 uint32_t bind_domain,
                                 uint64_t state_hash,
                                 A840ArenaStatus *out_status)
{
    bool unchanged = false;
    uint64_t key = ((uint64_t)bind_domain << 32u) | UINT64_C(0x53544154);
    A840ArenaStatus st;
    if (!table) {
        if (out_status) {
            *out_status = A840_ARENA_INVALID_ARG;
        }
        return false;
    }
    table->stats.lookups++;
    st = a840_state_table_insert_or_update(table, key, state_hash, &unchanged);
    if (out_status) {
        *out_status = st;
    }
    if (st != A840_ARENA_OK) {
        table->stats.misses++;
        return false;
    }
    if (!unchanged) {
        table->stats.misses++;
    }
    return unchanged;
}

A840StateTableStats a840_state_table_stats(const A840StateHashTable *table)
{
    A840StateTableStats zero;
    memset(&zero, 0, sizeof(zero));
    if (!table) {
        return zero;
    }
    return table->stats;
}

/* ============================================================================
 * 2. N-In-Flight Per-Frame 64B-Aligned Bump Arena
 * ============================================================================ */

A840ArenaStatus a840_frame_arena_init_external(A840FrameArena *arena,
                                               uint32_t frame_count,
                                               size_t per_frame_bytes,
                                               void *backing_mem,
                                               size_t backing_bytes)
{
    size_t aligned_per_frame;
    size_t required_total;
    uint32_t i;

    if (!arena || !backing_mem) {
        return A840_ARENA_INVALID_ARG;
    }
    if (frame_count < A840_FRAME_ARENA_MIN_FRAMES ||
        frame_count > A840_FRAME_ARENA_MAX_FRAMES ||
        per_frame_bytes == 0u) {
        return A840_ARENA_RANGE_ERROR;
    }
    if (((uintptr_t)backing_mem % A840_FRAME_ARENA_ALIGN) != 0u) {
        return A840_ARENA_ALIGNMENT;
    }
    aligned_per_frame = align_up_size(per_frame_bytes, A840_FRAME_ARENA_ALIGN);
    if (aligned_per_frame == SIZE_MAX || aligned_per_frame > SIZE_MAX / frame_count) {
        return A840_ARENA_RANGE_ERROR;
    }
    required_total = aligned_per_frame * frame_count;
    if (backing_bytes < required_total) {
        return A840_ARENA_RANGE_ERROR;
    }

    memset(arena, 0, sizeof(*arena));
    arena->backing_mem = (uint8_t *)backing_mem;
    arena->total_bytes = required_total;
    arena->per_frame_bytes = aligned_per_frame;
    arena->frame_count = frame_count;
    arena->owns_storage = false;

    for (i = 0u; i < frame_count; ++i) {
        arena->frames[i].base = arena->backing_mem + ((size_t)i * aligned_per_frame);
        arena->frames[i].capacity_bytes = aligned_per_frame;
        arena->frames[i].used_bytes = 0u;
        arena->frames[i].peak_bytes = 0u;
        arena->frames[i].frame_id = 0u;
        arena->frames[i].active = false;
    }
    return A840_ARENA_OK;
}

A840ArenaStatus a840_frame_arena_init(A840FrameArena *arena,
                                      uint32_t frame_count,
                                      size_t per_frame_bytes,
                                      const A840ArenaAllocator *allocator)
{
    A840ArenaAllocator alloc;
    size_t aligned_per_frame;
    size_t total_bytes;
    void *mem;
    A840ArenaStatus st;

    if (!arena || !allocator_arg_valid(allocator)) {
        return A840_ARENA_INVALID_ARG;
    }
    if (frame_count == 0u) {
        frame_count = A840_FRAME_ARENA_DEFAULT_FRAMES;
    }
    if (frame_count < A840_FRAME_ARENA_MIN_FRAMES ||
        frame_count > A840_FRAME_ARENA_MAX_FRAMES ||
        per_frame_bytes == 0u) {
        return A840_ARENA_RANGE_ERROR;
    }
    aligned_per_frame = align_up_size(per_frame_bytes, A840_FRAME_ARENA_ALIGN);
    if (aligned_per_frame == SIZE_MAX || aligned_per_frame > SIZE_MAX / frame_count) {
        return A840_ARENA_RANGE_ERROR;
    }
    total_bytes = aligned_per_frame * frame_count;

    alloc = resolve_allocator(allocator);
    mem = alloc.alloc_fn(alloc.ctx, total_bytes, A840_FRAME_ARENA_ALIGN);
    if (!mem) {
        return A840_ARENA_OOM;
    }

    st = a840_frame_arena_init_external(arena, frame_count, aligned_per_frame, mem, total_bytes);
    if (st != A840_ARENA_OK) {
        alloc.free_fn(alloc.ctx, mem);
        return st;
    }
    arena->owns_storage = true;
    arena->allocator = alloc;
    return A840_ARENA_OK;
}

void a840_frame_arena_destroy(A840FrameArena *arena)
{
    if (!arena) {
        return;
    }
    if (arena->owns_storage && arena->backing_mem && arena->allocator.free_fn) {
        arena->allocator.free_fn(arena->allocator.ctx, arena->backing_mem);
    }
    memset(arena, 0, sizeof(*arena));
}

A840ArenaStatus a840_frame_arena_begin_frame(A840FrameArena *arena,
                                             uint64_t frame_id,
                                             uint32_t *out_slot_index)
{
    uint32_t i;
    uint32_t start;

    if (!arena || !arena->backing_mem || frame_id == 0u) {
        return A840_ARENA_INVALID_ARG;
    }
    /* Reject duplicate active frame_id. */
    for (i = 0u; i < arena->frame_count; ++i) {
        if (arena->frames[i].active && arena->frames[i].frame_id == frame_id) {
            return A840_ARENA_INVALID_ARG;
        }
    }

    start = arena->has_active_frame ? ((arena->active_slot + 1u) % arena->frame_count) : 0u;
    for (i = 0u; i < arena->frame_count; ++i) {
        uint32_t idx = (start + i) % arena->frame_count;
        if (!arena->frames[idx].active) {
            arena->frames[idx].active = true;
            arena->frames[idx].frame_id = frame_id;
            arena->frames[idx].used_bytes = 0u;
            arena->active_slot = idx;
            arena->has_active_frame = true;
            arena->stats.frames_begun++;
            if (out_slot_index) {
                *out_slot_index = idx;
            }
            return A840_ARENA_OK;
        }
    }
    return A840_ARENA_BUSY;
}

A840ArenaStatus a840_frame_arena_retire_frame(A840FrameArena *arena, uint64_t frame_id)
{
    uint32_t i;
    if (!arena || !arena->backing_mem || frame_id == 0u) {
        return A840_ARENA_INVALID_ARG;
    }
    for (i = 0u; i < arena->frame_count; ++i) {
        if (arena->frames[i].active && arena->frames[i].frame_id == frame_id) {
            arena->frames[i].active = false;
            arena->frames[i].frame_id = 0u;
            arena->frames[i].used_bytes = 0u; /* O(1) cursor reset */
            arena->stats.frames_retired++;
            if (arena->has_active_frame && arena->active_slot == i) {
                arena->has_active_frame = false;
            }
            return A840_ARENA_OK;
        }
    }
    return A840_ARENA_NOT_FOUND;
}

void *a840_frame_arena_alloc(A840FrameArena *arena,
                             size_t bytes,
                             size_t alignment,
                             A840ArenaStatus *out_status)
{
    A840FrameSlice *slice;
    size_t eff_align;
    size_t offset;

    if (!arena || !arena->backing_mem || bytes == 0u) {
        if (out_status) {
            *out_status = A840_ARENA_INVALID_ARG;
        }
        return NULL;
    }
    if (!arena->has_active_frame || !arena->frames[arena->active_slot].active) {
        if (out_status) {
            *out_status = A840_ARENA_BUSY;
        }
        return NULL;
    }

    eff_align = (alignment == 0u) ? A840_FRAME_ARENA_ALIGN : alignment;
    if (!is_power_of_two_size(eff_align) || eff_align > A840_FRAME_ARENA_ALIGN) {
        if (out_status) {
            *out_status = A840_ARENA_ALIGNMENT;
        }
        return NULL;
    }

    slice = &arena->frames[arena->active_slot];
    offset = align_up_size(slice->used_bytes, eff_align);
    if (offset == SIZE_MAX || bytes > slice->capacity_bytes ||
        offset > slice->capacity_bytes - bytes) {
        arena->stats.oom_rejects++;
        if (out_status) {
            *out_status = A840_ARENA_OOM;
        }
        return NULL;
    }

    slice->used_bytes = offset + bytes;
    if (slice->used_bytes > slice->peak_bytes) {
        slice->peak_bytes = slice->used_bytes;
    }
    if (slice->used_bytes > arena->stats.high_water_bytes) {
        arena->stats.high_water_bytes = slice->used_bytes;
    }
    arena->stats.allocations++;
    if (out_status) {
        *out_status = A840_ARENA_OK;
    }
    return slice->base + offset;
}

A840FrameArenaStats a840_frame_arena_stats(const A840FrameArena *arena)
{
    A840FrameArenaStats zero;
    memset(&zero, 0, sizeof(zero));
    if (!arena) {
        return zero;
    }
    return arena->stats;
}

/* ============================================================================
 * 3. Generation-Tagged Command Slot Reuse Pool (ABA/UAF-Safe, Thread-Safe)
 * ============================================================================ */

static A840CmdSlotHandle make_handle(uint32_t index, uint32_t generation)
{
    return (((uint64_t)generation) << 32u) | (uint64_t)(index + 1u);
}

static bool decode_handle(const A840CmdSlotPool *pool,
                          A840CmdSlotHandle handle,
                          uint32_t *out_index,
                          uint32_t *out_generation)
{
    uint32_t raw_idx = (uint32_t)(handle & UINT32_MAX);
    uint32_t gen = (uint32_t)(handle >> 32u);
    if (handle == A840_CMD_SLOT_INVALID_HANDLE || raw_idx == 0u || gen == 0u) {
        return false;
    }
    raw_idx -= 1u;
    if (raw_idx >= pool->capacity) {
        return false;
    }
    *out_index = raw_idx;
    *out_generation = gen;
    return true;
}

static uint32_t bump_generation(uint32_t gen)
{
    return (gen == UINT32_MAX) ? 1u : (gen + 1u);
}

A840ArenaStatus a840_cmd_slot_pool_init(A840CmdSlotPool *pool,
                                        uint32_t capacity,
                                        uint32_t pm4_dwords_per_slot,
                                        const A840ArenaAllocator *allocator)
{
    A840ArenaAllocator alloc;
    A840CmdSlot *slots_mem = NULL;
    uint32_t *pm4_mem = NULL;
    uint32_t i;

    if (!pool || !allocator_arg_valid(allocator)) {
        return A840_ARENA_INVALID_ARG;
    }
    if (capacity == 0u || capacity > A840_CMD_SLOT_MAX_CAP) {
        return A840_ARENA_RANGE_ERROR;
    }
    if (pm4_dwords_per_slot > (1u << 20)) {
        return A840_ARENA_RANGE_ERROR;
    }

    memset(pool, 0, sizeof(*pool));
    alloc = resolve_allocator(allocator);

    slots_mem = (A840CmdSlot *)alloc.alloc_fn(alloc.ctx, (size_t)capacity * sizeof(*slots_mem),
                                              A840_FRAME_ARENA_ALIGN);
    if (!slots_mem) {
        return A840_ARENA_OOM;
    }
    if (pm4_dwords_per_slot > 0u) {
        size_t pm4_bytes = (size_t)capacity * (size_t)pm4_dwords_per_slot * sizeof(uint32_t);
        pm4_mem = (uint32_t *)alloc.alloc_fn(alloc.ctx, pm4_bytes, A840_FRAME_ARENA_ALIGN);
        if (!pm4_mem) {
            alloc.free_fn(alloc.ctx, slots_mem);
            return A840_ARENA_OOM;
        }
    }

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        if (pm4_mem) {
            alloc.free_fn(alloc.ctx, pm4_mem);
        }
        alloc.free_fn(alloc.ctx, slots_mem);
        return A840_ARENA_OOM;
    }

    pool->lock_initialized = true;
    pool->slots = slots_mem;
    pool->pm4_storage = pm4_mem;
    pool->capacity = capacity;
    pool->pm4_dwords_per_slot = pm4_dwords_per_slot;
    pool->free_head = 0u;
    pool->in_use_count = 0u;
    pool->owns_storage = true;
    pool->allocator = alloc;

    for (i = 0u; i < capacity; ++i) {
        slots_mem[i].index = i;
        slots_mem[i].generation = 1u;
        slots_mem[i].state = A840_CMD_SLOT_FREE;
        slots_mem[i].next_free = (i + 1u < capacity) ? (i + 1u) : A840_FREELIST_END;
        slots_mem[i].fence_seqno = 0u;
        slots_mem[i].pm4_words = pm4_mem ? (pm4_mem + ((size_t)i * pm4_dwords_per_slot)) : NULL;
        slots_mem[i].pm4_capacity_dwords = pm4_dwords_per_slot;
        slots_mem[i].pm4_used_dwords = 0u;
        slots_mem[i].user_payload = NULL;
    }
    return A840_ARENA_OK;
}

void a840_cmd_slot_pool_destroy(A840CmdSlotPool *pool)
{
    if (!pool) {
        return;
    }
    if (pool->lock_initialized) {
        pthread_mutex_destroy(&pool->lock);
    }
    if (pool->owns_storage && pool->allocator.free_fn) {
        if (pool->pm4_storage) {
            pool->allocator.free_fn(pool->allocator.ctx, pool->pm4_storage);
        }
        if (pool->slots) {
            pool->allocator.free_fn(pool->allocator.ctx, pool->slots);
        }
    }
    memset(pool, 0, sizeof(*pool));
}

A840ArenaStatus a840_cmd_slot_acquire(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle *out_handle,
                                      A840CmdSlot **out_slot)
{
    uint32_t idx;
    A840CmdSlot *slot;

    if (!pool || !pool->lock_initialized || !out_handle) {
        return A840_ARENA_INVALID_ARG;
    }
    *out_handle = A840_CMD_SLOT_INVALID_HANDLE;
    if (out_slot) {
        *out_slot = NULL;
    }

    pthread_mutex_lock(&pool->lock);
    if (pool->free_head == A840_FREELIST_END) {
        pool->stats.pool_empty_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_FULL;
    }

    idx = pool->free_head;
    slot = &pool->slots[idx];
    pool->free_head = slot->next_free;
    slot->next_free = A840_FREELIST_END;
    slot->state = A840_CMD_SLOT_ACQUIRED;
    slot->fence_seqno = 0u;
    slot->pm4_used_dwords = 0u;
    slot->user_payload = NULL;

    pool->in_use_count++;
    pool->stats.acquires++;
    pool->stats.in_use_current = pool->in_use_count;
    if (pool->in_use_count > pool->stats.in_use_peak) {
        pool->stats.in_use_peak = pool->in_use_count;
    }
    *out_handle = make_handle(idx, slot->generation);
    if (out_slot) {
        *out_slot = slot;
    }
    pthread_mutex_unlock(&pool->lock);
    return A840_ARENA_OK;
}

A840ArenaStatus a840_cmd_slot_resolve(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle handle,
                                      A840CmdSlot **out_slot)
{
    uint32_t idx = 0u;
    uint32_t gen = 0u;
    A840CmdSlot *slot;

    if (!pool || !pool->lock_initialized || !out_slot) {
        return A840_ARENA_INVALID_ARG;
    }
    *out_slot = NULL;
    if (!decode_handle(pool, handle, &idx, &gen)) {
        pthread_mutex_lock(&pool->lock);
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }

    pthread_mutex_lock(&pool->lock);
    slot = &pool->slots[idx];
    if (slot->state == A840_CMD_SLOT_FREE || slot->generation != gen) {
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }
    *out_slot = slot;
    pthread_mutex_unlock(&pool->lock);
    return A840_ARENA_OK;
}

A840ArenaStatus a840_cmd_slot_mark_submitted(A840CmdSlotPool *pool,
                                             A840CmdSlotHandle handle,
                                             uint64_t fence_seqno)
{
    uint32_t idx = 0u;
    uint32_t gen = 0u;
    A840CmdSlot *slot;

    if (!pool || !pool->lock_initialized || fence_seqno == 0u) {
        return A840_ARENA_INVALID_ARG;
    }
    if (!decode_handle(pool, handle, &idx, &gen)) {
        pthread_mutex_lock(&pool->lock);
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }

    pthread_mutex_lock(&pool->lock);
    slot = &pool->slots[idx];
    if (slot->state != A840_CMD_SLOT_ACQUIRED || slot->generation != gen) {
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }
    slot->state = A840_CMD_SLOT_SUBMITTED;
    slot->fence_seqno = fence_seqno;
    pool->stats.submits++;
    pthread_mutex_unlock(&pool->lock);
    return A840_ARENA_OK;
}

static void recycle_slot_locked(A840CmdSlotPool *pool, A840CmdSlot *slot)
{
    slot->generation = bump_generation(slot->generation);
    slot->state = A840_CMD_SLOT_FREE;
    slot->fence_seqno = 0u;
    slot->pm4_used_dwords = 0u;
    slot->user_payload = NULL;
    slot->next_free = pool->free_head;
    pool->free_head = slot->index;
    pool->in_use_count--;
    pool->stats.in_use_current = pool->in_use_count;
}

A840ArenaStatus a840_cmd_slot_release(A840CmdSlotPool *pool,
                                      A840CmdSlotHandle handle)
{
    uint32_t idx = 0u;
    uint32_t gen = 0u;
    A840CmdSlot *slot;

    if (!pool || !pool->lock_initialized) {
        return A840_ARENA_INVALID_ARG;
    }
    if (!decode_handle(pool, handle, &idx, &gen)) {
        pthread_mutex_lock(&pool->lock);
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }

    pthread_mutex_lock(&pool->lock);
    slot = &pool->slots[idx];
    if (slot->state == A840_CMD_SLOT_FREE || slot->generation != gen) {
        pool->stats.stale_handle_rejects++;
        pthread_mutex_unlock(&pool->lock);
        return A840_ARENA_STALE_HANDLE;
    }
    recycle_slot_locked(pool, slot);
    pool->stats.releases++;
    pthread_mutex_unlock(&pool->lock);
    return A840_ARENA_OK;
}

A840ArenaStatus a840_cmd_slot_retire_fence_seqno(A840CmdSlotPool *pool,
                                                 uint64_t completed_fence_seqno,
                                                 uint32_t *out_retired_count)
{
    uint32_t i;
    uint32_t retired = 0u;

    if (!pool || !pool->lock_initialized) {
        return A840_ARENA_INVALID_ARG;
    }

    pthread_mutex_lock(&pool->lock);
    for (i = 0u; i < pool->capacity; ++i) {
        A840CmdSlot *slot = &pool->slots[i];
        if (slot->state == A840_CMD_SLOT_SUBMITTED &&
            slot->fence_seqno <= completed_fence_seqno) {
            recycle_slot_locked(pool, slot);
            retired++;
        }
    }
    pool->stats.retired_slots += retired;
    pthread_mutex_unlock(&pool->lock);

    if (out_retired_count) {
        *out_retired_count = retired;
    }
    return A840_ARENA_OK;
}

A840CmdSlotPoolStats a840_cmd_slot_pool_stats(A840CmdSlotPool *pool)
{
    A840CmdSlotPoolStats stats;
    memset(&stats, 0, sizeof(stats));
    if (!pool || !pool->lock_initialized) {
        return stats;
    }
    pthread_mutex_lock(&pool->lock);
    stats = pool->stats;
    pthread_mutex_unlock(&pool->lock);
    return stats;
}
