/* SPDX-License-Identifier: MIT */
#ifndef A840_EMIT_OPS_H
#define A840_EMIT_OPS_H

#include "Adreno840EmitABI.h"

#define A840_TEX_CONST_DWORDS 16u
#define A840_SAMP_CONST_DWORDS 4u
#define A840_EMIT_BUFFER_COPY_MAX_DWORDS 6u
#define A840_EMIT_2D_CLEAR_MAX_DWORDS 29u
#define A840_EMIT_2D_BLIT_MAX_DWORDS 39u
#define A840_EMIT_WFI_MAX_DWORDS 1u
#define A840_EMIT_WAIT_MEM_WRITES_MAX_DWORDS 1u
#define A840_EMIT_CACHE_FLUSH_INV_MAX_DWORDS 8u
#define A840_EMIT_MEM_WRITE64_MAX_DWORDS 5u
#define A840_EMIT_TIMESTAMP_EVENT_MAX_DWORDS 6u
#define A840_EMIT_OPS_MAX_DWORDS A840_EMIT_2D_BLIT_MAX_DWORDS
#define A840_EMIT_OPS_MAX_RELOCS 4u

/* Logical input enums, not register images. Type values match Mesa's enums. */
enum {
    A840_TEX_1D = 0,
    A840_TEX_2D = 1,
    A840_TEX_CUBE = 2,
    A840_TEX_3D = 3,
    A840_TILE_LINEAR = 0,
    A840_TILE_TILED = 1,
    A840_TILE_UBWC = 2,
    A840_SWIZZLE_R = 0,
    A840_SWIZZLE_G = 1,
    A840_SWIZZLE_B = 2,
    A840_SWIZZLE_A = 3,
    A840_SWIZZLE_ZERO = 4,
    A840_SWIZZLE_ONE = 5,
    A840_FILTER_NEAREST = 0,
    A840_FILTER_LINEAR = 1,
    A840_MIP_NONE = 0,
    A840_MIP_NEAREST = 1,
    A840_MIP_LINEAR = 2,
    A840_ADDRESS_REPEAT = 0,
    A840_ADDRESS_CLAMP_TO_EDGE = 1,
    A840_ADDRESS_MIRROR_REPEAT = 2,
    A840_ADDRESS_CLAMP_TO_BORDER = 3,
    A840_ADDRESS_MIRROR_CLAMP = 4,
    A840_COMPARE_DISABLED = 8,
    A840_CACHE_UCHE_INV = 1u << 0,
    A840_CACHE_CCU_FLUSH_COLOR = 1u << 1,
    A840_CACHE_CCU_FLUSH_DEPTH = 1u << 2,
    A840_CACHE_CCU_INV_COLOR = 1u << 3
};

/* Logical flag for event_id, never emitted as a hardware bit. With this flag
 * payload must be zero and the event samples the GPU always-on counter.
 * Without it the event writes the caller's full 64-bit completion marker.
 * Both modes use the A8XX CP_EVENT_WRITE7 layout and insert no host/GPU WFI. */
#define A840_TIMESTAMP_GPU_CLOCK (UINT32_C(1) << 31)

/* Encode the genuine 64B A8XX_TEX_MEMOBJ layout imported by a6xx.xml:15.
 * HBB=16/UBWC=6 are global Kaanapali settings, not descriptor bitfields.
 * highest_bank_bit is checked, not smuggled into reserved descriptor words.
 * Caller supplies a precomputed single-plane layout; no allocation/layout
 * generation here. Multi-plane, buffer and weight descriptors are unsupported.
 * depth is layer count (cube: multiple of six; encoded as cube count).
 * Pitch is in bytes, slice_stride in bytes/4KiB units. pitch_align_log2=0
 * selects a 64B linear or FDL format/sample-aware tiled baseline; otherwise
 * supply the layout value. Unknown tiled formats require an explicit value.
 * For 3D mip chains supply the real smallest mip's min_slice_stride.
 * UBWC requires the real metadata slice stride and block geometry, in addition
 * to metadata VA/pitch. These come from the retained FDL layout, not guesses. */
