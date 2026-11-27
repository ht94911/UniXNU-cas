/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#define A840_EMIT_PIPELINE_INTERNAL
#include "a840_emit_msaa_tess_vis.h"

#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "a840_gmem_planner.h"
#include "chip_facts_a840.h"
#include "gen/a840_bitfields.h"
#include "gen/a840_gmem_bitfields.h"

#include <math.h>
#include <string.h>

/* Pure field shifts/masks/enum encodings verified against pinned Mesa 85fda3b36405:
 *   - a6xx.xml (MSAA, Depth/Stencil, Sample Counter, HS/DS, Stereo Rendering)
 *   - adreno_pm4.xml (ZPASS_DONE, CP_EVENT_WRITE7 sample count bits, DI_PT_PATCHES)
 * All register offsets and packet opcodes come strictly from Adreno840Regs.h. */
enum {
    /* adreno_common.xml:333-338 (a3xx_msaa_samples) */
    P4_MSAA_1X = 0u,
    P4_MSAA_2X = 1u,
    P4_MSAA_4X = 2u,
    P4_DEST_MSAA_DISABLE = 1u << 2, /* a6xx.xml:1832, 2268, 4467 */
    P4_SAMPLE_POS_LOCATION_ENABLE = 1u << 1, /* a6xx.xml:1840 */
    P4_SAMPLEFREQ_ENABLE = 1u << 0, /* a6xx.xml:2112, 2292 */

    /* a6xx.xml:2471 (RB_DEPTH_CNTL.Z_BOUNDS_ENABLE), 1655 (READ_ONLY) */
    P4_DEPTH_BOUNDS_ENABLE = 1u << 7,
    P4_DEPTH_BUFFER_READ_ONLY = 1u << 3,
    P4_ZMODE_MAX = 2u,

    /* a6xx.xml:2540-2543 (RB_SAMPLE_COUNTER_CNTL) */
    P4_SAMPLE_COUNTER_COPY = 1u << 1,

    /* adreno_pm4.xml:38, 1691-1696 (ZPASS_DONE & CP_EVENT_WRITE7 sample count) */
    P4_EVENT_ZPASS_DONE = 21u,
    P4_EV7_WRITE_SAMPLE_COUNT = 1u << 12,
    P4_EV7_SAMPLE_COUNT_END_OFFSET = 1u << 13,
    P4_EV7_WRITE_ACCUM_SAMPLE_COUNT_DIFF = 1u << 14,
    P4_VIS_SLOT_END_OFFSET_BYTES = 16u,
    P4_VIS_SLOT_RESULT_OFFSET_BYTES = 8u,

    /* a6xx.xml:2988-2999 (PC_CNTL), 3285-3299 (PC_HS_PARAM_1, PC_DS_PARAM) */
    P4_PC_CNTL_D3D_VERTEX_ORDERING = 1u << 2,
    P4_PC_HS_PARAM_1_MAX_SIZE = 2047u,
    P4_PC_DS_OUTPUT_SHIFT = 2u,
    P4_TESS_MIN_BUFFER_BYTES = 256u,
    P4_TESS_ALIGN_BYTES = 32u,

    /* adreno_pm4.xml:178-209, 872-887 (vgt_draw_initiator_a4xx patch fields) */
    P4_DI_PT_PATCHES0 = 31u,
    P4_DRAW_PATCH_TYPE_SHIFT = 12u,
    P4_DRAW_TESS_ENABLE = 1u << 17,

    /* a6xx.xml:1589-1602, 3013-3025 (stereo rendering / multiview) */
    P4_GRAS_SU_MULTIVIEW_ENABLE = 1u << 17,
    P4_STEREO_RT_INDEX_INCR = 1u << 0,
    P4_STEREO_VP_INDEX_INCR = 1u << 1,
    P4_STEREO_CNTL_ENABLE = 1u << 0,
    P4_STEREO_CNTL_DISABLE_MULTIPOS = 1u << 1,
    P4_STEREO_CNTL_VIEWS_SHIFT = 2u,
    P4_MAX_VIEWPORT_INDEX = 15u,
    P4_MAX_RT_INDEX = 15u,

    /* a6xx.xml:1843-1852 (programmable_msaa_pos 4-bit signed fixed radix 4) */
    P4_MSAA_POS_NIBBLE_MASK = 15u
};

static uint32_t float_bits(float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static uint32_t pack_xy_u16(uint32_t x, uint32_t y)
{
    return x | (y << A840_2D_COORD_Y_SHIFT);
}

static bool msaa_samples_to_hw(uint32_t samples, uint32_t *out_hw)
{
    switch (samples) {
    case 1u:
        *out_hw = P4_MSAA_1X;
        return true;
    case 2u:
        *out_hw = P4_MSAA_2X;
        return true;
    case 4u:
        *out_hw = P4_MSAA_4X;
        return true;
    default:
        return false;
    }
}

/* Encode [0.0, 1.0) pixel-relative coordinate into 4-bit signed fixed (radix 4)
 * centered at (0.5, 0.5) as expected by A6XX/A8XX PROGRAMMABLE_MSAA_POS. */
static bool encode_sample_coord_s04(float coord, uint32_t *out_nibble)
{
    float centered;
    int32_t q;
    if (!isfinite(coord) || coord < 0.0f || coord >= 1.0f) {
        return false;
    }
    centered = (coord - 0.5f) * 16.0f;
    q = (int32_t)floorf(centered + 0.5f);
    if (q < -8) {
        q = -8;
    } else if (q > 7) {
        q = 7;
    }
    *out_nibble = (uint32_t)(q & (int32_t)P4_MSAA_POS_NIBBLE_MASK);
    return true;
}

/* Standard Metal / D3D11 / Vulkan MSAA sample locations (pixel-space [0,1)). */
static void standard_sample_locations(uint32_t sample_count,
                                      A840SampleLocation out_locs[A840_MSAA_MAX_SAMPLES])
{
    static const A840SampleLocation k_1x[1] = {
        {0.5f, 0.5f}
    };
    static const A840SampleLocation k_2x[2] = {
        {0.75f, 0.75f},
        {0.25f, 0.25f}
    };
    static const A840SampleLocation k_4x[4] = {
        {0.375f, 0.125f},
        {0.875f, 0.375f},
        {0.125f, 0.625f},
        {0.625f, 0.875f}
    };
    uint32_t i;
    for (i = 0u; i < A840_MSAA_MAX_SAMPLES; ++i) {
        out_locs[i].x = 0.5f;
        out_locs[i].y = 0.5f;
    }
    if (sample_count == 1u) {
        out_locs[0] = k_1x[0];
    } else if (sample_count == 2u) {
        out_locs[0] = k_2x[0];
        out_locs[1] = k_2x[1];
    } else if (sample_count == 4u) {
        for (i = 0u; i < 4u; ++i) {
            out_locs[i] = k_4x[i];
        }
    }
}

static bool event_resolve_format_supported(uint32_t fmt_id)
{
    switch (fmt_id) {
    case A840_FMT6_A8_UNORM:
    case A840_FMT6_8_UNORM:
    case A840_FMT6_8_UINT:
    case A840_FMT6_8_SINT:
    case A840_FMT6_8_8_8_8_UNORM:
    case A840_FMT6_8_8_8_8_UINT:
    case A840_FMT6_8_8_8_8_SINT:
    case A840_FMT6_8_8_8_X8_UNORM:
    case A840_FMT6_10_10_10_2_UNORM:
    case A840_FMT6_10_10_10_2_UINT:
        return true;
    default:
        return false;
    }
}

static uint32_t round_up_u32(uint32_t v, uint32_t align)
{
    return (v + align - 1u) / align * align;
}

static bool va_ranges_overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    if (!a_bytes || !b_bytes) {
        return false;
    }
    return a <= b ? (b - a < a_bytes) : (a - b < b_bytes);
}

