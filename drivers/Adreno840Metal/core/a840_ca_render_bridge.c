/* SPDX-License-Identifier: MIT */
#include "a840_ca_render_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

static A840RtStatus emit_reg_pkt(A840CmdStream *cs, uint32_t reg, uint32_t val)
{
    return a840_rt_status_from_emit(a840_cs_emit_pkt4(cs, reg, &val, 1u));
}

static A840RtStatus emit_memoryless_depth_state(A840CmdStream *cs,
                                                const A840CaGlassPassDesc *pass)
{
    uint32_t depth_info = A840_DEPTH_32 | (A840_TILE6_3 << A840_DEPTH_TILE_SHIFT);
    uint32_t depth_cntl = A840_DEPTH_CLAMP | A840_DEPTH_OUTPUT_CLAMP |
                          ((uint32_t)(pass->depth_compare & A840_DEPTH_FUNC_MAX) << A840_DEPTH_FUNC_SHIFT);
    A840RtStatus st;

    if (pass->depth_test) {
        depth_cntl |= A840_DEPTH_TEST;
        if (pass->depth_write) {
            depth_cntl |= A840_DEPTH_WRITE;
        }
        if (pass->depth_compare != 0u && pass->depth_compare != A840_DEPTH_FUNC_MAX) {
            depth_cntl |= A840_DEPTH_READ;
        }
    }

    /* Program on-chip GMEM depth format and test/write controls while keeping
     * RB_DEPTH_BUFFER_BASE = 0 (zero main-memory BO, zero Memoryless reloc). */
    st = emit_reg_pkt(cs, A840_REG_RB_DEPTH_BUFFER_INFO, depth_info);
    if (st != A840_RT_OK) {
        return st;
    }
    st = emit_reg_pkt(cs, A840_REG_GRAS_SU_DEPTH_BUFFER_INFO, A840_DEPTH_32);
    if (st != A840_RT_OK) {
        return st;
    }
    return emit_reg_pkt(cs, A840_REG_RB_DEPTH_CNTL, depth_cntl);
}

void a840_ca_bridge_enable_phase3_router(A840DualPathRouter *router)
{
    if (!router) {
        return;
    }
    a840_router_enable_category(router,
                                (uint32_t)A840_ROUTE_PHASE2_ALL |
                                (uint32_t)A840_ROUTE_PHASE3_ALL);
    a840_router_set_gmem_engine(router, true);
}

A840RouteDecision a840_ca_bridge_eval_frame_route(A840DualPathRouter *router,
                                                  const A840CaFrameDesc *frame)
{
    A840RenderRouteQuery rq;
    uint32_t i;

    memset(&rq, 0, sizeof(rq));
    if (!frame || !frame->color_target) {
        return a840_router_eval_render(router, NULL);
    }

    rq.color_attachment_count = 1u;
    rq.color_mtl_formats[0] = frame->color_target->mtl_pixel_format;
    rq.color_tile_modes[0] = frame->color_target->tile_mode;
    rq.sample_count = frame->color_target->samples ? frame->color_target->samples : 1u;

    if (frame->color_target->kind == A840_P3_TEX_YCBCR_MULTIPLANE) {
        rq.has_ycbcr_attachment = true;
    }
    if (frame->color_target->storage_mode == A840_STORAGE_MEMORYLESS) {
        rq.has_memoryless_attachment = true;
    }

    if (frame->depth_target) {
        rq.depth_mtl_format = frame->depth_target->mtl_pixel_format;
        rq.depth_tile_mode = frame->depth_target->tile_mode;
        if (frame->depth_target->storage_mode == A840_STORAGE_MEMORYLESS) {
            rq.has_memoryless_attachment = true;
        }
    }

    for (i = 0u; i < frame->pass_count && i < A840_CA_MAX_GLASS_PASSES; ++i) {
        if (frame->passes[i].uses_fbfetch) {
            rq.uses_fbfetch = true;
            rq.uses_programmable_blend = true;
        }
    }

    return a840_router_eval_render(router, &rq);
}

