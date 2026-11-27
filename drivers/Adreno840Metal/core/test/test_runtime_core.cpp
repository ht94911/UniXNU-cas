/* SPDX-License-Identifier: MIT */
#include "a840_backend_transport.h"
#include "a840_dualpath_router.h"
#include "a840_runtime_device.h"
#include "a840_runtime_cmd.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static unsigned g_cases = 0;
static unsigned g_checks = 0;
static const char *g_active_case = "";

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_active_case, #expr); \
        std::exit(1); \
    } \
} while (0)

static void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

/* ---------- Helper builders ---------- */

static A840CompiledStage make_stage(uint32_t stage_kind, uint32_t instrlen = 4, uint32_t priv_bytes = 0)
{
    A840CompiledStage s{};
    s.abi_version = ADRENO_COMPILE_ABI_VERSION;
    s.stage = stage_kind;
    s.instruction_groups = instrlen * 8u;
    s.isa_dwords = instrlen * 32u;
    s.max_full_reg = 7;
    s.max_half_reg = 3;
    s.branchstack = 2;
    s.constlen_vec4 = 128u;
    s.private_mem_bytes = priv_bytes;
    s.shared_mem_bytes = (stage_kind == A840_STAGE_COMPUTE) ? 4096u : 0u;
    s.flags_out = (1u << 3) | (1u << 4); /* merged_regs | early_preamble */
    s.local_size[0] = 8u;
    s.local_size[1] = 4u;
    s.local_size[2] = 1u;
    return s;
}

static A840RtBuffer *create_buf(A840RtDevice *dev, uint64_t size, uint64_t align = 4096u,
                                uint32_t usage = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE)
{
    A840RtBufferDesc desc{};
    desc.size_bytes = size;
    desc.alignment = align;
    desc.usage_flags = usage;
    A840RtBuffer *buf = nullptr;
    CHECK(a840_rt_buffer_create(dev, &desc, &buf) == A840_RT_OK);
    CHECK(buf != nullptr);
    return buf;
}

static A840RtTexture *create_tex2d(A840RtDevice *dev, uint32_t mtl_fmt, uint32_t w, uint32_t h,
                                   uint32_t tile_mode = A840_TILE_LINEAR,
                                   uint32_t usage = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET)
{
    A840RtTextureDesc desc{};
    desc.mtl_pixel_format = mtl_fmt;
    desc.tex_type = A840_TEX_2D;
    desc.width = w;
    desc.height = h;
    desc.depth = 1u;
    desc.array_layers = 1u;
    desc.mip_levels = 1u;
    desc.samples = 1u;
    desc.tile_mode = tile_mode;
    desc.usage_flags = usage;
    A840RtTexture *tex = nullptr;
    CHECK(a840_rt_texture_create(dev, &desc, &tex) == A840_RT_OK);
    CHECK(tex != nullptr);
    return tex;
}

static A840RtGraphicsProgramDesc make_graphics_prog(const A840CompiledStage &vs,
                                                    const A840CompiledStage &fs,
                                                    A840RtBuffer *vs_buf,
                                                    A840RtBuffer *fs_buf)
{
    A840RtGraphicsProgramDesc prog{};
    prog.emit_desc.vs.stage = &vs;
    prog.emit_desc.fs.stage = &fs;
    prog.emit_desc.vs.bindless_tex = true;
    prog.emit_desc.fs.bindless_tex = true;
    prog.emit_desc.fs.bindless_samp = true;
    prog.emit_desc.vertex_input_count = 2u;
    prog.emit_desc.vertex_inputs[0] = {0, 0x0f};
    prog.emit_desc.vertex_inputs[1] = {4, 0x0f};
    prog.emit_desc.vertex_id = 8;
    prog.emit_desc.instance_id = A840_REGID_UNUSED;
    prog.emit_desc.primitive_id = A840_REGID_UNUSED;
    prog.emit_desc.view_id = A840_REGID_UNUSED;
    prog.emit_desc.position = 0;
    prog.emit_desc.point_size = A840_REGID_UNUSED;
    prog.emit_desc.varying_count = 1u;
    prog.emit_desc.varyings[0] = {4, 0x0f, false, 0u, A840_VARYING_NORMAL, false};
    prog.emit_desc.fs_inputs = {
        A840_REGID_UNUSED, A840_REGID_UNUSED, A840_REGID_UNUSED, A840_REGID_UNUSED,
        0, A840_REGID_UNUSED, A840_REGID_UNUSED, A840_REGID_UNUSED,
        A840_REGID_UNUSED, A840_REGID_UNUSED, A840_REGID_UNUSED, A840_REGID_UNUSED,
        0
    };
    prog.emit_desc.depth_output = A840_REGID_UNUSED;
    prog.emit_desc.sample_mask_output = A840_REGID_UNUSED;
    prog.emit_desc.stencil_output = A840_REGID_UNUSED;
    prog.emit_desc.colors[0] = {0, 0x0f, false};
    for (uint32_t i = 1u; i < A840_MAX_COLOR_TARGETS; ++i) {
        prog.emit_desc.colors[i] = {A840_REGID_UNUSED, 0, false};
    }
    prog.emit_desc.pixel_lod = true;
    prog.emit_desc.full_quad = true;
    prog.emit_desc.fs_thread128 = true;
    prog.vs_code_buffer = vs_buf;
    prog.fs_code_buffer = fs_buf;
    return prog;
}

static A840RtRenderStateDesc make_render_state(uint32_t w, uint32_t h, bool has_depth = false, bool has_stencil = false)
{
    A840RtRenderStateDesc st{};
    st.viewport_count = 1u;
    st.scissor_count = 1u;
    st.viewports[0] = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    st.scissors[0] = {0u, 0u, w, h};
    st.line_width = 1.0f;
    st.cull_back = true;
    st.front_clockwise = false;
    st.depth_test = has_depth;
    st.depth_write = has_depth;
    st.depth_clamp = false;
    st.stencil_test = has_stencil;
    st.depth_compare = 1u;
    st.front = {7u, 0u, 0u, 0u, 1u, 0xffu, 0xffu};
    st.back = {7u, 0u, 0u, 0u, 1u, 0xffu, 0xffu};
    st.sample_mask = UINT16_MAX;
    st.blend_color[3] = 1.0f;
    st.color_blends[0] = {
        true, 0x0fu,
        A840_FACTOR_SRC_ALPHA, A840_FACTOR_ONE_MINUS_SRC_ALPHA, A840_BLEND_ADD,
        A840_FACTOR_ONE, A840_FACTOR_ONE_MINUS_SRC_ALPHA, A840_BLEND_ADD
    };
    st.color_blends[1] = {
        false, 0x0fu,
        A840_FACTOR_ONE, A840_FACTOR_ZERO, A840_BLEND_ADD,
        A840_FACTOR_ONE, A840_FACTOR_ZERO, A840_BLEND_ADD
    };
    return st;
}

static std::vector<A840PM4Packet> decode_all_packets(const uint32_t *words, size_t count)
{
    std::vector<A840PM4Packet> out(count > 0 ? count : 1);
    size_t decoded = 0;
    CHECK(a840_pm4_decode(words, count, out.data(), out.size(), &decoded) == A840_EMIT_OK);
    out.resize(decoded);
    return out;
}

/* =========================================================================
 * Group A: DualPathRouter & All 16 DualPathPredicates.tsv Predicates
 * ========================================================================= */

