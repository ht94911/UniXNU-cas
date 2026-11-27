/* SPDX-License-Identifier: MIT */
#include "a840_heap_allocator.h"

#include <stdlib.h>
#include <string.h>

#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "a840_emit_ops.h"
#include "chip_facts_a840.h"

static bool is_pow2_u64(uint64_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static bool align_up_u64(uint64_t val, uint64_t align, uint64_t *out)
{
    uint64_t rem;
    if (!is_pow2_u64(align)) {
        return false;
    }
    rem = val & (align - 1u);
    if (rem == 0u) {
        *out = val;
        return true;
    }
    if (val > UINT64_MAX - (align - rem)) {
        return false;
    }
    *out = val + (align - rem);
    return true;
}

static bool spans_overlap(uint64_t a_off, uint64_t a_sz,
                          uint64_t b_off, uint64_t b_sz,
                          uint64_t *out_ov_off, uint64_t *out_ov_sz)
{
    uint64_t a_end;
    uint64_t b_end;
    uint64_t start;
    uint64_t end;

    if (!a_sz || !b_sz) {
        return false;
    }
    if (a_off > UINT64_MAX - a_sz || b_off > UINT64_MAX - b_sz) {
        return false;
    }
    a_end = a_off + a_sz;
    b_end = b_off + b_sz;
    start = a_off > b_off ? a_off : b_off;
    end = a_end < b_end ? a_end : b_end;
    if (start >= end) {
        return false;
    }
    if (out_ov_off) {
        *out_ov_off = start;
    }
    if (out_ov_sz) {
        *out_ov_sz = end - start;
    }
    return true;
}

static A840RtStatus ensure_free_cap(A840Heap *heap, size_t needed)
{
    size_t new_cap;
    A840HeapFreeBlock *grown;

    if (needed <= heap->free_cap) {
        return A840_RT_OK;
    }
    new_cap = heap->free_cap ? heap->free_cap : 16u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / sizeof(A840HeapFreeBlock)) {
        return A840_RT_OOM;
    }
    grown = (A840HeapFreeBlock *)realloc(heap->free_blocks, new_cap * sizeof(A840HeapFreeBlock));
    if (!grown) {
        return A840_RT_OOM;
    }
    heap->free_blocks = grown;
    heap->free_cap = new_cap;
    return A840_RT_OK;
}

static A840RtStatus ensure_span_cap(A840Heap *heap, size_t needed)
{
    size_t new_cap;
    A840HeapAliasSpan *grown;

    if (needed <= heap->span_cap) {
        return A840_RT_OK;
    }
    new_cap = heap->span_cap ? heap->span_cap : 32u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / sizeof(A840HeapAliasSpan)) {
        return A840_RT_OOM;
    }
    grown = (A840HeapAliasSpan *)realloc(heap->spans, new_cap * sizeof(A840HeapAliasSpan));
    if (!grown) {
        return A840_RT_OOM;
    }
    heap->spans = grown;
    heap->span_cap = new_cap;
    return A840_RT_OK;
}

/* Insert [offset, offset + size) into sorted free-list and coalesce neighbors. */
static A840RtStatus free_list_return(A840Heap *heap, uint64_t offset, uint64_t size)
{
    size_t pos = 0u;
    A840RtStatus st;

    if (!size) {
        return A840_RT_OK;
    }
    if (offset > heap->size_bytes || size > heap->size_bytes - offset) {
        return A840_RT_RANGE_ERROR;
    }

    while (pos < heap->free_count && heap->free_blocks[pos].offset < offset) {
        pos++;
    }

    if (pos > 0u) {
        uint64_t prev_end = heap->free_blocks[pos - 1u].offset + heap->free_blocks[pos - 1u].size;
        if (prev_end > offset) {
            return A840_RT_STATE_ERROR;
        }
    }
    if (pos < heap->free_count) {
        if (offset + size > heap->free_blocks[pos].offset) {
            return A840_RT_STATE_ERROR;
        }
    }

    st = ensure_free_cap(heap, heap->free_count + 1u);
    if (st != A840_RT_OK) {
        return st;
    }
    if (pos < heap->free_count) {
        memmove(&heap->free_blocks[pos + 1u],
                &heap->free_blocks[pos],
                (heap->free_count - pos) * sizeof(A840HeapFreeBlock));
    }
    heap->free_blocks[pos].offset = offset;
    heap->free_blocks[pos].size = size;
    heap->free_count++;

    /* Coalesce with right neighbor. */
    if (pos + 1u < heap->free_count &&
        heap->free_blocks[pos].offset + heap->free_blocks[pos].size == heap->free_blocks[pos + 1u].offset) {
        heap->free_blocks[pos].size += heap->free_blocks[pos + 1u].size;
        memmove(&heap->free_blocks[pos + 1u],
                &heap->free_blocks[pos + 2u],
                (heap->free_count - pos - 2u) * sizeof(A840HeapFreeBlock));
        heap->free_count--;
    }

    /* Coalesce with left neighbor. */
    if (pos > 0u &&
        heap->free_blocks[pos - 1u].offset + heap->free_blocks[pos - 1u].size == heap->free_blocks[pos].offset) {
        heap->free_blocks[pos - 1u].size += heap->free_blocks[pos].size;
        memmove(&heap->free_blocks[pos],
                &heap->free_blocks[pos + 1u],
                (heap->free_count - pos - 1u) * sizeof(A840HeapFreeBlock));
        heap->free_count--;
    }

    if (heap->used_bytes >= size) {
        heap->used_bytes -= size;
    } else {
        heap->used_bytes = 0u;
    }
    return A840_RT_OK;
}

