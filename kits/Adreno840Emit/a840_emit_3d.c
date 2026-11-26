/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#define A840_EMIT_PIPELINE_INTERNAL
#include "a840_emit_3d.h"
#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <math.h>

enum {
    VPC_LOCATION_UNUSED = UINT8_MAX,
    VPC_LINK_LIMIT = 32,
    VPC_COMPONENTS = 128,
    /* A840 inherits a7xx_base. See freedreno_devices.py:760,1396-1413. */
    A840_PRIM_ALLOC_THRESHOLD = 7,
};

static bool assigned(uint8_t regid)
{
    return regid < A840_REGID_UNUSED;
}

static A840EmitStatus emit_vertex_inputs(A840CmdStream *batch, const A840GraphicsProgramDesc *desc)
{
    const uint32_t count = desc->vertex_input_count;
    const uint8_t sysvals[] = {desc->vertex_id, desc->instance_id, desc->primitive_id, desc->view_id};
    if (count > A840_MAX_VERTEX_INPUTS) return A840_EMIT_RANGE_ERROR;
    for (size_t i = 0; i < sizeof(sysvals); ++i)
        if (sysvals[i] > A840_REGID_UNUSED) return A840_EMIT_RANGE_ERROR;

    const uint32_t vfd[] = {
        count << A840_VFD_FETCH_SHIFT | count << A840_VFD_DECODE_SHIFT,
        desc->vertex_id | (uint32_t)desc->instance_id << A840_VFD_INST_SHIFT |
        (uint32_t)desc->primitive_id << A840_VFD_PRIM_SHIFT | (uint32_t)desc->view_id << A840_VFD_VIEW_SHIFT,
    };
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_VFD_CNTL_0, vfd, 2));
    /* Gen8 sideband count matches tu_shader.cc:2268-2284. */
    const uint32_t sideband = (uint32_t)assigned(desc->vertex_id) +
        (uint32_t)assigned(desc->instance_id) + (uint32_t)assigned(desc->view_id);
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_PC_VS_INPUT_CNTL,
        count << A840_PC_VS_INSTR_SHIFT | sideband << A840_PC_VS_SIDEBAND_SHIFT));

    uint32_t destinations[A840_MAX_VERTEX_INPUTS];
    for (uint32_t i = 0; i < count; ++i) {
        const A840VertexInputDesc *input = &desc->vertex_inputs[i];
        if (input->component_mask > A840_VS_OUT_MASK_MAX || input->regid > A840_REGID_UNUSED ||
            (!assigned(input->regid) && input->component_mask))
            return A840_EMIT_RANGE_ERROR;
        destinations[i] = (uint32_t)input->regid << A840_VFD_DEST_REG_SHIFT | input->component_mask;
    }
    if (count)
        return a840_cs_emit_pkt4(batch, A840_REG_VFD_DEST_CNTL_0_INSTR, destinations, count);
    return A840_EMIT_OK;
}

static void link_output(uint32_t regs[16], uint32_t locations[8], uint32_t index,
    uint8_t regid, uint32_t mask, uint32_t location)
{
    const uint32_t output = regid | mask << A840_VS_OUT_MASK_SHIFT;
    regs[index / 2] |= output << ((index % 2) * A840_VS_OUT_NEXT_SHIFT);
    locations[index / 4] |= location << ((index % 4) * A840_VS_DEST_NEXT_SHIFT);
}

static A840EmitStatus emit_vpc(A840CmdStream *batch, const A840GraphicsProgramDesc *desc)
{
    uint32_t outputs[16] = {0}, locations[8] = {0};
    uint32_t interpolation[8] = {0}, replacement[8] = {0};
    uint32_t disabled[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const bool point_size = assigned(desc->point_size);
    const uint32_t count = desc->varying_count;
    if (count > VPC_LINK_LIMIT - 1u - (uint32_t)point_size || !assigned(desc->position) ||
        desc->point_size > A840_REGID_UNUSED)
        return A840_EMIT_RANGE_ERROR;

    uint32_t components = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const A840VaryingDesc *varying = &desc->varyings[i];
        const uint32_t mask = varying->component_mask;
        if (!assigned(varying->vs_regid) || !mask || mask > A840_VS_OUT_MASK_MAX)
            return A840_EMIT_RANGE_ERROR;
        link_output(outputs, locations, i, varying->vs_regid, mask, components);
        for (uint32_t c = 0; c < 4; ++c) {
            if (!(mask & (1u << c))) continue;
            if (varying->flat)
                interpolation[components / 16] |= A840_INTERP_FLAT << (2u * (components % 16));
            disabled[components / 32] &= ~(UINT32_C(1) << (components % 32));
            ++components;
        }
    }

    /* Position and optional point size are the last outputs consumed by VPC. */
    const uint32_t position = components;
    const uint32_t psize = point_size ? position + 4u : VPC_LOCATION_UNUSED;
    const uint32_t stride = position + 4u + (uint32_t)point_size;
    const uint32_t linked = count + 1u + (uint32_t)point_size;
    if (stride > VPC_COMPONENTS) return A840_EMIT_RANGE_ERROR;
    link_output(outputs, locations, count, desc->position, A840_VS_OUT_MASK_MAX, position);
    if (point_size) link_output(outputs, locations, count + 1u, desc->point_size, 1u, psize);

    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_VS_OUTPUT_0_REG, outputs, (linked + 1u) / 2u));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_VS_VPC_DEST_0_REG, locations, (linked + 3u) / 4u));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_SP_VS_OUTPUT_CNTL, linked));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_VPC_VS_CNTL,
        stride | position << A840_VPC_POSITION_SHIFT | psize << A840_VPC_POINT_SIZE_SHIFT));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_PC_VS_CNTL, stride | (point_size ? A840_PC_POINT_SIZE : 0u)));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_VPC_VS_SIV_CNTL, (uint32_t)VPC_LOCATION_UNUSED |
        (uint32_t)VPC_LOCATION_UNUSED << A840_VPC_SIV_VIEW_SHIFT |
        (uint32_t)VPC_LOCATION_UNUSED << A840_VPC_SIV_RATE_SHIFT));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_SU_VS_SIV_CNTL, 0));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_CL_VS_CLIP_CULL_DISTANCE, 0));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_VPC_VARYING_INTERP_MODE_0_MODE, interpolation, 8));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_VPC_VARYING_REPLACE_MODE_0_MODE, replacement, 8));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_VPC_VARYING_LM_TRANSFER_CNTL_0_DISABLE, disabled, 4));
    return a840_emit_reg(batch, A840_REG_VPC_PS_CNTL,
        components | (count ? A840_VPC_VARYING : 0u) |
        (uint32_t)VPC_LOCATION_UNUSED << A840_VPC_PRIM_ID_SHIFT |
        (uint32_t)VPC_LOCATION_UNUSED << A840_VPC_VIEW_ID_SHIFT);
}

