/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_emit_ops.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

static int va_overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    if (!a_bytes || !b_bytes) return 0;
    return a <= b ? b - a < a_bytes : a - b < b_bytes;
}

static uint64_t surface_bytes(const A840Surface2D *surface)
{
    uint32_t height = surface->height;
    if (surface->tile_mode != A840_TILE_LINEAR) {
        const A840OpsFormat *format = a840_ops_format_info(surface->format_id);
        uint32_t alignment = format->cpp == 1 ? 32 : 16;
        if (format->id == A840_FMT6_8_8_UNORM || format->id == A840_FMT6_8_8_SNORM ||
            format->id == A840_FMT6_8_8_UINT || format->id == A840_FMT6_8_8_SINT)
            alignment = 32;
        height = (height + alignment - 1) / alignment * alignment;
    }
    /* fd6_layout.c:79-91,281-282: tiled storage includes padded tile rows. */
    return (uint64_t)surface->pitch * height;
}

A840EmitStatus a840_emit_buffer_copy(A840CmdStream *cs, uint64_t src_bo,
    uint64_t src_va, uint64_t dst_bo, uint64_t dst_va, uint32_t bytes)
{
    A840OpsBatch batch;
    uint32_t payload[5];
    if (!bytes) return A840_EMIT_INVALID_ARG;
    if (bytes % sizeof(uint32_t)) return A840_EMIT_ALIGNMENT;
    A840_OPS_TRY(a840_ops_va(src_va, bytes, 4));
    A840_OPS_TRY(a840_ops_va(dst_va, bytes, 4));
    if (va_overlap(src_va, bytes, dst_va, bytes)) return A840_EMIT_ALIAS;
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    /* adreno_pm4.xml:1386-1401; tu_device.cc:2357-2360. Count is DWORDS. */
    payload[0] = bytes / (uint32_t)sizeof(uint32_t);
    payload[1] = (uint32_t)src_va;
    payload[2] = (uint32_t)(src_va >> 32);
    payload[3] = (uint32_t)dst_va;
    payload[4] = (uint32_t)(dst_va >> 32);
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch.cs, A840_CP_MEMCPY, payload, 5));
    A840_OPS_TRY(a840_ops_add_reloc(&batch, 2, src_bo, src_va, A840_RELOC_READ));
    A840_OPS_TRY(a840_ops_add_reloc(&batch, 4, dst_bo, dst_va, A840_RELOC_WRITE));
    return a840_ops_commit(cs, &batch);
}

static A840EmitStatus surface_validate(const A840Surface2D *surface)
{
    const A840OpsFormat *format;
    uint64_t bytes;
    if (!surface->width || !surface->height || !surface->pitch)
        return A840_EMIT_INVALID_ARG;
    if (surface->format_id > A840_TEX_3_FMT_MAX || surface->swap > 3 ||
        surface->width > A840_TEX_2_WIDTH_MAX || surface->height > A840_TEX_2_HEIGHT_MAX ||
        surface->srgb > 1 || surface->tile_mode > A840_TILE_UBWC ||
        surface->pitch > A840_2D_SRC_PITCH_MAX ||
        (surface->pitch >> A840_2D_DST_PITCH_SHR) > A840_2D_DST_PITCH_MAX)
        return A840_EMIT_RANGE_ERROR;
    format = a840_ops_format_info(surface->format_id);
    if (!format) return A840_EMIT_UNSUPPORTED;
    if (surface->srgb && (format->ifmt != A840_R2D_UNORM8 ||
        surface->format_id == A840_FMT6_8_SNORM || surface->format_id == A840_FMT6_8_8_SNORM ||
        surface->format_id == A840_FMT6_8_8_8_8_SNORM)) return A840_EMIT_UNSUPPORTED;
    if (surface->pitch % 64) return A840_EMIT_ALIGNMENT;
    if (surface->tile_mode != A840_TILE_LINEAR) {
        uint32_t pitch_alignment = format->cpp == 1 ? 128 : format->cpp * 64;
        if (format->id == A840_FMT6_8_8_UNORM || format->id == A840_FMT6_8_8_SNORM ||
            format->id == A840_FMT6_8_8_UINT || format->id == A840_FMT6_8_8_SINT)
            pitch_alignment = 128;
        /* fd6_layout.c:79-91,220: 2-byte non-R8G8 uses 256-byte rows. */
        else if (format->cpp == 2) pitch_alignment = 256;
        if (surface->pitch % pitch_alignment) return A840_EMIT_ALIGNMENT;
    }
    if (surface->pitch < (uint64_t)surface->width * format->cpp)
        return A840_EMIT_RANGE_ERROR;
    bytes = surface_bytes(surface);
    A840_OPS_TRY(a840_ops_va(surface->gpu_va, bytes,
        surface->tile_mode == A840_TILE_LINEAR ? 64 : 4096));
    if (surface->tile_mode != A840_TILE_UBWC) {
        if (surface->ubwc_meta_va || surface->ubwc_meta_pitch || surface->ubwc_meta_slice_stride)
            return A840_EMIT_INVALID_ARG;
    } else {
        if (!surface->ubwc_meta_pitch || !surface->ubwc_meta_slice_stride)
            return A840_EMIT_INVALID_ARG;
        if (surface->ubwc_meta_pitch % 64 || surface->ubwc_meta_slice_stride % 4096)
            return A840_EMIT_ALIGNMENT;
        if ((surface->ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) > A840_2D_FLAG_PITCH_MAX ||
            (surface->ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR) > A840_2D_FLAG_ARRAY_MAX)
            return A840_EMIT_RANGE_ERROR;
        A840_OPS_TRY(a840_ops_va(surface->ubwc_meta_va, surface->ubwc_meta_slice_stride, 64));
        if (va_overlap(surface->gpu_va, bytes, surface->ubwc_meta_va,
            surface->ubwc_meta_slice_stride)) return A840_EMIT_ALIAS;
    }
    return A840_EMIT_OK;
}