/* First-fit aligned allocation from free-list for Automatic heaps. */
static A840RtStatus free_list_alloc(A840Heap *heap,
                                    uint64_t size,
                                    uint64_t alignment,
                                    uint64_t *out_offset)
{
    size_t i;

    for (i = 0u; i < heap->free_count; ++i) {
        uint64_t blk_start = heap->free_blocks[i].offset;
        uint64_t blk_size = heap->free_blocks[i].size;
        uint64_t blk_end = blk_start + blk_size;
        uint64_t aligned_start = 0u;
        uint64_t prefix = 0u;
        uint64_t suffix = 0u;

        if (!align_up_u64(blk_start, alignment, &aligned_start)) {
            continue;
        }
        if (aligned_start >= blk_end || size > blk_end - aligned_start) {
            continue;
        }

        prefix = aligned_start - blk_start;
        suffix = blk_end - (aligned_start + size);

        if (prefix > 0u && suffix > 0u) {
            A840RtStatus st = ensure_free_cap(heap, heap->free_count + 1u);
            if (st != A840_RT_OK) {
                return st;
            }
            memmove(&heap->free_blocks[i + 2u],
                    &heap->free_blocks[i + 1u],
                    (heap->free_count - i - 1u) * sizeof(A840HeapFreeBlock));
            heap->free_blocks[i].offset = blk_start;
            heap->free_blocks[i].size = prefix;
            heap->free_blocks[i + 1u].offset = aligned_start + size;
            heap->free_blocks[i + 1u].size = suffix;
            heap->free_count++;
        } else if (prefix > 0u) {
            heap->free_blocks[i].size = prefix;
        } else if (suffix > 0u) {
            heap->free_blocks[i].offset = aligned_start + size;
            heap->free_blocks[i].size = suffix;
        } else {
            memmove(&heap->free_blocks[i],
                    &heap->free_blocks[i + 1u],
                    (heap->free_count - i - 1u) * sizeof(A840HeapFreeBlock));
            heap->free_count--;
        }

        heap->used_bytes += size;
        *out_offset = aligned_start;
        return A840_RT_OK;
    }

    return A840_RT_OOM;
}

static A840RtStatus validate_alignment_param(uint64_t align, uint64_t min_align, uint64_t *out_align)
{
    uint64_t a = align ? align : min_align;
    if (!is_pow2_u64(a) || a < min_align || a > A840_HEAP_MAX_ALIGN_BYTES) {
        return A840_RT_ALIGNMENT;
    }
    *out_align = a;
    return A840_RT_OK;
}

/* Analyze overlap against active & aliasable spans, enforcing non-aliasable
 * exclusivity and deriving layout/UBWC alias transition requirements. */