static A840EmitStatus emit_fragment_inputs(A840CmdStream *batch, const A840GraphicsProgramDesc *desc)
{
    const A840FragmentInputs *fs = &desc->fs_inputs;
    const uint8_t bary[] = {fs->persp_pixel, fs->persp_centroid, fs->persp_sample,
        fs->linear_pixel, fs->linear_centroid, fs->linear_sample};
    const uint8_t scalar[] = {fs->face, fs->sample_id, fs->sample_mask, fs->center_rhw, fs->shading_rate};
    if (fs->frag_coord_mask > A840_INTERP_COORD_MAX ||
        fs->frag_coord > A840_REGID_UNUSED ||
        (assigned(fs->frag_coord) && fs->frag_coord > A840_REGID_UNUSED - 4u) ||
        (assigned(fs->frag_coord) != (fs->frag_coord_mask != 0)))
        return A840_EMIT_RANGE_ERROR;

    uint32_t sysval_count = assigned(fs->frag_coord) ? 4u : 0u;
    for (size_t i = 0; i < sizeof(scalar); ++i) {
        if (scalar[i] > A840_REGID_UNUSED) return A840_EMIT_RANGE_ERROR;
        sysval_count += (uint32_t)assigned(scalar[i]);
    }
    const uint32_t bary_bit[] = {A840_INTERP_PERSP_PIXEL, A840_INTERP_PERSP_CENTROID,
        A840_INTERP_PERSP_SAMPLE, A840_INTERP_LINEAR_PIXEL, A840_INTERP_LINEAR_CENTROID,
        A840_INTERP_LINEAR_SAMPLE};
    uint32_t interp = (uint32_t)fs->frag_coord_mask << A840_INTERP_COORD_SHIFT;
    for (size_t i = 0; i < sizeof(bary); ++i) {
        if (bary[i] > A840_REGID_UNUSED ||
            (assigned(bary[i]) && bary[i] > A840_REGID_UNUSED - 2u))
            return A840_EMIT_RANGE_ERROR;
        if (assigned(bary[i])) {
            interp |= bary_bit[i];
            sysval_count += 2u;
        }
    }
    const uint32_t zw = assigned(fs->frag_coord) ? (uint32_t)fs->frag_coord + 2u : A840_REGID_UNUSED;
    const uint32_t prog[] = {
        (uint32_t)A840_PRIM_ALLOC_THRESHOLD << A840_SP_PRIM_THRESHOLD_SHIFT,
        fs->face | (uint32_t)fs->sample_id << A840_PROG_SAMPLE_SHIFT |
        (uint32_t)fs->sample_mask << A840_PROG_SAMPLE_MASK_SHIFT |
        (uint32_t)fs->center_rhw << A840_PROG_CENTER_SHIFT,
        fs->persp_pixel | (uint32_t)fs->linear_pixel << A840_PROG_LINEAR_PIXEL_SHIFT |
        (uint32_t)fs->persp_centroid << A840_PROG_PERSP_CENTROID_SHIFT |
        (uint32_t)fs->linear_centroid << A840_PROG_LINEAR_CENTROID_SHIFT,
        fs->persp_sample | (uint32_t)fs->linear_sample << A840_PROG_LINEAR_SAMPLE_SHIFT |
        (uint32_t)fs->frag_coord << A840_PROG_XY_SHIFT | zw << A840_PROG_ZW_SHIFT,
        A840_REGID_UNUSED | (uint32_t)fs->shading_rate << A840_PROG_SHADING_RATE_SHIFT,
    };
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_LB_PARAM_LIMIT, prog, 5));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_LB_PARAM_LIMIT,
        (uint32_t)A840_PRIM_ALLOC_THRESHOLD << A840_RB_PRIM_THRESHOLD_SHIFT));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_PS_CNTL,
        (desc->pixel_lod ? A840_RB_PS_PIXEL_LOD : 0u) | (desc->full_quad ? A840_RB_PS_LOD_MASK : 0u)));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_SP_PS_CNTL_1,
        sysval_count | A840_PS_DEFER_DISABLE | 1u << A840_PS_EVICT_MODE_SHIFT));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_SP_PS_WAVE_CNTL,
        desc->varying_count ? A840_WAVE_VARYINGS : 0u));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_SP_PS_INITIAL_TEX_LOAD_CNTL,
        A840_PS_CONST_SLOT_MAX << A840_PS_CONST_SLOT_SHIFT |
        A840_PS_COORD_SLOT_MAX << A840_PS_COORD_SLOT_SHIFT |
        (!assigned(fs->persp_pixel) ? A840_PS_IJ_DISABLE : 0u)));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_CL_INTERP_CNTL,
        interp | (assigned(fs->face) ? A840_GRAS_FACE : 0u) |
        (assigned(fs->center_rhw) ? A840_GRAS_CENTER : 0u)));
    const uint32_t rb_inputs[] = {
        interp | (desc->varying_count ? A840_RB_INTERP_ENABLE : 0u),
        (assigned(fs->sample_mask) ? A840_RB_INPUT_SAMPLE_MASK : 0u) |
        (assigned(fs->face) ? A840_RB_INPUT_FACE : 0u) |
        (assigned(fs->sample_id) ? A840_RB_INPUT_SAMPLE : 0u) |
        (assigned(fs->center_rhw) ? A840_RB_INPUT_CENTER : 0u) |
        (assigned(fs->shading_rate) ? A840_RB_INPUT_RATE : 0u),
    };
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_INTERP_CNTL, rb_inputs, 2));
    return a840_emit_reg(batch, A840_REG_RB_PS_SAMPLEFREQ_CNTL, 0);
}