static A840EmitStatus rect_validate(const A840Surface2D *surface,
    const A840Rect2D *rect, int destination)
{
    uint64_t right = (uint64_t)rect->x + rect->width;
    uint64_t bottom = (uint64_t)rect->y + rect->height;
    if (!rect->width || !rect->height) return A840_EMIT_INVALID_ARG;
    if (right > surface->width || bottom > surface->height)
        return A840_EMIT_RANGE_ERROR;
    if (destination && (right - 1 > A840_2D_COORD_MAX || bottom - 1 > A840_2D_COORD_MAX))
        return A840_EMIT_RANGE_ERROR;
    return A840_EMIT_OK;
}

static uint32_t rect_xy(uint32_t x, uint32_t y)
{
    return x | (y << A840_2D_COORD_Y_SHIFT);
}

static A840EmitStatus register_word(A840OpsBatch *batch, uint32_t reg, uint32_t value)
{
    return a840_cs_emit_pkt4(&batch->cs, reg, &value, 1);
}

static A840EmitStatus common_setup(A840OpsBatch *batch, const A840Surface2D *dst,
    const A840Surface2D *src, int clear)
{
    const A840OpsFormat *format = a840_ops_format_info(dst->format_id);
    const A840OpsFormat *src_format = src ? a840_ops_format_info(src->format_id) : format;
    uint32_t ifmt = dst->srgb ? A840_R2D_UNORM8_SRGB : format->ifmt;
    uint32_t control = (dst->format_id << A840_2D_FORMAT_SHIFT) |
        (UINT32_C(15) << A840_2D_MASK_SHIFT) | (ifmt << A840_2D_IFMT_SHIFT);
    /* Mesa tu_clear_blit.cc:680: SP_A2D_OUTPUT_INFO uses FP16 for 10_10_10_2_UNORM_DEST. */
    uint32_t sp_fmt = (dst->format_id == A840_FMT6_10_10_10_2_UNORM_DEST)
        ? A840_FMT6_16_16_16_16_FLOAT : dst->format_id;
    uint32_t output = (format->output_type << A840_2D_SP_TYPE_SHIFT) |
        (sp_fmt << A840_2D_SP_FORMAT_SHIFT) |
        (UINT32_C(15) << A840_2D_SP_MASK_SHIFT);
    if (clear) control |= A840_2D_SOLID_COLOR;
    if (src_format->half) output |= A840_2D_SP_HALF_PRECISION;
    if (dst->srgb) output |= A840_2D_SP_SRGB;
    /* tu_clear_blit.cc:698-747, r2d_setup_common<A8XX>. No blend or stale state. */
    A840_OPS_TRY(register_word(batch, A840_REG_RB_A2D_PIXEL_CNTL, 0));
    A840_OPS_TRY(register_word(batch, A840_REG_RB_A2D_BLT_CNTL, control));
    A840_OPS_TRY(register_word(batch, A840_REG_GRAS_A2D_BLT_CNTL, control));
    A840_OPS_TRY(register_word(batch, A840_REG_TPL1_A2D_BLT_CNTL,
        A840_A6XX_TEX_2D << A840_2D_TPL1_TYPE_SHIFT));
    return register_word(batch, A840_REG_SP_A2D_OUTPUT_INFO, output);
}

