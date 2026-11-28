/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_emit_ops.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"
#include "../Adreno840/chip_facts_a840.h"

/* Ordinary single-plane color formats for the W01 2D path. Intermediate types
 * follow tu_clear_blit.cc:175-207; output types follow :734-747. Other valid
 * XML formats may be encoded as textures, but must not silently enter this
 * color blitter (compressed/depth/YUV require additional state/handling). */
static const A840OpsFormat color_formats[] = {
    {A840_FMT6_A8_UNORM, 1, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_UNORM, 1, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_SNORM, 1, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_UINT, 1, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_8_SINT, 1, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_4_4_4_4_UNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_5_5_5_1_UNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_1_5_5_5_UNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_5_6_5_UNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_UNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_SNORM, 2, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_UINT, 2, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_8_8_SINT, 2, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_16_UNORM, 2, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_SNORM, 2, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_FLOAT, 2, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_16_UINT, 2, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_16_SINT, 2, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_8_8_8_8_UNORM, 4, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_8_X8_UNORM, 4, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_8_8_SNORM, 4, A840_R2D_UNORM8, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_8_8_8_8_UINT, 4, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_8_8_8_8_SINT, 4, A840_R2D_INT8, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_9_9_9_E5_FLOAT, 4, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_10_10_10_2_UNORM, 4, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_10_10_10_2_UNORM_DEST, 4, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_10_10_10_2_UINT, 4, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_11_11_10_FLOAT, 4, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_16_16_UNORM, 4, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_16_SNORM, 4, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_16_FLOAT, 4, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_16_16_UINT, 4, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_16_16_SINT, 4, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_32_FLOAT, 4, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_32_UINT, 4, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_32_SINT, 4, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_16_16_16_16_UNORM, 8, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_16_16_16_SNORM, 8, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_16_16_16_16_FLOAT, 8, A840_R2D_FLOAT16, A840_OUTPUT_IFMT_2D_FLOAT, 1},
    {A840_FMT6_16_16_16_16_UINT, 8, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_16_16_16_16_SINT, 8, A840_R2D_INT16, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_32_32_FLOAT, 8, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_32_32_UINT, 8, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_32_32_SINT, 8, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_SINT, 0},
    {A840_FMT6_32_32_32_32_FLOAT, 16, A840_R2D_FLOAT32, A840_OUTPUT_IFMT_2D_FLOAT, 0},
    {A840_FMT6_32_32_32_32_UINT, 16, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_UINT, 0},
    {A840_FMT6_32_32_32_32_SINT, 16, A840_R2D_INT32, A840_OUTPUT_IFMT_2D_SINT, 0}
};

const A840OpsFormat *a840_ops_format_info(uint32_t format_id)
{
    size_t i;
    for (i = 0; i < sizeof(color_formats) / sizeof(color_formats[0]); ++i)
        if (color_formats[i].id == format_id) return &color_formats[i];
    return NULL;
}