static A840EmitStatus validate_resolve_dst(const A840Surface2D *dst,
                                           const A840Rect2D *rect,
                                           const A840OpsFormat **out_fmt)
{
    const A840OpsFormat *fmt;
    uint32_t pitch_align;
    uint32_t height_align;
    uint32_t padded_h;
    uint64_t data_bytes;

    if (!dst->width || !dst->height || !dst->pitch || !rect->width || !rect->height) {
        return A840_EMIT_INVALID_ARG;
    }
    if (dst->format_id > A840_TEX_3_FMT_MAX || dst->swap > 3u ||
        dst->width > A840_TEX_2_WIDTH_MAX || dst->height > A840_TEX_2_HEIGHT_MAX ||
        dst->srgb > 1u || dst->tile_mode > A840_TILE_UBWC ||
        dst->pitch > A840_2D_SRC_PITCH_MAX ||
        (dst->pitch >> A840_2D_DST_PITCH_SHR) > A840_2D_DST_PITCH_MAX ||
        (dst->pitch >> A840_MRT_PITCH_SHR) > A840_MRT_PITCH_MAX) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (rect->x > dst->width || rect->y > dst->height ||
        rect->width > dst->width - rect->x || rect->height > dst->height - rect->y ||
        rect->x + rect->width - 1u > A840_2D_COORD_MAX ||
        rect->y + rect->height - 1u > A840_2D_COORD_MAX) {
        return A840_EMIT_RANGE_ERROR;
    }

    fmt = a840_ops_format_info(dst->format_id);
    if (!fmt) {
        return A840_EMIT_UNSUPPORTED;
    }
    if (dst->srgb && (fmt->ifmt != A840_R2D_UNORM8 ||
                      dst->format_id == A840_FMT6_8_SNORM ||
                      dst->format_id == A840_FMT6_8_8_SNORM ||
                      dst->format_id == A840_FMT6_8_8_8_8_SNORM)) {
        return A840_EMIT_UNSUPPORTED;
    }
    if (dst->pitch % 64u != 0u) {
        return A840_EMIT_ALIGNMENT;
    }

    pitch_align = 64u;
    height_align = A840_LIMIT_GMEM_ALIGN_H;
    if (dst->tile_mode != A840_TILE_LINEAR) {
        pitch_align = fmt->cpp == 1u ? 128u : fmt->cpp * 64u;
        height_align = fmt->cpp == 1u ? 32u : 16u;
        if (fmt->id == A840_FMT6_8_8_UNORM || fmt->id == A840_FMT6_8_8_SNORM ||
            fmt->id == A840_FMT6_8_8_UINT || fmt->id == A840_FMT6_8_8_SINT) {
            pitch_align = 128u;
            height_align = 32u;
        } else if (fmt->cpp == 2u) {
            pitch_align = 256u;
        }
        if (dst->pitch % pitch_align != 0u) {
            return A840_EMIT_ALIGNMENT;
        }
    }
    if ((uint64_t)dst->pitch < (uint64_t)dst->width * (uint64_t)fmt->cpp) {
        return A840_EMIT_RANGE_ERROR;
    }

    padded_h = round_up_u32(dst->height, height_align);
    data_bytes = (uint64_t)dst->pitch * (uint64_t)padded_h;
    A840_OPS_TRY(a840_ops_va(dst->gpu_va, data_bytes,
                             dst->tile_mode == A840_TILE_LINEAR ? 64u : 4096u));

    if (dst->tile_mode != A840_TILE_UBWC) {
        if (dst->ubwc_meta_va || dst->ubwc_meta_pitch || dst->ubwc_meta_slice_stride) {
            return A840_EMIT_INVALID_ARG;
        }
    } else {
        if (!dst->ubwc_meta_pitch || !dst->ubwc_meta_slice_stride) {
            return A840_EMIT_INVALID_ARG;
        }
        if (dst->ubwc_meta_pitch % 64u != 0u || dst->ubwc_meta_slice_stride % 4096u != 0u) {
            return A840_EMIT_ALIGNMENT;
        }
        if ((dst->ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) > A840_2D_FLAG_PITCH_MAX ||
            (dst->ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR) > A840_2D_FLAG_ARRAY_MAX) {
            return A840_EMIT_RANGE_ERROR;
        }
        A840_OPS_TRY(a840_ops_va(dst->ubwc_meta_va, dst->ubwc_meta_slice_stride, 64u));
        if (va_ranges_overlap(dst->gpu_va, data_bytes,
                              dst->ubwc_meta_va, dst->ubwc_meta_slice_stride)) {
            return A840_EMIT_ALIAS;
        }
    }

    *out_fmt = fmt;
    return A840_EMIT_OK;
}

/* ============================================================================
 * 1. MSAA State & GMEM->Sysmem MSAA Resolve Blit
 * ============================================================================ */