static A840EmitStatus emit_fragment_outputs(A840CmdStream *batch, const A840GraphicsProgramDesc *desc)
{
    if (desc->depth_output > A840_REGID_UNUSED || desc->sample_mask_output > A840_REGID_UNUSED ||
        desc->stencil_output > A840_REGID_UNUSED) return A840_EMIT_RANGE_ERROR;

    uint32_t outputs[A840_MAX_COLOR_TARGETS];
    uint32_t mask = 0, mrt_count = 0;
    for (uint32_t i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        const A840FragmentOutput *color = &desc->colors[i];
        if (color->regid > A840_REGID_UNUSED || color->component_mask > A840_VS_OUT_MASK_MAX ||
            (!assigned(color->regid) && color->component_mask))
            return A840_EMIT_RANGE_ERROR;
        outputs[i] = A840_REGID_UNUSED;
        if (color->component_mask) {
            mask |= (uint32_t)color->component_mask << (4u * i);
            mrt_count = i + 1u;
            outputs[i] = color->regid | (color->half_precision ? A840_PS_OUTPUT_HALF : 0u);
        }
    }

    const uint32_t sp[] = {
        mask,
        (uint32_t)desc->depth_output << A840_PS_OUTPUT_Z_SHIFT |
        (uint32_t)desc->sample_mask_output << A840_PS_OUTPUT_SAMPLE_SHIFT |
        (uint32_t)desc->stencil_output << A840_PS_OUTPUT_STENCIL_SHIFT,
        mrt_count,
    };
    const uint32_t rb[] = {
        (assigned(desc->depth_output) ? A840_RB_OUTPUT_Z : 0u) |
        (assigned(desc->sample_mask_output) ? A840_RB_OUTPUT_MASK : 0u) |
        (assigned(desc->stencil_output) ? A840_RB_OUTPUT_STENCIL : 0u),
        mrt_count,
    };
    const uint32_t const_output[] = {0, 0};
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_PS_OUTPUT_MASK, sp, 3));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_PS_OUTPUT_0_REG, outputs, A840_MAX_COLOR_TARGETS));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_PS_OUTPUT_CONST_CNTL, const_output, 2));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_PS_OUTPUT_CNTL, rb, 2));
    return a840_emit_reg(batch, A840_REG_RB_PS_OUTPUT_MASK, mask);
}

A840EmitStatus a840_emit_3d_program(A840CmdStream *cs, const A840GraphicsProgramDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840GraphicsProgramDesc)));
    A840_OPS_TRY(a840_ops_input_storage(cs, desc->vs.stage, sizeof(A840CompiledStage), _Alignof(A840CompiledStage)));
    A840_OPS_TRY(a840_ops_input_storage(cs, desc->fs.stage, sizeof(A840CompiledStage), _Alignof(A840CompiledStage)));

    uint32_t words[A840_EMIT_3D_PROGRAM_MAX_DWORDS];
    A840Reloc relocs[4];
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_3D_PROGRAM_MAX_DWORDS, relocs, 4));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_MODE_CNTL,
        A840_SP_CONST_DEMOTION | A840_MODE_ISAM_GL << A840_SP_ISAM_SHIFT));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_UPDATE_CNTL,
        A840_UPDATE_VS | A840_UPDATE_HS | A840_UPDATE_DS | A840_UPDATE_GS | A840_UPDATE_FS));
    const uint32_t disabled[] = {
        A840_REG_SP_HS_CONFIG, A840_REG_SP_DS_CONFIG, A840_REG_SP_GS_CONFIG,
        A840_REG_SP_HS_CONST_CONFIG, A840_REG_SP_DS_CONST_CONFIG, A840_REG_SP_GS_CONST_CONFIG,
    };
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i)
        A840_OPS_TRY(a840_emit_reg(&batch, disabled[i], 0));

    const uint32_t vs_flags = desc->vs.stage->flags_out, fs_flags = desc->fs.stage->flags_out;
    const uint32_t vs = ((vs_flags & (1u << 3)) ? A840_VS_MERGED : 0u) |
        ((vs_flags & (1u << 4)) ? A840_VS_PREAMBLE : 0u);
    const uint32_t fs = A840_PS_INOUT_OVERLAP |
        ((fs_flags & (1u << 3)) ? A840_PS_MERGED : 0u) |
        ((fs_flags & (1u << 4)) ? A840_PS_PREAMBLE : 0u) |
        ((desc->fs_thread128 ? A840_MODE_THREAD128 : A840_MODE_THREAD64) << A840_PS_THREADSIZE_SHIFT) |
        (desc->varying_count ? A840_PS_VARYING : 0u) |
        (desc->pixel_lod ? A840_PS_PIXEL_LOD : 0u) | (desc->full_quad ? A840_PS_LOD_MASK : 0u);
    A840_OPS_TRY(a840_emit_shader_bind(&batch, &desc->vs, A840_STAGE_VERTEX, vs));
    A840_OPS_TRY(a840_emit_shader_bind(&batch, &desc->fs, A840_STAGE_FRAGMENT, fs));
    A840_OPS_TRY(emit_vertex_inputs(&batch, desc));
    A840_OPS_TRY(emit_vpc(&batch, desc));
    A840_OPS_TRY(emit_fragment_inputs(&batch, desc));
    A840_OPS_TRY(emit_fragment_outputs(&batch, desc));
    return a840_emit_commit(cs, &batch);
}