static int valid_format(uint32_t format_id)
{
    static const uint32_t ids[] = {
        A840_FMT6_A8_UNORM, A840_FMT6_8_UNORM, A840_FMT6_8_SNORM, A840_FMT6_8_UINT,
        A840_FMT6_8_SINT, A840_FMT6_4_4_4_4_UNORM, A840_FMT6_5_5_5_1_UNORM, A840_FMT6_1_5_5_5_UNORM,
        A840_FMT6_5_6_5_UNORM, A840_FMT6_8_8_UNORM, A840_FMT6_8_8_SNORM, A840_FMT6_8_8_UINT,
        A840_FMT6_8_8_SINT, A840_FMT6_L8_A8_UNORM, A840_FMT6_16_UNORM, A840_FMT6_16_SNORM,
        A840_FMT6_16_FLOAT, A840_FMT6_16_UINT, A840_FMT6_16_SINT, A840_FMT6_8_8_8_UNORM,
        A840_FMT6_8_8_8_SNORM, A840_FMT6_8_8_8_UINT, A840_FMT6_8_8_8_SINT, A840_FMT6_8_8_8_8_UNORM,
        A840_FMT6_8_8_8_X8_UNORM, A840_FMT6_8_8_8_8_SNORM, A840_FMT6_8_8_8_8_UINT, A840_FMT6_8_8_8_8_SINT,
        A840_FMT6_9_9_9_E5_FLOAT, A840_FMT6_10_10_10_2_UNORM, A840_FMT6_10_10_10_2_UNORM_DEST, A840_FMT6_10_10_10_2_SNORM,
        A840_FMT6_10_10_10_2_UINT, A840_FMT6_10_10_10_2_SINT, A840_FMT6_11_11_10_FLOAT, A840_FMT6_16_16_UNORM,
        A840_FMT6_16_16_SNORM, A840_FMT6_16_16_FLOAT, A840_FMT6_16_16_UINT, A840_FMT6_16_16_SINT,
        A840_FMT6_32_UNORM, A840_FMT6_32_SNORM, A840_FMT6_32_FLOAT, A840_FMT6_32_UINT,
        A840_FMT6_32_SINT, A840_FMT6_32_FIXED, A840_FMT6_16_16_16_UNORM, A840_FMT6_16_16_16_SNORM,
        A840_FMT6_16_16_16_FLOAT, A840_FMT6_16_16_16_UINT, A840_FMT6_16_16_16_SINT, A840_FMT6_16_16_16_16_UNORM,
        A840_FMT6_16_16_16_16_SNORM, A840_FMT6_16_16_16_16_FLOAT, A840_FMT6_16_16_16_16_UINT, A840_FMT6_16_16_16_16_SINT,
        A840_FMT6_32_32_UNORM, A840_FMT6_32_32_SNORM, A840_FMT6_32_32_FLOAT, A840_FMT6_32_32_UINT,
        A840_FMT6_32_32_SINT, A840_FMT6_32_32_FIXED, A840_FMT6_32_32_32_UNORM, A840_FMT6_32_32_32_SNORM,
        A840_FMT6_32_32_32_UINT, A840_FMT6_32_32_32_SINT, A840_FMT6_32_32_32_FLOAT, A840_FMT6_32_32_32_FIXED,
        A840_FMT6_32_32_32_32_UNORM, A840_FMT6_32_32_32_32_SNORM, A840_FMT6_32_32_32_32_FLOAT, A840_FMT6_32_32_32_32_UINT,
        A840_FMT6_32_32_32_32_SINT, A840_FMT6_32_32_32_32_FIXED, A840_FMT6_G8R8B8R8_422_UNORM, A840_FMT6_R8G8R8B8_422_UNORM,
        A840_FMT6_R8_G8B8_2PLANE_420_UNORM, A840_FMT6_NV21, A840_FMT6_R8_G8_B8_3PLANE_420_UNORM, A840_FMT6_Z24_UNORM_S8_UINT_AS_R8G8B8A8,
        A840_FMT6_NV12_Y, A840_FMT6_NV12_UV, A840_FMT6_NV12_VU, A840_FMT6_NV12_4R,
        A840_FMT6_NV12_4R_Y, A840_FMT6_NV12_4R_UV, A840_FMT6_P010, A840_FMT6_P010_Y,
        A840_FMT6_P010_UV, A840_FMT6_TP10, A840_FMT6_TP10_Y, A840_FMT6_TP10_UV,
        A840_FMT6_Z24_UNORM_S8_UINT, A840_FMT6_ETC2_RG11_UNORM, A840_FMT6_ETC2_RG11_SNORM, A840_FMT6_ETC2_R11_UNORM,
        A840_FMT6_ETC2_R11_SNORM, A840_FMT6_ETC1, A840_FMT6_ETC2_RGB8, A840_FMT6_ETC2_RGBA8,
        A840_FMT6_ETC2_RGB8A1, A840_FMT6_DXT1, A840_FMT6_DXT3, A840_FMT6_DXT5,
        A840_FMT6_RGTC1_UNORM, A840_FMT6_RGTC1_UNORM_FAST, A840_FMT6_RGTC1_SNORM, A840_FMT6_RGTC1_SNORM_FAST,
        A840_FMT6_RGTC2_UNORM, A840_FMT6_RGTC2_UNORM_FAST, A840_FMT6_RGTC2_SNORM, A840_FMT6_RGTC2_SNORM_FAST,
        A840_FMT6_BPTC_UFLOAT, A840_FMT6_BPTC_FLOAT, A840_FMT6_BPTC, A840_FMT6_ASTC_4x4,
        A840_FMT6_ASTC_5x4, A840_FMT6_ASTC_5x5, A840_FMT6_ASTC_6x5, A840_FMT6_ASTC_6x6,
        A840_FMT6_ASTC_8x5, A840_FMT6_ASTC_8x6, A840_FMT6_ASTC_8x8, A840_FMT6_ASTC_10x5,
        A840_FMT6_ASTC_10x6, A840_FMT6_ASTC_10x8, A840_FMT6_ASTC_10x10, A840_FMT6_ASTC_12x10,
        A840_FMT6_ASTC_12x12, A840_FMT6_Z24_UINT_S8_UINT
    };
    size_t i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        if (ids[i] == format_id) return 1;
    return 0;
}