A840RtStatus a840_ca_bridge_encode_tile_draws(const A840CaFrameDesc *frame,
                                              A840CmdStream *draw_cs,
                                              A840CaFrameStats *out_stats)
{
    A840CaFrameStats local_stats;
    uint32_t i;
    size_t start_dwords;
    size_t start_relocs;

    if (!frame || !frame->color_target || !frame->pack_cache || !draw_cs ||
        frame->pass_count == 0u || frame->pass_count > A840_CA_MAX_GLASS_PASSES) {
        return A840_RT_INVALID_ARG;
    }
    if (frame->color_target->kind != A840_P3_TEX_UBWC6 || !frame->color_target->backing_bo) {
        return A840_RT_UNSUPPORTED;
    }
    if (frame->depth_target &&
        frame->depth_target->storage_mode == A840_STORAGE_MEMORYLESS &&
        (frame->depth_target->backing_bo != NULL ||
         frame->depth_target->data_range.bo != 0u ||
         frame->depth_target->data_range.va != 0u)) {
        return A840_RT_STATE_ERROR;
    }

    memset(&local_stats, 0, sizeof(local_stats));
    start_dwords = draw_cs->used_dwords;
    start_relocs = draw_cs->reloc_count;

    for (i = 0u; i < frame->pass_count; ++i) {
        const A840CaGlassPassDesc *pass = &frame->passes[i];
        A840PackLookupResult lookup;
        A840PipelineRecord record;
        A840GraphicsProgramDesc prog;
        A840RenderStateDesc rstate;
        A840DrawDesc draw;
        A840RtStatus st;
        A840EmitStatus est;
        uint32_t s;

        if (pass->vertex_count == 0u) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return A840_RT_INVALID_ARG;
        }

        /* On-chip FBFetch subpass dependency: flush CCU color + invalidate UCHE
         * WITHOUT issuing any CP_WAIT_FOR_IDLE (zero superfluous WFI). */
        if (i > 0u && pass->uses_fbfetch) {
            est = a840_emit_cache_flush_inv(
                draw_cs, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV);
            if (est != A840_EMIT_OK) {
                draw_cs->used_dwords = start_dwords;
                draw_cs->reloc_count = start_relocs;
                return a840_rt_status_from_emit(est);
            }
            local_stats.fbfetch_barriers_emitted++;
        }

        /* Zero-copy O(1) lookup: Tier-0 -> Tier-1. */
        st = a840_pack_cache_rt_lookup(frame->pack_cache, &pass->pipeline_key, &lookup);
        if (st != A840_RT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return st;
        }
        if (lookup.entry.stage_count != 2u) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return A840_RT_UNSUPPORTED;
        }
        if (lookup.hit_tier == A840_PACK_HIT_TIER0) {
            local_stats.tier0_hits++;
        } else {
            local_stats.tier1_hits++;
        }

        memset(&record, 0, sizeof(record));
        if (a840_pack_cache_rt_reconstruct_record(frame->pack_cache, &lookup, &record) != A840_RT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return A840_RT_STATE_ERROR;
        }

        memset(&prog, 0, sizeof(prog));
        st = a840_pack_cache_rt_make_shader_code(&lookup, 0u, &record.stages[0], &prog.vs);
        if (st == A840_RT_OK) {
            st = a840_pack_cache_rt_make_shader_code(&lookup, 1u, &record.stages[1], &prog.fs);
        }
        if (st != A840_RT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return st;
        }

        prog.vertex_id = A840_REGID_UNUSED;
        prog.instance_id = A840_REGID_UNUSED;
        prog.primitive_id = A840_REGID_UNUSED;
        prog.view_id = A840_REGID_UNUSED;
        prog.position = 0u;
        prog.point_size = A840_REGID_UNUSED;
        prog.fs_inputs.face = A840_REGID_UNUSED;
        prog.fs_inputs.sample_id = A840_REGID_UNUSED;
        prog.fs_inputs.sample_mask = A840_REGID_UNUSED;
        prog.fs_inputs.center_rhw = A840_REGID_UNUSED;
        prog.fs_inputs.persp_pixel = A840_REGID_UNUSED;
        prog.fs_inputs.linear_pixel = A840_REGID_UNUSED;
        prog.fs_inputs.persp_centroid = A840_REGID_UNUSED;
        prog.fs_inputs.linear_centroid = A840_REGID_UNUSED;
        prog.fs_inputs.persp_sample = A840_REGID_UNUSED;
        prog.fs_inputs.linear_sample = A840_REGID_UNUSED;
        prog.fs_inputs.frag_coord = A840_REGID_UNUSED;
        prog.fs_inputs.shading_rate = A840_REGID_UNUSED;
        prog.depth_output = A840_REGID_UNUSED;
        prog.sample_mask_output = A840_REGID_UNUSED;
        prog.stencil_output = A840_REGID_UNUSED;
        prog.colors[0].regid = 0u;
        prog.colors[0].component_mask = 15u;
        for (s = 1u; s < A840_MAX_COLOR_TARGETS; ++s) {
            prog.colors[s].regid = A840_REGID_UNUSED;
        }

        est = a840_emit_3d_program(draw_cs, &prog);
        if (est != A840_EMIT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return a840_rt_status_from_emit(est);
        }

        /* Optional pre-encoded SDS group directly from shared RO pack GPU VA. */
        if (pass->emit_sds) {
            uint32_t stage_idx = (lookup.stage_gpu[1].sds_gpu_va != 0u) ? 1u : 0u;
            if (lookup.stage_gpu[stage_idx].sds_gpu_va != 0u &&
                lookup.stage_gpu[stage_idx].sds_dwords > 0u) {
                A840SdsGroupDesc sds;
                st = a840_pack_cache_rt_make_sds_group(&lookup, stage_idx,
                                                       pass->sds_group_id,
                                                       A840_SDS_GMEM, &sds);
                if (st != A840_RT_OK) {
                    draw_cs->used_dwords = start_dwords;
                    draw_cs->reloc_count = start_relocs;
                    return st;
                }
                est = a840_emit_sds_group(draw_cs, &sds, 1u);
                if (est != A840_EMIT_OK) {
                    draw_cs->used_dwords = start_dwords;
                    draw_cs->reloc_count = start_relocs;
                    return a840_rt_status_from_emit(est);
                }
                local_stats.sds_groups_emitted++;
            }
        }

        memset(&rstate, 0, sizeof(rstate));
        rstate.viewport_count = 1u;
        rstate.scissor_count = 1u;
        rstate.viewports[0].width = (float)frame->color_target->width;
        rstate.viewports[0].height = (float)frame->color_target->height;
        rstate.viewports[0].min_depth = 0.0f;
        rstate.viewports[0].max_depth = 1.0f;
        rstate.scissors[0].width = frame->color_target->width;
        rstate.scissors[0].height = frame->color_target->height;
        rstate.line_width = 1.0f;
        rstate.sample_mask = 1u;
        rstate.depth_format = A840_DEPTH_NONE; /* Memoryless depth stays GMEM-only */
        rstate.colors[0].storage.data.bo_handle = frame->color_target->data_range.bo;
        rstate.colors[0].storage.data.gpu_va = frame->color_target->data_range.va;
        rstate.colors[0].storage.flag_buffer.bo_handle = frame->color_target->flag_range.bo;
        rstate.colors[0].storage.flag_buffer.gpu_va = frame->color_target->flag_range.va;
        rstate.colors[0].storage.pitch_bytes = frame->color_target->ubwc.data_pitch;
        rstate.colors[0].storage.layer_stride_bytes = frame->color_target->ubwc.data_slice_bytes;
        rstate.colors[0].storage.flag_pitch_bytes = frame->color_target->ubwc.flag_pitch;
        rstate.colors[0].storage.flag_layer_stride_bytes = frame->color_target->ubwc.flag_slice_bytes;
        rstate.colors[0].storage.tile_mode = A840_TILE_UBWC;
        rstate.colors[0].format = frame->color_target->format_caps->color_fmt_id;
        rstate.colors[0].swap = A840_WZYX;
        rstate.colors[0].srgb = frame->color_target->is_srgb;
        rstate.colors[0].blend.enabled = pass->blend_enabled;
        rstate.colors[0].blend.write_mask = 15u;
        rstate.colors[0].blend.src_rgb = A840_FACTOR_ONE;
        rstate.colors[0].blend.dst_rgb = pass->blend_enabled ? A840_FACTOR_ONE_MINUS_SRC_ALPHA : A840_FACTOR_ZERO;
        rstate.colors[0].blend.op_rgb = A840_BLEND_ADD;
        rstate.colors[0].blend.src_alpha = A840_FACTOR_ONE;
        rstate.colors[0].blend.dst_alpha = pass->blend_enabled ? A840_FACTOR_ONE_MINUS_SRC_ALPHA : A840_FACTOR_ZERO;
        rstate.colors[0].blend.op_alpha = A840_BLEND_ADD;

        est = a840_emit_3d_render_state(draw_cs, &rstate);
        if (est != A840_EMIT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return a840_rt_status_from_emit(est);
        }

        if (frame->depth_target &&
            frame->depth_target->storage_mode == A840_STORAGE_MEMORYLESS) {
            st = emit_memoryless_depth_state(draw_cs, pass);
            if (st != A840_RT_OK) {
                draw_cs->used_dwords = start_dwords;
                draw_cs->reloc_count = start_relocs;
                return st;
            }
        }

        memset(&draw, 0, sizeof(draw));
        draw.primitive = A840_PRIM_TRIANGLES;
        draw.index_type = A840_INDEX_NONE;
        draw.use_visibility = (frame->vsc != NULL);
        draw.count = pass->vertex_count;
        draw.instance_count = pass->instance_count ? pass->instance_count : 1u;

        est = a840_emit_3d_draw(draw_cs, &draw);
        if (est != A840_EMIT_OK) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return a840_rt_status_from_emit(est);
        }

        local_stats.passes_encoded++;
    }

    local_stats.draw_ib_dwords = (uint32_t)(draw_cs->used_dwords - start_dwords);
    local_stats.draw_ib_relocs = (uint32_t)(draw_cs->reloc_count - start_relocs);

    {
        uint32_t wfis = 0u, indirects = 0u, zero_bos = 0u;
        A840RtStatus ast = a840_ca_bridge_audit_pm4(
            draw_cs->words + start_dwords,
            draw_cs->used_dwords - start_dwords,
            draw_cs->relocs ? (draw_cs->relocs + start_relocs) : NULL,
            draw_cs->relocs ? (draw_cs->reloc_count - start_relocs) : 0u,
            &wfis, &indirects, &zero_bos);
        if (ast != A840_RT_OK || wfis != 0u) {
            draw_cs->used_dwords = start_dwords;
            draw_cs->reloc_count = start_relocs;
            return (ast != A840_RT_OK) ? ast : A840_RT_STATE_ERROR;
        }
        local_stats.draw_ib_wfi_count = wfis;
        local_stats.memoryless_reloc_count = 0u;
    }

    if (out_stats) {
        *out_stats = local_stats;
    }
    return A840_RT_OK;
}