A840EmitStatus a840_emit_msaa_state(A840CmdStream *cs, const A840MsaaStateDesc *desc)
{
    uint32_t words[A840_EMIT_MSAA_STATE_MAX_DWORDS];
    A840CmdStream batch;
    uint32_t hw_samples = 0u;
    uint32_t dest_cntl;
    uint32_t pos_cntl;
    uint32_t pos_word0 = 0u;
    uint32_t pos_words[2];
    A840SampleLocation locs[A840_MSAA_MAX_SAMPLES];
    uint32_t i;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840MsaaStateDesc)));
    if (!msaa_samples_to_hw(desc->sample_count, &hw_samples)) {
        return A840_EMIT_UNSUPPORTED;
    }

    if (desc->custom_sample_locations) {
        for (i = 0u; i < desc->sample_count; ++i) {
            locs[i] = desc->locations[i];
        }
        for (; i < A840_MSAA_MAX_SAMPLES; ++i) {
            locs[i].x = 0.5f;
            locs[i].y = 0.5f;
        }
    } else {
        standard_sample_locations(desc->sample_count, locs);
    }

    for (i = 0u; i < desc->sample_count; ++i) {
        uint32_t nx = 0u;
        uint32_t ny = 0u;
        if (!encode_sample_coord_s04(locs[i].x, &nx) ||
            !encode_sample_coord_s04(locs[i].y, &ny)) {
            return A840_EMIT_RANGE_ERROR;
        }
        pos_word0 |= (nx | (ny << 4u)) << (i * 8u);
    }

    dest_cntl = hw_samples | (desc->sample_count == 1u ? P4_DEST_MSAA_DISABLE : 0u);
    pos_cntl = (desc->sample_count > 1u || desc->custom_sample_locations)
                   ? P4_SAMPLE_POS_LOCATION_ENABLE
                   : 0u;
    pos_words[0] = pos_word0;
    pos_words[1] = 0u;

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_MSAA_STATE_MAX_DWORDS, NULL, 0u));

    /* Gen8 GRAS_SC_RAS_MSAA_CNTL (0x8232) + GRAS_SC_DEST_MSAA_CNTL (0x8233) */
    {
        uint32_t gras_msaa[2] = {hw_samples, dest_cntl};
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_GRAS_SC_RAS_MSAA_CNTL, gras_msaa, 2u));
    }
    /* RB_RAS_MSAA_CNTL (0x8802) + RB_DEST_MSAA_CNTL (0x8803) + RB_MSAA_SAMPLE_POS_CNTL (0x8804) */
    {
        uint32_t rb_msaa[3] = {hw_samples, dest_cntl, pos_cntl};
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_RB_RAS_MSAA_CNTL, rb_msaa, 3u));
    }
    /* TPL1_RAS_MSAA_CNTL (0xb300) + TPL1_DEST_MSAA_CNTL (0xb301) */
    {
        uint32_t tp_msaa[2] = {hw_samples, dest_cntl};
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_TPL1_RAS_MSAA_CNTL, tp_msaa, 2u));
    }
    /* GRAS sample position control + programmable sample positions (A8XX 0x8237..0x8239) */
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_SC_MSAA_SAMPLE_POS_CNTL, pos_cntl));
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_GRAS_SC_PROGRAMMABLE_MSAA_POS_0, pos_words, 2u));
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_RB_PROGRAMMABLE_MSAA_POS_0, pos_words, 2u));

    /* Per-sample FS execution controls (RB + GRAS LRZ) */
    {
        uint32_t samplefreq = (desc->sample_count > 1u && desc->sample_rate_shading)
                                  ? P4_SAMPLEFREQ_ENABLE
                                  : 0u;
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_PS_SAMPLEFREQ_CNTL, samplefreq));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_LRZ_PS_SAMPLEFREQ_CNTL, samplefreq));
    }

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_msaa_resolve_blit(A840CmdStream *cs,
                                           const A840MsaaResolveBlitDesc *desc)
{
    uint32_t words[A840_EMIT_MSAA_RESOLVE_BLIT_MAX_DWORDS];
    A840Reloc relocs[2];
    A840CmdStream batch;
    const A840OpsFormat *fmt = NULL;
    uint32_t hw_samples = 0u;
    uint32_t tile_w_enc;
    uint32_t tile_h_enc;
    uint32_t bin_size;
    bool use_2d;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc),
                                        _Alignof(A840MsaaResolveBlitDesc)));
    if (desc->src_samples != 2u && desc->src_samples != 4u) {
        return A840_EMIT_UNSUPPORTED;
    }
    (void)msaa_samples_to_hw(desc->src_samples, &hw_samples);
    if (desc->slot_index >= A840_GMEM_COLORS) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->mode != A840_MSAA_RESOLVE_AVERAGE && desc->mode != A840_MSAA_RESOLVE_SAMPLE0) {
        return A840_EMIT_INVALID_ARG;
    }
    if (desc->gmem_offset % A840_GMEM_BYTES_ALIGNMENT != 0u ||
        desc->gmem_offset >= A840_KAANAPALI_GMEM_BYTES) {
        return A840_EMIT_ALIGNMENT;
    }
    if (!desc->gmem_pitch || desc->gmem_pitch % 64u != 0u ||
        desc->gmem_pitch > A840_2D_SRC_PITCH_MAX) {
        return A840_EMIT_ALIGNMENT;
    }
    if (!desc->tile_w || !desc->tile_h ||
        desc->tile_w % A840_LIMIT_GMEM_ALIGN_W != 0u ||
        desc->tile_h % A840_LIMIT_GMEM_ALIGN_H != 0u ||
        desc->tile_w > A840_LIMIT_TILE_MAX_W || desc->tile_h > A840_LIMIT_TILE_MAX_H) {
        return A840_EMIT_RANGE_ERROR;
    }
    if ((uint64_t)desc->gmem_offset + (uint64_t)desc->gmem_pitch * (uint64_t)desc->tile_h >
        (uint64_t)A840_KAANAPALI_GMEM_BYTES) {
        return A840_EMIT_RANGE_ERROR;
    }
    A840_OPS_TRY(validate_resolve_dst(&desc->dst, &desc->resolve_rect, &fmt));
    if (desc->gmem_pitch < desc->tile_w * fmt->cpp * desc->src_samples) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->mode == A840_MSAA_RESOLVE_AVERAGE &&
        fmt->output_type != A840_OUTPUT_IFMT_2D_FLOAT) {
        /* Integer formats cannot box-average; caller must request SAMPLE0. */
        return A840_EMIT_UNSUPPORTED;
    }

    tile_w_enc = desc->tile_w >> GM_BIN_W_SHR;
    tile_h_enc = desc->tile_h >> GM_BIN_H_SHR;
    bin_size = tile_w_enc | (tile_h_enc << GM_BIN_H_SHIFT);
    use_2d = desc->use_2d_engine || !event_resolve_format_supported(desc->dst.format_id);

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_MSAA_RESOLVE_BLIT_MAX_DWORDS, relocs, 2u));

    if (desc->emit_ccu_uche_barriers) {
        A840_OPS_TRY(a840_emit_cache_flush_inv(&batch,
                                               A840_CACHE_CCU_FLUSH_COLOR |
                                                   A840_CACHE_UCHE_INV));
    }

    if (!use_2d) {
        /* Native RB CCU_RESOLVE path (GMEM MSAA -> 1x Sysmem Linear/Tiled/UBWC). */
        uint32_t tl = pack_xy_u16(desc->resolve_rect.x, desc->resolve_rect.y);
        uint32_t br = pack_xy_u16(desc->resolve_rect.x + desc->resolve_rect.width - 1u,
                                  desc->resolve_rect.y + desc->resolve_rect.height - 1u);
        uint32_t rect_words[2] = {tl, br};
        uint32_t sys_tile = (desc->dst.tile_mode == A840_TILE_LINEAR) ? A840_TILE6_LINEAR
                                                                       : A840_TILE6_3;
        uint32_t sys_info = sys_tile |
                            (desc->dst.format_id << GM_FORMAT_SHIFT) |
                            (desc->dst.swap << GM_SWAP_SHIFT) |
                            (desc->dst.tile_mode == A840_TILE_UBWC ? GM_FLAGS : 0u);
        uint32_t operation = GM_STORE | (desc->slot_index << GM_BUFFER_ID_SHIFT) |
                             (desc->mode == A840_MSAA_RESOLVE_SAMPLE0 ? GM_SAMPLE_ZERO : 0u);
        uint32_t ev_resolve = GM_EV_RESOLVE;
        uint32_t ev_group_end = GM_EV_GROUP_END;

        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_CNTL_0, 0u));
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_RB_RESOLVE_CNTL_1, rect_words, 2u));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_CNTL_3, bin_size));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_GMEM_BUFFER_INFO,
                                   hw_samples << GM_SAMPLES_SHIFT));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_GMEM_BUFFER_BASE,
                                   desc->gmem_offset));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_SYSTEM_BUFFER_INFO, sys_info));
        A840_OPS_TRY(a840_cs_emit_reg64_va(&batch, A840_REG_RB_RESOLVE_SYSTEM_BUFFER_BASE,
                                           desc->dst.bo_handle, desc->dst.gpu_va,
                                           A840_RELOC_WRITE));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_SYSTEM_BUFFER_PITCH,
                                   desc->dst.pitch >> A840_MRT_PITCH_SHR));
        if (desc->dst.tile_mode == A840_TILE_UBWC) {
            uint32_t flag_pitch =
                (desc->dst.ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) |
                ((uint32_t)(desc->dst.ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR)
                 << A840_2D_FLAG_ARRAY_SHIFT);
            A840_OPS_TRY(a840_cs_emit_reg64_va(&batch,
                                               A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_BASE,
                                               desc->dst.bo_handle, desc->dst.ubwc_meta_va,
                                               A840_RELOC_WRITE));
            A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_PITCH,
                                       flag_pitch));
        }
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_RESOLVE_OPERATION, operation));
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, &ev_resolve, 1u));
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, &ev_group_end, 1u));
    } else {
        /* A8XX 2D-engine MSAA resolve path (supports wide/FP16/SNORM/RG8 targets). */
        uint32_t blt_cntl = (desc->dst.format_id << A840_2D_FORMAT_SHIFT) |
                            (GM_RGBA_MASK << A840_2D_MASK_SHIFT) |
                            (fmt->ifmt << A840_2D_IFMT_SHIFT) |
                            GM_A2D_SCISSOR;
        uint32_t dst_tile = (desc->dst.tile_mode == A840_TILE_LINEAR) ? A840_TILE6_LINEAR
                                                                       : A840_TILE6_3;
        uint32_t dst_info = desc->dst.format_id |
                            (dst_tile << A840_2D_DST_TILE_SHIFT) |
                            (desc->dst.swap << A840_2D_DST_SWAP_SHIFT) |
                            (desc->dst.tile_mode == A840_TILE_UBWC ? A840_2D_DST_FLAGS : 0u) |
                            (desc->dst.srgb ? A840_2D_DST_SRGB : 0u);
        uint32_t sp_info = (fmt->output_type << A840_2D_SP_TYPE_SHIFT) |
                           (desc->dst.format_id << A840_2D_SP_FORMAT_SHIFT) |
                           (desc->dst.srgb ? A840_2D_SP_SRGB : 0u) |
                           (fmt->half ? A840_2D_SP_HALF_PRECISION : 0u) |
                           (GM_RGBA_MASK << A840_2D_SP_MASK_SHIFT);
        uint32_t src_info[5];
        uint32_t dst_rect_words[2];
        uint32_t src_coords[4];
        uint32_t ev_resolve_clean = GM_EV_RESOLVE_CLEAN;
        uint32_t blit_op = A840_BLIT_OP_SCALE;

        src_info[0] = desc->dst.format_id |
                      (GM_TILE6_2 << A840_2D_SRC_TILE_SHIFT) |
                      (desc->dst.swap << A840_2D_SRC_SWAP_SHIFT) |
                      (desc->dst.srgb ? A840_2D_SRC_SRGB : 0u) |
                      (hw_samples << GM_A2D_SAMPLES_SHIFT) |
                      (desc->mode == A840_MSAA_RESOLVE_AVERAGE ? GM_A2D_AVERAGE : 0u) |
                      A840_2D_SRC_UNK20 | A840_2D_SRC_UNK22;
        src_info[1] = desc->tile_w | (desc->tile_h << A840_2D_SRC_HEIGHT_SHIFT);
        src_info[2] = desc->gmem_offset;
        src_info[3] = 0u;
        src_info[4] = desc->gmem_pitch << A840_2D_SRC_PITCH_SHIFT;

        dst_rect_words[0] = pack_xy_u16(desc->resolve_rect.x, desc->resolve_rect.y);
        dst_rect_words[1] = pack_xy_u16(desc->resolve_rect.x + desc->resolve_rect.width - 1u,
                                        desc->resolve_rect.y + desc->resolve_rect.height - 1u);

        src_coords[0] = 0u;
        src_coords[1] = (desc->resolve_rect.width - 1u) << A840_2D_SRC_COORD_SHIFT;
        src_coords[2] = 0u;
        src_coords[3] = (desc->resolve_rect.height - 1u) << A840_2D_SRC_COORD_SHIFT;

        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_A2D_BLT_CNTL, blt_cntl));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_A2D_BLT_CNTL, blt_cntl));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_TPL1_A2D_BLT_CNTL, blt_cntl));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_A2D_OUTPUT_INFO, sp_info));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_A2D_DEST_BUFFER_INFO, dst_info));
        A840_OPS_TRY(a840_cs_emit_reg64_va(&batch, A840_REG_RB_A2D_DEST_BUFFER_BASE,
                                           desc->dst.bo_handle, desc->dst.gpu_va,
                                           A840_RELOC_WRITE));
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_A2D_DEST_BUFFER_PITCH,
                                   desc->dst.pitch >> A840_2D_DST_PITCH_SHR));
        if (desc->dst.tile_mode == A840_TILE_UBWC) {
            uint32_t flag_pitch =
                (desc->dst.ubwc_meta_pitch >> A840_2D_FLAG_PITCH_SHR) |
                ((uint32_t)(desc->dst.ubwc_meta_slice_stride >> A840_2D_FLAG_ARRAY_SHR)
                 << A840_2D_FLAG_ARRAY_SHIFT);
            A840_OPS_TRY(a840_cs_emit_reg64_va(&batch,
                                               A840_REG_RB_A2D_DEST_FLAG_BUFFER_BASE,
                                               desc->dst.bo_handle, desc->dst.ubwc_meta_va,
                                               A840_RELOC_WRITE));
            A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_A2D_DEST_FLAG_BUFFER_PITCH,
                                       flag_pitch));
        }
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_TPL1_A2D_SRC_TEXTURE_INFO, src_info, 5u));
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_GRAS_A2D_SCISSOR_TL, dst_rect_words, 2u));
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_GRAS_A2D_DEST_TL, dst_rect_words, 2u));
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_GRAS_A2D_SRC_XMIN, src_coords, 4u));
        A840_OPS_TRY(a840_emit_cache_flush_inv(&batch, A840_CACHE_UCHE_INV));
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, &ev_resolve_clean, 1u));
        A840_OPS_TRY(a840_emit_wfi(&batch));
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_BLIT, &blit_op, 1u));
    }

    if (desc->emit_ccu_uche_barriers) {
        A840_OPS_TRY(a840_emit_cache_flush_inv(&batch,
                                               A840_CACHE_CCU_FLUSH_COLOR |
                                                   A840_CACHE_CCU_INV_COLOR |
                                                   A840_CACHE_UCHE_INV));
        A840_OPS_TRY(a840_emit_wait_mem_writes(&batch));
    }

    return a840_emit_commit(cs, &batch);
}