static uint32_t log2_floor(uint32_t value)
{
    uint32_t result = 0;
    while (value > 1) { value >>= 1; ++result; }
    return result;
}

static uint32_t log2_ceil(uint32_t value)
{
    uint32_t result = 0;
    if (value) --value;
    while (value) { value >>= 1; ++result; }
    return result;
}

static int power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

static uint32_t tiled_pitch_alignment(const A840OpsFormat *format, uint32_t samples)
{
    uint32_t cpp = format->cpp * samples;
    /* fd6_layout.c:79-91,220; freedreno_layout.h:349-358 (R8G8 special case). */
    if (cpp == 1 || (samples == 1 && (format->id == A840_FMT6_8_8_UNORM ||
        format->id == A840_FMT6_8_8_SNORM || format->id == A840_FMT6_8_8_UINT ||
        format->id == A840_FMT6_8_8_SINT))) return 7;
    if (cpp == 2) return 8;
    return 6 + log2_floor(cpp);
}

static A840EmitStatus texture_validate(const A840TexDescInput *in)
{
    uint32_t dimension, encoded_depth, pitch_alignment;
    const A840OpsFormat *format;
    if (!in->width || !in->height || !in->depth || !in->pitch || !in->slice_stride)
        return A840_EMIT_INVALID_ARG;
    if (!valid_format(in->format_id) || in->swap > 3 || in->type > A840_TEX_3D ||
        in->tile_mode > A840_TILE_UBWC || in->srgb > 1 || in->tile_all > 1 ||
        in->width > A840_TEX_2_WIDTH_MAX || in->height > A840_TEX_2_HEIGHT_MAX ||
        !in->mip_levels || in->mip_levels > A840_TEX_6_MIPLVLS_MAX + 1 ||
        in->highest_bank_bit != A840_KAANAPALI_HIGHEST_BANK_BIT)
        return A840_EMIT_RANGE_ERROR;
    if (in->format_id == A840_FMT6_R8_G8B8_2PLANE_420_UNORM ||
        in->format_id == A840_FMT6_NV21 || in->format_id == A840_FMT6_R8_G8_B8_3PLANE_420_UNORM ||
        in->format_id == A840_FMT6_NV12_4R || in->format_id == A840_FMT6_P010 ||
        in->format_id == A840_FMT6_TP10) return A840_EMIT_UNSUPPORTED;
    if (in->type == A840_TEX_1D && in->height != 1) return A840_EMIT_INVALID_ARG;
    if (in->type == A840_TEX_CUBE && (in->width != in->height || in->depth % 6))
        return A840_EMIT_INVALID_ARG;
    encoded_depth = in->type == A840_TEX_CUBE ? in->depth / 6 : in->depth;
    if (encoded_depth > A840_TEX_1_DEPTH_MAX) return A840_EMIT_RANGE_ERROR;
    dimension = in->width > in->height ? in->width : in->height;
    if (in->type == A840_TEX_3D && in->depth > dimension) dimension = in->depth;
    if (in->mip_levels > log2_floor(dimension) + 1) return A840_EMIT_RANGE_ERROR;
    if (!power_of_two(in->samples) || in->samples > 8) return A840_EMIT_RANGE_ERROR;
    if (in->samples > 1 && (in->type != A840_TEX_2D || in->mip_levels != 1))
        return A840_EMIT_UNSUPPORTED;
    format = a840_ops_format_info(in->format_id);
    if (format && in->srgb && (format->ifmt != A840_R2D_UNORM8 ||
        in->format_id == A840_FMT6_8_SNORM || in->format_id == A840_FMT6_8_8_SNORM ||
        in->format_id == A840_FMT6_8_8_8_8_SNORM)) return A840_EMIT_UNSUPPORTED;
    if (in->tile_mode != A840_TILE_LINEAR && !in->pitch_align_log2 && !format)
        return A840_EMIT_UNSUPPORTED;
    pitch_alignment = in->pitch_align_log2 ? in->pitch_align_log2 :
        in->tile_mode == A840_TILE_LINEAR ? 6 : tiled_pitch_alignment(format, in->samples);
    if (pitch_alignment < 6 || pitch_alignment > A840_TEX_6_MIN_LINE_OFFSET_MAX + 6 ||
        in->pitch > A840_TEX_6_TEX_LINE_OFFSET_MAX / 8 ||
        (in->slice_stride >> A840_TEX_7_ARRAY_SLICE_OFFSET_SHR) > A840_TEX_7_ARRAY_SLICE_OFFSET_MAX)
        return A840_EMIT_RANGE_ERROR;
    if (in->pitch % (UINT32_C(1) << pitch_alignment) || in->slice_stride % 4096)
        return A840_EMIT_ALIGNMENT;
    if (format && in->tile_mode != A840_TILE_LINEAR &&
        in->pitch % (UINT32_C(1) << tiled_pitch_alignment(format, in->samples)))
        return A840_EMIT_ALIGNMENT;
    /* Known plain formats have texel rows; compressed-format row counts belong
     * to the caller's FDL layout and must not be mistaken for texel height. */
    if (format) {
        uint32_t rows = in->height;
        if (in->tile_mode != A840_TILE_LINEAR) {
            uint32_t alignment = format->cpp * in->samples == 1 ? 32 : 16;
            if (in->samples == 1 && (in->format_id == A840_FMT6_8_8_UNORM ||
                in->format_id == A840_FMT6_8_8_SNORM || in->format_id == A840_FMT6_8_8_UINT ||
                in->format_id == A840_FMT6_8_8_SINT))
                alignment = 32;
            rows = (rows + alignment - 1) / alignment * alignment;
        }
        if (in->pitch < (uint64_t)in->width * format->cpp * in->samples ||
            in->slice_stride < (uint64_t)in->pitch * rows) return A840_EMIT_RANGE_ERROR;
    }
    if (in->type == A840_TEX_3D) {
        uint64_t min_stride = in->min_slice_stride;
        if (!min_stride && in->mip_levels == 1) min_stride = in->slice_stride;
        if (!min_stride || min_stride > in->slice_stride) return A840_EMIT_INVALID_ARG;
        if (min_stride % 4096) return A840_EMIT_ALIGNMENT;
        if ((min_stride >> A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_SHR) > A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_MAX)
            return A840_EMIT_RANGE_ERROR;
    } else if (in->min_slice_stride) return A840_EMIT_INVALID_ARG;
    A840_OPS_TRY(a840_ops_va(in->gpu_va, in->slice_stride * in->depth,
        in->tile_mode == A840_TILE_LINEAR ? 64 : 4096));
    {
        size_t i;
        for (i = 0; i < 4; ++i)
            if (in->swizzle[i] > A840_SWIZZLE_ONE) return A840_EMIT_RANGE_ERROR;
    }
    if (in->tile_mode != A840_TILE_UBWC) {
        if (in->ubwc_meta_va || in->ubwc_meta_pitch || in->ubwc_meta_slice_stride ||
            in->ubwc_block_width || in->ubwc_block_height) return A840_EMIT_INVALID_ARG;
    } else {
        uint32_t logw, logh;
        if (!in->ubwc_meta_pitch || !in->ubwc_meta_slice_stride)
            return A840_EMIT_INVALID_ARG;
        if (!power_of_two(in->ubwc_block_width) || !power_of_two(in->ubwc_block_height) ||
            in->ubwc_block_width > 64 || in->ubwc_block_height > 32)
            return A840_EMIT_RANGE_ERROR;
        if (in->ubwc_meta_pitch % 64 || in->ubwc_meta_slice_stride % 4096)
            return A840_EMIT_ALIGNMENT;
        if ((in->ubwc_meta_pitch >> A840_TEX_5_FLAG_BUFFER_PITCH_SHR) > A840_TEX_5_FLAG_BUFFER_PITCH_MAX ||
            (in->ubwc_meta_slice_stride >> A840_TEX_8_FLAG_ARRAY_PITCH_SHR) > A840_TEX_8_FLAG_ARRAY_PITCH_MAX)
            return A840_EMIT_RANGE_ERROR;
        if (in->ubwc_meta_slice_stride < (uint64_t)in->ubwc_meta_pitch *
            ((in->height + in->ubwc_block_height - 1) / in->ubwc_block_height))
            return A840_EMIT_RANGE_ERROR;
        logw = log2_ceil((in->width + in->ubwc_block_width - 1) / in->ubwc_block_width);
        logh = log2_ceil((in->height + in->ubwc_block_height - 1) / in->ubwc_block_height);
        if (logw > A840_TEX_8_FLAG_BUFFER_LOGW_MAX || logh > A840_TEX_8_FLAG_BUFFER_LOGH_MAX)
            return A840_EMIT_RANGE_ERROR;
        if (in->type == A840_TEX_3D && in->mip_levels > 1) return A840_EMIT_UNSUPPORTED;
        A840_OPS_TRY(a840_ops_va(in->ubwc_meta_va, in->ubwc_meta_slice_stride * in->depth, 64));
        if (in->gpu_va < in->ubwc_meta_va + in->ubwc_meta_slice_stride * in->depth &&
            in->ubwc_meta_va < in->gpu_va + in->slice_stride * in->depth)
            return A840_EMIT_ALIAS;
    }
    return A840_EMIT_OK;
}

