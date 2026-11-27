/* SPDX-License-Identifier: MIT */
#ifndef A840_HEAP_ALLOCATOR_H
#define A840_HEAP_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_runtime_device.h"
#include "a840_ubwc_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_HEAP_MIN_ALIGN_BYTES = 64u,
    A840_HEAP_MAX_ALIGN_BYTES = 65536u,
    A840_HEAP_MAX_ALIAS_RECORDS = 512u
};

typedef enum A840HeapType {
    A840_HEAP_TYPE_AUTOMATIC = 0,
    A840_HEAP_TYPE_PLACEMENT = 1
} A840HeapType;

typedef enum A840PurgeableState {
    A840_PURGEABLE_NONVOLATILE = 0,
    A840_PURGEABLE_VOLATILE = 1,
    A840_PURGEABLE_EMPTY = 2
} A840PurgeableState;

typedef enum A840HeapResourceKind {
    A840_HEAP_RES_BUFFER = 0,
    A840_HEAP_RES_TEXTURE = 1
} A840HeapResourceKind;

typedef struct A840HeapDesc {
    A840HeapType type;
    uint64_t size_bytes;        /* > 0, rounded up to 4 KiB internally */
    uint64_t alignment;         /* 0 defaults to 64 KiB; power-of-two in [4 KiB, 64 KiB] */
    uint32_t usage_flags;       /* Bitmask of A840_BUF_USAGE_* */
    bool cpu_visible;
} A840HeapDesc;

typedef struct A840HeapAliasTransition {
    bool requires_alias_barrier;
    bool ubwc_meta_invalidate_required;
    uint32_t prev_tile_mode;
    uint32_t next_tile_mode;
    uint32_t prev_mtl_format;
    uint32_t next_mtl_format;
    uint64_t prev_alias_epoch;
    uint64_t next_alias_epoch;
    uint64_t overlap_offset;
    uint64_t overlap_bytes;
    uint64_t bo_handle;
    uint64_t ubwc_flag_va;      /* Non-zero if UBWC flag metadata exists to clear/invalidate */
    uint64_t ubwc_flag_bytes;
} A840HeapAliasTransition;

typedef struct A840HeapFreeBlock {
    uint64_t offset;
    uint64_t size;
} A840HeapFreeBlock;

typedef struct A840HeapAliasSpan {
    uint64_t offset;
    uint64_t size;
    uint64_t alias_epoch;
    uint32_t tile_mode;         /* A840_TILE_* */
    uint32_t mtl_pixel_format;
    uint64_t ubwc_flag_offset;
    uint64_t ubwc_flag_bytes;
    bool active;
    bool aliasable;
} A840HeapAliasSpan;

typedef struct A840Heap {
    A840RtDevice *device;
    uint64_t device_id;
    A840HeapType type;
    A840PurgeableState purgeable_state;
    uint32_t refcount;

    /* Single backing KMD BO (zero per-suballoc BO allocations). */
    A840RtBuffer *backing_bo;
    uint64_t size_bytes;
    uint64_t alignment;
    uint64_t used_bytes;
    uint64_t next_epoch;

    /* Coalescing free-list for Automatic heaps. */
    A840HeapFreeBlock *free_blocks;
    size_t free_count;
    size_t free_cap;

    /* History of live and aliasable spans for conflict/layout transition tracking. */
    A840HeapAliasSpan *spans;
    size_t span_count;
    size_t span_cap;

    uint64_t live_suballocs;
    uint64_t alias_reuse_count;
    uint64_t alias_barrier_count;
} A840Heap;

typedef struct A840HeapBufferDesc {
    uint64_t size_bytes;
    uint64_t alignment;         /* 0 defaults to 64B; power-of-two <= 64 KiB */
    uint64_t placement_offset;  /* Used only for A840_HEAP_TYPE_PLACEMENT */
    uint32_t usage_flags;
} A840HeapBufferDesc;