static A840RtStatus evaluate_aliasing(A840Heap *heap,
                                      uint64_t offset,
                                      uint64_t size,
                                      uint32_t next_tile_mode,
                                      uint32_t next_mtl_format,
                                      uint64_t next_ubwc_flag_offset,
                                      uint64_t next_ubwc_flag_bytes,
                                      A840HeapAliasTransition *out_tr)
{
    size_t i;
    bool found_alias = false;
    uint64_t latest_epoch = 0u;

    memset(out_tr, 0, sizeof(*out_tr));
    out_tr->next_tile_mode = next_tile_mode;
    out_tr->next_mtl_format = next_mtl_format;
    out_tr->bo_handle = heap->backing_bo->bo_handle;

    for (i = 0u; i < heap->span_count; ++i) {
        const A840HeapAliasSpan *sp = &heap->spans[i];
        uint64_t ov_off = 0u;
        uint64_t ov_sz = 0u;

        if (!spans_overlap(offset, size, sp->offset, sp->size, &ov_off, &ov_sz)) {
            continue;
        }
        /* Active non-aliasable live resource cannot be overwritten by Placement/Auto. */
        if (sp->active && !sp->aliasable) {
            return A840_RT_STATE_ERROR;
        }

        if (!found_alias || sp->alias_epoch >= latest_epoch) {
            bool layout_changed = (sp->tile_mode != next_tile_mode) ||
                                  (sp->mtl_pixel_format != next_mtl_format);
            bool ubwc_involved = (sp->tile_mode == A840_TILE_UBWC) ||
                                 (next_tile_mode == A840_TILE_UBWC);

            found_alias = true;
            latest_epoch = sp->alias_epoch;
            out_tr->prev_tile_mode = sp->tile_mode;
            out_tr->prev_mtl_format = sp->mtl_pixel_format;
            out_tr->prev_alias_epoch = sp->alias_epoch;
            out_tr->overlap_offset = ov_off;
            out_tr->overlap_bytes = ov_sz;
            out_tr->requires_alias_barrier = true;

            if (ubwc_involved && layout_changed) {
                out_tr->ubwc_meta_invalidate_required = true;
                if (next_tile_mode == A840_TILE_UBWC && next_ubwc_flag_bytes > 0u) {
                    out_tr->ubwc_flag_va = heap->backing_bo->gpu_va + next_ubwc_flag_offset;
                    out_tr->ubwc_flag_bytes = next_ubwc_flag_bytes;
                } else if (sp->tile_mode == A840_TILE_UBWC && sp->ubwc_flag_bytes > 0u) {
                    out_tr->ubwc_flag_va = heap->backing_bo->gpu_va + sp->ubwc_flag_offset;
                    out_tr->ubwc_flag_bytes = sp->ubwc_flag_bytes;
                }
            }
        }
    }

    if (found_alias) {
        heap->alias_reuse_count++;
        if (out_tr->requires_alias_barrier) {
            heap->alias_barrier_count++;
        }
    }
    return A840_RT_OK;
}

static A840RtStatus record_span(A840Heap *heap,
                                uint64_t offset,
                                uint64_t size,
                                uint32_t tile_mode,
                                uint32_t mtl_pixel_format,
                                uint64_t ubwc_flag_offset,
                                uint64_t ubwc_flag_bytes,
                                uint64_t *out_epoch,
                                size_t *out_index)
{
    A840RtStatus st;
    size_t idx;
    uint64_t epoch;

    if (heap->span_count >= A840_HEAP_MAX_ALIAS_RECORDS) {
        size_t oldest = SIZE_MAX;
        uint64_t oldest_epoch = UINT64_MAX;
        size_t i;
        for (i = 0u; i < heap->span_count; ++i) {
            if (!heap->spans[i].active && heap->spans[i].alias_epoch < oldest_epoch) {
                oldest = i;
                oldest_epoch = heap->spans[i].alias_epoch;
            }
        }
        if (oldest != SIZE_MAX) {
            idx = oldest;
        } else {
            return A840_RT_CAPACITY;
        }
    } else {
        st = ensure_span_cap(heap, heap->span_count + 1u);
        if (st != A840_RT_OK) {
            return st;
        }
        idx = heap->span_count++;
    }

    epoch = ++heap->next_epoch;
    heap->spans[idx].offset = offset;
    heap->spans[idx].size = size;
    heap->spans[idx].alias_epoch = epoch;
    heap->spans[idx].tile_mode = tile_mode;
    heap->spans[idx].mtl_pixel_format = mtl_pixel_format;
    heap->spans[idx].ubwc_flag_offset = ubwc_flag_offset;
    heap->spans[idx].ubwc_flag_bytes = ubwc_flag_bytes;
    heap->spans[idx].active = true;
    heap->spans[idx].aliasable = false;

    *out_epoch = epoch;
    *out_index = idx;
    return A840_RT_OK;
}