A840RtStatus a840_ca_bridge_submit_frame(A840RtGmemQueue *gmem_q,
                                         const A840CaFrameDesc *frame,
                                         A840RtBuffer *main_ib,
                                         A840RtBuffer *draw_ib,
                                         A840Reloc *scratch,
                                         size_t scratch_cap,
                                         A840RtBuffer *const extra_resident[],
                                         size_t extra_count,
                                         A840CaFrameFlight *out_flight)
{
    A840DualPathRouter *router;
    A840RouteDecision decision;
    A840CmdStream draw_cs;
    A840Reloc draw_relocs[128];
    A840CaFrameStats stats;
    A840RtGmemPass gpass;
    A840RtBuffer *resident[A840_GMEM_MAX_RESIDENT];
    size_t res_count = 0u;
    size_t i;
    A840RtStatus st;
    A840EmitStatus est;

    if (!gmem_q || !gmem_q->device || !frame || !frame->color_target ||
        !frame->pack_cache || !main_ib || !draw_ib || !scratch ||
        !scratch_cap || !out_flight || (extra_count > 0u && !extra_resident)) {
        return A840_RT_INVALID_ARG;
    }
    if (!draw_ib->cpu_ptr || draw_ib->size_bytes < 4u ||
        (draw_ib->usage_flags & A840_BUF_USAGE_READ) == 0u) {
        return A840_RT_INVALID_ARG;
    }

    router = frame->router ? frame->router : a840_rt_device_router(gmem_q->device);
    decision = a840_ca_bridge_eval_frame_route(router, frame);
    if (decision.path != A840_ROUTE_PATH_RAW) {
        memset(out_flight, 0, sizeof(*out_flight));
        out_flight->stats.route_decision = decision;
        return A840_RT_FALLBACK_REQUIRED;
    }

    est = a840_cs_init(&draw_cs, (uint32_t *)draw_ib->cpu_ptr,
                       (size_t)(draw_ib->size_bytes / 4u),
                       draw_relocs, sizeof(draw_relocs) / sizeof(draw_relocs[0]));
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    memset(&stats, 0, sizeof(stats));
    st = a840_ca_bridge_encode_tile_draws(frame, &draw_cs, &stats);
    if (st != A840_RT_OK) {
        return st;
    }
    stats.route_decision = decision;

    memset(&gpass, 0, sizeof(gpass));
    gpass.desc.width = frame->color_target->width;
    gpass.desc.height = frame->color_target->height;
    gpass.desc.draw_count = frame->pass_count;
    for (i = 0u; i < frame->pass_count; ++i) {
        if (frame->passes[i].uses_fbfetch) {
            gpass.desc.uses_fbfetch = true;
        }
    }

    st = a840_ubwc_texture_fill_gmem_attachment(frame->color_target,
                                                frame->color_load,
                                                frame->color_store,
                                                &gpass.desc.attachments[0],
                                                &gpass.bindings[0]);
    if (st != A840_RT_OK) {
        return st;
    }
    memcpy(gpass.bindings[0].clear, frame->clear_color, sizeof(gpass.bindings[0].clear));

    if (frame->depth_target) {
        st = a840_ubwc_texture_fill_gmem_attachment(frame->depth_target,
                                                    A840_LOAD_CLEAR,
                                                    A840_STORE_DONT_CARE,
                                                    &gpass.desc.attachments[A840_GMEM_DEPTH],
                                                    &gpass.bindings[A840_GMEM_DEPTH]);
        if (st != A840_RT_OK) {
            return st;
        }
        gpass.bindings[A840_GMEM_DEPTH].clear[0] = frame->clear_depth;
    }

    est = a840_gmem_plan(&gpass.desc, &stats.gmem_plan);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    if (stats.gmem_plan.path != A840_PATH_GMEM) {
        return A840_RT_UNSUPPORTED;
    }

    gpass.vsc = frame->vsc;
    gpass.tile_draws.bo = draw_ib->bo_handle;
    gpass.tile_draws.va = draw_ib->gpu_va;
    gpass.tile_draws.bytes = (uint64_t)draw_cs.used_dwords * sizeof(uint32_t);

    /* Build deduplicated residency list (never includes any Memoryless BO). */
    resident[res_count++] = frame->color_target->backing_bo;
    if (frame->color_target->linear_resolve_bo && res_count < A840_GMEM_MAX_RESIDENT) {
        resident[res_count++] = frame->color_target->linear_resolve_bo;
    }
    if (res_count < A840_GMEM_MAX_RESIDENT) {
        resident[res_count++] = draw_ib;
    }
    for (i = 0u; i < extra_count; ++i) {
        bool dup = false;
        size_t j;
        if (!extra_resident[i] || extra_resident[i] == main_ib) {
            return A840_RT_INVALID_ARG;
        }
        for (j = 0u; j < res_count; ++j) {
            if (resident[j] == extra_resident[i]) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            if (res_count >= A840_GMEM_MAX_RESIDENT) {
                return A840_RT_CAPACITY;
            }
            resident[res_count++] = extra_resident[i];
        }
    }

    memset(out_flight, 0, sizeof(*out_flight));
    st = a840_rt_gmem_submit(gmem_q, &gpass, main_ib, scratch, scratch_cap,
                             resident, res_count, &out_flight->gmem_flight);
    if (st != A840_RT_OK) {
        return st;
    }
    out_flight->stats = stats;
    return A840_RT_OK;
}