static void test_group_a_router()
{
    begin_case("router_all_16_predicates_exact_strings");
    {
        const char *expected[16] = {
            "raw:unimplemented.global",
            "raw:blit.unsupported_format",
            "raw:blit.3d_or_compressed",
            "raw:compute.uncompiled_kernel",
            "raw:compute.indirect_dispatch",
            "raw:render.gmem_required",
            "raw:render.ubwc_attachment",
            "raw:render.ycbcr_or_xr10",
            "raw:render.fbfetch_blend",
            "raw:render.tess_or_amplify",
            "raw:render.indirect_or_icb",
            "raw:render.msaa_resolve",
            "raw:resource.heap_aliasing",
            "raw:resource.sparse_texture",
            "raw:compiler.daemon_unavailable",
            "raw:sync.shared_event_cross_proc"
        };
        CHECK(A840_PRED_COUNT == 16);
        CHECK(a840_router_predicate_name(A840_PRED_NONE) == nullptr);
        CHECK(a840_router_lookup_predicate(nullptr) == A840_PRED_NONE);
        CHECK(a840_router_lookup_predicate("raw:nonexistent") == A840_PRED_NONE);
        for (int i = 1; i <= A840_PRED_COUNT; ++i) {
            const char *name = a840_router_predicate_name(static_cast<A840FallbackPredicate>(i));
            CHECK(name != nullptr);
            CHECK(std::strcmp(name, expected[i - 1]) == 0);
            CHECK(a840_router_lookup_predicate(name) == static_cast<A840FallbackPredicate>(i));
        }

        /* Cross-check directly against contracts/DualPathPredicates.tsv if accessible from cwd. */
        std::ifstream tsv("contracts/DualPathPredicates.tsv");
        if (tsv.is_open()) {
            std::string line;
            CHECK(static_cast<bool>(std::getline(tsv, line))); /* header */
            size_t row_idx = 0;
            while (std::getline(tsv, line)) {
                if (line.empty()) continue;
                size_t tab = line.find('\t');
                CHECK(tab != std::string::npos);
                std::string pred = line.substr(0, tab);
                CHECK(row_idx < 16);
                CHECK(pred == expected[row_idx]);
                CHECK(a840_router_lookup_predicate(pred.c_str()) ==
                      static_cast<A840FallbackPredicate>(row_idx + 1));
                ++row_idx;
            }
            CHECK(row_idx == 16);
        }
    }

    begin_case("router_all_off_returns_unimplemented_global");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_NONE);

        A840BlitRouteQuery bq{};
        bq.is_buffer_copy = true;
        auto db = a840_router_eval_blit(&r, &bq);
        CHECK(db.path == A840_ROUTE_PATH_FALLBACK);
        CHECK(db.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
        CHECK(std::strcmp(db.predicate, "raw:unimplemented.global") == 0);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        auto dc = a840_router_eval_compute(&r, &cq);
        CHECK(dc.path == A840_ROUTE_PATH_FALLBACK);
        CHECK(dc.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = 70u;
        rq.color_tile_modes[0] = A840_TILE_LINEAR;
        rq.sample_count = 1u;
        auto dr = a840_router_eval_render(&r, &rq);
        CHECK(dr.path == A840_ROUTE_PATH_FALLBACK);
        CHECK(dr.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        A840ResourceRouteQuery resq{};
        auto dres = a840_router_eval_resource(&r, &resq);
        CHECK(dres.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        A840CompilerRouteQuery compq{};
        compq.precompiled_available = true;
        auto dcomp = a840_router_eval_compiler(&r, &compq);
        CHECK(dcomp.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        A840SyncRouteQuery sq{};
        auto ds = a840_router_eval_sync(&r, &sq);
        CHECK(ds.predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        CHECK(r.raw_hits == 0u);
        CHECK(r.fallback_total == 6u);
        CHECK(a840_router_predicate_count(&r, A840_PRED_UNIMPLEMENTED_GLOBAL) == 6u);
    }

    begin_case("router_enable_blit_only_isolation");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_NONE);
        a840_router_enable_category(&r, A840_ROUTE_BLIT);

        A840BlitRouteQuery bq{};
        bq.is_clear = true;
        bq.dst_mtl_format = 80u; /* BGRA8Unorm */
        bq.tex_type = A840_TEX_2D;
        bq.dst_tile_mode = A840_TILE_TILED;
        CHECK(a840_router_eval_blit(&r, &bq).path == A840_ROUTE_PATH_RAW);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = 80u;
        CHECK(a840_router_eval_render(&r, &rq).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);

        a840_router_disable_category(&r, A840_ROUTE_BLIT);
        CHECK(a840_router_eval_blit(&r, &bq).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
    }

    begin_case("router_enable_compute_only_isolation");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_COMPUTE);
        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        CHECK(a840_router_eval_compute(&r, &cq).path == A840_ROUTE_PATH_RAW);

        A840BlitRouteQuery bq{};
        bq.is_buffer_copy = true;
        CHECK(a840_router_eval_blit(&r, &bq).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
    }

    begin_case("router_enable_render_only_isolation");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = 70u; /* RGBA8Unorm */
        rq.color_tile_modes[0] = A840_TILE_TILED;
        rq.sample_count = 1u;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
    }

    begin_case("router_blit_unsupported_format_unknown");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_BLIT);
        A840BlitRouteQuery q{};
        q.is_clear = true;
        q.dst_mtl_format = 9999u;
        q.tex_type = A840_TEX_2D;
        auto d = a840_router_eval_blit(&r, &q);
        CHECK(d.path == A840_ROUTE_PATH_FALLBACK);
        CHECK(d.predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);
        CHECK(std::strcmp(d.predicate, "raw:blit.unsupported_format") == 0);
    }

    begin_case("router_blit_unsupported_format_xr_depth_and_filter");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_BLIT);

        A840BlitRouteQuery q{};
        q.is_clear = true;
        q.tex_type = A840_TEX_2D;
        q.dst_mtl_format = 554u; /* BGR10_XR */
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);

        q.dst_mtl_format = 252u; /* Depth32Float */
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);

        q.dst_mtl_format = 90u; /* RGB10A2Unorm (not in R2D color_formats) */
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);

        q.is_clear = false;
        q.src_mtl_format = 73u; /* RGBA8Uint */
        q.dst_mtl_format = 73u;
        q.filter = A840_FILTER_LINEAR; /* linear filter unsupported on uint */
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);

        q.filter = A840_FILTER_NEAREST;
        CHECK(a840_router_eval_blit(&r, &q).path == A840_ROUTE_PATH_RAW);
        CHECK(a840_router_predicate_count(&r, A840_PRED_BLIT_UNSUPPORTED_FORMAT) == 4u);
    }

    begin_case("router_blit_3d_texture_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_BLIT);
        A840BlitRouteQuery q{};
        q.is_clear = true;
        q.dst_mtl_format = 70u;
        q.tex_type = A840_TEX_3D;
        auto d = a840_router_eval_blit(&r, &q);
        CHECK(d.predicate_id == A840_PRED_BLIT_3D_OR_COMPRESSED);
        CHECK(std::strcmp(d.predicate, "raw:blit.3d_or_compressed") == 0);
    }

    begin_case("router_blit_ubwc_and_astc_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_BLIT);

        A840BlitRouteQuery q{};
        q.tex_type = A840_TEX_2D;
        q.src_mtl_format = 70u;
        q.dst_mtl_format = 70u;
        q.dst_tile_mode = A840_TILE_UBWC;
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_3D_OR_COMPRESSED);

        q.dst_tile_mode = A840_TILE_LINEAR;
        q.src_mtl_format = 204u; /* ASTC_4x4_LDR */
        CHECK(a840_router_eval_blit(&r, &q).predicate_id == A840_PRED_BLIT_3D_OR_COMPRESSED);
        CHECK(a840_router_predicate_count(&r, A840_PRED_BLIT_3D_OR_COMPRESSED) == 2u);
    }

    begin_case("router_compute_uncompiled_kernel_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_COMPUTE);
        A840ComputeRouteQuery q{};
        q.has_compiled_kernel = false;
        q.has_valid_local_size = true;
        auto d = a840_router_eval_compute(&r, &q);
        CHECK(d.predicate_id == A840_PRED_COMPUTE_UNCOMPILED_KERNEL);
        CHECK(std::strcmp(d.predicate, "raw:compute.uncompiled_kernel") == 0);

        q.has_compiled_kernel = true;
        q.has_valid_local_size = false;
        CHECK(a840_router_eval_compute(&r, &q).predicate_id == A840_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }

    begin_case("router_compute_indirect_dispatch_and_icb");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_COMPUTE);
        A840ComputeRouteQuery q{};
        q.has_compiled_kernel = true;
        q.has_valid_local_size = true;
        q.is_indirect_dispatch = true;
        auto d = a840_router_eval_compute(&r, &q);
        CHECK(d.predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);
        CHECK(std::strcmp(d.predicate, "raw:compute.indirect_dispatch") == 0);

        q.is_indirect_dispatch = false;
        q.uses_icb = true;
        CHECK(a840_router_eval_compute(&r, &q).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);
    }

    begin_case("router_render_memoryless_gmem_required");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.has_memoryless_attachment = true;
        auto d = a840_router_eval_render(&r, &q);
        CHECK(d.predicate_id == A840_PRED_RENDER_GMEM_REQUIRED);
        CHECK(std::strcmp(d.predicate, "raw:render.gmem_required") == 0);
    }

    begin_case("router_render_fbfetch_phase2_gmem_required");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.uses_fbfetch = true;
        auto d = a840_router_eval_render(&r, &q);
        CHECK(d.predicate_id == A840_PRED_RENDER_GMEM_REQUIRED);
    }

    begin_case("router_render_msaa_resolve_phase2_gmem_required");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.sample_count = 4u;
        q.has_msaa_resolve = true;
        auto d = a840_router_eval_render(&r, &q);
        CHECK(d.predicate_id == A840_PRED_RENDER_GMEM_REQUIRED);
    }

    begin_case("router_render_ubwc_attachment_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 80u;
        q.color_tile_modes[0] = A840_TILE_UBWC;
        auto d = a840_router_eval_render(&r, &q);
        CHECK(d.predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
        CHECK(std::strcmp(d.predicate, "raw:render.ubwc_attachment") == 0);

        q.color_tile_modes[0] = A840_TILE_TILED;
        q.depth_mtl_format = 252u;
        q.depth_tile_mode = A840_TILE_UBWC;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
    }

    begin_case("router_render_ycbcr_and_xr10_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        const uint32_t xr_fmts[] = {552u, 553u, 554u, 555u};
        for (uint32_t fmt : xr_fmts) {
            A840RenderRouteQuery q{};
            q.color_attachment_count = 1u;
            q.color_mtl_formats[0] = fmt;
            auto d = a840_router_eval_render(&r, &q);
            CHECK(d.predicate_id == A840_PRED_RENDER_YCBCR_OR_XR10);
            CHECK(std::strcmp(d.predicate, "raw:render.ycbcr_or_xr10") == 0);
        }
        A840RenderRouteQuery yq{};
        yq.has_ycbcr_attachment = true;
        CHECK(a840_router_eval_render(&r, &yq).predicate_id == A840_PRED_RENDER_YCBCR_OR_XR10);
        CHECK(a840_router_predicate_count(&r, A840_PRED_RENDER_YCBCR_OR_XR10) == 5u);
    }

    begin_case("router_render_fbfetch_blend_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.uses_programmable_blend = true;
        auto d = a840_router_eval_render(&r, &q);
        CHECK(d.predicate_id == A840_PRED_RENDER_FBFETCH_BLEND);
        CHECK(std::strcmp(d.predicate, "raw:render.fbfetch_blend") == 0);

        a840_router_set_gmem_engine(&r, true);
        q.uses_programmable_blend = false;
        q.uses_fbfetch = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_FBFETCH_BLEND);
    }

    begin_case("router_render_tess_and_amplify_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);

        q.uses_tessellation = false;
        q.uses_vertex_amplification = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);
    }

    begin_case("router_render_indirect_and_icb_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.uses_indirect_draw = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);

        q.uses_indirect_draw = false;
        q.uses_icb = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);
    }

    begin_case("router_render_msaa_resolve_predicate");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = 70u;
        q.sample_count = 4u;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_MSAA_RESOLVE);

        a840_router_set_gmem_engine(&r, true);
        q.sample_count = 1u;
        q.has_msaa_resolve = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_MSAA_RESOLVE);
    }

    begin_case("router_resource_compiler_sync_predicates");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_ALL);

        A840ResourceRouteQuery rq{};
        rq.uses_heap_aliasing = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_HEAP_ALIASING);
        rq.uses_sparse_texture = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_SPARSE_TEXTURE);
        rq.uses_heap_aliasing = false;
        rq.uses_sparse_texture = false;
        CHECK(a840_router_eval_resource(&r, &rq).path == A840_ROUTE_PATH_RAW);

        A840CompilerRouteQuery cq{};
        cq.precompiled_available = false;
        cq.daemon_available = false;
        CHECK(a840_router_eval_compiler(&r, &cq).predicate_id == A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
        cq.precompiled_available = true;
        CHECK(a840_router_eval_compiler(&r, &cq).path == A840_ROUTE_PATH_RAW);

        A840SyncRouteQuery sq{};
        sq.cross_process_shared_event = true;
        sq.abi_v3_supported = false;
        CHECK(a840_router_eval_sync(&r, &sq).predicate_id == A840_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
        sq.abi_v3_supported = true;
        CHECK(a840_router_eval_sync(&r, &sq).path == A840_ROUTE_PATH_RAW);
    }

    begin_case("router_format_decision_and_reset_stats");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_PHASE2_ALL);
        A840BlitRouteQuery bq{};
        bq.is_buffer_copy = true;
        auto raw_d = a840_router_eval_blit(&r, &bq);
        char buf[128]{};
        CHECK(a840_router_format_decision(&raw_d, buf, sizeof(buf)) > 0);
        CHECK(std::strcmp(buf, "path=raw") == 0);

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = 554u;
        auto fb_d = a840_router_eval_render(&r, &rq);
        CHECK(a840_router_format_decision(&fb_d, buf, sizeof(buf)) > 0);
        CHECK(std::strcmp(buf, "fallback:raw:render.ycbcr_or_xr10") == 0);

        CHECK(r.raw_hits == 1u);
        CHECK(r.fallback_total == 1u);
        a840_router_reset_stats(&r);
        CHECK(r.raw_hits == 0u);
        CHECK(r.fallback_total == 0u);
        CHECK(a840_router_predicate_count(&r, A840_PRED_RENDER_YCBCR_OR_XR10) == 0u);
    }
}

/* =========================================================================
 * Group B: Mock Transport & NativeAdrenoABI v2/v3 Verification
 * ========================================================================= */