static uint32_t float_word(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}

static uint32_t scissor_xy(uint32_t x, uint32_t y)
{
    return x | y << A840_SCISSOR_Y_SHIFT;
}

static void empty_scissor(uint32_t words[2])
{
    words[0] = scissor_xy(1, 1);
    words[1] = 0;
}

static uint32_t guardband(float offset, float scale)
{
    /* Mesa freedreno_guardband.h:17-80, A4XX+ signed viewport bounds. */
    const float extent = fabsf(scale);
    const float adj = fminf((32768.0f + offset) / extent, (32767.0f - offset) / extent);
    if (adj < 1.0f || !isfinite(adj)) return A840_GUARDBAND_H_MAX;
    int exponent;
    const float mantissa = frexpf(adj, &exponent);
    if (exponent > 8) return A840_GUARDBAND_H_MAX;
    return (uint32_t)(exponent - 1) << 6 | ((uint32_t)(mantissa * 128.0f) - 64u);
}

static A840EmitStatus emit_viewports(A840CmdStream *batch, const A840RenderStateDesc *desc)
{
    const uint32_t count = desc->viewport_count;
    if (!count || count > A840_MAX_VIEWPORTS || !desc->scissor_count ||
        desc->scissor_count > A840_MAX_VIEWPORTS) return A840_EMIT_RANGE_ERROR;
    const uint32_t vp_stride = A840_REG_GRAS_CL_VIEWPORT_1_XOFFSET - A840_REG_GRAS_CL_VIEWPORT_0_XOFFSET;
    const uint32_t sc_stride = A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_BR - A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_TL + 1u;
    const uint32_t z_stride = A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MAX - A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MIN + 1u;
    uint32_t z_min[A840_MAX_VIEWPORTS], z_max[A840_MAX_VIEWPORTS];
    uint32_t guard_x = A840_GUARDBAND_H_MAX, guard_y = A840_GUARDBAND_V_MAX;

    for (uint32_t i = 0; i < count; ++i) {
        const A840Viewport *vp = &desc->viewports[i];
        float x_end = vp->x + vp->width, y_end = vp->y + vp->height;
        if (!isfinite(vp->x) || !isfinite(vp->y) || !isfinite(x_end) || !isfinite(y_end) ||
            !isfinite(vp->min_depth) || !isfinite(vp->max_depth) ||
            vp->width <= 0.0f || vp->height == 0.0f ||
            vp->width > A840_LIMIT_MAX_TEXTURE_SIZE || fabsf(vp->height) > A840_LIMIT_MAX_TEXTURE_SIZE ||
            vp->x < -32768.0f || x_end > 32767.0f ||
            fminf(vp->y, y_end) < -32768.0f || fmaxf(vp->y, y_end) > 32767.0f ||
            vp->min_depth < 0.0f || vp->min_depth > 1.0f ||
            vp->max_depth < 0.0f || vp->max_depth > 1.0f)
            return A840_EMIT_RANGE_ERROR;

        float sx = vp->width * 0.5f, sy = vp->height * 0.5f;
        float ox = vp->x + sx, oy = vp->y + sy;
        const uint32_t xyzw[] = {
            float_word(ox), float_word(sx), float_word(oy), float_word(sy),
            float_word(vp->min_depth), float_word(vp->max_depth - vp->min_depth),
        };
        A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_GRAS_CL_VIEWPORT_0_XOFFSET + i * vp_stride,
            xyzw, 6));

        uint32_t clip[2];
        const float left = fmaxf(vp->x, 0.0f), right = ceilf(x_end);
        const float top = fmaxf(fminf(vp->y, y_end), 0.0f), bottom = ceilf(fmaxf(vp->y, y_end));
        if (right <= left || bottom <= top) {
            empty_scissor(clip);
        } else {
            clip[0] = scissor_xy((uint32_t)left, (uint32_t)top);
            clip[1] = scissor_xy((uint32_t)right - 1u, (uint32_t)bottom - 1u);
        }
        A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_GRAS_SC_VIEWPORT_SCISSOR_0_TL + i * sc_stride,
            clip, 2));

        const uint32_t depth[] = {
            float_word(desc->depth_clamp ? fminf(vp->min_depth, vp->max_depth) : 0.0f),
            float_word(desc->depth_clamp ? fmaxf(vp->min_depth, vp->max_depth) : 1.0f),
        };
        A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_GRAS_CL_VIEWPORT_ZCLAMP_0_MIN + i * z_stride,
            depth, 2));
        z_min[i] = depth[0];
        z_max[i] = depth[1];
        uint32_t gx = guardband(ox, sx), gy = guardband(oy, sy);
        if (gx < guard_x) guard_x = gx;
        if (gy < guard_y) guard_y = gy;
    }
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_VIEWPORT_ZCLAMP_MIN_0_REG, z_min, count));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_VIEWPORT_ZCLAMP_MAX_0_REG, z_max, count));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_CL_GUARDBAND_CLIP_ADJ,
        guard_x << A840_GUARDBAND_H_SHIFT | guard_y << A840_GUARDBAND_V_SHIFT));

    uint32_t scissors[2u * A840_MAX_VIEWPORTS];
    for (uint32_t i = 0; i < desc->scissor_count; ++i) {
        const A840Rect2D *rect = &desc->scissors[i];
        uint32_t *pair = &scissors[2u * i];
        if (!rect->width || !rect->height) {
            empty_scissor(pair);
            continue;
        }
        const uint32_t max = INT16_MAX;
        if (rect->x > max || rect->y > max ||
            rect->width - 1u > max - rect->x || rect->height - 1u > max - rect->y)
            return A840_EMIT_RANGE_ERROR;
        pair[0] = scissor_xy(rect->x, rect->y);
        pair[1] = scissor_xy(rect->x + rect->width - 1u, rect->y + rect->height - 1u);
    }
    return a840_cs_emit_pkt4(batch, A840_REG_GRAS_SC_SCREEN_SCISSOR_0_TL, scissors,
        2u * desc->scissor_count);
}