/* ============================================================================
 * 2. Depth/Stencil Variants & Visibility / Occlusion Query
 * ============================================================================ */

static bool stencil_face_valid(const A840StencilFace *face)
{
    return face->compare <= A840_STENCIL_FUNC_MAX &&
           face->fail <= A840_STENCIL_FAIL_MAX &&
           face->pass <= A840_STENCIL_PASS_MAX &&
           face->depth_fail <= A840_STENCIL_ZFAIL_MAX;
}

A840EmitStatus a840_emit_depth_stencil_variant(A840CmdStream *cs,
                                               const A840DepthStencilVariantDesc *desc)
{
    uint32_t words[A840_EMIT_DEPTH_STENCIL_VARIANT_MAX_DWORDS];
    A840CmdStream batch;
    uint32_t depth_cntl = 0u;
    uint32_t stencil_cntl = 0u;
    A840StencilFace back;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc),
                                        _Alignof(A840DepthStencilVariantDesc)));
    if (desc->depth_compare > A840_DEPTH_FUNC_MAX || (uint32_t)desc->z_mode > P4_ZMODE_MAX) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->depth_write && desc->read_only_depth) {
        return A840_EMIT_INVALID_ARG;
    }
    if (desc->depth_bounds_test) {
        if (!isfinite(desc->depth_bounds_min) || !isfinite(desc->depth_bounds_max) ||
            desc->depth_bounds_min < 0.0f || desc->depth_bounds_max > 1.0f ||
            desc->depth_bounds_min > desc->depth_bounds_max) {
            return A840_EMIT_RANGE_ERROR;
        }
    }
    back = desc->separate_stencil_faces ? desc->back : desc->front;
    if (desc->stencil_test && (!stencil_face_valid(&desc->front) || !stencil_face_valid(&back))) {
        return A840_EMIT_RANGE_ERROR;
    }

    if (desc->depth_test) {
        depth_cntl |= A840_DEPTH_TEST |
                      ((uint32_t)desc->depth_compare << A840_DEPTH_FUNC_SHIFT);
        if (desc->depth_write) {
            depth_cntl |= A840_DEPTH_WRITE;
        }
        /* Read enable for compare funcs other than NEVER (0) and ALWAYS (7). */
        if (desc->depth_compare != 0u && desc->depth_compare != A840_DEPTH_FUNC_MAX) {
            depth_cntl |= A840_DEPTH_READ;
        }
    }
    if (desc->depth_clamp) {
        depth_cntl |= A840_DEPTH_CLAMP | A840_DEPTH_OUTPUT_CLAMP;
    }
    if (desc->depth_bounds_test) {
        depth_cntl |= P4_DEPTH_BOUNDS_ENABLE | A840_DEPTH_READ;
    }

    if (desc->stencil_test) {
        stencil_cntl = A840_STENCIL_ENABLE | A840_STENCIL_READ |
                       (desc->separate_stencil_faces ? A840_STENCIL_ENABLE_BACK : 0u) |
                       ((uint32_t)desc->front.compare << A840_STENCIL_FUNC_SHIFT) |
                       ((uint32_t)desc->front.fail << A840_STENCIL_FAIL_SHIFT) |
                       ((uint32_t)desc->front.pass << A840_STENCIL_PASS_SHIFT) |
                       ((uint32_t)desc->front.depth_fail << A840_STENCIL_ZFAIL_SHIFT) |
                       ((uint32_t)back.compare << A840_STENCIL_FUNC_BACK_SHIFT) |
                       ((uint32_t)back.fail << A840_STENCIL_FAIL_BACK_SHIFT) |
                       ((uint32_t)back.pass << A840_STENCIL_PASS_BACK_SHIFT) |
                       ((uint32_t)back.depth_fail << A840_STENCIL_ZFAIL_BACK_SHIFT);
    }

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_DEPTH_STENCIL_VARIANT_MAX_DWORDS, NULL, 0u));

    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_SU_DEPTH_PLANE_CNTL, (uint32_t)desc->z_mode));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_DEPTH_PLANE_CNTL, (uint32_t)desc->z_mode));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_SU_DEPTH_CNTL,
                               desc->depth_test ? A840_GRAS_DEPTH_TEST : 0u));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_SU_STENCIL_CNTL,
                               desc->stencil_test ? A840_GRAS_STENCIL_TEST : 0u));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_DEPTH_CNTL, depth_cntl));
    {
        uint32_t bounds[2] = {
            desc->depth_bounds_test ? float_bits(desc->depth_bounds_min) : float_bits(0.0f),
            desc->depth_bounds_test ? float_bits(desc->depth_bounds_max) : float_bits(1.0f)
        };
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_RB_DEPTH_BOUND_MIN, bounds, 2u));
    }
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_STENCIL_CNTL, stencil_cntl));
    {
        uint32_t ref_mask_wr[3] = {
            (uint32_t)desc->front.reference |
                ((uint32_t)back.reference << A840_STENCIL_BACK_BYTE_SHIFT),
            (uint32_t)desc->front.read_mask |
                ((uint32_t)back.read_mask << A840_STENCIL_BACK_BYTE_SHIFT),
            (uint32_t)desc->front.write_mask |
                ((uint32_t)back.write_mask << A840_STENCIL_BACK_BYTE_SHIFT)
        };
        A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_RB_STENCIL_REF_CNTL, ref_mask_wr, 3u));
    }

    return a840_emit_commit(cs, &batch);
}