A840EmitStatus a840_encode_tex_const(const A840TexDescInput *in, uint32_t out_words[16])
{
    uint32_t words[A840_TEX_CONST_DWORDS] = {0};
    uint32_t pitch_alignment, depth;
    size_t i;
    A840_OPS_TRY(a840_ops_descriptor_storage(in, sizeof(*in), _Alignof(A840TexDescInput),
        out_words, A840_TEX_CONST_DWORDS));
    A840_OPS_TRY(texture_validate(in));
    pitch_alignment = in->pitch_align_log2 ? in->pitch_align_log2 :
        in->tile_mode == A840_TILE_LINEAR ? 6 :
        tiled_pitch_alignment(a840_ops_format_info(in->format_id), in->samples);
    depth = in->type == A840_TEX_CUBE ? in->depth / 6 : in->depth;
    words[0] = (uint32_t)in->gpu_va;
    words[1] = (uint32_t)(in->gpu_va >> 32) |
        (in->type << A840_TEX_1_TYPE_SHIFT) | (depth << A840_TEX_1_DEPTH_SHIFT);
    words[2] = in->width | (in->height << A840_TEX_2_HEIGHT_SHIFT) |
        (log2_floor(in->samples) << A840_TEX_2_SAMPLES_SHIFT);
    words[3] = in->format_id | (in->swap << A840_TEX_3_SWAP_SHIFT);
    for (i = 0; i < 4; ++i) {
        /* a8xx_enums.xml:289-296; explicit XYZW=3..6, constants 0/1=1/2. */
        uint32_t swizzle = in->swizzle[i] < 4 ? in->swizzle[i] + 3 : in->swizzle[i] - 3;
        words[3] |= swizzle << (A840_TEX_3_SWIZ_X_SHIFT + (uint32_t)i * 3);
    }
    words[4] = in->tile_mode == A840_TILE_LINEAR ? A840_TILE6_LINEAR : A840_TILE6_3;
    if (in->tile_all) words[4] |= A840_TEX_4_TILE_ALL;
    if (in->srgb) words[4] |= A840_TEX_4_SRGB;
    words[6] = in->pitch * 8 | ((pitch_alignment - 6) << A840_TEX_6_MIN_LINE_OFFSET_SHIFT) |
        ((in->mip_levels - 1) << A840_TEX_6_MIPLVLS_SHIFT);
    words[7] = (uint32_t)(in->slice_stride >> A840_TEX_7_ARRAY_SLICE_OFFSET_SHR);
    if (in->type == A840_TEX_3D) {
        uint64_t min_stride = in->min_slice_stride ? in->min_slice_stride : in->slice_stride;
        words[7] |= (uint32_t)(min_stride >> A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_SHR) <<
            A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_SHIFT;
    }
    if (in->tile_mode == A840_TILE_UBWC) {
        uint32_t logw = log2_ceil((in->width + in->ubwc_block_width - 1) / in->ubwc_block_width);
        uint32_t logh = log2_ceil((in->height + in->ubwc_block_height - 1) / in->ubwc_block_height);
        words[4] |= A840_TEX_4_FLAG | (uint32_t)in->ubwc_meta_va;
        words[5] = (uint32_t)(in->ubwc_meta_va >> 32) |
            ((in->ubwc_meta_pitch >> A840_TEX_5_FLAG_BUFFER_PITCH_SHR) << A840_TEX_5_FLAG_BUFFER_PITCH_SHIFT);
        words[8] = (uint32_t)(in->ubwc_meta_slice_stride >> A840_TEX_8_FLAG_ARRAY_PITCH_SHR) |
            (logw << A840_TEX_8_FLAG_BUFFER_LOGW_SHIFT) | (logh << A840_TEX_8_FLAG_BUFFER_LOGH_SHIFT);
    }
    memcpy(out_words, words, sizeof(words));
    return A840_EMIT_OK;
}