A840RtStatus a840_heap_create(A840RtDevice *device,
                              const A840HeapDesc *desc,
                              A840Heap **out_heap)
{
    A840Heap *heap;
    A840RtBufferDesc bdesc;
    A840RtStatus st;
    uint64_t align;
    uint64_t aligned_size;

    if (!device || !desc || !out_heap) {
        return A840_RT_INVALID_ARG;
    }
    *out_heap = NULL;

    if (desc->type != A840_HEAP_TYPE_AUTOMATIC && desc->type != A840_HEAP_TYPE_PLACEMENT) {
        return A840_RT_INVALID_ARG;
    }
    if (!desc->size_bytes) {
        return A840_RT_INVALID_ARG;
    }

    align = desc->alignment ? desc->alignment : A840_HEAP_MAX_ALIGN_BYTES;
    if (!is_pow2_u64(align) || align < A840_RT_PAGE_BYTES || align > A840_HEAP_MAX_ALIGN_BYTES) {
        return A840_RT_ALIGNMENT;
    }
    if (!align_up_u64(desc->size_bytes, A840_RT_PAGE_BYTES, &aligned_size)) {
        return A840_RT_RANGE_ERROR;
    }
    if (aligned_size > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    heap = (A840Heap *)calloc(1u, sizeof(*heap));
    if (!heap) {
        return A840_RT_OOM;
    }

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size_bytes = aligned_size;
    bdesc.alignment = align;
    bdesc.usage_flags = desc->usage_flags ? desc->usage_flags
                                          : (A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE);
    if (desc->cpu_visible) {
        bdesc.usage_flags |= A840_BUF_USAGE_CPU_VISIBLE;
    }

    st = a840_rt_buffer_create(device, &bdesc, &heap->backing_bo);
    if (st != A840_RT_OK) {
        free(heap);
        return st;
    }

    heap->device = device;
    heap->device_id = device->device_id;
    heap->type = desc->type;
    heap->purgeable_state = A840_PURGEABLE_NONVOLATILE;
    heap->refcount = 1u;
    heap->size_bytes = aligned_size;
    heap->alignment = align;
    heap->used_bytes = 0u;
    heap->next_epoch = 0u;

    if (desc->type == A840_HEAP_TYPE_AUTOMATIC) {
        st = ensure_free_cap(heap, 16u);
        if (st != A840_RT_OK) {
            a840_rt_buffer_release(heap->backing_bo);
            free(heap);
            return st;
        }
        heap->free_blocks[0].offset = 0u;
        heap->free_blocks[0].size = aligned_size;
        heap->free_count = 1u;
    }

    *out_heap = heap;
    return A840_RT_OK;
}

void a840_heap_retain(A840Heap *heap)
{
    if (!heap) {
        return;
    }
    heap->refcount++;
}

void a840_heap_release(A840Heap *heap)
{
    if (!heap) {
        return;
    }
    if (heap->refcount > 1u) {
        heap->refcount--;
        return;
    }
    if (heap->backing_bo) {
        a840_rt_buffer_release(heap->backing_bo);
    }
    free(heap->free_blocks);
    free(heap->spans);
    free(heap);
}

A840RtStatus a840_heap_set_purgeable(A840Heap *heap,
                                     A840PurgeableState new_state,
                                     A840PurgeableState *out_prev_state)
{
    A840PurgeableState prev;

    if (!heap) {
        return A840_RT_INVALID_ARG;
    }
    if (new_state != A840_PURGEABLE_NONVOLATILE &&
        new_state != A840_PURGEABLE_VOLATILE &&
        new_state != A840_PURGEABLE_EMPTY) {
        return A840_RT_INVALID_ARG;
    }
    prev = heap->purgeable_state;
    if (out_prev_state) {
        *out_prev_state = prev;
    }
    if (prev == A840_PURGEABLE_EMPTY && new_state != A840_PURGEABLE_EMPTY) {
        return A840_RT_STATE_ERROR;
    }
    heap->purgeable_state = new_state;
    return A840_RT_OK;
}

A840PurgeableState a840_heap_get_purgeable(const A840Heap *heap)
{
    return heap ? heap->purgeable_state : A840_PURGEABLE_EMPTY;
}

A840RtStatus a840_heap_validate_for_submission(const A840Heap *heap)
{
    if (!heap || !heap->backing_bo) {
        return A840_RT_INVALID_ARG;
    }
    if (heap->purgeable_state == A840_PURGEABLE_EMPTY) {
        return A840_RT_STATE_ERROR;
    }
    return A840_RT_OK;
}

uint64_t a840_heap_used_bytes(const A840Heap *heap)
{
    return heap ? heap->used_bytes : 0u;
}

uint64_t a840_heap_free_bytes(const A840Heap *heap)
{
    if (!heap || heap->used_bytes >= heap->size_bytes) {
        return 0u;
    }
    return heap->size_bytes - heap->used_bytes;
}

size_t a840_heap_free_block_count(const A840Heap *heap)
{
    return heap ? heap->free_count : 0u;
}

uint64_t a840_heap_backing_bo_handle(const A840Heap *heap)
{
    return (heap && heap->backing_bo) ? heap->backing_bo->bo_handle : 0u;
}

static A840RtStatus acquire_heap_range(A840Heap *heap,
                                       uint64_t alloc_size,
                                       uint64_t align,
                                       uint64_t placement_offset,
                                       uint64_t *out_offset)
{
    if (heap->purgeable_state == A840_PURGEABLE_EMPTY) {
        return A840_RT_STATE_ERROR;
    }
    if (!alloc_size || alloc_size > heap->size_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    if (heap->type == A840_HEAP_TYPE_PLACEMENT) {
        if ((placement_offset & (align - 1u)) != 0u) {
            return A840_RT_ALIGNMENT;
        }
        if (placement_offset > heap->size_bytes || alloc_size > heap->size_bytes - placement_offset) {
            return A840_RT_RANGE_ERROR;
        }
        *out_offset = placement_offset;
        return A840_RT_OK;
    }

    return free_list_alloc(heap, alloc_size, align, out_offset);
}

A840RtStatus a840_heap_alloc_buffer(A840Heap *heap,
                                    const A840HeapBufferDesc *desc,
                                    A840HeapSubAlloc **out_sub)
{
    A840HeapSubAlloc *sub;
    A840HeapAliasTransition tr;
    A840RtStatus st;
    uint64_t align = 0u;
    uint64_t aligned_size = 0u;
    uint64_t offset = 0u;
    uint64_t epoch = 0u;
    size_t span_idx = 0u;

    if (!heap || !desc || !out_sub) {
        return A840_RT_INVALID_ARG;
    }
    *out_sub = NULL;
    if (!desc->size_bytes) {
        return A840_RT_INVALID_ARG;
    }

    st = validate_alignment_param(desc->alignment, A840_HEAP_MIN_ALIGN_BYTES, &align);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!align_up_u64(desc->size_bytes, A840_HEAP_MIN_ALIGN_BYTES, &aligned_size)) {
        return A840_RT_RANGE_ERROR;
    }

    st = acquire_heap_range(heap, aligned_size, align, desc->placement_offset, &offset);
    if (st != A840_RT_OK) {
        return st;
    }
    st = evaluate_aliasing(heap, offset, aligned_size, A840_TILE_LINEAR, 0u, 0u, 0u, &tr);
    if (st != A840_RT_OK) {
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, aligned_size);
        }
        return st;
    }

    st = record_span(heap, offset, aligned_size, A840_TILE_LINEAR, 0u, 0u, 0u, &epoch, &span_idx);
    if (st != A840_RT_OK) {
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, aligned_size);
        }
        return st;
    }
    tr.next_alias_epoch = epoch;

    sub = (A840HeapSubAlloc *)calloc(1u, sizeof(*sub));
    if (!sub) {
        heap->spans[span_idx].active = false;
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, aligned_size);
        }
        return A840_RT_OOM;
    }

    sub->heap = heap;
    sub->device = heap->device;
    sub->device_id = heap->device_id;
    sub->kind = A840_HEAP_RES_BUFFER;
    sub->refcount = 1u;
    sub->bo_handle = heap->backing_bo->bo_handle;
    sub->offset = offset;
    sub->size_bytes = aligned_size;
    sub->alignment = align;
    sub->gpu_va = heap->backing_bo->gpu_va + offset;
    sub->cpu_ptr = heap->backing_bo->cpu_ptr
                       ? (void *)((uint8_t *)heap->backing_bo->cpu_ptr + offset)
                       : NULL;
    sub->alias_epoch = epoch;
    sub->span_index = span_idx;
    sub->tile_mode = A840_TILE_LINEAR;
    sub->last_transition = tr;

    if (heap->type == A840_HEAP_TYPE_PLACEMENT) {
        if (offset + aligned_size > heap->used_bytes) {
            heap->used_bytes = offset + aligned_size;
        }
    }
    heap->live_suballocs++;
    a840_heap_retain(heap);

    *out_sub = sub;
    return A840_RT_OK;
}