typedef struct A840HeapTextureDesc {
    uint32_t mtl_pixel_format;
    uint32_t tex_type;          /* A840_TEX_1D / 2D / CUBE / 3D */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t samples;
    uint32_t tile_mode;         /* A840_TILE_LINEAR / TILED / UBWC */
    uint32_t highest_bank_bit;  /* 0 defaults to 16; UBWC enforces 16 */
    uint64_t alignment;         /* 0 uses layout-required alignment (<= 64 KiB) */
    uint64_t placement_offset;  /* Used only for A840_HEAP_TYPE_PLACEMENT */
    uint32_t usage_flags;
} A840HeapTextureDesc;

typedef struct A840HeapSubAlloc {
    A840Heap *heap;
    A840RtDevice *device;
    uint64_t device_id;
    A840HeapResourceKind kind;
    uint32_t refcount;

    uint64_t bo_handle;         /* Always equals heap->backing_bo->bo_handle */
    uint64_t offset;
    uint64_t size_bytes;
    uint64_t alignment;
    uint64_t gpu_va;
    void *cpu_ptr;              /* NULL if heap is not CPU-visible */

    uint64_t alias_epoch;
    size_t span_index;
    bool aliasable;
    bool destroyed;

    /* Texture metadata (valid when kind == A840_HEAP_RES_TEXTURE). */
    uint32_t mtl_pixel_format;
    uint32_t tile_mode;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t pitch_bytes;
    uint64_t slice_stride_bytes;
    A840UbwcLayout ubwc_layout;
    uint64_t data_gpu_va;
    uint64_t ubwc_flag_gpu_va;
    uint32_t tex_const[A840_TEX_CONST_DWORDS];

    /* Aliasing transition captured at sub-allocation time. */
    A840HeapAliasTransition last_transition;
} A840HeapSubAlloc;

/* Heap lifecycle & purgeable state management. */
A840RtStatus a840_heap_create(A840RtDevice *device,
                              const A840HeapDesc *desc,
                              A840Heap **out_heap);
void a840_heap_retain(A840Heap *heap);
void a840_heap_release(A840Heap *heap);

A840RtStatus a840_heap_set_purgeable(A840Heap *heap,
                                     A840PurgeableState new_state,
                                     A840PurgeableState *out_prev_state);
A840PurgeableState a840_heap_get_purgeable(const A840Heap *heap);
A840RtStatus a840_heap_validate_for_submission(const A840Heap *heap);

uint64_t a840_heap_used_bytes(const A840Heap *heap);
uint64_t a840_heap_free_bytes(const A840Heap *heap);
size_t a840_heap_free_block_count(const A840Heap *heap);
uint64_t a840_heap_backing_bo_handle(const A840Heap *heap);

/* Sub-allocation API (zero extra KMD BO allocations). */
A840RtStatus a840_heap_alloc_buffer(A840Heap *heap,
                                    const A840HeapBufferDesc *desc,
                                    A840HeapSubAlloc **out_sub);

A840RtStatus a840_heap_alloc_texture(A840Heap *heap,
                                     const A840HeapTextureDesc *desc,
                                     A840HeapSubAlloc **out_sub);

/* Mark an active sub-allocation as aliasable: returns its byte span to the
 * reuse pool (for Automatic) / allows placement overlap (for Placement) while
 * retaining alias_epoch + previous tiling/format metadata. */
A840RtStatus a840_heap_suballoc_make_aliasable(A840HeapSubAlloc *sub);
bool a840_heap_suballoc_is_aliasable(const A840HeapSubAlloc *sub);

void a840_heap_suballoc_retain(A840HeapSubAlloc *sub);
void a840_heap_suballoc_release(A840HeapSubAlloc *sub);

/* Validate sub-allocation belongs to device and its heap is non-EMPTY. */
A840RtStatus a840_heap_suballoc_validate(const A840RtDevice *device,
                                         const A840HeapSubAlloc *sub);

/* Emit A8XX PM4 aliasing barrier + UBWC metadata invalidation/zeroing sequence
 * (CCU_FLUSH_COLOR | CCU_INV_COLOR | UCHE_INV + CP_WAIT_MEM_WRITES + optional
 * UBWC metadata fast-invalidate header write). Passes pm4_check --chip A8XX. */
A840EmitStatus a840_heap_emit_alias_transition(A840CmdStream *cs,
                                               const A840HeapAliasTransition *transition);

#ifdef __cplusplus
}
#endif

#endif /* A840_HEAP_ALLOCATOR_H */