static void test_group_b_transport()
{
    begin_case("transport_caps_kaanapali_a840_facts");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        A840BackendTransport t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);
        CHECK(caps.abi_version == NativeAdrenoAbiVersionV3);
        CHECK(caps.chip_id == static_cast<uint32_t>(A840_KAANAPALI_CHIP_ID));
        CHECK(caps.highest_bank_bit == A840_KAANAPALI_HIGHEST_BANK_BIT);
        CHECK(caps.gmem_bytes == A840_KAANAPALI_GMEM_BYTES);
        CHECK(caps.ubwc_version == 6u);
        CHECK(caps.ubwc_swizzle == 6u);
        CHECK(caps.macrotile_mode == 1u);
        CHECK(caps.input_bits == 48u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_bo_alloc_map_unmap_free_lifecycle");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        A840BoAllocDesc ad{8192u, 12u, 0u};
        uint64_t bo = 0;
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &bo) == A840_TRANSPORT_OK);
        CHECK(bo != 0);
        CHECK(a840_mock_transport_live_bo_count(mock) == 1u);
        CHECK(a840_mock_transport_is_bo_live(mock, bo));

        A840BoMapDesc md{};
        md.handle = bo;
        md.gpu_va = caps.va_start;
        md.flags = A840_BO_MAP_GPU | A840_BO_MAP_CPU;
        md.permissions = A840_BO_PERM_READ | A840_BO_PERM_WRITE;
        md.shareability = A840_BO_SHARE_INNER;
        A840BoMapReply mr{};
        CHECK(t.ops->bo_map(t.ctx, &md, &mr) == A840_TRANSPORT_OK);
        CHECK(mr.gpu_va == caps.va_start);
        CHECK(mr.cpu_ptr != nullptr);
        CHECK(mr.cpu_token != 0u);
        CHECK(mr.bytes == 8192u);

        A840BoUnmapDesc bad_unmap{bo, A840_BO_UNMAP_CPU, mr.cpu_token + 99u};
        CHECK(t.ops->bo_unmap(t.ctx, &bad_unmap) == A840_TRANSPORT_STATE_ERROR);

        A840BoUnmapDesc ok_unmap{bo, A840_BO_UNMAP_GPU | A840_BO_UNMAP_CPU, mr.cpu_token};
        CHECK(t.ops->bo_unmap(t.ctx, &ok_unmap) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_OK);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        CHECK(!a840_mock_transport_is_bo_live(mock, bo));
        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_DANGLING_BO);

        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_bo_busy_if_cpu_still_mapped");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840BoAllocDesc ad{4096u, 12u, 0u};
        uint64_t bo = 0;
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &bo) == A840_TRANSPORT_OK);
        A840BoMapDesc md{bo, 0u, A840_BO_MAP_CPU, 0u, 0u, 0u};
        A840BoMapReply mr{};
        CHECK(t.ops->bo_map(t.ctx, &md, &mr) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_BUSY);
        A840BoUnmapDesc ud{bo, A840_BO_UNMAP_CPU, mr.cpu_token};
        CHECK(t.ops->bo_unmap(t.ctx, &ud) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_bo_import_v2_and_no_cpu_token");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        alignas(4096) std::array<uint8_t, 8192> host_pages{};
        A840BoImportDesc bad_imp{host_pages.data() + 16, 4096u};
        uint64_t bo = 0;
        CHECK(t.ops->bo_import(t.ctx, &bad_imp, &bo) == A840_TRANSPORT_INVALID_ARG);

        A840BoImportDesc imp{host_pages.data(), 8192u};
        CHECK(t.ops->bo_import(t.ctx, &imp, &bo) == A840_TRANSPORT_OK);

        A840BoMapDesc cpu_md{bo, caps.va_start, A840_BO_MAP_GPU | A840_BO_MAP_CPU,
                             A840_BO_PERM_READ, 0u, A840_BO_SHARE_INNER};
        A840BoMapReply mr{};
        CHECK(t.ops->bo_map(t.ctx, &cpu_md, &mr) == A840_TRANSPORT_UNSUPPORTED);

        A840BoMapDesc gpu_md{bo, caps.va_start, A840_BO_MAP_GPU,
                             A840_BO_PERM_READ, 0u, A840_BO_SHARE_INNER};
        CHECK(t.ops->bo_map(t.ctx, &gpu_md, &mr) == A840_TRANSPORT_OK);
        CHECK(mr.cpu_ptr == nullptr);
        CHECK(mr.cpu_token == 0u);
        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_dangling_bo_ib_and_reloc_rejected");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        uint64_t q = 0;
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_TRANSPORT_OK);

        uint64_t ib_bo = 0, target_bo = 0;
        A840BoAllocDesc ad{4096u, 12u, 0u};
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &ib_bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &target_bo) == A840_TRANSPORT_OK);

        A840BoMapDesc m1{ib_bo, caps.va_start, A840_BO_MAP_GPU | A840_BO_MAP_CPU,
                         A840_BO_PERM_READ, 0u, A840_BO_SHARE_INNER};
        A840BoMapDesc m2{target_bo, caps.va_start + 4096u, A840_BO_MAP_GPU,
                         A840_BO_PERM_READ | A840_BO_PERM_WRITE, 0u, A840_BO_SHARE_INNER};
        A840BoMapReply r1{}, r2{};
        CHECK(t.ops->bo_map(t.ctx, &m1, &r1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_map(t.ctx, &m2, &r2) == A840_TRANSPORT_OK);

        /* Free target_bo to create a dangling relocation handle. */
        CHECK(t.ops->bo_free(t.ctx, target_bo) == A840_TRANSPORT_OK);

        A840SubmitIb ib{ib_bo, r1.gpu_va, 16u};
        A840Reloc dangling_reloc{1u, A840_RELOC_WRITE, target_bo, r2.gpu_va};
        A840SubmitDesc sub{q, 0u, 0u, &ib, 1u, &dangling_reloc, 1u};
        A840SubmitReply srep{};
        CHECK(t.ops->queue_submit(t.ctx, &sub, &srep) == A840_TRANSPORT_DANGLING_BO);
        CHECK(a840_mock_transport_submission_count(mock) == 0u);

        /* Also reject dangling IB BO handle. */
        A840BoUnmapDesc u1{ib_bo, A840_BO_UNMAP_CPU | A840_BO_UNMAP_GPU, r1.cpu_token};
        CHECK(t.ops->bo_unmap(t.ctx, &u1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, ib_bo) == A840_TRANSPORT_OK);
        sub.relocs = nullptr;
        sub.reloc_count = 0u;
        CHECK(t.ops->queue_submit(t.ctx, &sub, &srep) == A840_TRANSPORT_DANGLING_BO);
        CHECK(a840_mock_transport_submission_count(mock) == 0u);

        CHECK(t.ops->queue_destroy(t.ctx, q) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_oob_ib_and_oob_reloc_rejected");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        uint64_t q = 0, ib_bo = 0, ro_bo = 0;
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_TRANSPORT_OK);
        A840BoAllocDesc ad{4096u, 12u, 0u};
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &ib_bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &ro_bo) == A840_TRANSPORT_OK);
        A840BoMapDesc m1{ib_bo, caps.va_start, A840_BO_MAP_GPU, A840_BO_PERM_READ, 0u, 3u};
        A840BoMapDesc m2{ro_bo, caps.va_start + 4096u, A840_BO_MAP_GPU, A840_BO_PERM_READ, 0u, 3u};
        A840BoMapReply r1{}, r2{};
        CHECK(t.ops->bo_map(t.ctx, &m1, &r1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_map(t.ctx, &m2, &r2) == A840_TRANSPORT_OK);

        A840SubmitReply rep{};
        A840SubmitIb oob_ib{ib_bo, r1.gpu_va + 4092u, 8u};
        A840SubmitDesc sub{q, 0u, 0u, &oob_ib, 1u, nullptr, 0u};
        CHECK(t.ops->queue_submit(t.ctx, &sub, &rep) == A840_TRANSPORT_OUT_OF_BOUNDS);

        A840SubmitIb unaligned_ib{ib_bo, r1.gpu_va, 6u};
        sub.ibs = &unaligned_ib;
        CHECK(t.ops->queue_submit(t.ctx, &sub, &rep) == A840_TRANSPORT_OUT_OF_BOUNDS);

        A840SubmitIb ok_ib{ib_bo, r1.gpu_va, 16u};
        A840Reloc oob_rel{1u, A840_RELOC_READ, ro_bo, r2.gpu_va + 4096u};
        sub.ibs = &ok_ib;
        sub.relocs = &oob_rel;
        sub.reloc_count = 1u;
        CHECK(t.ops->queue_submit(t.ctx, &sub, &rep) == A840_TRANSPORT_OUT_OF_BOUNDS);

        A840Reloc write_ro_rel{1u, A840_RELOC_WRITE, ro_bo, r2.gpu_va};
        sub.relocs = &write_ro_rel;
        CHECK(t.ops->queue_submit(t.ctx, &sub, &rep) == A840_TRANSPORT_STATE_ERROR);
        CHECK(a840_mock_transport_submission_count(mock) == 0u);

        CHECK(t.ops->bo_free(t.ctx, ib_bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, ro_bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->queue_destroy(t.ctx, q) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_v3_zero_page_and_surface_meta");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);

        NativeAdrenoAbiZeroPageReplyV3 zp{};
        CHECK(t.ops->get_zero_page_v3(t.ctx, &zp) == A840_TRANSPORT_OK);
        CHECK(zp.gpu_va != 0u && (zp.gpu_va % 65536u) == 0u);
        CHECK(zp.bytes >= 65536u);

        uint64_t bo = 0;
        A840BoAllocDesc ad{65536u, 12u, 0u};
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &bo) == A840_TRANSPORT_OK);

        NativeAdrenoAbiSurfaceMetaV3 meta{};
        meta.bo_handle = bo;
        meta.op = 1u; /* set */
        meta.ubwc_enabled = 1u;
        meta.ubwc_version = 6u;
        meta.highest_bank_bit = 15u; /* wrong HBB on Kaanapali */
        meta.swizzle_levels = 6u;
        meta.macrotile_mode = 1u;
        meta.meta_offset = 0u;
        meta.meta_pitch = 64u;
        CHECK(t.ops->surface_meta_v3(t.ctx, &meta) == A840_TRANSPORT_INVALID_ARG);

        meta.highest_bank_bit = 16u;
        meta.flags = 1u;
        CHECK(t.ops->surface_meta_v3(t.ctx, &meta) == A840_TRANSPORT_OK);

        NativeAdrenoAbiSurfaceMetaV3 got{};
        got.bo_handle = bo;
        got.op = 0u; /* get */
        CHECK(t.ops->surface_meta_v3(t.ctx, &got) == A840_TRANSPORT_OK);
        CHECK(got.ubwc_enabled == 1u && got.ubwc_version == 6u && got.highest_bank_bit == 16u);
        CHECK(got.meta_pitch == 64u && got.flags == 1u);

        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->surface_meta_v3(t.ctx, &got) == A840_TRANSPORT_DANGLING_BO);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_v3_wait_any_and_wait_all");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = false;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        uint64_t q1 = 0, q2 = 0, bo = 0;
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_NORMAL, &q1) == A840_TRANSPORT_OK);
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_HIGH, &q2) == A840_TRANSPORT_OK);
        A840BoAllocDesc ad{4096u, 12u, 0u};
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &bo) == A840_TRANSPORT_OK);
        A840BoMapDesc md{bo, caps.va_start, A840_BO_MAP_GPU, A840_BO_PERM_READ, 0u, 3u};
        A840BoMapReply mr{};
        CHECK(t.ops->bo_map(t.ctx, &md, &mr) == A840_TRANSPORT_OK);

        A840SubmitIb ib{bo, mr.gpu_va, 4u};
        A840SubmitDesc s1{q1, 0u, 0u, &ib, 1u, nullptr, 0u};
        A840SubmitDesc s2{q2, 0u, 0u, &ib, 1u, nullptr, 0u};
        A840SubmitReply r1{}, r2{};
        CHECK(t.ops->queue_submit(t.ctx, &s1, &r1) == A840_TRANSPORT_OK);
        CHECK(t.ops->queue_submit(t.ctx, &s2, &r2) == A840_TRANSPORT_OK);

        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 2u;
        req.flags = 0u; /* wait_any */
        req.queues[0] = q1; req.fences[0] = r1.fence;
        req.queues[1] = q2; req.fences[1] = r2.fence;
        NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        CHECK(t.ops->wait_any_v3(t.ctx, &req, &wrep) == A840_TRANSPORT_TIMEOUT);
        CHECK(wrep.satisfied_mask == 0u && wrep.timed_out == 1u);

        CHECK(a840_mock_transport_complete_queue(mock, q2, r2.fence) == A840_TRANSPORT_OK);
        CHECK(t.ops->wait_any_v3(t.ctx, &req, &wrep) == A840_TRANSPORT_OK);
        CHECK(wrep.satisfied_mask == 2u && wrep.timed_out == 0u);

        req.flags = 1u; /* wait_all */
        CHECK(t.ops->wait_any_v3(t.ctx, &req, &wrep) == A840_TRANSPORT_TIMEOUT);
        CHECK(wrep.satisfied_mask == 2u && wrep.timed_out == 1u);

        CHECK(a840_mock_transport_complete_queue(mock, q1, r1.fence) == A840_TRANSPORT_OK);
        CHECK(t.ops->wait_any_v3(t.ctx, &req, &wrep) == A840_TRANSPORT_OK);
        CHECK(wrep.satisfied_mask == 3u && wrep.timed_out == 0u);

        CHECK(t.ops->bo_free(t.ctx, bo) == A840_TRANSPORT_OK);
        CHECK(t.ops->queue_destroy(t.ctx, q1) == A840_TRANSPORT_OK);
        CHECK(t.ops->queue_destroy(t.ctx, q2) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_v2_only_mode_gating");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.abi_version = NativeAdrenoAbiVersionV2;
        cfg.abi_flags = NativeAdrenoParamsV2HostImport | NativeAdrenoParamsV2OwnedCoherent;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);

        NativeAdrenoAbiZeroPageReplyV3 zp{};
        CHECK(t.ops->get_zero_page_v3(t.ctx, &zp) == A840_TRANSPORT_UNSUPPORTED);
        NativeAdrenoAbiWaitAnyV3 wreq{};
        wreq.count = 1u;
        NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        CHECK(t.ops->wait_any_v3(t.ctx, &wreq, &wrep) == A840_TRANSPORT_UNSUPPORTED);
        NativeAdrenoAbiSurfaceMetaV3 sm{};
        CHECK(t.ops->surface_meta_v3(t.ctx, &sm) == A840_TRANSPORT_UNSUPPORTED);

        /* Device gracefully falls back to owned zero page BO on v2 transport. */
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_NONE, &dev) == A840_RT_OK);
        CHECK(dev->zero_page_owned);
        CHECK(a840_rt_device_zero_page_va(dev) != 0u);
        CHECK((a840_rt_device_zero_page_va(dev) % 65536u) == 0u);
        CHECK(a840_mock_transport_live_bo_count(mock) == 2u); /* zero_page + border_color */
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_queue_priority_validation");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        uint64_t q = 0;
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_DISABLED, &q) == A840_TRANSPORT_INVALID_ARG);
        CHECK(t.ops->queue_create(t.ctx, 6u, &q) == A840_TRANSPORT_INVALID_ARG);
        CHECK(t.ops->queue_create(t.ctx, A840_QUEUE_PRIORITY_REALTIME, &q) == A840_TRANSPORT_OK);
        CHECK(a840_mock_transport_live_queue_count(mock) == 1u);
        CHECK(t.ops->queue_destroy(t.ctx, q) == A840_TRANSPORT_OK);
        CHECK(a840_mock_transport_live_queue_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("transport_overlapping_gpu_va_rejected");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840TransportCaps caps{};
        CHECK(t.ops->query_caps(t.ctx, &caps) == A840_TRANSPORT_OK);

        uint64_t b1 = 0, b2 = 0;
        A840BoAllocDesc ad{8192u, 12u, 0u};
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &b1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_alloc(t.ctx, &ad, &b2) == A840_TRANSPORT_OK);
        A840BoMapDesc m1{b1, caps.va_start, A840_BO_MAP_GPU, A840_BO_PERM_READ, 0u, 3u};
        A840BoMapDesc m2{b2, caps.va_start + 4096u, A840_BO_MAP_GPU, A840_BO_PERM_READ, 0u, 3u};
        A840BoMapReply r1{}, r2{};
        CHECK(t.ops->bo_map(t.ctx, &m1, &r1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_map(t.ctx, &m2, &r2) == A840_TRANSPORT_STATE_ERROR);
        CHECK(t.ops->bo_free(t.ctx, b1) == A840_TRANSPORT_OK);
        CHECK(t.ops->bo_free(t.ctx, b2) == A840_TRANSPORT_OK);
        a840_mock_transport_destroy(mock);
    }
}