static uint32_t format_cpp_from_caps(const A840FormatCaps *caps)
{
    if (!caps || !caps->block_bits || (caps->block_bits % 8u) != 0u) {
        return 0u;
    }
    if ((caps->flags & A840_FORMAT_COMPRESSED) != 0u) {
        return 0u;
    }
    return caps->block_bits / 8u;
}

A840RtStatus a840_heap_alloc_texture(A840Heap *heap,
                                     const A840HeapTextureDesc *desc,
                                     A840HeapSubAlloc **out_sub)
{
    const A840FormatCaps *caps;
    A840HeapSubAlloc *sub;
    A840HeapAliasTransition tr;
    A840UbwcLayout ubwc;
    A840TexDescInput tex_in;
    uint32_t tex_const[A840_TEX_CONST_DWORDS];
    A840RtStatus st;
    A840EmitStatus est;
    uint32_t depth;
    uint32_t layers;
    uint32_t mips;
    uint32_t samples;
    uint32_t hbb;
    uint32_t pitch = 0u;
    uint32_t pitch_align_log2 = 0u;
    uint64_t slice_stride = 0u;
    uint64_t total_bytes = 0u;
    uint64_t min_align = 64u;
    uint64_t align = 0u;
    uint64_t offset = 0u;
    uint64_t data_offset_rel = 0u;
    uint64_t flag_offset_rel = 0u;
    uint64_t flag_bytes = 0u;
    uint64_t epoch = 0u;
    size_t span_idx = 0u;

    if (!heap || !desc || !out_sub) {
        return A840_RT_INVALID_ARG;
    }
    *out_sub = NULL;

    if (!desc->width || !desc->height ||
        desc->width > A840_LIMIT_MAX_TEXTURE_SIZE || desc->height > A840_LIMIT_MAX_TEXTURE_SIZE) {
        return A840_RT_RANGE_ERROR;
    }
    if (desc->tile_mode != A840_TILE_LINEAR &&
        desc->tile_mode != A840_TILE_TILED &&
        desc->tile_mode != A840_TILE_UBWC) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->tex_type != A840_TEX_1D && desc->tex_type != A840_TEX_2D &&
        desc->tex_type != A840_TEX_CUBE && desc->tex_type != A840_TEX_3D) {
        return A840_RT_INVALID_ARG;
    }

    caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    if (!caps || (caps->flags & A840_FORMAT_COMPRESSED) != 0u) {
        return A840_RT_UNSUPPORTED;
    }

    depth = desc->depth ? desc->depth : 1u;
    layers = desc->array_layers ? desc->array_layers
                                : (desc->tex_type == A840_TEX_CUBE ? 6u : 1u);
    mips = desc->mip_levels ? desc->mip_levels : 1u;
    samples = desc->samples ? desc->samples : 1u;
    hbb = desc->highest_bank_bit ? desc->highest_bank_bit : A840_KAANAPALI_HIGHEST_BANK_BIT;

    if (hbb != A840_KAANAPALI_HIGHEST_BANK_BIT) {
        return A840_RT_UNSUPPORTED;
    }

    memset(&ubwc, 0, sizeof(ubwc));
    if (desc->tile_mode == A840_TILE_UBWC) {
        if (!caps->ubwc_supported || desc->tex_type != A840_TEX_2D || mips != 1u || depth != 1u) {
            return A840_RT_UNSUPPORTED;
        }
        st = a840_ubwc_compute_layout(desc->mtl_pixel_format, desc->width, desc->height,
                                      layers, samples, &ubwc);
        if (st != A840_RT_OK) {
            return st;
        }
        min_align = A840_RT_PAGE_BYTES;
        pitch = ubwc.data_pitch;
        pitch_align_log2 = ubwc.data_pitch_align_log2;
        slice_stride = ubwc.data_slice_bytes;
        total_bytes = ubwc.total_bytes;
        flag_offset_rel = ubwc.flag_offset;
        flag_bytes = ubwc.flag_total_bytes;
        data_offset_rel = ubwc.data_offset;
    } else {
        uint32_t cpp = format_cpp_from_caps(caps);
        uint64_t row_bytes;
        uint64_t aligned_pitch = 0u;
        uint64_t padded_h;
        uint64_t total_slices;
        if (!cpp) {
            return A840_RT_UNSUPPORTED;
        }
        row_bytes = (uint64_t)desc->width * (uint64_t)cpp * (uint64_t)samples;
        if (desc->tile_mode == A840_TILE_LINEAR) {
            min_align = A840_HEAP_MIN_ALIGN_BYTES;
            if (!align_up_u64(row_bytes, 64u, &aligned_pitch) || aligned_pitch > UINT32_MAX) {
                return A840_RT_RANGE_ERROR;
            }
            pitch = (uint32_t)aligned_pitch;
            padded_h = desc->height;
            if (padded_h > UINT64_MAX / aligned_pitch) {
                return A840_RT_RANGE_ERROR;
            }
            if (!align_up_u64(aligned_pitch * padded_h, A840_RT_PAGE_BYTES, &slice_stride)) {
                return A840_RT_RANGE_ERROR;
            }
        } else {
            min_align = A840_RT_PAGE_BYTES;
            if (!align_up_u64(row_bytes, 256u, &aligned_pitch) || aligned_pitch > UINT32_MAX) {
                return A840_RT_RANGE_ERROR;
            }
            pitch = (uint32_t)aligned_pitch;
            pitch_align_log2 = 8u;
            if (!align_up_u64(desc->height, 16u, &padded_h)) {
                return A840_RT_RANGE_ERROR;
            }
            if (padded_h > UINT64_MAX / aligned_pitch) {
                return A840_RT_RANGE_ERROR;
            }
            if (!align_up_u64(aligned_pitch * padded_h, A840_RT_PAGE_BYTES, &slice_stride)) {
                return A840_RT_RANGE_ERROR;
            }
        }
        total_slices = (desc->tex_type == A840_TEX_3D) ? (uint64_t)depth : (uint64_t)layers;
        if (total_slices > UINT64_MAX / slice_stride) {
            return A840_RT_RANGE_ERROR;
        }
        total_bytes = slice_stride * total_slices;
        data_offset_rel = 0u;
    }

    st = validate_alignment_param(desc->alignment, min_align, &align);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!align_up_u64(total_bytes, A840_HEAP_MIN_ALIGN_BYTES, &total_bytes)) {
        return A840_RT_RANGE_ERROR;
    }

    st = acquire_heap_range(heap, total_bytes, align, desc->placement_offset, &offset);
    if (st != A840_RT_OK) {
        return st;
    }

    st = evaluate_aliasing(heap, offset, total_bytes, desc->tile_mode, desc->mtl_pixel_format,
                           offset + flag_offset_rel, flag_bytes, &tr);
    if (st != A840_RT_OK) {
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, total_bytes);
        }
        return st;
    }

    memset(&tex_in, 0, sizeof(tex_in));
    tex_in.format_id = (desc->tile_mode == A840_TILE_LINEAR) ? caps->a840_fmt_id : caps->tiled_fmt_id;
    tex_in.swap = (desc->tile_mode == A840_TILE_LINEAR) ? caps->swap : caps->tiled_swap;
    tex_in.type = desc->tex_type;
    tex_in.width = desc->width;
    tex_in.height = desc->height;
    tex_in.depth = (desc->tex_type == A840_TEX_3D) ? depth : layers;
    tex_in.mip_levels = mips;
    tex_in.samples = samples;
    tex_in.pitch = pitch;
    tex_in.slice_stride = slice_stride;
    tex_in.gpu_va = heap->backing_bo->gpu_va + offset + data_offset_rel;
    tex_in.tile_mode = desc->tile_mode;
    tex_in.highest_bank_bit = hbb;
    tex_in.srgb = (caps->flags & A840_FORMAT_SRGB) ? 1u : 0u;
    tex_in.pitch_align_log2 = pitch_align_log2;
    tex_in.swizzle[0] = A840_SWIZZLE_R;
    tex_in.swizzle[1] = A840_SWIZZLE_G;
    tex_in.swizzle[2] = A840_SWIZZLE_B;
    tex_in.swizzle[3] = A840_SWIZZLE_A;
    if (desc->tile_mode == A840_TILE_UBWC) {
        tex_in.ubwc_meta_va = heap->backing_bo->gpu_va + offset + flag_offset_rel;
        tex_in.ubwc_meta_pitch = ubwc.flag_pitch;
        tex_in.ubwc_meta_slice_stride = ubwc.flag_slice_bytes;
        tex_in.ubwc_block_width = ubwc.ubwc_block_width;
        tex_in.ubwc_block_height = ubwc.ubwc_block_height;
    }

    est = a840_encode_tex_const(&tex_in, tex_const);
    if (est != A840_EMIT_OK) {
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, total_bytes);
        }
        return a840_rt_status_from_emit(est);
    }

    st = record_span(heap, offset, total_bytes, desc->tile_mode, desc->mtl_pixel_format,
                     offset + flag_offset_rel, flag_bytes, &epoch, &span_idx);
    if (st != A840_RT_OK) {
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, total_bytes);
        }
        return st;
    }
    tr.next_alias_epoch = epoch;

    sub = (A840HeapSubAlloc *)calloc(1u, sizeof(*sub));
    if (!sub) {
        heap->spans[span_idx].active = false;
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
            (void)free_list_return(heap, offset, total_bytes);
        }
        return A840_RT_OOM;
    }

    sub->heap = heap;
    sub->device = heap->device;
    sub->device_id = heap->device_id;
    sub->kind = A840_HEAP_RES_TEXTURE;
    sub->refcount = 1u;
    sub->bo_handle = heap->backing_bo->bo_handle;
    sub->offset = offset;
    sub->size_bytes = total_bytes;
    sub->alignment = align;
    sub->gpu_va = heap->backing_bo->gpu_va + offset;
    sub->cpu_ptr = heap->backing_bo->cpu_ptr
                       ? (void *)((uint8_t *)heap->backing_bo->cpu_ptr + offset)
                       : NULL;
    sub->alias_epoch = epoch;
    sub->span_index = span_idx;

    sub->mtl_pixel_format = desc->mtl_pixel_format;
    sub->tile_mode = desc->tile_mode;
    sub->width = desc->width;
    sub->height = desc->height;
    sub->depth = depth;
    sub->array_layers = layers;
    sub->pitch_bytes = pitch;
    sub->slice_stride_bytes = slice_stride;
    sub->ubwc_layout = ubwc;
    sub->data_gpu_va = tex_in.gpu_va;
    sub->ubwc_flag_gpu_va = (desc->tile_mode == A840_TILE_UBWC) ? tex_in.ubwc_meta_va : 0u;
    memcpy(sub->tex_const, tex_const, sizeof(tex_const));
    sub->last_transition = tr;

    if (heap->type == A840_HEAP_TYPE_PLACEMENT) {
        if (offset + total_bytes > heap->used_bytes) {
            heap->used_bytes = offset + total_bytes;
        }
    }
    heap->live_suballocs++;
    a840_heap_retain(heap);

    *out_sub = sub;
    return A840_RT_OK;
}