typedef struct A840TexDescInput {
    uint32_t format_id;
    uint32_t swap;
    uint32_t type;
    uint32_t width, height, depth;
    uint32_t mip_levels, samples;
    uint32_t pitch;
    uint64_t slice_stride;
    uint64_t gpu_va;
    uint32_t tile_mode;
    uint64_t ubwc_meta_va;
    uint32_t ubwc_meta_pitch;
    uint32_t highest_bank_bit;
    uint32_t swizzle[4];
    uint32_t srgb;
    uint32_t pitch_align_log2;
    uint32_t tile_all;
    uint64_t min_slice_stride;
    uint64_t ubwc_meta_slice_stride;
    uint32_t ubwc_block_width, ubwc_block_height;
} A840TexDescInput;

/* Floating LOD values use Mesa's truncation-to-8-fractional-bits encoding.
 * Finite range checks precede any float-to-integer conversion. max_aniso is
 * 1/2/4/8/16, not an encoded exponent; compare_func is hardware 0..7 or the
 * explicit A840_COMPARE_DISABLED sentinel. */
typedef struct A840SampDescInput {
    uint32_t min_filter, mag_filter, mip_filter;
    uint32_t address_s, address_t, address_r;
    uint32_t max_aniso;
    float min_lod, max_lod, lod_bias;
    uint32_t compare_func;
    uint32_t border_color_idx;
    uint32_t unnormalized_coords;
} A840SampDescInput;

/* Precomputed color surface; same logical tiling modes as texture input.
 * W01 2D operations support ordinary uncompressed color formats. Linear
 * VA/pitch align to 64B; tiled VA aligns to 4KiB with FDL row alignment.
 * Rectangles are positive, half-open and bounded by the surface.
 * Global HBB/UBWC/CCU setup and inter-operation dependencies remain the runtime
 * caller's responsibility; the explicit barrier emitters below are available.
 * No global init register, firmware state, memory or phone is touched here. */
typedef struct A840Surface2D {
    uint64_t bo_handle, gpu_va;
    uint32_t format_id, swap;
    uint32_t width, height, pitch, tile_mode, srgb;
    uint64_t ubwc_meta_va;
    uint32_t ubwc_meta_pitch;
    uint64_t ubwc_meta_slice_stride;
} A840Surface2D;

typedef struct A840Rect2D {
    uint32_t x, y, width, height;
} A840Rect2D;

typedef struct A840Clear2DDesc {
    A840Surface2D dst;
    A840Rect2D rect;
    /* Per-channel RB_A2D_CLEAR_COLOR values already converted to the format's
     * 2D intermediate type (UNORM8, half/float bits, signed/unsigned integer).
     * This is four channels, not an API float vector or one packed pixel. */
    uint32_t color[4];
} A840Clear2DDesc;

typedef struct A840Blit2DDesc {
    A840Surface2D src, dst;
    A840Rect2D src_rect, dst_rect;
    uint32_t filter;
} A840Blit2DDesc;

#ifdef __cplusplus
extern "C" {
#endif

/* All failures leave the entire output descriptor/CS/relocation storage intact.
 * Caller-owned input, output, and CS metadata must not overlap. */
A840EmitStatus a840_encode_tex_const(const A840TexDescInput *in, uint32_t out_words[16]);
A840EmitStatus a840_encode_samp_const(const A840SampDescInput *in, uint32_t out_words[4]);
A840EmitStatus a840_emit_buffer_copy(A840CmdStream *cs, uint64_t src_bo,
    uint64_t src_va, uint64_t dst_bo, uint64_t dst_va, uint32_t bytes);
A840EmitStatus a840_emit_2d_clear(A840CmdStream *cs, const A840Clear2DDesc *desc);
A840EmitStatus a840_emit_2d_blit(A840CmdStream *cs, const A840Blit2DDesc *desc);
A840EmitStatus a840_emit_wfi(A840CmdStream *cs);
A840EmitStatus a840_emit_wait_mem_writes(A840CmdStream *cs);
A840EmitStatus a840_emit_cache_flush_inv(A840CmdStream *cs, uint32_t mask);
A840EmitStatus a840_emit_mem_write64(A840CmdStream *cs, uint64_t bo,
    uint64_t dst_va, uint64_t value);
/* Supported timestamp events: RB_DONE_TS / CACHE_FLUSH_TS (Mesa identifiers).
 * GPU_CLOCK selects actual counter sampling; otherwise payload is a marker. */
A840EmitStatus a840_emit_timestamp_event(A840CmdStream *cs, uint32_t event_id,
    uint64_t bo, uint64_t dst_va, uint64_t payload);

#ifdef __cplusplus
}
#endif