static bool stencil_face_valid(const A840StencilFace *face)
{
    return face->compare <= A840_STENCIL_FUNC_MAX && face->fail <= A840_STENCIL_FAIL_MAX &&
        face->pass <= A840_STENCIL_PASS_MAX && face->depth_fail <= A840_STENCIL_ZFAIL_MAX;
}

static A840EmitStatus emit_raster_depth_stencil(A840CmdStream *batch, const A840RenderStateDesc *desc)
{
    const float half_width = desc->line_width * 0.5f;
    const float fixed_width = half_width * (float)(1u << A840_RASTER_LINE_RADIX);
    if (!isfinite(fixed_width) || fixed_width < 1.0f || fixed_width > A840_RASTER_LINE_MAX ||
        desc->depth_compare > A840_DEPTH_FUNC_MAX || !stencil_face_valid(&desc->front) ||
        !stencil_face_valid(&desc->back))
        return A840_EMIT_RANGE_ERROR;

    const uint32_t raster = (desc->cull_front ? A840_RASTER_CULL_FRONT : 0u) |
        (desc->cull_back ? A840_RASTER_CULL_BACK : 0u) |
        (desc->front_clockwise ? A840_RASTER_CW : 0u) |
        (uint32_t)fixed_width << A840_RASTER_LINE_SHIFT;
    const bool has_depth = desc->depth_format != A840_DEPTH_NONE;
    uint32_t depth = 0;
    if (has_depth) {
        depth = A840_DEPTH_CLAMP | A840_DEPTH_OUTPUT_CLAMP |
            (uint32_t)desc->depth_compare << A840_DEPTH_FUNC_SHIFT;
        if (desc->depth_test) {
            depth |= A840_DEPTH_TEST | (desc->depth_write ? A840_DEPTH_WRITE : 0u);
            if (desc->depth_compare != 0 && desc->depth_compare != A840_DEPTH_FUNC_MAX)
                depth |= A840_DEPTH_READ;
        }
    }
    const A840StencilFace *front = &desc->front, *back = &desc->back;
    const uint32_t stencil = (desc->stencil_test ?
        A840_STENCIL_ENABLE | A840_STENCIL_ENABLE_BACK | A840_STENCIL_READ : 0u) |
        (uint32_t)front->compare << A840_STENCIL_FUNC_SHIFT |
        (uint32_t)front->fail << A840_STENCIL_FAIL_SHIFT |
        (uint32_t)front->pass << A840_STENCIL_PASS_SHIFT |
        (uint32_t)front->depth_fail << A840_STENCIL_ZFAIL_SHIFT |
        (uint32_t)back->compare << A840_STENCIL_FUNC_BACK_SHIFT |
        (uint32_t)back->fail << A840_STENCIL_FAIL_BACK_SHIFT |
        (uint32_t)back->pass << A840_STENCIL_PASS_BACK_SHIFT |
        (uint32_t)back->depth_fail << A840_STENCIL_ZFAIL_BACK_SHIFT;
    const uint32_t refs[] = {
        front->reference | (uint32_t)back->reference << A840_STENCIL_BACK_BYTE_SHIFT,
        front->read_mask | (uint32_t)back->read_mask << A840_STENCIL_BACK_BYTE_SHIFT,
        front->write_mask | (uint32_t)back->write_mask << A840_STENCIL_BACK_BYTE_SHIFT,
    };
    const uint32_t gras[] = {
        has_depth && desc->depth_test ? A840_GRAS_DEPTH_TEST : 0u,
        desc->stencil_test ? A840_GRAS_STENCIL_TEST : 0u,
    };
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_SU_CNTL, raster));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_DEPTH_CNTL, depth));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_STENCIL_CNTL, stencil));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_GRAS_SU_DEPTH_CNTL, gras, 2));
    return a840_cs_emit_pkt4(batch, A840_REG_RB_STENCIL_REF_CNTL, refs, 3);
}

static bool has_flags(const A840AttachmentStorage *storage)
{
    return storage->flag_buffer.gpu_va || storage->flag_buffer.bo_handle ||
        storage->flag_pitch_bytes || storage->flag_layer_stride_bytes;
}

static A840EmitStatus encode_pitch(uint64_t bytes, uint32_t shift, uint32_t max, uint32_t *out)
{
    if (!bytes || (bytes >> shift) > max) return A840_EMIT_RANGE_ERROR;
    if (bytes % (UINT64_C(1) << shift)) return A840_EMIT_ALIGNMENT;
    *out = (uint32_t)(bytes >> shift);
    return A840_EMIT_OK;
}

/* Part after the format/control word: pitch, array pitch, VA, and GMEM base. */
static A840EmitStatus encode_attachment(const A840AttachmentStorage *storage,
    uint32_t pitch_max, uint32_t array_max, uint32_t values[5])
{
    if (!storage->data.gpu_va)
        return storage->data.bo_handle || has_flags(storage) ? A840_EMIT_INVALID_ARG : A840_EMIT_OK;
    if (storage->tile_mode > A840_TILE_UBWC) return A840_EMIT_RANGE_ERROR;
    if (storage->layer_stride_bytes < storage->pitch_bytes) return A840_EMIT_RANGE_ERROR;
    const uint32_t align = storage->tile_mode == A840_TILE_LINEAR ? 64u : 4096u;
    A840_OPS_TRY(a840_ops_va(storage->data.gpu_va, storage->layer_stride_bytes, align));
    A840_OPS_TRY(encode_pitch(storage->pitch_bytes, A840_MRT_PITCH_SHR, pitch_max, &values[0]));
    A840_OPS_TRY(encode_pitch(storage->layer_stride_bytes, A840_MRT_ARRAY_PITCH_SHR, array_max, &values[1]));
    values[2] = (uint32_t)storage->data.gpu_va;
    values[3] = (uint32_t)(storage->data.gpu_va >> 32);
    return A840_EMIT_OK;
}