A840RtStatus a840_heap_suballoc_make_aliasable(A840HeapSubAlloc *sub)
{
    A840Heap *heap;
    A840RtStatus st;

    if (!sub || !sub->heap || sub->destroyed) {
        return A840_RT_INVALID_ARG;
    }
    if (sub->aliasable) {
        return A840_RT_OK;
    }

    heap = sub->heap;
    if (sub->span_index < heap->span_count &&
        heap->spans[sub->span_index].alias_epoch == sub->alias_epoch) {
        heap->spans[sub->span_index].aliasable = true;
    }

    if (heap->type == A840_HEAP_TYPE_AUTOMATIC) {
        st = free_list_return(heap, sub->offset, sub->size_bytes);
        if (st != A840_RT_OK) {
            return st;
        }
    }

    sub->aliasable = true;
    return A840_RT_OK;
}

bool a840_heap_suballoc_is_aliasable(const A840HeapSubAlloc *sub)
{
    return sub ? sub->aliasable : false;
}

void a840_heap_suballoc_retain(A840HeapSubAlloc *sub)
{
    if (!sub) {
        return;
    }
    sub->refcount++;
}

void a840_heap_suballoc_release(A840HeapSubAlloc *sub)
{
    A840Heap *heap;

    if (!sub) {
        return;
    }
    if (sub->refcount > 1u) {
        sub->refcount--;
        return;
    }

    heap = sub->heap;
    if (heap && !sub->destroyed) {
        if (sub->span_index < heap->span_count &&
            heap->spans[sub->span_index].alias_epoch == sub->alias_epoch) {
            heap->spans[sub->span_index].active = false;
            heap->spans[sub->span_index].aliasable = true;
        }
        if (heap->type == A840_HEAP_TYPE_AUTOMATIC && !sub->aliasable) {
            (void)free_list_return(heap, sub->offset, sub->size_bytes);
        }
        if (heap->live_suballocs > 0u) {
            heap->live_suballocs--;
        }
        sub->destroyed = true;
        a840_heap_release(heap);
    }
    free(sub);
}