static A840EmitStatus destination_setup(A840OpsBatch *batch, const A840Surface2D *surface)
{
    uint32_t info = surface->format_id | (surface->swap << A840_2D_DST_SWAP_SHIFT);
    uint32_t payload[4], flags[3] = {0};
    size_t start;
    if (surface->tile_mode != A840_TILE_LINEAR)
        info |= A840_TILE6_3 << A840_2D_DST_TILE_SHIFT;
    if (surface->srgb) info |= A840_2D_DST_SRGB;
    if (surface->tile_mode == A840_TILE_UBWC) info |= A840_2D_DST_FLAGS;
    payload[0] = info;
    payload[1] = (uint32_t)surface->gpu_va;
    payload[2] = (uint32_t)(surface->gpu_va >> 32);
    payload[3] = surface->pitch >> A840_2D_DST_PITCH_SHR;
    start = batch->cs.used_dwords;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch->cs, A840_REG_RB_A2D_DEST_BUFFER_INFO, payload, 4));
    A840_OPS_TRY(a840_ops_add_reloc(batch, start + 2, surface->bo_handle,
        surface->gpu_va, A840_RELOC_WRITE));
    if (surface->tile_mode == A840_TILE_UBWC) {
        flags[0] = (uint32_t)surface->ubwc_meta_va;
        flags[1] = (uint32_t)(surface->ubwc_meta_va >> 32);
        flags[2] = (surface->ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) |
            ((uint32_t)(surface->ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR) << A840_2D_FLAG_ARRAY_SHIFT);
    }
    start = batch->cs.used_dwords;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch->cs, A840_REG_RB_A2D_DEST_FLAG_BUFFER_BASE, flags, 3));
    if (surface->tile_mode == A840_TILE_UBWC)
        A840_OPS_TRY(a840_ops_add_reloc(batch, start + 1, surface->bo_handle,
            surface->ubwc_meta_va, A840_RELOC_WRITE));
    return A840_EMIT_OK;
}

static A840EmitStatus source_setup(A840OpsBatch *batch, const A840Surface2D *surface,
    uint32_t filter)
{
    /* TPL1 source uses 10_10_10_2_UNORM (0x36), never _UNORM_DEST (0x37). */
    uint32_t src_fmt = (surface->format_id == A840_FMT6_10_10_10_2_UNORM_DEST)
        ? A840_FMT6_10_10_10_2_UNORM : surface->format_id;
    uint32_t info = src_fmt | (surface->swap << A840_2D_SRC_SWAP_SHIFT) |
        A840_2D_SRC_UNK20 | A840_2D_SRC_UNK22;
    uint32_t payload[5], flags[3] = {0};
    size_t start;
    if (surface->tile_mode != A840_TILE_LINEAR)
        info |= A840_TILE6_3 << A840_2D_SRC_TILE_SHIFT;
    if (surface->srgb) info |= A840_2D_SRC_SRGB;
    if (filter == A840_FILTER_LINEAR) info |= A840_2D_SRC_FILTER;
    if (surface->tile_mode == A840_TILE_UBWC) info |= A840_2D_SRC_FLAGS;
    payload[0] = info;
    payload[1] = surface->width | (surface->height << A840_2D_SRC_HEIGHT_SHIFT);
    payload[2] = (uint32_t)surface->gpu_va;
    payload[3] = (uint32_t)(surface->gpu_va >> 32);
    payload[4] = surface->pitch << A840_2D_SRC_PITCH_SHIFT;
    start = batch->cs.used_dwords;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch->cs, A840_REG_TPL1_A2D_SRC_TEXTURE_INFO, payload, 5));
    A840_OPS_TRY(a840_ops_add_reloc(batch, start + 3, surface->bo_handle,
        surface->gpu_va, A840_RELOC_READ));
    if (surface->tile_mode == A840_TILE_UBWC) {
        flags[0] = (uint32_t)surface->ubwc_meta_va;
        flags[1] = (uint32_t)(surface->ubwc_meta_va >> 32);
        flags[2] = (surface->ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) |
            ((uint32_t)(surface->ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR) << A840_2D_FLAG_ARRAY_SHIFT);
    }
    start = batch->cs.used_dwords;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch->cs, A840_REG_TPL1_A2D_SRC_TEXTURE_FLAG_BASE, flags, 3));
    if (surface->tile_mode == A840_TILE_UBWC)
        A840_OPS_TRY(a840_ops_add_reloc(batch, start + 1, surface->bo_handle,
            surface->ubwc_meta_va, A840_RELOC_READ));
    return A840_EMIT_OK;
}