static A840EmitStatus emit_flag_buffer(A840CmdStream *batch, uint32_t reg,
    const A840AttachmentStorage *storage, bool depth)
{
    uint32_t values[3] = {0};
    const bool ubwc = storage->data.gpu_va && storage->tile_mode == A840_TILE_UBWC;
    if (ubwc) {
        if (storage->flag_layer_stride_bytes < storage->flag_pitch_bytes) return A840_EMIT_RANGE_ERROR;
        A840_OPS_TRY(a840_ops_va(storage->flag_buffer.gpu_va, storage->flag_layer_stride_bytes, 64));
        uint32_t pitch, array;
        A840_OPS_TRY(encode_pitch(storage->flag_pitch_bytes, A840_COLOR_FLAG_PITCH_SHR,
            depth ? A840_DEPTH_FLAG_PITCH_MAX : A840_COLOR_FLAG_PITCH_MAX, &pitch));
        /* fd6_view.cc:457-459 feeds XML ARRAY_PITCH a dword count. */
        A840_OPS_TRY(encode_pitch(storage->flag_layer_stride_bytes, A840_COLOR_FLAG_ARRAY_SHR + 2u,
            depth ? A840_DEPTH_FLAG_ARRAY_MAX : A840_COLOR_FLAG_ARRAY_MAX, &array));
        values[0] = (uint32_t)storage->flag_buffer.gpu_va;
        values[1] = (uint32_t)(storage->flag_buffer.gpu_va >> 32);
        values[2] = pitch | array << A840_COLOR_FLAG_ARRAY_SHIFT;
    } else if (has_flags(storage)) {
        return A840_EMIT_INVALID_ARG;
    }
    size_t index = batch->used_dwords + 1u;
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, reg, values, 3));
    if (ubwc)
        A840_OPS_TRY(a840_emit_record_address(batch, index, storage->flag_buffer,
            A840_RELOC_READ | A840_RELOC_WRITE));
    return A840_EMIT_OK;
}

static A840EmitStatus emit_depth_targets(A840CmdStream *batch, const A840RenderStateDesc *desc)
{
    const A840AttachmentStorage *depth = &desc->depth, *stencil = &desc->stencil;
    const uint32_t format = desc->depth_format;
    if (format != A840_DEPTH_NONE && format != A840_DEPTH_16 &&
        format != A840_DEPTH_24_8 && format != A840_DEPTH_32) return A840_EMIT_RANGE_ERROR;
    if ((format != A840_DEPTH_NONE) != (depth->data.gpu_va != 0) ||
        ((desc->depth_test || desc->depth_write) && !depth->data.gpu_va) ||
        (desc->stencil_test && format != A840_DEPTH_24_8 && !stencil->data.gpu_va))
        return A840_EMIT_INVALID_ARG;
    if ((depth->data.gpu_va && depth->tile_mode == A840_TILE_LINEAR) ||
        (stencil->data.gpu_va && stencil->tile_mode != A840_TILE_TILED) || has_flags(stencil))
        return A840_EMIT_UNSUPPORTED;

    uint32_t depth_regs[6] = {0}, stencil_regs[6] = {0};
    A840_OPS_TRY(encode_attachment(depth, A840_DEPTH_PITCH_MAX, A840_DEPTH_ARRAY_PITCH_MAX, depth_regs + 1));
    A840_OPS_TRY(encode_attachment(stencil, A840_STENCIL_PITCH_MAX, A840_STENCIL_ARRAY_PITCH_MAX, stencil_regs + 1));
    if (depth->data.gpu_va)
        depth_regs[0] = format | A840_TILE6_3 << A840_DEPTH_TILE_SHIFT |
            (depth->tile_mode == A840_TILE_UBWC ? A840_DEPTH_UBWC : 0u);
    if (stencil->data.gpu_va)
        stencil_regs[0] = A840_STENCIL_SEPARATE | A840_TILE6_3 << A840_STENCIL_TILE_SHIFT;

    size_t index = batch->used_dwords + 4u;
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_DEPTH_BUFFER_INFO, depth_regs, 6));
    if (depth->data.gpu_va)
        A840_OPS_TRY(a840_emit_record_address(batch, index, depth->data, A840_RELOC_READ | A840_RELOC_WRITE));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_GRAS_SU_DEPTH_BUFFER_INFO, format));
    A840_OPS_TRY(emit_flag_buffer(batch, A840_REG_RB_DEPTH_FLAG_BUFFER_BASE, depth, true));
    index = batch->used_dwords + 4u;
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_STENCIL_BUFFER_INFO, stencil_regs, 6));
    if (stencil->data.gpu_va)
        A840_OPS_TRY(a840_emit_record_address(batch, index, stencil->data, A840_RELOC_READ | A840_RELOC_WRITE));
    return A840_EMIT_OK;
}

static const A840FormatCaps *mrt_format(const A840MrtDesc *target)
{
    for (size_t i = 0; i < A840_FORMAT_CAPS_COUNT; ++i) {
        const A840FormatCaps *format = &a840_format_caps_data[i];
        if (format->color_fmt_id == target->format && format->renderable &&
            !(format->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_COMPRESSED)) &&
            target->srgb == ((format->flags & A840_FORMAT_SRGB) != 0) &&
            target->swap == (target->storage.tile_mode == A840_TILE_LINEAR ? format->color_swap : A840_WZYX))
            return format;
    }
    return NULL;
}