/* =========================================================================
 * Group C: Deterministic 48-bit VA Allocator, Device, Buffer, Texture
 * ========================================================================= */

static void test_group_c_device_resources()
{
    begin_case("va_allocator_4k_and_64k_alignment");
    {
        A840VaAllocator va{};
        const uint64_t base = UINT64_C(0x10000000);
        const uint64_t size = UINT64_C(0x100000); /* 1 MiB */
        CHECK(a840_va_allocator_init(&va, base, size) == A840_RT_OK);

        uint64_t a1 = 0, a2 = 0, a3 = 0;
        CHECK(a840_va_allocator_alloc(&va, 4096u, 4096u, &a1) == A840_RT_OK);
        CHECK(a1 == base);
        CHECK(a840_va_allocator_alloc(&va, 4096u, 65536u, &a2) == A840_RT_OK);
        CHECK((a2 % 65536u) == 0u && a2 == base + 65536u);
        /* Next 4 KiB alloc deterministically fills the prefix gap [base+4096, base+65536). */
        CHECK(a840_va_allocator_alloc(&va, 4096u, 4096u, &a3) == A840_RT_OK);
        CHECK(a3 == base + 4096u);
        CHECK(a840_va_allocator_used_bytes(&va) == 12288u);

        CHECK(a840_va_allocator_free(&va, a1, 4096u) == A840_RT_OK);
        CHECK(a840_va_allocator_free(&va, a2, 4096u) == A840_RT_OK);
        CHECK(a840_va_allocator_free(&va, a3, 4096u) == A840_RT_OK);
        CHECK(a840_va_allocator_used_bytes(&va) == 0u);
        CHECK(a840_va_allocator_free_bytes(&va) == size);
        CHECK(a840_va_allocator_free_block_count(&va) == 1u);
        a840_va_allocator_destroy(&va);
    }

    begin_case("va_allocator_free_coalesce_and_reuse");
    {
        A840VaAllocator va{};
        const uint64_t base = UINT64_C(0x20000000);
        CHECK(a840_va_allocator_init(&va, base, 65536u * 4u) == A840_RT_OK);

        uint64_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        CHECK(a840_va_allocator_alloc(&va, 65536u, 65536u, &p0) == A840_RT_OK);
        CHECK(a840_va_allocator_alloc(&va, 65536u, 65536u, &p1) == A840_RT_OK);
        CHECK(a840_va_allocator_alloc(&va, 65536u, 65536u, &p2) == A840_RT_OK);
        CHECK(a840_va_allocator_alloc(&va, 65536u, 65536u, &p3) == A840_RT_OK);
        CHECK(a840_va_allocator_free_block_count(&va) == 0u);

        /* Free p1 then p2 -> coalesce into one 128 KiB block at p1. */
        CHECK(a840_va_allocator_free(&va, p1, 65536u) == A840_RT_OK);
        CHECK(a840_va_allocator_free(&va, p2, 65536u) == A840_RT_OK);
        CHECK(a840_va_allocator_free_block_count(&va) == 1u);

        uint64_t reused = 0;
        CHECK(a840_va_allocator_alloc(&va, 131072u, 65536u, &reused) == A840_RT_OK);
        CHECK(reused == p1);

        CHECK(a840_va_allocator_free(&va, p0, 65536u) == A840_RT_OK);
        CHECK(a840_va_allocator_free(&va, p3, 65536u) == A840_RT_OK);
        CHECK(a840_va_allocator_free(&va, reused, 131072u) == A840_RT_OK);
        CHECK(a840_va_allocator_free_block_count(&va) == 1u);
        a840_va_allocator_destroy(&va);
    }

    begin_case("va_allocator_48bit_limit_rejected");
    {
        A840VaAllocator va{};
        const uint64_t limit48 = UINT64_C(1) << 48;
        CHECK(a840_va_allocator_init(&va, limit48 - 4096u, 8192u) == A840_RT_RANGE_ERROR);
        CHECK(a840_va_allocator_init(&va, limit48, 4096u) == A840_RT_RANGE_ERROR);
        CHECK(a840_va_allocator_init(&va, 0u, 4096u) == A840_RT_ALIGNMENT);
        CHECK(a840_va_allocator_init(&va, 4096u + 64u, 4096u) == A840_RT_ALIGNMENT);

        CHECK(a840_va_allocator_init(&va, limit48 - 65536u, 65536u) == A840_RT_OK);
        uint64_t got = 0;
        CHECK(a840_va_allocator_alloc(&va, 65536u + 4096u, 4096u, &got) == A840_RT_RANGE_ERROR);
        CHECK(a840_va_allocator_alloc(&va, 65536u, 65536u, &got) == A840_RT_OK);
        CHECK(got == limit48 - 65536u);
        CHECK(a840_va_allocator_alloc(&va, 4096u, 4096u, &got) == A840_RT_RANGE_ERROR);
        CHECK(a840_va_allocator_free(&va, limit48 - 65536u, 4096u) == A840_RT_INVALID_ARG);
        CHECK(a840_va_allocator_free(&va, limit48 - 65536u, 65536u) == A840_RT_OK);
        a840_va_allocator_destroy(&va);
    }

    begin_case("device_create_v3_facts_zero_page_border_color");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);

        const A840ChipFacts *f = a840_rt_device_chip_facts(dev);
        CHECK(f != nullptr);
        CHECK(f->chip_id == A840_KAANAPALI_CHIP_ID);
        CHECK(f->highest_bank_bit == 16u);
        CHECK(f->gmem_bytes == A840_KAANAPALI_GMEM_BYTES);
        CHECK(!dev->zero_page_owned);
        CHECK(a840_rt_device_zero_page_va(dev) != 0u);
        CHECK((a840_rt_device_zero_page_va(dev) % 65536u) == 0u);
        CHECK(a840_rt_device_border_color_va(dev) != 0u);
        CHECK(a840_rt_device_border_color_bo(dev) != 0u);
        CHECK(a840_mock_transport_live_bo_count(mock) == 1u); /* border_color */

        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("device_create_null_and_bad_transport_rejected");
    {
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(nullptr, A840_ROUTE_NONE, &dev) == A840_RT_INVALID_ARG);
        A840BackendTransport empty{};
        CHECK(a840_rt_device_create(&empty, A840_ROUTE_NONE, &dev) == A840_RT_INVALID_ARG);
    }

    begin_case("buffer_create_4k_64k_host_import_and_write");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);

        A840RtBuffer *b4k = create_buf(dev, 1024u, 4096u);
        A840RtBuffer *b64k = create_buf(dev, 8192u, 65536u);
        CHECK((b4k->gpu_va % 4096u) == 0u && b4k->alloc_bytes == 4096u);
        CHECK((b64k->gpu_va % 65536u) == 0u && b64k->alloc_bytes == 8192u);

        const uint32_t data[4] = {1u, 2u, 3u, 4u};
        CHECK(a840_rt_buffer_write(b4k, 0u, data, sizeof(data)) == A840_RT_OK);
        CHECK(a840_rt_buffer_write(b4k, 1020u, data, sizeof(data)) == A840_RT_RANGE_ERROR);

        alignas(4096) std::array<uint8_t, 4096> host_mem{};
        A840RtBufferDesc imp_desc{};
        imp_desc.size_bytes = 4096u;
        imp_desc.alignment = 4096u;
        imp_desc.usage_flags = A840_BUF_USAGE_READ;
        imp_desc.host_import = true;
        imp_desc.host_ptr = host_mem.data();
        A840RtBuffer *bimp = nullptr;
        CHECK(a840_rt_buffer_create(dev, &imp_desc, &bimp) == A840_RT_OK);
        CHECK(bimp->imported && bimp->cpu_ptr == host_mem.data());

        a840_rt_buffer_retain(b4k);
        CHECK(b4k->refcount == 2u);
        a840_rt_buffer_release(b4k);
        CHECK(b4k->refcount == 1u);
        a840_rt_buffer_release(b4k);
        a840_rt_buffer_release(b64k);
        a840_rt_buffer_release(bimp);
        CHECK(dev->live_buffers == 0u);

        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("texture_create_linear_rgba8_and_bgra8_descriptor");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);

        A840RtTexture *rgba = create_tex2d(dev, 70u, 64u, 32u, A840_TILE_LINEAR);
        A840RtTexture *bgra = create_tex2d(dev, 80u, 100u, 50u, A840_TILE_LINEAR);
        CHECK(rgba->pitch_bytes == 256u);
        CHECK((bgra->pitch_bytes % 64u) == 0u && bgra->pitch_bytes == 448u);
        CHECK((bgra->slice_stride_bytes % 4096u) == 0u);
        CHECK(rgba->swap == A840_WZYX);
        CHECK(bgra->swap == A840_WXYZ);

        /* Cross-check 64B A8XX texture descriptor against direct a840_encode_tex_const(). */
        A840TexDescInput expected_in{};
        expected_in.format_id = bgra->format_id;
        expected_in.swap = bgra->swap;
        expected_in.type = A840_TEX_2D;
        expected_in.width = 100u;
        expected_in.height = 50u;
        expected_in.depth = 1u;
        expected_in.mip_levels = 1u;
        expected_in.samples = 1u;
        expected_in.pitch = bgra->pitch_bytes;
        expected_in.slice_stride = bgra->slice_stride_bytes;
        expected_in.gpu_va = bgra->gpu_va;
        expected_in.tile_mode = A840_TILE_LINEAR;
        expected_in.highest_bank_bit = 16u;
        expected_in.pitch_align_log2 = 6u;
        expected_in.swizzle[0] = A840_SWIZZLE_R;
        expected_in.swizzle[1] = A840_SWIZZLE_G;
        expected_in.swizzle[2] = A840_SWIZZLE_B;
        expected_in.swizzle[3] = A840_SWIZZLE_A;
        uint32_t expected_words[16]{};
        CHECK(a840_encode_tex_const(&expected_in, expected_words) == A840_EMIT_OK);
        CHECK(std::memcmp(bgra->tex_const, expected_words, sizeof(expected_words)) == 0);

        a840_rt_texture_release(rgba);
        a840_rt_texture_release(bgra);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("texture_create_tiled_rgba8_r8g8_r16f_depth32_stencil8");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);

        A840RtTexture *r8g8 = create_tex2d(dev, 30u, 33u, 17u, A840_TILE_TILED); /* RG8Unorm: 128B pitch, 32-row align */
        CHECK(r8g8->pitch_bytes == 128u && r8g8->pitch_align_log2 == 7u);
        CHECK(r8g8->slice_stride_bytes >= 128u * 32u);

        A840RtTexture *bgra_t = create_tex2d(dev, 80u, 64u, 64u, A840_TILE_TILED);
        CHECK(bgra_t->pitch_bytes == 256u && bgra_t->swap == A840_WZYX && bgra_t->color_swap == A840_WZYX);

        A840RtTexture *rgba16f = create_tex2d(dev, 115u, 64u, 32u, A840_TILE_TILED);
        CHECK(rgba16f->pitch_bytes == 512u && rgba16f->pitch_align_log2 == 9u);

        A840RtTexture *d32 = create_tex2d(dev, 252u, 64u, 64u, A840_TILE_TILED);
        A840RtTexture *s8 = create_tex2d(dev, 253u, 64u, 64u, A840_TILE_TILED);
        CHECK(d32->pitch_bytes == 256u && s8->pitch_bytes == 128u);

        a840_rt_texture_release(r8g8);
        a840_rt_texture_release(bgra_t);
        a840_rt_texture_release(rgba16f);
        a840_rt_texture_release(d32);
        a840_rt_texture_release(s8);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("texture_create_rejects_unsupported_and_bad_dims");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);

        const size_t base_bos = a840_mock_transport_live_bo_count(mock);
        A840RtTexture *tex = nullptr;
        A840RtTextureDesc d{};
        d.mtl_pixel_format = 9999u; d.tex_type = A840_TEX_2D; d.width = 16; d.height = 16;
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_UNSUPPORTED);

        d.mtl_pixel_format = 554u; /* XR10 */
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_UNSUPPORTED);

        d.mtl_pixel_format = 70u; d.tile_mode = A840_TILE_UBWC;
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_UNSUPPORTED);

        d.tile_mode = A840_TILE_LINEAR; d.width = 0u;
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_INVALID_ARG);

        d.width = 16u; d.pitch_bytes = 68u; /* not 64B aligned */
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_ALIGNMENT);

        d.pitch_bytes = 0u; d.tex_type = A840_TEX_CUBE; d.array_layers = 5u;
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_INVALID_ARG);

        d.tex_type = A840_TEX_2D; d.array_layers = 1u; d.mip_levels = 10u; /* > log2(16)+1 */
        CHECK(a840_rt_texture_create(dev, &d, &tex) == A840_RT_RANGE_ERROR);

        CHECK(a840_mock_transport_live_bo_count(mock) == base_bos);
        CHECK(dev->live_textures == 0u);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("cross_device_buffer_rejected");
    {
        A840MockTransport *m1 = nullptr, *m2 = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &m1) == A840_TRANSPORT_OK);
        CHECK(a840_mock_transport_create(nullptr, &m2) == A840_TRANSPORT_OK);
        auto t1 = a840_mock_transport_interface(m1);
        auto t2 = a840_mock_transport_interface(m2);
        A840RtDevice *d1 = nullptr, *d2 = nullptr;
        CHECK(a840_rt_device_create(&t1, A840_ROUTE_PHASE2_ALL, &d1) == A840_RT_OK);
        CHECK(a840_rt_device_create(&t2, A840_ROUTE_PHASE2_ALL, &d2) == A840_RT_OK);

        A840RtQueue *q1 = nullptr;
        CHECK(a840_rt_queue_create(d1, A840_QUEUE_PRIORITY_NORMAL, &q1) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q1, nullptr, &cb) == A840_RT_OK);

        A840RtBuffer *b1 = create_buf(d1, 4096u);
        A840RtBuffer *b2 = create_buf(d2, 4096u);
        CHECK(a840_rt_device_owns_buffer(d1, b1));
        CHECK(!a840_rt_device_owns_buffer(d1, b2));

        CHECK(a840_rt_cb_encode_mem_write64(cb, b2, 0u, 42u) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_cb_encode_timestamp(cb, b2, 0u, 1u, false) == A840_RT_WRONG_DEVICE);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b2, 0u, b1, 0u, 64u) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        A840RtComputeEncoder *ce = nullptr;
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        auto cs_stage = make_stage(A840_STAGE_COMPUTE);
        A840RtComputePipelineDesc cpd{};
        cpd.stage = &cs_stage;
        cpd.code_buffer = b2;
        cpd.local_id_regid = 4;
        cpd.wg_id_regid = 8;
        CHECK(a840_rt_compute_set_pipeline(ce, &cpd) == A840_RT_WRONG_DEVICE);
        A840RtBuffer *tables[1] = {b2};
        CHECK(a840_rt_compute_set_bindless(ce, 1u, tables, nullptr) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_compute_set_consts_indirect(ce, 0u, 1u, 128u, b2, 0u) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        a840_rt_compute_destroy(ce);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q1);
        a840_rt_device_destroy(d1);
        a840_rt_device_destroy(d2);
        a840_mock_transport_destroy(m1);
        a840_mock_transport_destroy(m2);
    }

    begin_case("cross_device_texture_rejected");
    {
        A840MockTransport *m1 = nullptr, *m2 = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &m1) == A840_TRANSPORT_OK);
        CHECK(a840_mock_transport_create(nullptr, &m2) == A840_TRANSPORT_OK);
        auto t1 = a840_mock_transport_interface(m1);
        auto t2 = a840_mock_transport_interface(m2);
        A840RtDevice *d1 = nullptr, *d2 = nullptr;
        CHECK(a840_rt_device_create(&t1, A840_ROUTE_PHASE2_ALL, &d1) == A840_RT_OK);
        CHECK(a840_rt_device_create(&t2, A840_ROUTE_PHASE2_ALL, &d2) == A840_RT_OK);

        A840RtQueue *q1 = nullptr;
        CHECK(a840_rt_queue_create(d1, A840_QUEUE_PRIORITY_NORMAL, &q1) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q1, nullptr, &cb) == A840_RT_OK);

        A840RtTexture *tex1 = create_tex2d(d1, 70u, 32u, 32u, A840_TILE_LINEAR);
        A840RtTexture *tex2 = create_tex2d(d2, 70u, 32u, 32u, A840_TILE_LINEAR);
        CHECK(!a840_rt_device_owns_texture(d1, tex2));

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        A840Rect2D rect{0u, 0u, 32u, 32u};
        const uint32_t col[4] = {0u, 0u, 0u, 255u};
        CHECK(a840_rt_blit_clear_2d(be, tex2, &rect, col) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_blit_2d(be, tex2, &rect, tex1, &rect, A840_FILTER_NEAREST) == A840_RT_WRONG_DEVICE);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        A840RtRenderPassDesc rp{};
        rp.color_count = 1u;
        rp.color_targets[0] = tex2;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_WRONG_DEVICE);
        CHECK(re == nullptr);

        a840_rt_cb_release(cb);
        a840_rt_texture_release(tex1);
        a840_rt_texture_release(tex2);
        a840_rt_queue_destroy(q1);
        a840_rt_device_destroy(d1);
        a840_rt_device_destroy(d2);
        a840_mock_transport_destroy(m1);
        a840_mock_transport_destroy(m2);
    }

    begin_case("null_argument_guards_across_runtime");
    {
        A840RtBuffer *b = nullptr;
        A840RtTexture *tx = nullptr;
        A840RtQueue *q = nullptr;
        A840RtCmdBuffer *cb = nullptr;
        A840RtBlitEncoder *be = nullptr;
        A840RtComputeEncoder *ce = nullptr;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_buffer_create(nullptr, nullptr, &b) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_texture_create(nullptr, nullptr, &tx) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_queue_create(nullptr, 1u, &q) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_create(nullptr, nullptr, &cb) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_begin_blit(nullptr, &be) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_begin_compute(nullptr, &ce) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_begin_render_pass(nullptr, nullptr, &re) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_commit(nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_rt_cb_wait_until_completed(nullptr, 0u) == A840_RT_INVALID_ARG);
    }

    begin_case("device_va_deterministic_across_two_runs");
    {
        uint64_t va_a = 0, va_b = 0;
        uint32_t desc_a[16]{}, desc_b[16]{};
        for (int run = 0; run < 2; ++run) {
            A840MockTransport *mock = nullptr;
            CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
            auto t = a840_mock_transport_interface(mock);
            A840RtDevice *dev = nullptr;
            CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
            A840RtTexture *tex = create_tex2d(dev, 80u, 128u, 64u, A840_TILE_TILED);
            if (run == 0) {
                va_a = tex->gpu_va;
                std::memcpy(desc_a, tex->tex_const, sizeof(desc_a));
            } else {
                va_b = tex->gpu_va;
                std::memcpy(desc_b, tex->tex_const, sizeof(desc_b));
            }
            a840_rt_texture_release(tex);
            a840_rt_device_destroy(dev);
            a840_mock_transport_destroy(mock);
        }
        CHECK(va_a == va_b);
        CHECK(std::memcmp(desc_a, desc_b, sizeof(desc_a)) == 0);
    }
}