static A840EmitStatus validate_vis_query(const A840VisQueryDesc *desc, uint64_t *out_slot_va)
{
    uint64_t slot_offset;
    if (desc->mode != A840_VIS_QUERY_COUNTING && desc->mode != A840_VIS_QUERY_BOOLEAN) {
        return A840_EMIT_INVALID_ARG;
    }
    if (desc->buffer_bytes < A840_VIS_QUERY_SLOT_STRIDE_BYTES ||
        desc->buffer_bytes % A840_VIS_QUERY_SLOT_ALIGN_BYTES != 0u) {
        return A840_EMIT_ALIGNMENT;
    }
    A840_OPS_TRY(a840_ops_va(desc->query_buffer.gpu_va, desc->buffer_bytes,
                             A840_VIS_QUERY_SLOT_ALIGN_BYTES));
    if ((uint64_t)desc->slot_index >= desc->buffer_bytes / A840_VIS_QUERY_SLOT_STRIDE_BYTES) {
        return A840_EMIT_RANGE_ERROR;
    }
    slot_offset = (uint64_t)desc->slot_index * A840_VIS_QUERY_SLOT_STRIDE_BYTES;
    if (desc->query_buffer.gpu_va > UINT64_MAX - slot_offset) {
        return A840_EMIT_RANGE_ERROR;
    }
    *out_slot_va = desc->query_buffer.gpu_va + slot_offset;
    return A840_EMIT_OK;
}