#ifdef A840_EMIT_OPS_INTERNAL
/* Shared private helpers live here because the task permits exactly six files.
 * Every multi-packet operation is built in a bounded stack batch and committed
 * only after checking ALL target word/relocation capacity and aliases. */
#include <string.h>

typedef struct A840OpsSpan { uintptr_t address; size_t bytes; } A840OpsSpan;
typedef struct A840OpsBatch {
    uint32_t words[A840_EMIT_OPS_MAX_DWORDS];
    A840Reloc relocs[A840_EMIT_OPS_MAX_RELOCS];
    A840CmdStream cs;
} A840OpsBatch;
typedef struct A840OpsFormat {
    uint32_t id, cpp, ifmt, output_type, half;
} A840OpsFormat;

const A840OpsFormat *a840_ops_format_info(uint32_t format_id);

#define A840_OPS_TRY(call) do { \
    A840EmitStatus a840_ops_status_ = (call); \
    if (a840_ops_status_ != A840_EMIT_OK) return a840_ops_status_; \
} while (0)

static inline A840EmitStatus a840_ops_span(const void *ptr, size_t count,
    size_t size, size_t alignment, A840OpsSpan *span)
{
    span->address = (uintptr_t)ptr;
    span->bytes = 0;
    if (!count) return A840_EMIT_OK;
    if (!ptr) return A840_EMIT_INVALID_ARG;
    if (count > SIZE_MAX / size) return A840_EMIT_RANGE_ERROR;
    if (span->address % alignment) return A840_EMIT_ALIGNMENT;
    span->bytes = count * size;
    if (span->bytes > UINTPTR_MAX - span->address) return A840_EMIT_RANGE_ERROR;
    return A840_EMIT_OK;
}

static inline int a840_ops_overlap(A840OpsSpan a, A840OpsSpan b)
{
    if (!a.bytes || !b.bytes) return 0;
    return a.address <= b.address ? b.address - a.address < a.bytes
                                 : a.address - b.address < b.bytes;
}

static inline A840EmitStatus a840_ops_descriptor_storage(const void *in,
    size_t in_size, size_t in_alignment, uint32_t *out, size_t out_count)
{
    A840OpsSpan input, output;
    A840_OPS_TRY(a840_ops_span(in, 1, in_size, in_alignment, &input));
    A840_OPS_TRY(a840_ops_span(out, out_count, sizeof(*out), _Alignof(uint32_t), &output));
    return a840_ops_overlap(input, output) ? A840_EMIT_ALIAS : A840_EMIT_OK;
}

static inline A840EmitStatus a840_ops_cs_storage(A840CmdStream *cs,
    A840OpsSpan *words, A840OpsSpan *relocs, A840OpsSpan *metadata)
{
    A840_OPS_TRY(a840_ops_span(cs, 1, sizeof(*cs), _Alignof(A840CmdStream), metadata));
    if (cs->used_dwords > cs->capacity_dwords || cs->reloc_count > cs->reloc_capacity)
        return A840_EMIT_INVALID_ARG;
    A840_OPS_TRY(a840_ops_span(cs->words, cs->capacity_dwords,
        sizeof(*cs->words), _Alignof(uint32_t), words));
    A840_OPS_TRY(a840_ops_span(cs->relocs, cs->reloc_capacity,
        sizeof(*cs->relocs), _Alignof(A840Reloc), relocs));
    if (a840_ops_overlap(*words, *relocs) || a840_ops_overlap(*words, *metadata) ||
        a840_ops_overlap(*relocs, *metadata)) return A840_EMIT_ALIAS;
    return A840_EMIT_OK;
}