static A840EmitStatus destination_rect(A840OpsBatch *batch, const A840Rect2D *rect)
{
    uint32_t payload[2];
    payload[0] = rect_xy(rect->x, rect->y);
    payload[1] = rect_xy(rect->x + rect->width - 1, rect->y + rect->height - 1);
    return a840_cs_emit_pkt4(&batch->cs, A840_REG_GRAS_A2D_DEST_TL, payload, 2);
}

static A840EmitStatus run_blit(A840OpsBatch *batch)
{
    /* Mesa r2d_run:tu_clear_blit.cc:797-798 uses SCALE for both solids/copies. */
    const uint32_t operation = A840_BLIT_OP_SCALE;
    return a840_cs_emit_pkt7(&batch->cs, A840_CP_BLIT, &operation, 1);
}

A840EmitStatus a840_emit_2d_clear(A840CmdStream *cs, const A840Clear2DDesc *desc)
{
    A840OpsBatch batch;
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840Clear2DDesc)));
    A840_OPS_TRY(surface_validate(&desc->dst));
    A840_OPS_TRY(rect_validate(&desc->dst, &desc->rect, 1));
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    A840_OPS_TRY(common_setup(&batch, &desc->dst, NULL, 1));
    A840_OPS_TRY(destination_rect(&batch, &desc->rect));
    A840_OPS_TRY(destination_setup(&batch, &desc->dst));
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch.cs, A840_REG_RB_A2D_CLEAR_COLOR_DW0, desc->color, 4));
    A840_OPS_TRY(run_blit(&batch));
    return a840_ops_commit(cs, &batch);
}

A840EmitStatus a840_emit_2d_blit(A840CmdStream *cs, const A840Blit2DDesc *desc)
{
    A840OpsBatch batch;
    const A840OpsFormat *src_format, *dst_format;
    uint64_t src_bytes, dst_bytes;
    uint32_t coords[4];
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840Blit2DDesc)));
    A840_OPS_TRY(surface_validate(&desc->src));
    A840_OPS_TRY(surface_validate(&desc->dst));
    A840_OPS_TRY(rect_validate(&desc->src, &desc->src_rect, 0));
    A840_OPS_TRY(rect_validate(&desc->dst, &desc->dst_rect, 1));
    if (desc->filter > A840_FILTER_LINEAR) return A840_EMIT_RANGE_ERROR;
    src_format = a840_ops_format_info(desc->src.format_id);
    dst_format = a840_ops_format_info(desc->dst.format_id);
    if (src_format->output_type != dst_format->output_type ||
        (desc->filter == A840_FILTER_LINEAR && src_format->output_type != A840_OUTPUT_IFMT_2D_FLOAT))
        return A840_EMIT_UNSUPPORTED;
    src_bytes = surface_bytes(&desc->src);
    dst_bytes = surface_bytes(&desc->dst);
    /* No memmove semantics; callers must not blit overlapping surface storage. */
    if (va_overlap(desc->src.gpu_va, src_bytes, desc->dst.gpu_va, dst_bytes) ||
        va_overlap(desc->src.gpu_va, src_bytes, desc->dst.ubwc_meta_va, desc->dst.ubwc_meta_slice_stride) ||
        va_overlap(desc->src.ubwc_meta_va, desc->src.ubwc_meta_slice_stride, desc->dst.gpu_va, dst_bytes) ||
        va_overlap(desc->src.ubwc_meta_va, desc->src.ubwc_meta_slice_stride,
            desc->dst.ubwc_meta_va, desc->dst.ubwc_meta_slice_stride)) return A840_EMIT_ALIAS;
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    A840_OPS_TRY(common_setup(&batch, &desc->dst, &desc->src, 0));
    A840_OPS_TRY(destination_rect(&batch, &desc->dst_rect));
    /* GRAS source coordinates are signed fixed point with eight low bits.
     * Positive bounded inputs avoid any signed-shift behavior. */
    coords[0] = desc->src_rect.x << A840_2D_SRC_COORD_SHIFT;
    coords[1] = (desc->src_rect.x + desc->src_rect.width - 1) << A840_2D_SRC_COORD_SHIFT;
    coords[2] = desc->src_rect.y << A840_2D_SRC_COORD_SHIFT;
    coords[3] = (desc->src_rect.y + desc->src_rect.height - 1) << A840_2D_SRC_COORD_SHIFT;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch.cs, A840_REG_GRAS_A2D_SRC_XMIN, coords, 4));
    A840_OPS_TRY(destination_setup(&batch, &desc->dst));
    A840_OPS_TRY(source_setup(&batch, &desc->src, desc->filter));
    A840_OPS_TRY(run_blit(&batch));
    return a840_ops_commit(cs, &batch);
}