A840EmitStatus a840_emit_visibility_query_begin(A840CmdStream *cs,
                                                const A840VisQueryDesc *desc)
{
    uint32_t words[A840_EMIT_VIS_QUERY_BEGIN_MAX_DWORDS];
    A840Reloc relocs[3];
    A840CmdStream batch;
    uint64_t slot_va = 0u;
    A840GpuAddress slot_addr;
    uint32_t ev_payload[3];
    size_t va_idx;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840VisQueryDesc)));
    A840_OPS_TRY(validate_vis_query(desc, &slot_va));
    slot_addr.bo_handle = desc->query_buffer.bo_handle;
    slot_addr.gpu_va = slot_va;

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_VIS_QUERY_BEGIN_MAX_DWORDS, relocs, 3u));

    /* Enable RB sample counter copy (a6xx.xml:2540). */
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_SAMPLE_COUNTER_CNTL, P4_SAMPLE_COUNTER_COPY));
    if (desc->emit_rb_sample_counter_base) {
        A840_OPS_TRY(a840_cs_emit_reg64_va(&batch, A840_REG_RB_SAMPLE_COUNTER_BASE,
                                           slot_addr.bo_handle, slot_addr.gpu_va,
                                           A840_RELOC_WRITE));
    }

    /* A8XX CP_EVENT_WRITE7 with ZPASS_DONE | WRITE_SAMPLE_COUNT (tu_query_pool.cc:1095). */
    ev_payload[0] = P4_EVENT_ZPASS_DONE | P4_EV7_WRITE_SAMPLE_COUNT;
    ev_payload[1] = (uint32_t)slot_va;
    ev_payload[2] = (uint32_t)(slot_va >> 32);
    va_idx = batch.used_dwords + 2u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, ev_payload, 3u));
    A840_OPS_TRY(a840_emit_record_address(&batch, va_idx, slot_addr, A840_RELOC_WRITE));

    /* When outside a render pass, pair ZPASS_DONE with a closing event so the
     * hardware begin/end ZPASS_DONE state machine stays balanced. */
    if (!desc->inside_render_pass) {
        ev_payload[0] = P4_EVENT_ZPASS_DONE | P4_EV7_WRITE_SAMPLE_COUNT |
                        P4_EV7_SAMPLE_COUNT_END_OFFSET | P4_EV7_WRITE_ACCUM_SAMPLE_COUNT_DIFF;
        va_idx = batch.used_dwords + 2u;
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, ev_payload, 3u));
        A840_OPS_TRY(a840_emit_record_address(&batch, va_idx, slot_addr, A840_RELOC_WRITE));
    }

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_visibility_query_end(A840CmdStream *cs,
                                              const A840VisQueryDesc *desc)
{
    uint32_t words[A840_EMIT_VIS_QUERY_END_MAX_DWORDS];
    A840Reloc relocs[3];
    A840CmdStream batch;
    uint64_t slot_va = 0u;
    uint64_t end_va;
    A840GpuAddress slot_addr;
    A840GpuAddress end_addr;
    uint32_t ev_payload[3];
    size_t va_idx;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840VisQueryDesc)));
    A840_OPS_TRY(validate_vis_query(desc, &slot_va));
    end_va = slot_va + P4_VIS_SLOT_END_OFFSET_BYTES;
    slot_addr.bo_handle = desc->query_buffer.bo_handle;
    slot_addr.gpu_va = slot_va;
    end_addr.bo_handle = desc->query_buffer.bo_handle;
    end_addr.gpu_va = end_va;

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_VIS_QUERY_END_MAX_DWORDS, relocs, 3u));

    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_RB_SAMPLE_COUNTER_CNTL, P4_SAMPLE_COUNTER_COPY));
    if (desc->emit_rb_sample_counter_base) {
        A840_OPS_TRY(a840_cs_emit_reg64_va(&batch, A840_REG_RB_SAMPLE_COUNTER_BASE,
                                           end_addr.bo_handle, end_addr.gpu_va,
                                           A840_RELOC_WRITE));
    }

    if (!desc->inside_render_pass) {
        ev_payload[0] = P4_EVENT_ZPASS_DONE | P4_EV7_WRITE_SAMPLE_COUNT;
        ev_payload[1] = (uint32_t)end_va;
        ev_payload[2] = (uint32_t)(end_va >> 32);
        va_idx = batch.used_dwords + 2u;
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, ev_payload, 3u));
        A840_OPS_TRY(a840_emit_record_address(&batch, va_idx, end_addr, A840_RELOC_WRITE));
    }

    /* Hardware writes end count at (slot_va + 16) and accumulates
     * *(slot_va + 8) += *(slot_va + 16) - *(slot_va) atomically across tiles. */
    ev_payload[0] = P4_EVENT_ZPASS_DONE | P4_EV7_WRITE_SAMPLE_COUNT |
                    P4_EV7_SAMPLE_COUNT_END_OFFSET | P4_EV7_WRITE_ACCUM_SAMPLE_COUNT_DIFF;
    ev_payload[1] = (uint32_t)slot_va;
    ev_payload[2] = (uint32_t)(slot_va >> 32);
    va_idx = batch.used_dwords + 2u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, ev_payload, 3u));
    A840_OPS_TRY(a840_emit_record_address(&batch, va_idx, slot_addr, A840_RELOC_WRITE));

    /* Make visibility query writes visible before subsequent reads / host fences. */
    A840_OPS_TRY(a840_emit_wait_mem_writes(&batch));

    return a840_emit_commit(cs, &batch);
}

/* ============================================================================
 * 3. Tessellation & Vertex Amplification (Multiview)
 * ============================================================================ */

static bool tess_domain_valid(A840TessDomain d)
{
    return d == A840_TESS_DOMAIN_QUAD || d == A840_TESS_DOMAIN_TRIANGLE;
}