static inline A840EmitStatus a840_ops_input_storage(A840CmdStream *cs,
    const void *input, size_t size, size_t alignment)
{
    A840OpsSpan words, relocs, metadata, source;
    A840_OPS_TRY(a840_ops_cs_storage(cs, &words, &relocs, &metadata));
    A840_OPS_TRY(a840_ops_span(input, 1, size, alignment, &source));
    if (a840_ops_overlap(source, words) || a840_ops_overlap(source, relocs) ||
        a840_ops_overlap(source, metadata)) return A840_EMIT_ALIAS;
    return A840_EMIT_OK;
}

static inline A840EmitStatus a840_ops_batch_init(A840OpsBatch *batch)
{
    return a840_cs_init(&batch->cs, batch->words, A840_EMIT_OPS_MAX_DWORDS,
        batch->relocs, A840_EMIT_OPS_MAX_RELOCS);
}

static inline A840EmitStatus a840_ops_add_reloc(A840OpsBatch *batch,
    size_t at, uint64_t bo, uint64_t va, uint32_t flags)
{
    A840Reloc *reloc;
    if (batch->cs.reloc_count >= A840_EMIT_OPS_MAX_RELOCS) return A840_EMIT_CAPACITY;
    if (at >= batch->cs.used_dwords || at > UINT32_MAX) return A840_EMIT_RANGE_ERROR;
    reloc = &batch->relocs[batch->cs.reloc_count++];
    reloc->dword_offset = (uint32_t)at;
    reloc->flags = flags;
    reloc->bo_handle = bo;
    reloc->gpu_va = va;
    return A840_EMIT_OK;
}

static inline A840EmitStatus a840_ops_commit(A840CmdStream *cs, const A840OpsBatch *batch)
{
    A840OpsSpan words, relocs, metadata;
    size_t i, start, reloc_count;
    A840_OPS_TRY(a840_ops_cs_storage(cs, &words, &relocs, &metadata));
    if (batch->cs.used_dwords > cs->capacity_dwords - cs->used_dwords)
        return A840_EMIT_CAPACITY;
    reloc_count = cs->relocs ? batch->cs.reloc_count : 0;
    if (reloc_count > cs->reloc_capacity - cs->reloc_count) return A840_EMIT_CAPACITY;
    start = cs->used_dwords;
    for (i = 0; i < reloc_count; ++i)
        if (start > (size_t)UINT32_MAX - batch->relocs[i].dword_offset)
            return A840_EMIT_RANGE_ERROR;
    if (batch->cs.used_dwords)
        memcpy(cs->words + start, batch->words, batch->cs.used_dwords * sizeof(*cs->words));
    for (i = 0; i < reloc_count; ++i) {
        A840Reloc reloc = batch->relocs[i];
        reloc.dword_offset += (uint32_t)start;
        cs->relocs[cs->reloc_count + i] = reloc;
    }
    cs->used_dwords += batch->cs.used_dwords;
    cs->reloc_count += reloc_count;
    return A840_EMIT_OK;
}

static inline A840EmitStatus a840_ops_va(uint64_t va, uint64_t bytes, uint32_t alignment)
{
    const uint64_t limit = UINT64_C(1) << 48;
    if (!bytes || va >= limit || bytes > limit - va) return A840_EMIT_RANGE_ERROR;
    if (!va) return A840_EMIT_INVALID_ARG;
    if (va % alignment) return A840_EMIT_ALIGNMENT;
    return A840_EMIT_OK;
}
#endif /* A840_EMIT_OPS_INTERNAL */
#endif /* A840_EMIT_OPS_H */
