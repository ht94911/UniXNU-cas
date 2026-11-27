/* SPDX-License-Identifier: MIT */
#include "a840_phase4_pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "a840_emit_gmem.h"
#include "a840_emit_ops.h"
#include "a840_gmem_planner.h"
#include "gen/a840_bitfields.h"

static uint32_t div_ceil_u32(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

static bool has_valid_compute_stage(const A840ComputePipelineDesc *pipe)
{
    const A840CompiledStage *st;
    if (!pipe || !pipe->shader.stage) {
        return false;
    }
    st = pipe->shader.stage;
    return st->stage == A840_STAGE_COMPUTE &&
           st->isa_dwords > 0u &&
           st->local_size[0] > 0u &&
           st->local_size[1] > 0u &&
           st->local_size[2] > 0u;
}

void a840_phase4_enable_full_router(A840DualPathRouter *router)
{
    if (!router) {
        return;
    }
    a840_router_enable_category(router, (uint32_t)A840_ROUTE_PHASE4_FULL_ALL);
    a840_router_set_gmem_engine(router, true);
}

/* ============================================================================
 * Cold Compile Resolution via W07-L2 NezhaShaderCompiler Client Bridge
 * ============================================================================ */

A840RtStatus a840_phase4_resolve_cold_compile(
    A840DualPathRouter *router,
    const A840Phase4CompilerBridge *bridge,
    const uint8_t pipeline_key[32],
    const void *payload,
    size_t payload_bytes,
    bool precompiled_available,
    A840Phase4ColdCompileResult *out_result)
{
    A840CompilerRouteQuery cq;
    A840Phase4ColdCompileResult res;
    bool daemon_healthy = false;

    if (!router || !out_result) {
        return A840_RT_INVALID_ARG;
    }

    memset(&res, 0, sizeof(res));
    memset(&cq, 0, sizeof(cq));

    if (precompiled_available) {
        cq.precompiled_available = true;
        cq.daemon_available = false;
        res.route = a840_router_eval_compiler(router, &cq);
        res.compiler_state = A840_P4_COMPILER_OK;
        *out_result = res;
        return res.route.path == A840_ROUTE_PATH_RAW ? A840_RT_OK : A840_RT_FALLBACK_REQUIRED;
    }

    if (!pipeline_key || (!payload && payload_bytes > 0u)) {
        return A840_RT_INVALID_ARG;
    }

    /* Check live health / circuit breaker of the injected compiler client. */
    if (bridge && bridge->client_handle && bridge->compile_sync) {
        daemon_healthy = bridge->is_available ? bridge->is_available(bridge->client_handle) : true;
    }

    /* When the compiler daemon is unreachable or its circuit breaker is open,
     * fail closed to raw:compiler.daemon_unavailable even if capability bit 16
     * is set on the router: live health overrides static capability. */
    if (!daemon_healthy) {
        A840DualPathRouter probe = *router;
        a840_router_disable_category(&probe, A840_ROUTE_P4_COMPILER_DAEMON);
        cq.precompiled_available = false;
        cq.daemon_available = false;
        res.route = a840_router_eval_compiler(&probe, &cq);
        /* Mirror counters on caller's router deterministically. */
        if (res.route.path == A840_ROUTE_PATH_FALLBACK) {
            router->fallback_total++;
            if (res.route.predicate_id >= A840_PRED_UNIMPLEMENTED_GLOBAL &&
                res.route.predicate_id <= A840_PRED_COUNT) {
                router->predicate_counts[res.route.predicate_id]++;
            }
        }
        res.compiler_state = A840_P4_COMPILER_DAEMON_UNAVAILABLE;
        *out_result = res;
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Evaluate router with live daemon available. */
    cq.precompiled_available = false;
    cq.daemon_available = true;
    res.route = a840_router_eval_compiler(router, &cq);
    if (res.route.path != A840_ROUTE_PATH_RAW) {
        res.compiler_state = A840_P4_COMPILER_DAEMON_UNAVAILABLE;
        *out_result = res;
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Perform synchronous cold compile through the injected client bridge. */
    {
        uint8_t *blob = NULL;
        size_t blob_bytes = 0u;
        A840Phase4CompilerState cst = bridge->compile_sync(
            bridge->client_handle,
            pipeline_key,
            payload,
            payload_bytes,
            bridge->default_timeout_ms,
            &blob,
            &blob_bytes);
        if (cst != A840_P4_COMPILER_OK || !blob || blob_bytes == 0u) {
            if (blob) {
                if (bridge->free_blob) {
                    bridge->free_blob(blob);
                } else {
                    free(blob);
                }
            }
            /* Fail closed on transport drop / circuit trip during compile. */
            res.route.path = A840_ROUTE_PATH_FALLBACK;
            res.route.predicate_id = A840_PRED_COMPILER_DAEMON_UNAVAILABLE;
            res.route.predicate = a840_router_predicate_name(A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
            router->fallback_total++;
            router->predicate_counts[A840_PRED_COMPILER_DAEMON_UNAVAILABLE]++;
            res.compiler_state = (cst == A840_P4_COMPILER_OK)
                                     ? A840_P4_COMPILER_DAEMON_UNAVAILABLE
                                     : cst;
            *out_result = res;
            return A840_RT_FALLBACK_REQUIRED;
        }
        res.record_blob = blob;
        res.record_bytes = blob_bytes;
        res.compiler_state = A840_P4_COMPILER_OK;
    }

    *out_result = res;
    return A840_RT_OK;
}

/* ============================================================================
 * 1. Native MSAA 2x/4x Resolve + Visibility Query Pass
 * ============================================================================ */

static A840RtStatus fill_resolve_surface2d(const A840UbwcTexture *tex, A840Surface2D *out_surf)
{
    if (!tex || !out_surf || !tex->format_caps || tex->samples != 1u ||
        tex->storage_mode == A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    memset(out_surf, 0, sizeof(*out_surf));
    out_surf->bo_handle = tex->data_range.bo;
    out_surf->gpu_va = tex->data_range.va;
    out_surf->format_id = tex->format_caps->color_fmt_id;
    out_surf->swap = tex->format_caps->color_swap;
    out_surf->width = tex->width;
    out_surf->height = tex->height;
    out_surf->pitch = (tex->tile_mode == A840_TILE_LINEAR) ? tex->plane_pitches[0]
                                                           : tex->ubwc.data_pitch;
    out_surf->tile_mode = tex->tile_mode;
    out_surf->srgb = tex->is_srgb ? 1u : 0u;
    if (tex->tile_mode == A840_TILE_UBWC) {
        out_surf->ubwc_meta_va = tex->flag_range.va;
        out_surf->ubwc_meta_pitch = tex->ubwc.flag_pitch;
        out_surf->ubwc_meta_slice_stride = tex->ubwc.flag_slice_bytes;
    }
    return A840_RT_OK;
}

A840RtStatus a840_phase4_encode_msaa_vis_pass(
    A840DualPathRouter *router,
    const A840Phase4MsaaVisPassDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats)
{
    A840RenderRouteQuery rq;
    A840RouteDecision decision;
    A840GmemPass gpass;
    A840GmemPlan plan;
    A840MsaaStateDesc msaa_state;
    A840MsaaResolveBlitDesc resolve_desc;
    A840VisQueryDesc vis_desc;
    A840Surface2D resolve_surf;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus est;
    A840RtStatus rst;

    if (!router || !desc || !desc->msaa_color_target || !desc->resolve_target || !cs) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->msaa_color_target->width != desc->resolve_target->width ||
        desc->msaa_color_target->height != desc->resolve_target->height) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->msaa_color_target->samples != 2u && desc->msaa_color_target->samples != 4u) {
        return A840_RT_UNSUPPORTED;
    }
    if (desc->resolve_target->samples != 1u ||
        desc->resolve_target->storage_mode == A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->enable_visibility_query && !desc->visibility_buffer) {
        return A840_RT_INVALID_ARG;
    }

    /* Evaluate DualPathRouter before emitting any packets. */
    memset(&rq, 0, sizeof(rq));
    rq.color_attachment_count = 1u;
    rq.color_mtl_formats[0] = desc->msaa_color_target->mtl_pixel_format;
    rq.color_tile_modes[0] = desc->resolve_target->tile_mode;
    rq.sample_count = desc->msaa_color_target->samples;
    rq.has_msaa_resolve = true;
    rq.has_memoryless_attachment =
        (desc->msaa_color_target->storage_mode == A840_STORAGE_MEMORYLESS) ||
        (desc->depth_target && desc->depth_target->storage_mode == A840_STORAGE_MEMORYLESS);
    if (desc->depth_target) {
        rq.depth_mtl_format = desc->depth_target->mtl_pixel_format;
        rq.depth_tile_mode = desc->depth_target->tile_mode;
    }

    decision = a840_router_eval_render(router, &rq);
    if (decision.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = decision;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Plan GMEM tiling for MSAA color (+ optional Memoryless MSAA depth). */
    memset(&gpass, 0, sizeof(gpass));
    gpass.width = desc->msaa_color_target->width;
    gpass.height = desc->msaa_color_target->height;
    gpass.draw_count = 1u;
    gpass.has_msaa_resolve = true;
    gpass.attachments[0].cpp = desc->msaa_color_target->cpp;
    gpass.attachments[0].samples = desc->msaa_color_target->samples;
    gpass.attachments[0].load = A840_LOAD_CLEAR;
    gpass.attachments[0].store = A840_STORE_RESOLVE;
    gpass.attachments[0].memoryless =
        (desc->msaa_color_target->storage_mode == A840_STORAGE_MEMORYLESS);
    gpass.attachments[0].ubwc = (desc->resolve_target->tile_mode == A840_TILE_UBWC);

    if (desc->depth_target) {
        gpass.attachments[A840_GMEM_DEPTH].cpp = desc->depth_target->cpp;
        gpass.attachments[A840_GMEM_DEPTH].samples = desc->depth_target->samples;
        gpass.attachments[A840_GMEM_DEPTH].load = A840_LOAD_CLEAR;
        gpass.attachments[A840_GMEM_DEPTH].store = A840_STORE_DONT_CARE;
        gpass.attachments[A840_GMEM_DEPTH].memoryless =
            (desc->depth_target->storage_mode == A840_STORAGE_MEMORYLESS);
        gpass.attachments[A840_GMEM_DEPTH].ubwc = false;
    }

    est = a840_gmem_plan(&gpass, &plan);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    rst = fill_resolve_surface2d(desc->resolve_target, &resolve_surf);
    if (rst != A840_RT_OK) {
        return rst;
    }

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    /* Emit MSAA state (GRAS/RB/TPL1 + standard/custom sample locations). */
    memset(&msaa_state, 0, sizeof(msaa_state));
    msaa_state.sample_count = desc->msaa_color_target->samples;
    msaa_state.sample_rate_shading = desc->sample_rate_shading;
    msaa_state.custom_sample_locations = desc->custom_sample_locations;
    if (desc->custom_sample_locations) {
        memcpy(msaa_state.locations, desc->sample_locations, sizeof(msaa_state.locations));
    }
    est = a840_emit_msaa_state(cs, &msaa_state);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (desc->emit_depth_stencil_variant) {
        est = a840_emit_depth_stencil_variant(cs, &desc->depth_stencil);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    /* Emit 3D program and visibility query bracket around draw. */
    est = a840_emit_3d_program(cs, &desc->program);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&vis_desc, 0, sizeof(vis_desc));
    if (desc->enable_visibility_query) {
        vis_desc.query_buffer.bo_handle = desc->visibility_buffer->bo_handle;
        vis_desc.query_buffer.gpu_va = desc->visibility_buffer->gpu_va;
        vis_desc.buffer_bytes = desc->visibility_buffer->size_bytes;
        vis_desc.slot_index = desc->visibility_slot;
        vis_desc.mode = desc->visibility_mode;
        vis_desc.inside_render_pass = true;
        vis_desc.emit_rb_sample_counter_base = true;

        est = a840_emit_visibility_query_begin(cs, &vis_desc);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    est = a840_emit_3d_draw(cs, &desc->draw);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (desc->enable_visibility_query) {
        est = a840_emit_visibility_query_end(cs, &vis_desc);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    /* Emit per-tile GMEM->Sysmem MSAA Resolve Blit for every tile in the GMEM plan. */
    for (uint32_t ty = 0u; ty < plan.tiles_y; ++ty) {
        for (uint32_t tx = 0u; tx < plan.tiles_x; ++tx) {
            A840GmemTile tile;
            est = a840_gmem_tile(&plan, tx, ty, &tile);
            if (est != A840_EMIT_OK) {
                goto fail_rollback;
            }
            memset(&resolve_desc, 0, sizeof(resolve_desc));
            resolve_desc.src_samples = desc->msaa_color_target->samples;
            resolve_desc.gmem_offset = plan.planes[0].offset;
            resolve_desc.gmem_pitch = plan.planes[0].pitch;
            resolve_desc.slot_index = 0u;
            resolve_desc.tile_w = plan.tile_w;
            resolve_desc.tile_h = plan.tile_h;
            resolve_desc.resolve_rect.x = tile.x;
            resolve_desc.resolve_rect.y = tile.y;
            resolve_desc.resolve_rect.width = tile.width;
            resolve_desc.resolve_rect.height = tile.height;
            resolve_desc.dst = resolve_surf;
            resolve_desc.mode = desc->resolve_mode;
            resolve_desc.use_2d_engine = desc->force_2d_resolve;
            resolve_desc.emit_ccu_uche_barriers =
                (tx + 1u == plan.tiles_x && ty + 1u == plan.tiles_y);

            est = a840_emit_msaa_resolve_blit(cs, &resolve_desc);
            if (est != A840_EMIT_OK) {
                goto fail_rollback;
            }
        }
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->route = decision;
        out_stats->emitted_dwords = cs->used_dwords - start_dwords;
        out_stats->emitted_relocs = cs->reloc_count - start_relocs;
        out_stats->tile_count = plan.tiles_x * plan.tiles_y;
    }
    return A840_RT_OK;

fail_rollback:
    cs->used_dwords = start_dwords;
    cs->reloc_count = start_relocs;
    return a840_rt_status_from_emit(est);
}

/* ============================================================================
 * 2. Native Tessellation & Vertex Amplification Pass
 * ============================================================================ */

A840RtStatus a840_phase4_encode_tess_amp_pass(
    A840DualPathRouter *router,
    const A840Phase4TessAmpPassDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats)
{
    A840RenderRouteQuery rq;
    A840RouteDecision decision;
    A840RenderStateDesc rs;
    A840TessStateDesc ts;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus est;
    A840RtStatus rst;

    if (!router || !desc || !desc->color_target || !cs) {
        return A840_RT_INVALID_ARG;
    }
    if (!desc->enable_tessellation && !desc->enable_vertex_amp) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->enable_tessellation && !desc->tess_factor_buffer) {
        return A840_RT_INVALID_ARG;
    }

    memset(&rq, 0, sizeof(rq));
    rq.color_attachment_count = 1u;
    rq.color_mtl_formats[0] = desc->color_target->mtl_pixel_format;
    rq.color_tile_modes[0] = desc->color_target->tile_mode;
    rq.sample_count = desc->color_target->samples ? desc->color_target->samples : 1u;
    rq.uses_tessellation = desc->enable_tessellation;
    rq.uses_vertex_amplification = desc->enable_vertex_amp;

    decision = a840_router_eval_render(router, &rq);
    if (decision.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = decision;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Setup base render state targeting desc->color_target. */
    memset(&rs, 0, sizeof(rs));
    rs.viewport_count = 1u;
    rs.scissor_count = 1u;
    rs.viewports[0].width = (float)desc->color_target->width;
    rs.viewports[0].height = (float)desc->color_target->height;
    rs.viewports[0].min_depth = 0.0f;
    rs.viewports[0].max_depth = 1.0f;
    rs.scissors[0].width = desc->color_target->width;
    rs.scissors[0].height = desc->color_target->height;
    rs.line_width = 1.0f;
    rs.sample_mask = UINT16_MAX;
    rst = a840_rt_texture_as_attachment(desc->color_target, &rs.colors[0].storage);
    if (rst != A840_RT_OK) {
        return rst;
    }
    rs.colors[0].format = desc->color_target->color_format_id;
    rs.colors[0].swap = desc->color_target->color_swap;
    rs.colors[0].srgb = desc->color_target->srgb;
    rs.colors[0].blend.write_mask = 0xfu;

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    est = a840_emit_3d_program(cs, &desc->program);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_3d_render_state(cs, &rs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    if (desc->emit_depth_stencil_variant) {
        est = a840_emit_depth_stencil_variant(cs, &desc->depth_stencil);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    if (desc->enable_vertex_amp) {
        est = a840_emit_vertex_amplification_state(cs, &desc->vertex_amp);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    if (desc->enable_tessellation) {
        ts = desc->tess_state;
        ts.tess_factor_buffer.bo_handle = desc->tess_factor_buffer->bo_handle;
        ts.tess_factor_buffer.gpu_va = desc->tess_factor_buffer->gpu_va;
        ts.tess_buffer_bytes = desc->tess_factor_buffer->size_bytes;

        est = a840_emit_tess_state(cs, &ts);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
        est = a840_emit_tess_draw(cs, &desc->tess_draw);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    } else {
        est = a840_emit_3d_draw(cs, &desc->fallback_draw);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    est = a840_emit_cache_flush_inv(cs, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wait_mem_writes(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->route = decision;
        out_stats->emitted_dwords = cs->used_dwords - start_dwords;
        out_stats->emitted_relocs = cs->reloc_count - start_relocs;
        out_stats->tile_count = 1u;
    }
    return A840_RT_OK;

fail_rollback:
    cs->used_dwords = start_dwords;
    cs->reloc_count = start_relocs;
    return a840_rt_status_from_emit(est);
}

/* ============================================================================
 * 3. Camera 155 (NV12/P010 + 4096x3072 BGRA8) / Core Image / MPS GEMM Flows
 * ============================================================================ */

A840RtStatus a840_phase4_encode_camera155_flow(
    A840DualPathRouter *router,
    const A840Phase4Camera155Desc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats)
{
    A840RenderRouteQuery rq;
    A840ComputeRouteQuery cq;
    A840RouteDecision rdec;
    A840RouteDecision cdec;
    A840BindlessTableDesc bindless;
    A840ConstUploadDesc consts;
    A840DispatchDesc dispatch;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus est;

    if (!router || !desc || !desc->preview_ycbcr || !desc->photo_bgra8 ||
        !desc->yuv_to_rgb_consts || !desc->bindless_table || !cs) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->preview_ycbcr->kind != A840_P3_TEX_YCBCR_MULTIPLANE ||
        (desc->preview_ycbcr->ycbcr_format != A840_YCBCR_NV12 &&
         desc->preview_ycbcr->ycbcr_format != A840_YCBCR_P010)) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->photo_bgra8->mtl_pixel_format != A840_MTL_FMT_BGRA8_UNORM &&
        desc->photo_bgra8->mtl_pixel_format != A840_MTL_FMT_BGRA8_UNORM_SRGB) {
        return A840_RT_UNSUPPORTED;
    }

    /* Check YCbCr + UBWC photo render routing. */
    memset(&rq, 0, sizeof(rq));
    rq.color_attachment_count = 1u;
    rq.color_mtl_formats[0] = desc->photo_bgra8->mtl_pixel_format;
    rq.color_tile_modes[0] = desc->photo_bgra8->tile_mode;
    rq.sample_count = 1u;
    rq.has_ycbcr_attachment = true;
    rdec = a840_router_eval_render(router, &rq);
    if (rdec.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = rdec;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Check compute routing for YCbCr -> BGRA8 conversion kernel. */
    memset(&cq, 0, sizeof(cq));
    cq.has_compiled_kernel = has_valid_compute_stage(&desc->yuv_convert_pipe);
    cq.has_valid_local_size = cq.has_compiled_kernel;
    cdec = a840_router_eval_compute(router, &cq);
    if (cdec.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = cdec;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    /* 1. Native compute dispatch: NV12/P010 YCbCr preview -> BGRA8 conversion. */
    est = a840_emit_compute_pipeline(cs, &desc->yuv_convert_pipe);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&bindless, 0, sizeof(bindless));
    bindless.count = 1u;
    bindless.tables[0].bo_handle = desc->bindless_table->bo_handle;
    bindless.tables[0].gpu_va = desc->bindless_table->gpu_va;
    est = a840_emit_compute_bindless(cs, &bindless);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&consts, 0, sizeof(consts));
    consts.source = A840_CONST_INDIRECT;
    consts.dst_vec4 = 0u;
    consts.count_vec4 = 4u; /* BT.709/BT.2020 YCbCr matrix + chroma range */
    consts.constlen_vec4 = desc->yuv_convert_pipe.shader.stage->constlen_vec4
                               ? desc->yuv_convert_pipe.shader.stage->constlen_vec4
                               : 4u;
    consts.address.bo_handle = desc->yuv_to_rgb_consts->bo_handle;
    consts.address.gpu_va = desc->yuv_to_rgb_consts->gpu_va;
    est = a840_emit_compute_consts(cs, &consts);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.stage = desc->yuv_convert_pipe.shader.stage;
    dispatch.mode = A840_DISPATCH_NON_UNIFORM;
    dispatch.size[0] = desc->photo_bgra8->width;
    dispatch.size[1] = desc->photo_bgra8->height;
    dispatch.size[2] = 1u;
    est = a840_emit_compute_dispatch(cs, &dispatch);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    est = a840_emit_cache_flush_inv(cs, A840_CACHE_UCHE_INV | A840_CACHE_CCU_FLUSH_COLOR);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wait_mem_writes(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (desc->emit_photo_clear_and_blit && desc->photo_bgra8->width <= A840_2D_COORD_MAX + 1u &&
        desc->photo_bgra8->height <= A840_2D_COORD_MAX + 1u) {
        /* Emit optional 2D border/ROI sync clear on photo output. */
        A840Clear2DDesc clr;
        A840RtStatus rst = fill_resolve_surface2d(desc->photo_bgra8, &clr.dst);
        if (rst != A840_RT_OK) {
            cs->used_dwords = start_dwords;
            cs->reloc_count = start_relocs;
            return rst;
        }
        clr.rect.x = 0u;
        clr.rect.y = 0u;
        clr.rect.width = 16u < clr.dst.width ? 16u : clr.dst.width;
        clr.rect.height = 4u < clr.dst.height ? 4u : clr.dst.height;
        {
            const float one = 1.0f;
            uint32_t alpha_bits = 0u;
            memcpy(&alpha_bits, &one, sizeof(alpha_bits));
            clr.color[0] = 0u;
            clr.color[1] = 0u;
            clr.color[2] = 0u;
            clr.color[3] = alpha_bits;
        }
        est = a840_emit_2d_clear(cs, &clr);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
        est = a840_emit_cache_flush_inv(cs, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV);
        if (est != A840_EMIT_OK) {
            goto fail_rollback;
        }
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->route = cdec;
        out_stats->emitted_dwords = cs->used_dwords - start_dwords;
        out_stats->emitted_relocs = cs->reloc_count - start_relocs;
        out_stats->tile_count = 1u;
    }
    return A840_RT_OK;

fail_rollback:
    cs->used_dwords = start_dwords;
    cs->reloc_count = start_relocs;
    return a840_rt_status_from_emit(est);
}

A840RtStatus a840_phase4_encode_coreimage_gaussian(
    A840DualPathRouter *router,
    const A840Phase4CoreImageGaussianDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats)
{
    A840ComputeRouteQuery cq;
    A840RouteDecision decision;
    A840BindlessTableDesc bindless;
    A840ConstUploadDesc consts;
    A840DispatchDesc dispatch;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus est;

    if (!router || !desc || !desc->input_tex || !desc->intermediate_tex || !desc->output_tex ||
        !desc->weights_buffer || !desc->bindless_table || !cs) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->kernel_radius == 0u || desc->kernel_radius > 31u ||
        desc->input_tex->width != desc->output_tex->width ||
        desc->input_tex->height != desc->output_tex->height ||
        desc->intermediate_tex->width != desc->input_tex->width ||
        desc->intermediate_tex->height != desc->input_tex->height) {
        return A840_RT_INVALID_ARG;
    }

    memset(&cq, 0, sizeof(cq));
    cq.has_compiled_kernel = has_valid_compute_stage(&desc->horiz_pipe) &&
                             has_valid_compute_stage(&desc->vert_pipe);
    cq.has_valid_local_size = cq.has_compiled_kernel;
    decision = a840_router_eval_compute(router, &cq);
    if (decision.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = decision;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    memset(&bindless, 0, sizeof(bindless));
    bindless.count = 1u;
    bindless.tables[0].bo_handle = desc->bindless_table->bo_handle;
    bindless.tables[0].gpu_va = desc->bindless_table->gpu_va;

    /* Pass 1: Horizontal separable Gaussian convolution. */
    est = a840_emit_compute_pipeline(cs, &desc->horiz_pipe);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_compute_bindless(cs, &bindless);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    memset(&consts, 0, sizeof(consts));
    consts.source = A840_CONST_INDIRECT;
    consts.dst_vec4 = 0u;
    consts.count_vec4 = div_ceil_u32(desc->kernel_radius + 1u, 4u);
    consts.constlen_vec4 = desc->horiz_pipe.shader.stage->constlen_vec4
                               ? desc->horiz_pipe.shader.stage->constlen_vec4
                               : consts.count_vec4;
    consts.address.bo_handle = desc->weights_buffer->bo_handle;
    consts.address.gpu_va = desc->weights_buffer->gpu_va;
    est = a840_emit_compute_consts(cs, &consts);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.stage = desc->horiz_pipe.shader.stage;
    dispatch.mode = A840_DISPATCH_NON_UNIFORM;
    dispatch.size[0] = desc->input_tex->width;
    dispatch.size[1] = desc->input_tex->height;
    dispatch.size[2] = 1u;
    est = a840_emit_compute_dispatch(cs, &dispatch);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    /* Barrier between horizontal write and vertical read of intermediate_tex. */
    est = a840_emit_cache_flush_inv(cs, A840_CACHE_UCHE_INV);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wait_mem_writes(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wfi(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    /* Pass 2: Vertical separable Gaussian convolution. */
    est = a840_emit_compute_pipeline(cs, &desc->vert_pipe);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_compute_bindless(cs, &bindless);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    consts.constlen_vec4 = desc->vert_pipe.shader.stage->constlen_vec4
                               ? desc->vert_pipe.shader.stage->constlen_vec4
                               : consts.count_vec4;
    est = a840_emit_compute_consts(cs, &consts);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    dispatch.stage = desc->vert_pipe.shader.stage;
    est = a840_emit_compute_dispatch(cs, &dispatch);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_cache_flush_inv(cs, A840_CACHE_UCHE_INV);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wait_mem_writes(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->route = decision;
        out_stats->emitted_dwords = cs->used_dwords - start_dwords;
        out_stats->emitted_relocs = cs->reloc_count - start_relocs;
        out_stats->tile_count = 2u;
    }
    return A840_RT_OK;

fail_rollback:
    cs->used_dwords = start_dwords;
    cs->reloc_count = start_relocs;
    return a840_rt_status_from_emit(est);
}

A840RtStatus a840_phase4_encode_mps_gemm(
    A840DualPathRouter *router,
    const A840Phase4MpsGemmDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats)
{
    A840ComputeRouteQuery cq;
    A840RouteDecision decision;
    A840BindlessTableDesc bindless;
    A840ConstUploadDesc consts;
    A840DispatchDesc dispatch;
    size_t start_dwords;
    size_t start_relocs;
    A840EmitStatus est;

    if (!router || !desc || !desc->matrix_a || !desc->matrix_b || !desc->matrix_c ||
        !desc->uniform_params || !desc->bindless_table || !cs) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->m == 0u || desc->n == 0u || desc->k == 0u ||
        desc->tile_m == 0u || desc->tile_n == 0u) {
        return A840_RT_INVALID_ARG;
    }

    memset(&cq, 0, sizeof(cq));
    cq.has_compiled_kernel = has_valid_compute_stage(&desc->gemm_pipe);
    cq.has_valid_local_size = cq.has_compiled_kernel;
    decision = a840_router_eval_compute(router, &cq);
    if (decision.path != A840_ROUTE_PATH_RAW) {
        if (out_stats) {
            memset(out_stats, 0, sizeof(*out_stats));
            out_stats->route = decision;
        }
        return A840_RT_FALLBACK_REQUIRED;
    }

    start_dwords = cs->used_dwords;
    start_relocs = cs->reloc_count;

    est = a840_emit_compute_pipeline(cs, &desc->gemm_pipe);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&bindless, 0, sizeof(bindless));
    bindless.count = 1u;
    bindless.tables[0].bo_handle = desc->bindless_table->bo_handle;
    bindless.tables[0].gpu_va = desc->bindless_table->gpu_va;
    est = a840_emit_compute_bindless(cs, &bindless);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&consts, 0, sizeof(consts));
    consts.source = A840_CONST_INDIRECT;
    consts.dst_vec4 = 0u;
    consts.count_vec4 = 2u; /* GEMM dimensions + alpha/beta + strides */
    consts.constlen_vec4 = desc->gemm_pipe.shader.stage->constlen_vec4
                               ? desc->gemm_pipe.shader.stage->constlen_vec4
                               : 2u;
    consts.address.bo_handle = desc->uniform_params->bo_handle;
    consts.address.gpu_va = desc->uniform_params->gpu_va;
    est = a840_emit_compute_consts(cs, &consts);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.stage = desc->gemm_pipe.shader.stage;
    dispatch.mode = A840_DISPATCH_UNIFORM;
    dispatch.size[0] = div_ceil_u32(desc->n, desc->tile_n);
    dispatch.size[1] = div_ceil_u32(desc->m, desc->tile_m);
    dispatch.size[2] = 1u;
    est = a840_emit_compute_dispatch(cs, &dispatch);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    est = a840_emit_cache_flush_inv(cs, A840_CACHE_UCHE_INV);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }
    est = a840_emit_wait_mem_writes(cs);
    if (est != A840_EMIT_OK) {
        goto fail_rollback;
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->route = decision;
        out_stats->emitted_dwords = cs->used_dwords - start_dwords;
        out_stats->emitted_relocs = cs->reloc_count - start_relocs;
        out_stats->tile_count = dispatch.size[0] * dispatch.size[1];
    }
    return A840_RT_OK;

fail_rollback:
    cs->used_dwords = start_dwords;
    cs->reloc_count = start_relocs;
    return a840_rt_status_from_emit(est);
}

/* ============================================================================
 * Mock / KMD Transport Submission Loop Helpers
 * ============================================================================ */

A840RtStatus a840_phase4_submit_cs(
    A840RtDevice *device,
    A840RtQueue *queue,
    const uint32_t *words,
    size_t dwords,
    A840RtBuffer *const *resident_buffers,
    size_t resident_count,
    uint32_t *out_submit_seq)
{
    A840RtBufferDesc ib_desc;
    A840RtBuffer *ib_bo = NULL;
    A840RtCmdBuffer *cb = NULL;
    A840RtBlitEncoder *enc = NULL;
    A840RtStatus st;
    size_t ib_bytes;
    size_t i;

    if (!device || !queue || !words || dwords == 0u) {
        return A840_RT_INVALID_ARG;
    }
    if (dwords > (SIZE_MAX / sizeof(uint32_t)) - 64u) {
        return A840_RT_RANGE_ERROR;
    }
    for (i = 0u; i < resident_count; ++i) {
        if (!resident_buffers || !resident_buffers[i] ||
            !a840_rt_device_owns_buffer(device, resident_buffers[i])) {
            return A840_RT_WRONG_DEVICE;
        }
    }

    /* Validate PM4 structural integrity before allocating/submitting IB. */
    {
        A840PM4Packet *pkts = (A840PM4Packet *)malloc(dwords * sizeof(*pkts));
        size_t pkt_count = 0u;
        A840EmitStatus est;
        if (!pkts) {
            return A840_RT_OOM;
        }
        est = a840_pm4_decode(words, dwords, pkts, dwords, &pkt_count);
        free(pkts);
        if (est != A840_EMIT_OK || pkt_count == 0u) {
            return a840_rt_status_from_emit(est == A840_EMIT_OK ? A840_EMIT_INVALID_ARG : est);
        }
    }

    ib_bytes = dwords * sizeof(uint32_t);
    memset(&ib_desc, 0, sizeof(ib_desc));
    ib_desc.size_bytes = ib_bytes < 4096u ? 4096u : ib_bytes;
    ib_desc.alignment = 4096u;
    ib_desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    st = a840_rt_buffer_create(device, &ib_desc, &ib_bo);
    if (st != A840_RT_OK) {
        return st;
    }
    st = a840_rt_buffer_write(ib_bo, 0u, words, ib_bytes);
    if (st != A840_RT_OK) {
        a840_rt_buffer_release(ib_bo);
        return st;
    }

    st = a840_rt_cb_create(queue, NULL, &cb);
    if (st != A840_RT_OK) {
        a840_rt_buffer_release(ib_bo);
        return st;
    }
    st = a840_rt_cb_begin_blit(cb, &enc);
    if (st != A840_RT_OK) {
        a840_rt_cb_release(cb);
        a840_rt_buffer_release(ib_bo);
        return st;
    }
    st = a840_rt_blit_barrier(enc, A840_CACHE_UCHE_INV, true, false);
    if (st == A840_RT_OK) {
        st = a840_rt_blit_end(enc);
    }
    a840_rt_blit_destroy(enc);
    if (st == A840_RT_OK) {
        st = a840_rt_cb_commit(cb);
    }
    if (st == A840_RT_OK) {
        st = a840_rt_cb_wait_until_completed(cb, UINT64_C(5000000000));
    }
    if (st == A840_RT_OK && out_submit_seq) {
        *out_submit_seq = (uint32_t)a840_rt_cb_submit_fence(cb);
    }

    a840_rt_cb_release(cb);
    a840_rt_buffer_release(ib_bo);
    return st;
}

A840RtStatus a840_phase4_submit_msaa_vis_pass(
    A840RtDevice *device,
    A840RtQueue *queue,
    const A840Phase4MsaaVisPassDesc *desc,
    A840Phase4PassStats *out_stats)
{
    uint32_t words[2048];
    A840Reloc relocs[128];
    A840CmdStream cs;
    A840Phase4PassStats stats;
    A840RtBuffer *residents[4];
    size_t res_count = 0u;
    A840RtStatus st;

    if (!device || !queue || !desc) {
        return A840_RT_INVALID_ARG;
    }
    if (a840_cs_init(&cs, words, 2048u, relocs, 128u) != A840_EMIT_OK) {
        return A840_RT_INVALID_ARG;
    }
    memset(&stats, 0, sizeof(stats));
    st = a840_phase4_encode_msaa_vis_pass(a840_rt_device_router(device), desc, &cs, &stats);
    if (st != A840_RT_OK) {
        if (out_stats) {
            *out_stats = stats;
        }
        return st;
    }

    if (desc->resolve_target && desc->resolve_target->backing_bo) {
        residents[res_count++] = desc->resolve_target->backing_bo;
    }
    if (desc->msaa_color_target && desc->msaa_color_target->backing_bo) {
        residents[res_count++] = desc->msaa_color_target->backing_bo;
    }
    if (desc->enable_visibility_query && desc->visibility_buffer) {
        residents[res_count++] = desc->visibility_buffer;
    }

    st = a840_phase4_submit_cs(device, queue, cs.words, cs.used_dwords,
                               residents, res_count, &stats.submit_seq);
    if (out_stats) {
        *out_stats = stats;
    }
    return st;
}

A840RtStatus a840_phase4_submit_tess_amp_pass(
    A840RtDevice *device,
    A840RtQueue *queue,
    const A840Phase4TessAmpPassDesc *desc,
    A840Phase4PassStats *out_stats)
{
    uint32_t words[2048];
    A840Reloc relocs[128];
    A840CmdStream cs;
    A840Phase4PassStats stats;
    A840RtBuffer *residents[2];
    size_t res_count = 0u;
    A840RtStatus st;

    if (!device || !queue || !desc) {
        return A840_RT_INVALID_ARG;
    }
    if (a840_cs_init(&cs, words, 2048u, relocs, 128u) != A840_EMIT_OK) {
        return A840_RT_INVALID_ARG;
    }
    memset(&stats, 0, sizeof(stats));
    st = a840_phase4_encode_tess_amp_pass(a840_rt_device_router(device), desc, &cs, &stats);
    if (st != A840_RT_OK) {
        if (out_stats) {
            *out_stats = stats;
        }
        return st;
    }

    if (desc->enable_tessellation && desc->tess_factor_buffer) {
        residents[res_count++] = desc->tess_factor_buffer;
    }

    st = a840_phase4_submit_cs(device, queue, cs.words, cs.used_dwords,
                               residents, res_count, &stats.submit_seq);
    if (out_stats) {
        *out_stats = stats;
    }
    return st;
}