static bool tess_spacing_valid(A840TessSpacing s)
{
    return s == A840_TESS_SPACING_EQUAL ||
           s == A840_TESS_SPACING_FRACTIONAL_ODD ||
           s == A840_TESS_SPACING_FRACTIONAL_EVEN;
}

static bool tess_winding_valid(A840TessWinding w)
{
    return w == A840_TESS_WINDING_POINTS ||
           w == A840_TESS_WINDING_LINES ||
           w == A840_TESS_WINDING_CW ||
           w == A840_TESS_WINDING_CCW;
}

A840EmitStatus a840_emit_tess_state(A840CmdStream *cs, const A840TessStateDesc *desc)
{
    uint32_t words[A840_EMIT_TESS_STATE_MAX_DWORDS];
    A840Reloc reloc;
    A840CmdStream batch;
    uint32_t hs_out_cp;
    uint32_t hs_ds_regs[3];
    uint32_t pc_cntl;
    uint32_t subdraw_vertices;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840TessStateDesc)));
    if (!tess_domain_valid(desc->domain) ||
        !tess_spacing_valid(desc->spacing) ||
        !tess_winding_valid(desc->winding)) {
        return A840_EMIT_UNSUPPORTED;
    }
    if (desc->control_points == 0u || desc->control_points > A840_TESS_MAX_CONTROL_POINTS) {
        return A840_EMIT_RANGE_ERROR;
    }
    hs_out_cp = desc->hs_output_control_points ? desc->hs_output_control_points
                                               : desc->control_points;
    if (hs_out_cp > A840_TESS_MAX_CONTROL_POINTS) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->patch_local_mem_16b == 0u || desc->patch_local_mem_16b > P4_PC_HS_PARAM_1_MAX_SIZE) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->subdraw_patch_limit == 0u ||
        desc->subdraw_patch_limit > (UINT32_MAX / desc->control_points)) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->tess_buffer_bytes < P4_TESS_MIN_BUFFER_BYTES) {
        return A840_EMIT_RANGE_ERROR;
    }
    A840_OPS_TRY(a840_ops_va(desc->tess_factor_buffer.gpu_va, desc->tess_buffer_bytes,
                             P4_TESS_ALIGN_BYTES));

    /* A8XX contiguous PC_HS_PARAM_0 (0x9b10), PC_HS_PARAM_1 (0x9b11), PC_DS_PARAM (0x9b12). */
    hs_ds_regs[0] = hs_out_cp;
    hs_ds_regs[1] = desc->patch_local_mem_16b;
    hs_ds_regs[2] = ((uint32_t)desc->spacing) |
                    (((uint32_t)desc->winding) << P4_PC_DS_OUTPUT_SHIFT);

    pc_cntl = desc->d3d_vertex_ordering ? P4_PC_CNTL_D3D_VERTEX_ORDERING : 0u;
    /* A8XX tess BO is double-buffered across two draws; subdraw size in vertices. */
    subdraw_vertices = desc->subdraw_patch_limit * desc->control_points;

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_TESS_STATE_MAX_DWORDS, &reloc, 1u));
    A840_OPS_TRY(a840_cs_emit_reg64_va(&batch, A840_REG_PC_TESS_BASE,
                                       desc->tess_factor_buffer.bo_handle,
                                       desc->tess_factor_buffer.gpu_va,
                                       A840_RELOC_READ | A840_RELOC_WRITE));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_HS_PATCH_SIZE, desc->control_points));
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_PC_HS_PARAM_0, hs_ds_regs, 3u));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_CNTL, pc_cntl));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_VPC_PC_CNTL, pc_cntl));
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_SET_SUBDRAW_SIZE, &subdraw_vertices, 1u));

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_tess_draw(A840CmdStream *cs, const A840TessDrawDesc *desc)
{
    uint32_t words[A840_EMIT_TESS_DRAW_MAX_DWORDS];
    A840Reloc reloc;
    A840CmdStream batch;
    uint32_t prim_type;
    uint32_t vertex_count;
    uint32_t first_vertex;
    uint32_t initiator;
    uint32_t offsets[2];
    uint32_t payload[7];
    bool indexed;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840TessDrawDesc)));
    if (!tess_domain_valid(desc->domain)) {
        return A840_EMIT_UNSUPPORTED;
    }
    if (desc->control_points == 0u || desc->control_points > A840_TESS_MAX_CONTROL_POINTS) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->index_type != A840_INDEX_NONE &&
        desc->index_type != A840_INDEX_UINT16 &&
        desc->index_type != A840_INDEX_UINT32) {
        return A840_EMIT_INVALID_ARG;
    }
    if (!desc->patch_count || !desc->instance_count) {
        return A840_EMIT_OK;
    }
    if (desc->patch_count > UINT32_MAX / desc->control_points ||
        desc->first_patch > UINT32_MAX / desc->control_points ||
        desc->instance_count - 1u > UINT32_MAX - desc->first_instance) {
        return A840_EMIT_RANGE_ERROR;
    }

    vertex_count = desc->patch_count * desc->control_points;
    first_vertex = desc->first_patch * desc->control_points;
    /* DI_PT_PATCHES0 (0x1f) + (control_points - 1), valid for 1..32 control points. */
    prim_type = P4_DI_PT_PATCHES0 + (desc->control_points - 1u);
    indexed = (desc->index_type != A840_INDEX_NONE);

    initiator = (prim_type << A840_DRAW_PRIMITIVE_SHIFT) |
                (((desc->use_visibility ? A840_DRAW_USE_VIS : A840_DRAW_IGNORE_VIS))
                 << A840_DRAW_VISIBILITY_SHIFT) |
                (((uint32_t)desc->domain) << P4_DRAW_PATCH_TYPE_SHIFT) |
                P4_DRAW_TESS_ENABLE;

    if (indexed) {
        if (first_vertex >= desc->index_capacity ||
            vertex_count > desc->index_capacity - first_vertex) {
            return A840_EMIT_RANGE_ERROR;
        }
        A840_OPS_TRY(a840_ops_va(desc->index_buffer.gpu_va,
                                 (uint64_t)desc->index_capacity * (uint64_t)desc->index_type,
                                 (uint32_t)desc->index_type));
        initiator |= (A840_DRAW_DMA << A840_DRAW_SOURCE_SHIFT) |
                     ((desc->index_type == A840_INDEX_UINT16 ? A840_DRAW_INDEX_16
                                                             : A840_DRAW_INDEX_32)
                      << A840_DRAW_INDEX_SHIFT);
    } else {
        if (vertex_count - 1u > UINT32_MAX - first_vertex) {
            return A840_EMIT_RANGE_ERROR;
        }
        initiator |= (A840_DRAW_AUTO << A840_DRAW_SOURCE_SHIFT);
    }

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_TESS_DRAW_MAX_DWORDS, &reloc, 1u));

    offsets[0] = indexed ? 0u : first_vertex;
    offsets[1] = desc->first_instance;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_VFD_INDEX_OFFSET, offsets, 2u));
    /* A840 CP_PROTECT PROT_NA(0x09e00, 0x01ff) blocks direct Type-4 writes to PC_DRAWCALL_CNTL;
     * Mesa Turnip passes initiator solely in CP_DRAW_INDX_OFFSET word 0 (tu_cmd_buffer.cc:9056,9130). */

    payload[0] = initiator;
    payload[1] = desc->instance_count;
    payload[2] = vertex_count;
    payload[3] = first_vertex;
    payload[4] = (uint32_t)desc->index_buffer.gpu_va;
    payload[5] = (uint32_t)(desc->index_buffer.gpu_va >> 32);
    payload[6] = desc->index_capacity;
    {
        size_t idx_va = batch.used_dwords + 5u;
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_DRAW_INDX_OFFSET, payload,
                                       indexed ? 7u : 3u));
        if (indexed) {
            A840_OPS_TRY(a840_emit_record_address(&batch, idx_va, desc->index_buffer,
                                                  A840_RELOC_READ));
        }
    }

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_vertex_amplification_state(A840CmdStream *cs,
                                                    const A840VertexAmpStateDesc *desc)
{
    uint32_t words[A840_EMIT_VERTEX_AMP_STATE_MAX_DWORDS];
    A840CmdStream batch;
    uint32_t view_mask;
    uint32_t stereo_cntl;
    uint32_t gras_stereo;
    uint32_t i;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc),
                                        _Alignof(A840VertexAmpStateDesc)));
    if (desc->amp_count < 1u || desc->amp_count > A840_VERTEX_AMP_MAX_VIEWS) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (desc->viewport_count != 0u &&
        (desc->viewport_count < desc->amp_count ||
         desc->viewport_count > A840_VERTEX_AMP_MAX_VIEWS)) {
        return A840_EMIT_RANGE_ERROR;
    }
    for (i = 0u; i < desc->amp_count; ++i) {
        if (desc->mappings[i].viewport_array_index > P4_MAX_VIEWPORT_INDEX ||
            desc->mappings[i].render_target_array_index > P4_MAX_RT_INDEX) {
            return A840_EMIT_RANGE_ERROR;
        }
    }

    if (desc->amp_count == 1u) {
        view_mask = 0u;
        stereo_cntl = 0u;
        gras_stereo = 0u;
    } else {
        view_mask = (1u << desc->amp_count) - 1u;
        stereo_cntl = P4_STEREO_CNTL_ENABLE |
                      (desc->multi_pos_output ? 0u : P4_STEREO_CNTL_DISABLE_MULTIPOS) |
                      (desc->amp_count << P4_STEREO_CNTL_VIEWS_SHIFT);
        gras_stereo = (desc->increment_rt_index ? P4_STEREO_RT_INDEX_INCR : 0u) |
                      (desc->increment_viewport_index ? P4_STEREO_VP_INDEX_INCR : 0u);
        if (gras_stereo == 0u) {
            /* Gen8 requires at least one of RT or Viewport increment for stereo rendering. */
            gras_stereo = P4_STEREO_VP_INDEX_INCR;
        }
    }

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_VERTEX_AMP_STATE_MAX_DWORDS, NULL, 0u));

    /* A8XX Multiview / Stereo Rendering register triad: VFD + VPC + PC + GRAS_SU_STEREO_CNTL */
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_VFD_STEREO_RENDERING_CNTL, stereo_cntl));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_VPC_STEREO_RENDERING_CNTL, stereo_cntl));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_VPC_STEREO_RENDERING_VIEWMASK, view_mask));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_STEREO_RENDERING_CNTL, stereo_cntl));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_STEREO_RENDERING_VIEWMASK, view_mask));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_GRAS_SU_STEREO_CNTL, gras_stereo));

    /* Optional per-view viewport + scissor array mapping (for amp_count 1..2 views). */
    if (desc->viewport_count > 0u) {
        const uint32_t vp_stride =
            A840_REG_GRAS_CL_VIEWPORT_1_XOFFSET - A840_REG_GRAS_CL_VIEWPORT_0_XOFFSET;
        const uint32_t sc_stride =
            A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_BR - A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_TL + 1u;
        const uint32_t z_stride =
            A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MAX - A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MIN + 1u;

        for (i = 0u; i < desc->viewport_count; ++i) {
            const A840Viewport *vp = &desc->viewports[i];
            const A840Rect2D *sc = &desc->scissors[i];
            uint32_t slot = desc->mappings[i].viewport_array_index;
            float sx;
            float sy;
            float ox;
            float oy;
            uint32_t vp_words[6];
            uint32_t sc_words[2];
            uint32_t z_words[2];

            if (!isfinite(vp->x) || !isfinite(vp->y) || !isfinite(vp->width) ||
                !isfinite(vp->height) || !isfinite(vp->min_depth) || !isfinite(vp->max_depth) ||
                vp->width <= 0.0f || vp->height == 0.0f ||
                vp->min_depth < 0.0f || vp->min_depth > 1.0f ||
                vp->max_depth < 0.0f || vp->max_depth > 1.0f) {
                return A840_EMIT_RANGE_ERROR;
            }
            if (!sc->width || !sc->height ||
                sc->x + sc->width - 1u > A840_2D_COORD_MAX ||
                sc->y + sc->height - 1u > A840_2D_COORD_MAX) {
                return A840_EMIT_RANGE_ERROR;
            }

            sx = vp->width * 0.5f;
            sy = vp->height * 0.5f;
            ox = vp->x + sx;
            oy = vp->y + sy;
            vp_words[0] = float_bits(ox);
            vp_words[1] = float_bits(sx);
            vp_words[2] = float_bits(oy);
            vp_words[3] = float_bits(sy);
            vp_words[4] = float_bits(vp->min_depth);
            vp_words[5] = float_bits(vp->max_depth - vp->min_depth);

            sc_words[0] = pack_xy_u16(sc->x, sc->y);
            sc_words[1] = pack_xy_u16(sc->x + sc->width - 1u, sc->y + sc->height - 1u);

            z_words[0] = float_bits(vp->min_depth <= vp->max_depth ? vp->min_depth : vp->max_depth);
            z_words[1] = float_bits(vp->min_depth <= vp->max_depth ? vp->max_depth : vp->min_depth);

            A840_OPS_TRY(a840_cs_emit_pkt4(&batch,
                                           A840_REG_GRAS_CL_VIEWPORT_0_XOFFSET + slot * vp_stride,
                                           vp_words, 6u));
            A840_OPS_TRY(a840_cs_emit_pkt4(&batch,
                                           A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_TL + slot * sc_stride,
                                           sc_words, 2u));
            A840_OPS_TRY(a840_cs_emit_pkt4(&batch,
                                           A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MIN + slot * z_stride,
                                           z_words, 2u));
        }
    }

    return a840_emit_commit(cs, &batch);
}