/* =========================================================================
 * Group D: Encoders, Negative Semantics, Keepalive, FIFO Callbacks
 * ========================================================================= */

struct CallbackLog {
    std::vector<std::string> events;
};

struct CallbackTag {
    CallbackLog *log;
    const char *label;
};

static void record_cb_event(A840RtCmdBuffer *cb, void *user_data)
{
    CHECK(cb != nullptr);
    auto *tag = static_cast<CallbackTag *>(user_data);
    tag->log->events.emplace_back(tag->label);
}

static void test_group_d_cmd_and_negative()
{
    begin_case("encoder_exclusive_active_guard");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtTexture *rt = create_tex2d(dev, 70u, 32u, 32u, A840_TILE_LINEAR);

        A840RtBlitEncoder *be = nullptr, *be2 = nullptr;
        A840RtComputeEncoder *ce = nullptr, *ce2 = nullptr;
        A840RtRenderEncoder *re = nullptr, *re2 = nullptr;
        A840RtRenderPassDesc rp{};
        rp.color_count = 1u;
        rp.color_targets[0] = rt;

        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_cb_begin_blit(cb, &be2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        CHECK(a840_rt_cb_begin_blit(cb, &be2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_compute(cb, &ce2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        a840_rt_compute_destroy(ce);

        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_OK);
        CHECK(a840_rt_cb_begin_blit(cb, &be2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_compute(cb, &ce2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re2) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        a840_rt_render_destroy(re);

        a840_rt_cb_release(cb);
        a840_rt_texture_release(rt);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("ended_encoder_rejects_reencoding_and_double_end");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);
        A840RtTexture *rt = create_tex2d(dev, 70u, 32u, 32u, A840_TILE_LINEAR);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_blit_end(be) == A840_RT_STATE_ERROR);
        a840_rt_blit_destroy(be);

        A840RtComputeEncoder *ce = nullptr;
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threadgroups(ce, 1u, 1u, 1u) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_compute_end(ce) == A840_RT_STATE_ERROR);
        a840_rt_compute_destroy(ce);

        A840RtRenderPassDesc rp{};
        rp.color_count = 1u;
        rp.color_targets[0] = rt;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_OK);
        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        const size_t used_after = a840_rt_cb_used_dwords(cb);
        A840RtDrawDesc dd{};
        dd.primitive = A840_PRIM_TRIANGLES;
        dd.count = 3u;
        dd.instance_count = 1u;
        CHECK(a840_rt_render_draw(re, &dd) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_render_end_pass(re) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_used_dwords(cb) == used_after);
        a840_rt_render_destroy(re);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_texture_release(rt);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("empty_cb_commit_rejected");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);

        CHECK(a840_rt_cb_commit(cb) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_state(cb) == A840_CB_RECORDING);
        CHECK(a840_mock_transport_submission_count(mock) == 0u);

        a840_rt_cb_release(cb);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("double_commit_rejected");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_cb_add_completed_handler(cb, record_cb_event, nullptr) == A840_RT_STATE_ERROR);
        CHECK(a840_mock_transport_submission_count(mock) == 1u);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("ib_capacity_overflow_transactional_rollback");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);

        /* Capacity of 10 dwords: fits 1 buffer copy (6 dwords), rejects 2D clear (29 dwords)
         * and rejects commit's RB_DONE_TS (6 dwords -> total 12 > 10) without corrupting state! */
        A840RtCmdBufferDesc cbd{10u, 16u};
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, &cbd, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);
        A840RtTexture *tex = create_tex2d(dev, 70u, 32u, 32u, A840_TILE_LINEAR);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
        const size_t used_before = a840_rt_cb_used_dwords(cb);
        const size_t relocs_before = a840_rt_cb_reloc_count(cb);
        CHECK(used_before == 6u && relocs_before == 2u);

        A840Rect2D rect{0u, 0u, 32u, 32u};
        const uint32_t col[4] = {1u, 2u, 3u, 4u};
        CHECK(a840_rt_blit_clear_2d(be, tex, &rect, col) == A840_RT_CAPACITY);
        CHECK(a840_rt_cb_used_dwords(cb) == used_before);
        CHECK(a840_rt_cb_reloc_count(cb) == relocs_before);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        CHECK(a840_rt_cb_commit(cb) == A840_RT_CAPACITY);
        CHECK(a840_rt_cb_state(cb) == A840_CB_RECORDING);
        CHECK(a840_rt_cb_used_dwords(cb) == used_before);
        CHECK(a840_rt_cb_reloc_count(cb) == relocs_before);
        CHECK(a840_mock_transport_submission_count(mock) == 0u);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_texture_release(tex);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("reloc_capacity_overflow_transactional_rollback");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);

        A840RtCmdBufferDesc cbd{256u, 1u}; /* only 1 reloc slot; buffer copy needs 2 */
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, &cbd, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_CAPACITY);
        CHECK(a840_rt_cb_used_dwords(cb) == 0u);
        CHECK(a840_rt_cb_reloc_count(cb) == 0u);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("compute_dispatch_without_pipeline_and_indirect_fallback");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *ind = create_buf(dev, 4096u);

        A840RtComputeEncoder *ce = nullptr;
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threadgroups(ce, 1u, 1u, 1u) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_compute_dispatch_threads(ce, 8u, 4u, 1u) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_compute_dispatch_indirect(ce, ind, 0u) == A840_RT_FALLBACK_REQUIRED);
        CHECK(a840_rt_cb_last_route(cb).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        a840_rt_compute_destroy(ce);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(ind);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("render_draw_without_program_or_state_and_fallback_predicates");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtTexture *rt = create_tex2d(dev, 70u, 32u, 32u, A840_TILE_TILED);

        A840RtRenderPassDesc bad_rp{};
        bad_rp.color_count = 1u;
        bad_rp.color_targets[0] = rt;
        bad_rp.has_memoryless_attachment = true;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &bad_rp, &re) == A840_RT_FALLBACK_REQUIRED);
        CHECK(a840_rt_cb_last_route(cb).predicate_id == A840_PRED_RENDER_GMEM_REQUIRED);
        CHECK(re == nullptr);

        bad_rp.has_memoryless_attachment = false;
        bad_rp.has_ycbcr_attachment = true;
        CHECK(a840_rt_cb_begin_render_pass(cb, &bad_rp, &re) == A840_RT_FALLBACK_REQUIRED);
        CHECK(a840_rt_cb_last_route(cb).predicate_id == A840_PRED_RENDER_YCBCR_OR_XR10);

        A840RtRenderPassDesc ok_rp{};
        ok_rp.color_count = 1u;
        ok_rp.color_targets[0] = rt;
        CHECK(a840_rt_cb_begin_render_pass(cb, &ok_rp, &re) == A840_RT_OK);
        A840RtDrawDesc dd{};
        dd.primitive = A840_PRIM_TRIANGLES;
        dd.count = 3u;
        dd.instance_count = 1u;
        CHECK(a840_rt_render_draw(re, &dd) == A840_RT_STATE_ERROR);
        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        a840_rt_render_destroy(re);

        a840_rt_cb_release(cb);
        a840_rt_texture_release(rt);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("router_disabled_blocks_native_encoder_ops");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_NONE, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_FALLBACK_REQUIRED);
        CHECK(a840_rt_cb_last_route(cb).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
        CHECK(a840_rt_cb_used_dwords(cb) == 0u);

        a840_router_enable_category(a840_rt_device_router(dev), A840_ROUTE_BLIT);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
        CHECK(a840_rt_cb_used_dwords(cb) == 6u);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("deferred_completion_strict_fifo_callbacks");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = false;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        CallbackLog log;
        CallbackTag s1a{&log, "s1a"}, s1b{&log, "s1b"}, c1a{&log, "c1a"}, c1b{&log, "c1b"};
        CallbackTag s2a{&log, "s2a"}, c2a{&log, "c2a"};
        CallbackTag s3a{&log, "s3a"}, c3a{&log, "c3a"};

        A840RtCmdBuffer *cbs[3]{};
        for (int i = 0; i < 3; ++i) {
            CHECK(a840_rt_cb_create(q, nullptr, &cbs[i]) == A840_RT_OK);
            A840RtBlitEncoder *be = nullptr;
            CHECK(a840_rt_cb_begin_blit(cbs[i], &be) == A840_RT_OK);
            CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
            CHECK(a840_rt_blit_end(be) == A840_RT_OK);
            a840_rt_blit_destroy(be);
        }
        CHECK(a840_rt_cb_add_scheduled_handler(cbs[0], record_cb_event, &s1a) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cbs[0], record_cb_event, &s1b) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cbs[0], record_cb_event, &c1a) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cbs[0], record_cb_event, &c1b) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cbs[1], record_cb_event, &s2a) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cbs[1], record_cb_event, &c2a) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cbs[2], record_cb_event, &s3a) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cbs[2], record_cb_event, &c3a) == A840_RT_OK);

        for (int i = 0; i < 3; ++i) {
            CHECK(a840_rt_cb_commit(cbs[i]) == A840_RT_OK);
            CHECK(a840_rt_cb_state(cbs[i]) == A840_CB_SCHEDULED);
        }
        CHECK(a840_rt_queue_pending_count(q) == 3u);
        CHECK(a840_rt_cb_wait_until_completed(cbs[0], 0u) == A840_RT_TIMEOUT);

        /* Complete up through CB3 and wait on CB3 first -> MUST drain CB1, CB2, CB3 in strict FIFO! */
        CHECK(a840_mock_transport_complete_queue(mock, a840_rt_queue_handle(q),
                                                 a840_rt_cb_submit_fence(cbs[2])) == A840_TRANSPORT_OK);
        CHECK(a840_rt_cb_wait_until_completed(cbs[2], 1000u) == A840_RT_OK);
        CHECK(a840_rt_queue_pending_count(q) == 0u);
        for (int i = 0; i < 3; ++i) {
            CHECK(a840_rt_cb_state(cbs[i]) == A840_CB_COMPLETED);
        }

        const std::vector<std::string> expected = {
            "s1a", "s1b", "s2a", "s3a", "c1a", "c1b", "c2a", "c3a"
        };
        CHECK(log.events == expected);

        for (int i = 0; i < 3; ++i) a840_rt_cb_release(cbs[i]);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("resource_keepalive_until_gpu_completion");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = false;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);

        A840RtBuffer *src = create_buf(dev, 4096u);
        A840RtBuffer *dst = create_buf(dev, 4096u);
        A840RtTexture *tex = create_tex2d(dev, 70u, 32u, 32u, A840_TILE_LINEAR);
        const uint64_t src_bo = src->bo_handle;
        const uint64_t dst_bo = dst->bo_handle;
        const uint64_t tex_bo = tex->bo_handle;

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, src, 0u, dst, 0u, 128u) == A840_RT_OK);
        A840Rect2D rect{0u, 0u, 32u, 32u};
        const uint32_t col[4] = {10u, 20u, 30u, 255u};
        CHECK(a840_rt_blit_clear_2d(be, tex, &rect, col) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        /* Drop caller references BEFORE commit — CB must keep BOs alive! */
        a840_rt_buffer_release(src);
        a840_rt_buffer_release(dst);
        a840_rt_texture_release(tex);
        CHECK(a840_mock_transport_is_bo_live(mock, src_bo));
        CHECK(a840_mock_transport_is_bo_live(mock, dst_bo));
        CHECK(a840_mock_transport_is_bo_live(mock, tex_bo));

        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_retained_buffer_count(cb) == 3u); /* src, dst, queue ts_buffer */
        CHECK(a840_rt_cb_retained_texture_count(cb) == 1u);
        CHECK(a840_mock_transport_is_bo_live(mock, src_bo));
        CHECK(a840_mock_transport_is_bo_live(mock, dst_bo));
        CHECK(a840_mock_transport_is_bo_live(mock, tex_bo));

        /* Once GPU completes, CB releases retained resources automatically. */
        a840_mock_transport_complete_all(mock);
        CHECK(a840_rt_cb_wait_until_completed(cb, 0u) == A840_RT_OK);
        CHECK(a840_rt_cb_retained_buffer_count(cb) == 0u);
        CHECK(a840_rt_cb_retained_texture_count(cb) == 0u);
        CHECK(!a840_mock_transport_is_bo_live(mock, src_bo));
        CHECK(!a840_mock_transport_is_bo_live(mock, dst_bo));
        CHECK(!a840_mock_transport_is_bo_live(mock, tex_bo));

        a840_rt_cb_release(cb);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("commit_auto_appends_rb_done_ts_and_validates_relocs");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);

        CHECK(a840_mock_transport_submission_count(mock) == 1u);
        A840MockSubmissionView view{};
        CHECK(a840_mock_transport_get_submission(mock, 0u, &view) == A840_TRANSPORT_OK);
        CHECK(view.reloc_count == 3u); /* copy src, copy dst, RB_DONE_TS slot */
        auto pkts = decode_all_packets(view.words, view.dword_count);
        CHECK(pkts.size() == 2u);
        CHECK(pkts[0].type == 7u && pkts[0].identifier == A840_CP_MEMCPY);
        CHECK(pkts[1].type == 7u && pkts[1].identifier == A840_CP_EVENT_WRITE7);
        CHECK(pkts[1].payload_words == 5u);
        const uint32_t ev_word0 = view.words[pkts[1].header_word + 1u];
        CHECK((ev_word0 & 0xffu) == A840_EVENT_RB_DONE_TS);
        const uint64_t marker = static_cast<uint64_t>(view.words[pkts[1].header_word + 4u]) |
                                (static_cast<uint64_t>(view.words[pkts[1].header_word + 5u]) << 32);
        CHECK(marker == a840_rt_cb_completion_marker(cb));
        CHECK(marker == 1u);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    begin_case("render_pass_with_depth_stencil_and_sds");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);

        A840RtTexture *color = create_tex2d(dev, 80u, 64u, 64u, A840_TILE_TILED);
        A840RtTexture *depth = create_tex2d(dev, 252u, 64u, 64u, A840_TILE_TILED);
        A840RtTexture *stencil = create_tex2d(dev, 253u, 64u, 64u, A840_TILE_TILED);
        A840RtBuffer *vs_buf = create_buf(dev, 4096u);
        A840RtBuffer *fs_buf = create_buf(dev, 4096u);
        A840RtBuffer *sds_buf = create_buf(dev, 4096u);
        A840RtBuffer *idx_buf = create_buf(dev, 4096u);

        A840RtRenderPassDesc rp{};
        rp.color_count = 1u;
        rp.color_targets[0] = color;
        rp.color_clear[0] = true;
        rp.color_clear_values[0][3] = 255u;
        rp.depth_target = depth;
        rp.stencil_target = stencil;

        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_OK);
        auto vs = make_stage(A840_STAGE_VERTEX);
        auto fs = make_stage(A840_STAGE_FRAGMENT);
        auto prog = make_graphics_prog(vs, fs, vs_buf, fs_buf);
        CHECK(a840_rt_render_set_program(re, &prog) == A840_RT_OK);
        auto st = make_render_state(64u, 64u, true, true);
        CHECK(a840_rt_render_set_state(re, &st) == A840_RT_OK);

        A840RtSdsGroupDesc sds[2]{};
        sds[0] = {3u, A840_SDS_SYSMEM, 8u, sds_buf, 0u};
        sds[1] = {7u, 0u, 0u, nullptr, 0u}; /* disable slot */
        CHECK(a840_rt_render_set_sds(re, sds, 2u) == A840_RT_OK);

        A840RtDrawDesc d_nonidx{};
        d_nonidx.primitive = A840_PRIM_TRIANGLES;
        d_nonidx.index_type = A840_INDEX_NONE;
        d_nonidx.count = 6u;
        d_nonidx.instance_count = 1u;
        CHECK(a840_rt_render_draw(re, &d_nonidx) == A840_RT_OK);

        A840RtDrawDesc d_idx16{};
        d_idx16.primitive = A840_PRIM_TRIANGLES;
        d_idx16.index_type = A840_INDEX_UINT16;
        d_idx16.count = 12u;
        d_idx16.instance_count = 2u;
        d_idx16.index_capacity = 24u;
        d_idx16.index_buffer = idx_buf;
        CHECK(a840_rt_render_draw(re, &d_idx16) == A840_RT_OK);

        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        a840_rt_render_destroy(re);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_wait_until_completed(cb, 0u) == A840_RT_OK);

        A840MockSubmissionView view{};
        CHECK(a840_mock_transport_get_submission(mock, 0u, &view) == A840_TRANSPORT_OK);
        auto pkts = decode_all_packets(view.words, view.dword_count);
        CHECK(pkts.size() > 20u);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(vs_buf);
        a840_rt_buffer_release(fs_buf);
        a840_rt_buffer_release(sds_buf);
        a840_rt_buffer_release(idx_buf);
        a840_rt_texture_release(color);
        a840_rt_texture_release(depth);
        a840_rt_texture_release(stencil);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("queue_destroy_with_inflight_cb_leak_free");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = false;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBuffer *b1 = create_buf(dev, 4096u);
        A840RtBuffer *b2 = create_buf(dev, 4096u);

        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, b1, 0u, b2, 0u, 64u) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        a840_rt_cb_release(cb); /* Queue holds the sole remaining ref! */
        a840_rt_buffer_release(b1);
        a840_rt_buffer_release(b2);

        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        CHECK(a840_mock_transport_live_queue_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }
}