A840RtStatus a840_ca_bridge_retire_frame(A840CaFrameFlight *flight,
                                         uint64_t timeout_ns)
{
    A840RtStatus st;
    if (!flight) {
        return A840_RT_INVALID_ARG;
    }
    st = a840_rt_gmem_retire(&flight->gmem_flight, timeout_ns);
    if (st == A840_RT_OK) {
        memset(&flight->gmem_flight, 0, sizeof(flight->gmem_flight));
    }
    return st;
}

A840RtStatus a840_ca_bridge_audit_pm4(const uint32_t *words,
                                      size_t dwords,
                                      const A840Reloc *relocs,
                                      size_t reloc_count,
                                      uint32_t *out_wfi_count,
                                      uint32_t *out_indirect_count,
                                      uint32_t *out_zero_bo_relocs)
{
    A840PM4Packet stack_pkts[512];
    A840PM4Packet *pkts = stack_pkts;
    size_t cap = sizeof(stack_pkts) / sizeof(stack_pkts[0]);
    size_t count = 0u;
    uint32_t wfis = 0u;
    uint32_t indirects = 0u;
    uint32_t zero_bos = 0u;
    A840EmitStatus est;
    size_t i;

    if (!words || dwords == 0u) {
        return A840_RT_INVALID_ARG;
    }
    if (dwords > cap) {
        pkts = (A840PM4Packet *)malloc(dwords * sizeof(*pkts));
        if (!pkts) {
            return A840_RT_OOM;
        }
        cap = dwords;
    }
    est = a840_pm4_decode(words, dwords, pkts, cap, &count);
    if (est != A840_EMIT_OK) {
        if (pkts != stack_pkts) {
            free(pkts);
        }
        return a840_rt_status_from_emit(est);
    }

    for (i = 0u; i < count; ++i) {
        if (pkts[i].type == 7u) {
            if (pkts[i].identifier == A840_CP_WAIT_FOR_IDLE) {
                wfis++;
            } else if (pkts[i].identifier == A840_CP_INDIRECT_BUFFER) {
                indirects++;
            }
        }
    }
    if (pkts != stack_pkts) {
        free(pkts);
    }

    if (relocs) {
        for (i = 0u; i < reloc_count; ++i) {
            if (relocs[i].bo_handle == 0u) {
                zero_bos++;
            }
        }
    }

    if (out_wfi_count) {
        *out_wfi_count = wfis;
    }
    if (out_indirect_count) {
        *out_indirect_count = indirects;
    }
    if (out_zero_bo_relocs) {
        *out_zero_bo_relocs = zero_bos;
    }
    return A840_RT_OK;
}