static bool ordinary_blend_factor(uint8_t factor)
{
    return factor <= A840_FACTOR_ONE ||
        (factor >= A840_FACTOR_SRC_COLOR && factor <= A840_FACTOR_SRC_ALPHA_SATURATE);
}

static A840EmitStatus emit_color_targets(A840CmdStream *batch, const A840RenderStateDesc *desc)
{
    uint32_t formats[A840_MAX_COLOR_TARGETS] = {0}, sp_blend[A840_MAX_COLOR_TARGETS] = {0};
    uint32_t blend_mask = 0, srgb_mask = 0, mrt_count = 0;
    const uint32_t mrt_stride = A840_REG_RB_MRT_1_CONTROL - A840_REG_RB_MRT_0_CONTROL;
    const uint32_t flags_stride = A840_REG_RB_COLOR_FLAG_BUFFER_1_ADDR - A840_REG_RB_COLOR_FLAG_BUFFER_0_ADDR;

    for (uint32_t i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        const A840MrtDesc *target = &desc->colors[i];
        const A840AttachmentStorage *storage = &target->storage;
        uint32_t values[8] = {0};
        A840_OPS_TRY(encode_attachment(storage, A840_MRT_PITCH_MAX, A840_MRT_ARRAY_PITCH_MAX, values + 3));
        if (storage->data.gpu_va) {
            const A840FormatCaps *format = mrt_format(target);
            if (format == NULL) return A840_EMIT_UNSUPPORTED;
            const bool ubwc = storage->tile_mode == A840_TILE_UBWC;
            const A840BlendDesc *blend = &target->blend;
            if ((ubwc && !format->ubwc_supported) || (blend->enabled && !format->blendable))
                return A840_EMIT_UNSUPPORTED;
            if (blend->write_mask > A840_MRT_MASK_MAX || blend->op_rgb > A840_BLEND_MAX ||
                blend->op_alpha > A840_BLEND_MAX || !ordinary_blend_factor(blend->src_rgb) ||
                !ordinary_blend_factor(blend->dst_rgb) || !ordinary_blend_factor(blend->src_alpha) ||
                !ordinary_blend_factor(blend->dst_alpha))
                return A840_EMIT_RANGE_ERROR;

            const uint32_t enabled = blend->enabled ? A840_MRT_COLOR_BLEND | A840_MRT_ALPHA_BLEND : 0u;
            values[0] = enabled | (uint32_t)blend->write_mask << A840_MRT_MASK_SHIFT;
            values[1] = (uint32_t)blend->src_rgb << A840_MRT_RGB_SRC_SHIFT |
                (uint32_t)blend->op_rgb << A840_MRT_RGB_OP_SHIFT |
                (uint32_t)blend->dst_rgb << A840_MRT_RGB_DST_SHIFT |
                (uint32_t)blend->src_alpha << A840_MRT_ALPHA_SRC_SHIFT |
                (uint32_t)blend->op_alpha << A840_MRT_ALPHA_OP_SHIFT |
                (uint32_t)blend->dst_alpha << A840_MRT_ALPHA_DST_SHIFT;
            const uint32_t tile = storage->tile_mode == A840_TILE_LINEAR ? A840_TILE6_LINEAR : A840_TILE6_3;
            values[2] = target->format | tile << A840_MRT_TILE_SHIFT |
                target->swap << A840_MRT_SWAP_SHIFT | (ubwc ? A840_MRT_UBWC : 0u);
            formats[i] = target->format;
            if (strstr(format->pipe_format, "_SINT")) formats[i] |= A840_PS_MRT_SINT;
            if (strstr(format->pipe_format, "_UINT")) formats[i] |= A840_PS_MRT_UINT;
            sp_blend[i] = (blend->enabled ? A840_SP_MRT_COLOR_BLEND | A840_SP_MRT_ALPHA_BLEND : 0u) |
                (uint32_t)blend->write_mask << A840_SP_MRT_MASK_SHIFT;
            if (blend->enabled) blend_mask |= 1u << i;
            if (target->srgb) srgb_mask |= 1u << i;
            mrt_count = i + 1u;
        }

        size_t index = batch->used_dwords + 6u;
        A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_MRT_0_CONTROL + i * mrt_stride, values, 8));
        if (storage->data.gpu_va)
            A840_OPS_TRY(a840_emit_record_address(batch, index, storage->data, A840_RELOC_READ | A840_RELOC_WRITE));
        A840_OPS_TRY(emit_flag_buffer(batch, A840_REG_RB_COLOR_FLAG_BUFFER_0_ADDR + i * flags_stride,
            storage, false));
    }

    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_PS_MRT_0_REG, formats, A840_MAX_COLOR_TARGETS));
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_MRT_BLEND_CNTL_0_REG, sp_blend, A840_MAX_COLOR_TARGETS));
    uint32_t constants[4];
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(desc->blend_color[i])) return A840_EMIT_RANGE_ERROR;
        constants[i] = float_word(desc->blend_color[i]);
    }
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_RB_BLEND_CONSTANT_RED_FP32, constants, 4));
    const uint32_t sp[] = {blend_mask | A840_SP_INDEPENDENT_BLEND, srgb_mask};
    A840_OPS_TRY(a840_cs_emit_pkt4(batch, A840_REG_SP_BLEND_CNTL, sp, 2));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_BLEND_CNTL, blend_mask | A840_RB_INDEPENDENT_BLEND |
        (uint32_t)desc->sample_mask << A840_RB_BLEND_SAMPLE_SHIFT));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_SRGB_CNTL, srgb_mask));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_SP_PS_MRT_CNTL, mrt_count));
    A840_OPS_TRY(a840_emit_reg(batch, A840_REG_RB_PS_MRT_CNTL, mrt_count));
    return a840_emit_reg(batch, A840_REG_GRAS_LRZ_MRT_BUFFER_INFO_0,
        desc->colors[0].storage.data.gpu_va ? desc->colors[0].format : A840_FMT6_NONE);
}