A840EmitStatus a840_encode_samp_const(const A840SampDescInput *in, uint32_t out_words[4])
{
    uint32_t words[A840_SAMP_CONST_DWORDS] = {0};
    uint32_t aniso, min_filter, mag_filter;
    const float lod_max = (float)A840_SAMP_1_MAX_LOD_MAX / 256.0f;
    const float bias_max = (float)(A840_SAMP_0_LOD_BIAS_MAX >> 1) / 256.0f;
    A840_OPS_TRY(a840_ops_descriptor_storage(in, sizeof(*in), _Alignof(A840SampDescInput),
        out_words, A840_SAMP_CONST_DWORDS));
    if (in->min_filter > A840_FILTER_LINEAR || in->mag_filter > A840_FILTER_LINEAR ||
        in->mip_filter > A840_MIP_LINEAR || in->address_s > A840_ADDRESS_MIRROR_CLAMP ||
        in->address_t > A840_ADDRESS_MIRROR_CLAMP || in->address_r > A840_ADDRESS_MIRROR_CLAMP ||
        !power_of_two(in->max_aniso) || in->max_aniso > 16 ||
        in->compare_func > A840_COMPARE_DISABLED || in->border_color_idx > A840_SAMP_2_BCOLOR_MAX ||
        in->unnormalized_coords > 1 || !(in->min_lod >= 0.0f && in->min_lod <= lod_max) ||
        !(in->max_lod >= in->min_lod && in->max_lod <= lod_max) ||
        !(in->lod_bias >= -16.0f && in->lod_bias <= bias_max))
        return A840_EMIT_RANGE_ERROR;
    if (in->unnormalized_coords && (in->max_aniso != 1 || in->mip_filter != A840_MIP_NONE ||
        in->min_filter != in->mag_filter || in->min_lod != 0.0f || in->max_lod != 0.0f ||
        in->lod_bias != 0.0f || in->compare_func != A840_COMPARE_DISABLED ||
        (in->address_s != A840_ADDRESS_CLAMP_TO_EDGE && in->address_s != A840_ADDRESS_CLAMP_TO_BORDER) ||
        (in->address_t != A840_ADDRESS_CLAMP_TO_EDGE && in->address_t != A840_ADDRESS_CLAMP_TO_BORDER) ||
        (in->address_r != A840_ADDRESS_CLAMP_TO_EDGE && in->address_r != A840_ADDRESS_CLAMP_TO_BORDER)))
        return A840_EMIT_INVALID_ARG;
    aniso = log2_floor(in->max_aniso);
    min_filter = in->min_filter == A840_FILTER_LINEAR && aniso ? A840_A6XX_TEX_ANISO : in->min_filter;
    mag_filter = in->mag_filter == A840_FILTER_LINEAR && aniso ? A840_A6XX_TEX_ANISO : in->mag_filter;
    words[0] = (mag_filter << A840_SAMP_0_XY_MAG_SHIFT) | (min_filter << A840_SAMP_0_XY_MIN_SHIFT) |
        (in->address_s << A840_SAMP_0_WRAP_S_SHIFT) | (in->address_t << A840_SAMP_0_WRAP_T_SHIFT) |
        (in->address_r << A840_SAMP_0_WRAP_R_SHIFT) | (aniso << A840_SAMP_0_ANISO_SHIFT);
    /* Range validated above; signed fixed-point needs its two's-complement field. */
    words[0] |= ((uint32_t)(int32_t)(in->lod_bias * 256.0f) & A840_SAMP_0_LOD_BIAS_MAX) <<
        A840_SAMP_0_LOD_BIAS_SHIFT;
    if (in->mip_filter == A840_MIP_LINEAR) words[0] |= A840_SAMP_0_MIPFILTER_LINEAR_NEAR;
    if (in->mip_filter == A840_MIP_NONE) words[0] |= A840_SAMP_0_MIPMAPING_DIS;
    words[1] = (uint32_t)(in->max_lod * 256.0f) |
        ((uint32_t)(in->min_lod * 256.0f) << A840_SAMP_1_MIN_LOD_SHIFT);
    if (in->compare_func != A840_COMPARE_DISABLED)
        words[1] |= in->compare_func << A840_SAMP_1_COMPARE_FUNC_SHIFT;
    if (in->unnormalized_coords) words[1] |= A840_SAMP_1_UNNORM_COORDS;
    words[2] = in->border_color_idx << A840_SAMP_2_BCOLOR_SHIFT;
    memcpy(out_words, words, sizeof(words));
    return A840_EMIT_OK;
}