/* =========================================================================
 * Group E: Comprehensive End-to-End Virtual GPU Workload & PM4 Dump
 * ========================================================================= */

static void test_group_e_e2e_and_dump(const char *dump_path)
{
    begin_case("e2e_single_cb_all_stages");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_HIGH, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);

        A840RtBuffer *src_buf = create_buf(dev, 8192u, 4096u);
        A840RtBuffer *dst_buf = create_buf(dev, 8192u, 65536u);
        A840RtBuffer *cs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *bindless_tbl = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *const_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *vs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *fs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *idx_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *ts_buf = create_buf(dev, 4096u, 4096u);
        A840RtTexture *tex_lin = create_tex2d(dev, 70u, 64u, 64u, A840_TILE_LINEAR);
        A840RtTexture *tex_tiled = create_tex2d(dev, 80u, 64u, 64u, A840_TILE_TILED);
        A840RtTexture *depth_tex = create_tex2d(dev, 252u, 64u, 64u, A840_TILE_TILED);

        /* 1. Blit stage: buffer copy + 2D clear + 2D blit */
        A840RtBlitEncoder *be = nullptr;
        CHECK(a840_rt_cb_begin_blit(cb, &be) == A840_RT_OK);
        CHECK(a840_rt_blit_copy_buffer(be, src_buf, 0u, dst_buf, 256u, 1024u) == A840_RT_OK);
        A840Rect2D full_rect{0u, 0u, 64u, 64u};
        const uint32_t clear_rgba[4] = {32u, 64u, 128u, 255u};
        CHECK(a840_rt_blit_clear_2d(be, tex_lin, &full_rect, clear_rgba) == A840_RT_OK);
        CHECK(a840_rt_blit_barrier(be, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);
        CHECK(a840_rt_blit_2d(be, tex_lin, &full_rect, tex_tiled, &full_rect, A840_FILTER_LINEAR) == A840_RT_OK);
        CHECK(a840_rt_blit_end(be) == A840_RT_OK);
        a840_rt_blit_destroy(be);

        CHECK(a840_rt_cb_encode_barrier(cb, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);

        /* 2. Compute stage: pipeline + bindless + direct & indirect consts + uniform & non-uniform dispatch */
        A840RtComputeEncoder *ce = nullptr;
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        auto cs_stage = make_stage(A840_STAGE_COMPUTE, 4u, 0u);
        A840RtComputePipelineDesc cpd{};
        cpd.stage = &cs_stage;
        cpd.code_buffer = cs_code;
        cpd.bindless_tex = true;
        cpd.bindless_uav = true;
        cpd.local_id_regid = 4u;
        cpd.wg_id_regid = 8u;
        CHECK(a840_rt_compute_set_pipeline(ce, &cpd) == A840_RT_OK);
        A840RtBuffer *tables[2] = {bindless_tbl, nullptr};
        const uint64_t offsets[2] = {0u, 0u};
        CHECK(a840_rt_compute_set_bindless(ce, 2u, tables, offsets) == A840_RT_OK);
        const uint32_t imm_consts[4] = {1u, 2u, 3u, 4u};
        CHECK(a840_rt_compute_set_consts_direct(ce, 0u, 1u, 128u, imm_consts) == A840_RT_OK);
        CHECK(a840_rt_compute_set_consts_indirect(ce, 2u, 2u, 128u, const_buf, 64u) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threadgroups(ce, 4u, 2u, 1u) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threads(ce, 19u, 11u, 1u) == A840_RT_OK);
        CHECK(a840_rt_compute_barrier(ce, A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        a840_rt_compute_destroy(ce);

        /* 3. Sysmem 3D Render stage: VS + FS + MRT + Depth + non-indexed & indexed draws */
        A840RtRenderPassDesc rp{};
        rp.color_count = 1u;
        rp.color_targets[0] = tex_tiled;
        rp.depth_target = depth_tex;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &rp, &re) == A840_RT_OK);
        auto vs = make_stage(A840_STAGE_VERTEX, 4u, 0u);
        auto fs = make_stage(A840_STAGE_FRAGMENT, 4u, 0u);
        auto prog = make_graphics_prog(vs, fs, vs_code, fs_code);
        CHECK(a840_rt_render_set_program(re, &prog) == A840_RT_OK);
        auto rstate = make_render_state(64u, 64u, true, false);
        CHECK(a840_rt_render_set_state(re, &rstate) == A840_RT_OK);

        A840RtDrawDesc d1{};
        d1.primitive = A840_PRIM_TRIANGLES;
        d1.index_type = A840_INDEX_NONE;
        d1.count = 3u;
        d1.instance_count = 1u;
        CHECK(a840_rt_render_draw(re, &d1) == A840_RT_OK);

        A840RtDrawDesc d2{};
        d2.primitive = A840_PRIM_TRIANGLES;
        d2.index_type = A840_INDEX_UINT32;
        d2.count = 6u;
        d2.instance_count = 1u;
        d2.index_capacity = 12u;
        d2.index_buffer = idx_buf;
        CHECK(a840_rt_render_draw(re, &d2) == A840_RT_OK);

        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        a840_rt_render_destroy(re);

        /* 4. Sync / Timestamp before commit */
        CHECK(a840_rt_cb_encode_mem_write64(cb, ts_buf, 0u, UINT64_C(0x123456789abcdef0)) == A840_RT_OK);
        CHECK(a840_rt_cb_encode_timestamp(cb, ts_buf, 8u, 0u, true) == A840_RT_OK);

        CallbackLog log;
        CallbackTag stg{&log, "single_sched"}, cmp{&log, "single_comp"};
        CHECK(a840_rt_cb_add_scheduled_handler(cb, record_cb_event, &stg) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cb, record_cb_event, &cmp) == A840_RT_OK);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_wait_until_completed(cb, 0u) == A840_RT_OK);
        CHECK(log.events.size() == 2u && log.events[0] == "single_sched" && log.events[1] == "single_comp");

        A840MockSubmissionView view{};
        CHECK(a840_mock_transport_get_submission(mock, 0u, &view) == A840_TRANSPORT_OK);
        auto pkts = decode_all_packets(view.words, view.dword_count);
        CHECK(pkts.size() > 50u);

        a840_rt_cb_release(cb);
        a840_rt_buffer_release(src_buf);
        a840_rt_buffer_release(dst_buf);
        a840_rt_buffer_release(cs_code);
        a840_rt_buffer_release(bindless_tbl);
        a840_rt_buffer_release(const_buf);
        a840_rt_buffer_release(vs_code);
        a840_rt_buffer_release(fs_code);
        a840_rt_buffer_release(idx_buf);
        a840_rt_buffer_release(ts_buf);
        a840_rt_texture_release(tex_lin);
        a840_rt_texture_release(tex_tiled);
        a840_rt_texture_release(depth_tex);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }

    begin_case("e2e_multi_cb_frame_pipeline_and_dump");
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = false;
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        auto t = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&t, A840_ROUTE_PHASE2_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_HIGH, &q) == A840_RT_OK);

        A840RtBuffer *upload_buf = create_buf(dev, 16384u, 4096u);
        A840RtBuffer *dst_buf = create_buf(dev, 16384u, 65536u);
        A840RtBuffer *cs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *bindless_b0 = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *bindless_b1 = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *consts_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *vs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *fs_code = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *sds_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *idx16_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *idx32_buf = create_buf(dev, 4096u, 4096u);
        A840RtBuffer *marker_buf = create_buf(dev, 4096u, 4096u);
        A840RtTexture *src_tex = create_tex2d(dev, 70u, 128u, 64u, A840_TILE_LINEAR);
        A840RtTexture *mrt0 = create_tex2d(dev, 80u, 128u, 64u, A840_TILE_TILED);
        A840RtTexture *mrt1 = create_tex2d(dev, 115u, 128u, 64u, A840_TILE_TILED);
        A840RtTexture *depth = create_tex2d(dev, 252u, 128u, 64u, A840_TILE_TILED);
        A840RtTexture *stencil = create_tex2d(dev, 253u, 128u, 64u, A840_TILE_TILED);

        /* Write hardware 64B texture descriptors into bindless table BO. */
        CHECK(a840_rt_buffer_write(bindless_b0, 0u, src_tex->tex_const, sizeof(src_tex->tex_const)) == A840_RT_OK);
        CHECK(a840_rt_buffer_write(bindless_b0, 64u, mrt0->tex_const, sizeof(mrt0->tex_const)) == A840_RT_OK);

        CallbackLog log;
        CallbackTag s1{&log, "cb1_sched"}, c1{&log, "cb1_comp"};
        CallbackTag s2{&log, "cb2_sched"}, c2{&log, "cb2_comp"};
        CallbackTag s3{&log, "cb3_sched"}, c3{&log, "cb3_comp"};

        /* CB1: Blit Copy + 2D Clear + 2D Blit */
        A840RtCmdBuffer *cb1 = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb1) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cb1, record_cb_event, &s1) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cb1, record_cb_event, &c1) == A840_RT_OK);
        {
            A840RtBlitEncoder *be = nullptr;
            CHECK(a840_rt_cb_begin_blit(cb1, &be) == A840_RT_OK);
            CHECK(a840_rt_blit_copy_buffer(be, upload_buf, 0u, dst_buf, 0u, 4096u) == A840_RT_OK);
            A840Rect2D r{0u, 0u, 128u, 64u};
            const uint32_t clr[4] = {16u, 32u, 48u, 255u};
            CHECK(a840_rt_blit_clear_2d(be, src_tex, &r, clr) == A840_RT_OK);
            CHECK(a840_rt_blit_barrier(be, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);
            CHECK(a840_rt_blit_2d(be, src_tex, &r, mrt0, &r, A840_FILTER_LINEAR) == A840_RT_OK);
            CHECK(a840_rt_blit_end(be) == A840_RT_OK);
            a840_rt_blit_destroy(be);
        }

        /* CB2: Compute Uniform & Non-Uniform Dispatch + Bindless + Direct/Indirect Consts */
        A840RtCmdBuffer *cb2 = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb2) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cb2, record_cb_event, &s2) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cb2, record_cb_event, &c2) == A840_RT_OK);
        {
            A840RtComputeEncoder *ce = nullptr;
            CHECK(a840_rt_cb_begin_compute(cb2, &ce) == A840_RT_OK);
            auto cs = make_stage(A840_STAGE_COMPUTE, 5u, 0u);
            cs.shared_mem_bytes = 16384u;
            A840RtComputePipelineDesc cpd{};
            cpd.stage = &cs;
            cpd.code_buffer = cs_code;
            cpd.bindless_tex = true;
            cpd.bindless_samp = true;
            cpd.bindless_uav = true;
            cpd.bindless_ubo = true;
            cpd.local_id_regid = 4u;
            cpd.wg_id_regid = 8u;
            CHECK(a840_rt_compute_set_pipeline(ce, &cpd) == A840_RT_OK);
            A840RtBuffer *btables[2] = {bindless_b0, bindless_b1};
            const uint64_t boffs[2] = {0u, 64u};
            CHECK(a840_rt_compute_set_bindless(ce, 2u, btables, boffs) == A840_RT_OK);
            const uint32_t cwords[8] = {10u, 20u, 30u, 40u, 50u, 60u, 70u, 80u};
            CHECK(a840_rt_compute_set_consts_direct(ce, 0u, 2u, 128u, cwords) == A840_RT_OK);
            CHECK(a840_rt_compute_set_consts_indirect(ce, 4u, 2u, 128u, consts_buf, 0u) == A840_RT_OK);
            CHECK(a840_rt_compute_dispatch_threadgroups(ce, 8u, 4u, 1u) == A840_RT_OK);
            CHECK(a840_rt_compute_dispatch_threads(ce, 35u, 21u, 1u) == A840_RT_OK);
            CHECK(a840_rt_compute_barrier(ce, A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);
            CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
            a840_rt_compute_destroy(ce);
        }

        /* CB3: Sysmem 3D Render (2 MRTs + Depth + Stencil + SDS + Non-Indexed + Indexed16 + Indexed32) + Sync */
        A840RtCmdBuffer *cb3 = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb3) == A840_RT_OK);
        CHECK(a840_rt_cb_add_scheduled_handler(cb3, record_cb_event, &s3) == A840_RT_OK);
        CHECK(a840_rt_cb_add_completed_handler(cb3, record_cb_event, &c3) == A840_RT_OK);
        {
            A840RtRenderPassDesc rp{};
            rp.color_count = 2u;
            rp.color_targets[0] = mrt0;
            rp.color_targets[1] = mrt1;
            rp.color_clear[0] = true;
            rp.color_clear_values[0][3] = 255u;
            rp.depth_target = depth;
            rp.stencil_target = stencil;

            A840RtRenderEncoder *re = nullptr;
            CHECK(a840_rt_cb_begin_render_pass(cb3, &rp, &re) == A840_RT_OK);
            auto vs = make_stage(A840_STAGE_VERTEX, 5u, 0u);
            auto fs = make_stage(A840_STAGE_FRAGMENT, 5u, 0u);
            auto prog = make_graphics_prog(vs, fs, vs_code, fs_code);
            prog.emit_desc.colors[1] = {4u, 0x0fu, true};
            CHECK(a840_rt_render_set_program(re, &prog) == A840_RT_OK);
            auto st = make_render_state(128u, 64u, true, true);
            CHECK(a840_rt_render_set_state(re, &st) == A840_RT_OK);

            A840RtSdsGroupDesc sds[2]{};
            sds[0] = {1u, A840_SDS_SYSMEM, 12u, sds_buf, 0u};
            sds[1] = {2u, A840_SDS_SYSMEM | A840_SDS_GMEM, 6u, sds_buf, 64u};
            CHECK(a840_rt_render_set_sds(re, sds, 2u) == A840_RT_OK);

            A840RtDrawDesc d0{};
            d0.primitive = A840_PRIM_TRIANGLES;
            d0.index_type = A840_INDEX_NONE;
            d0.count = 3u;
            d0.instance_count = 1u;
            CHECK(a840_rt_render_draw(re, &d0) == A840_RT_OK);

            A840RtDrawDesc d1{};
            d1.primitive = A840_PRIM_TRIANGLE_STRIP;
            d1.index_type = A840_INDEX_UINT16;
            d1.count = 6u;
            d1.instance_count = 2u;
            d1.index_capacity = 16u;
            d1.index_buffer = idx16_buf;
            CHECK(a840_rt_render_draw(re, &d1) == A840_RT_OK);

            A840RtDrawDesc d2{};
            d2.primitive = A840_PRIM_TRIANGLES;
            d2.index_type = A840_INDEX_UINT32;
            d2.count = 9u;
            d2.instance_count = 1u;
            d2.base_vertex = 4;
            d2.index_capacity = 18u;
            d2.index_buffer = idx32_buf;
            CHECK(a840_rt_render_draw(re, &d2) == A840_RT_OK);

            CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
            a840_rt_render_destroy(re);

            CHECK(a840_rt_cb_encode_mem_write64(cb3, marker_buf, 0u, UINT64_C(0x0102030405060708)) == A840_RT_OK);
            CHECK(a840_rt_cb_encode_timestamp(cb3, marker_buf, 8u, 0u, true) == A840_RT_OK);
        }

        CHECK(a840_rt_cb_commit(cb1) == A840_RT_OK);
        CHECK(a840_rt_cb_commit(cb2) == A840_RT_OK);
        CHECK(a840_rt_cb_commit(cb3) == A840_RT_OK);
        CHECK(a840_mock_transport_submission_count(mock) == 3u);
        CHECK(a840_rt_queue_pending_count(q) == 3u);

        a840_mock_transport_complete_all(mock);
        CHECK(a840_rt_queue_wait_idle(q, 0u) == A840_RT_OK);
        CHECK(a840_rt_queue_pending_count(q) == 0u);

        const std::vector<std::string> expected_fifo = {
            "cb1_sched", "cb2_sched", "cb3_sched",
            "cb1_comp", "cb2_comp", "cb3_comp"
        };
        CHECK(log.events == expected_fifo);

        size_t total_dwords = 0u;
        const uint32_t *words = a840_mock_transport_captured_words(mock, &total_dwords);
        CHECK(words != nullptr && total_dwords > 200u);
        auto pkts = decode_all_packets(words, total_dwords);
        CHECK(pkts.size() > 80u);

        if (dump_path != nullptr) {
            CHECK(a840_mock_transport_dump_pm4(mock, dump_path) == A840_TRANSPORT_OK);
            std::printf("DUMP: %s (%zu dwords, %zu packets, %zu submissions)\n",
                        dump_path, total_dwords, pkts.size(),
                        a840_mock_transport_submission_count(mock));
        }

        a840_rt_cb_release(cb1);
        a840_rt_cb_release(cb2);
        a840_rt_cb_release(cb3);
        a840_rt_buffer_release(upload_buf);
        a840_rt_buffer_release(dst_buf);
        a840_rt_buffer_release(cs_code);
        a840_rt_buffer_release(bindless_b0);
        a840_rt_buffer_release(bindless_b1);
        a840_rt_buffer_release(consts_buf);
        a840_rt_buffer_release(vs_code);
        a840_rt_buffer_release(fs_code);
        a840_rt_buffer_release(sds_buf);
        a840_rt_buffer_release(idx16_buf);
        a840_rt_buffer_release(idx32_buf);
        a840_rt_buffer_release(marker_buf);
        a840_rt_texture_release(src_tex);
        a840_rt_texture_release(mrt0);
        a840_rt_texture_release(mrt1);
        a840_rt_texture_release(depth);
        a840_rt_texture_release(stencil);
        a840_rt_queue_destroy(q);
        a840_rt_device_destroy(dev);
        CHECK(a840_mock_transport_live_bo_count(mock) == 0u);
        CHECK(a840_mock_transport_live_queue_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }
}

int main(int argc, char **argv)
{
    const char *dump_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-submitted-pm4") == 0 && i + 1 < argc && !dump_path) {
            dump_path = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--dump-submitted-pm4 <path>]\n", argv[0]);
            return 2;
        }
    }

    test_group_a_router();
    test_group_b_transport();
    test_group_c_device_resources();
    test_group_d_cmd_and_negative();
    test_group_e_e2e_and_dump(dump_path);

    CHECK(g_cases >= 55u);
    std::printf("PASS: W03-L3 runtime core: %u independent cases, %u assertions\n",
                g_cases, g_checks);
    return 0;
}