A840EmitStatus a840_emit_3d_render_state(A840CmdStream *cs, const A840RenderStateDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840RenderStateDesc)));
    uint32_t words[A840_EMIT_3D_RENDER_STATE_MAX_DWORDS];
    /* Eight color planes + flag buffers, depth + flags, separate stencil. */
    A840Reloc relocs[2u * A840_MAX_COLOR_TARGETS + 3u];
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_3D_RENDER_STATE_MAX_DWORDS, relocs,
        sizeof(relocs) / sizeof(relocs[0])));
    const uint32_t mode = A840_MODE_DIRECT_RENDER;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_SET_MARKER, &mode, 1));
    A840_OPS_TRY(emit_viewports(&batch, desc));
    A840_OPS_TRY(emit_raster_depth_stencil(&batch, desc));
    A840_OPS_TRY(emit_depth_targets(&batch, desc));
    A840_OPS_TRY(emit_color_targets(&batch, desc));
    return a840_emit_commit(cs, &batch);
}

static bool draw_primitive(uint32_t primitive)
{
    switch (primitive) {
    case A840_PRIM_POINTS:
    case A840_PRIM_LINES:
    case A840_PRIM_LINE_STRIP:
    case A840_PRIM_TRIANGLES:
    case A840_PRIM_TRIANGLE_STRIP:
    case A840_PRIM_TRIANGLE_FAN:
        return true;
    default:
        return false;
    }
}

A840EmitStatus a840_emit_3d_draw(A840CmdStream *cs, const A840DrawDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840DrawDesc)));
    if (!draw_primitive(desc->primitive)) return A840_EMIT_UNSUPPORTED;
    if (desc->index_type != A840_INDEX_NONE && desc->index_type != A840_INDEX_UINT16 &&
        desc->index_type != A840_INDEX_UINT32) return A840_EMIT_INVALID_ARG;
    if (!desc->count || !desc->instance_count) return A840_EMIT_OK;
    if (desc->instance_count - 1u > UINT32_MAX - desc->first_instance) return A840_EMIT_RANGE_ERROR;

    const bool indexed = desc->index_type != A840_INDEX_NONE;
    uint32_t initiator = desc->primitive << A840_DRAW_PRIMITIVE_SHIFT |
        (desc->use_visibility ? A840_DRAW_USE_VIS : A840_DRAW_IGNORE_VIS) << A840_DRAW_VISIBILITY_SHIFT;
    if (indexed) {
        if (desc->first_index >= desc->index_capacity ||
            desc->count > desc->index_capacity - desc->first_index) return A840_EMIT_RANGE_ERROR;
        A840_OPS_TRY(a840_ops_va(desc->index_buffer.gpu_va,
            (uint64_t)desc->index_capacity * (uint32_t)desc->index_type, (uint32_t)desc->index_type));
        initiator |= A840_DRAW_DMA << A840_DRAW_SOURCE_SHIFT |
            (desc->index_type == A840_INDEX_UINT16 ? A840_DRAW_INDEX_16 : A840_DRAW_INDEX_32) << A840_DRAW_INDEX_SHIFT;
    } else {
        if (desc->count - 1u > UINT32_MAX - desc->first_vertex) return A840_EMIT_RANGE_ERROR;
        initiator |= A840_DRAW_AUTO << A840_DRAW_SOURCE_SHIFT;
    }
    uint32_t words[A840_EMIT_3D_DRAW_MAX_DWORDS];
    A840Reloc reloc;
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_3D_DRAW_MAX_DWORDS, &reloc, 1));
    const uint32_t offsets[] = {
        indexed ? (uint32_t)desc->base_vertex : desc->first_vertex, desc->first_instance,
    };
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_VFD_INDEX_OFFSET, offsets, 2));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_DRAWCALL_CNTL, initiator));
    const uint32_t payload[] = {
        initiator, desc->instance_count, desc->count, desc->first_index,
        (uint32_t)desc->index_buffer.gpu_va, (uint32_t)(desc->index_buffer.gpu_va >> 32), desc->index_capacity,
    };
    const size_t index = batch.used_dwords + 5u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_DRAW_INDX_OFFSET, payload, indexed ? 7u : 3u));
    if (indexed)
        A840_OPS_TRY(a840_emit_record_address(&batch, index, desc->index_buffer, A840_RELOC_READ));
    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_sds_group(A840CmdStream *cs, const A840SdsGroupDesc *groups, size_t count)
{
    if (count > ADRENO840_MAX_SDS_SLOTS) return A840_EMIT_RANGE_ERROR;
    if (!count) {
        A840OpsSpan words, relocs, metadata;
        return a840_ops_cs_storage(cs, &words, &relocs, &metadata);
    }
    A840_OPS_TRY(a840_ops_input_storage(cs, groups, count * sizeof(*groups), _Alignof(A840SdsGroupDesc)));
    uint32_t words[A840_EMIT_SDS_GROUP_MAX_DWORDS];
    A840Reloc relocs[ADRENO840_MAX_SDS_SLOTS];
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_SDS_GROUP_MAX_DWORDS,
        relocs, ADRENO840_MAX_SDS_SLOTS));
    for (size_t i = 0; i < count; ++i) {
        const A840SdsGroupDesc *group = &groups[i];
        if (group->state_dwords)
            A840_OPS_TRY(a840_ops_va(group->state.gpu_va, (uint64_t)group->state_dwords * 4u, 4u));
        A840_OPS_TRY(a840_cs_emit_sds_group(&batch, group->group_id, group->enable_mask,
            group->state.bo_handle, group->state.gpu_va, group->state_dwords));
    }
    return a840_emit_commit(cs, &batch);
}