A840RtStatus a840_heap_suballoc_validate(const A840RtDevice *device,
                                         const A840HeapSubAlloc *sub)
{
    if (!device || !sub || !sub->heap || sub->destroyed) {
        return A840_RT_INVALID_ARG;
    }
    if (sub->device != device || sub->device_id != device->device_id) {
        return A840_RT_WRONG_DEVICE;
    }
    return a840_heap_validate_for_submission(sub->heap);
}

A840EmitStatus a840_heap_emit_alias_transition(A840CmdStream *cs,
                                               const A840HeapAliasTransition *transition)
{
    A840CmdStream cursor;
    A840EmitStatus st;
    uint32_t cache_mask;

    if (!cs || !transition) {
        return A840_EMIT_INVALID_ARG;
    }
    if (!transition->requires_alias_barrier && !transition->ubwc_meta_invalidate_required) {
        return A840_EMIT_OK;
    }

    cursor = *cs;
    cache_mask = A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_CCU_INV_COLOR | A840_CACHE_UCHE_INV;
    st = a840_emit_cache_flush_inv(&cursor, cache_mask);
    if (st != A840_EMIT_OK) {
        return st;
    }
    st = a840_emit_wait_mem_writes(&cursor);
    if (st != A840_EMIT_OK) {
        return st;
    }
    st = a840_emit_wfi(&cursor);
    if (st != A840_EMIT_OK) {
        return st;
    }

    if (transition->ubwc_meta_invalidate_required &&
        transition->ubwc_flag_va != 0u &&
        transition->ubwc_flag_bytes >= 8u) {
        if ((transition->ubwc_flag_va % 8u) != 0u) {
            return A840_EMIT_ALIGNMENT;
        }
        st = a840_emit_mem_write64(&cursor, transition->bo_handle, transition->ubwc_flag_va, 0u);
        if (st != A840_EMIT_OK) {
            return st;
        }
        st = a840_emit_wait_mem_writes(&cursor);
        if (st != A840_EMIT_OK) {
            return st;
        }
        st = a840_emit_cache_flush_inv(&cursor, A840_CACHE_CCU_INV_COLOR | A840_CACHE_UCHE_INV);
        if (st != A840_EMIT_OK) {
            return st;
        }
    }

    cs->used_dwords = cursor.used_dwords;
    cs->reloc_count = cursor.reloc_count;
    return A840_EMIT_OK;
}
