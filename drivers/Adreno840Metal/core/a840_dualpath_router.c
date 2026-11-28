/* SPDX-License-Identifier: MIT */
#include "a840_dualpath_router.h"

#include <stdio.h>
#include <string.h>

#include "Adreno840Caps.h"
#include "a840_emit_ops.h"

enum {
    A840_CAT_IDX_BLIT = 0u,
    A840_CAT_IDX_COMPUTE = 1u,
    A840_CAT_IDX_RENDER = 2u,
    A840_CAT_IDX_RESOURCE = 3u,
    A840_CAT_IDX_COMPILER = 4u,
    A840_CAT_IDX_SYNC = 5u
};

/* Phase 3 (bits 6..9, matching a840_ca_render_bridge.h) and Phase 4
 * (bits 10..13 in a840_argbuf_icb.h, bits 14..16 in a840_phase4_pipeline.h)
 * capability bits stored in upper bits of enabled_categories without altering
 * Phase 2 A840_ROUTE_ALL (bits 0..5). */
enum {
    A840_ROUTE_P3_GMEM = 1u << 6,
    A840_ROUTE_P3_UBWC = 1u << 7,
    A840_ROUTE_P3_FBFETCH = 1u << 8,
    A840_ROUTE_P3_XR10_YCBCR = 1u << 9,
    A840_ROUTE_P4_HEAP_ALIASING = 1u << 10,
    A840_ROUTE_P4_INDIRECT_COMPUTE = 1u << 11,
    A840_ROUTE_P4_INDIRECT_RENDER_ICB = 1u << 12,
    A840_ROUTE_P4_SPARSE = 1u << 13,
    A840_ROUTE_P4_MSAA_RESOLVE = 1u << 14,
    A840_ROUTE_P4_TESS_AMPLIFY = 1u << 15,
    A840_ROUTE_P4_COMPILER_DAEMON = 1u << 16,
    A840_ROUTE_VALID_MASK = (uint32_t)A840_ROUTE_ALL |
                            A840_ROUTE_P3_GMEM |
                            A840_ROUTE_P3_UBWC |
                            A840_ROUTE_P3_FBFETCH |
                            A840_ROUTE_P3_XR10_YCBCR |
                            A840_ROUTE_P4_HEAP_ALIASING |
                            A840_ROUTE_P4_INDIRECT_COMPUTE |
                            A840_ROUTE_P4_INDIRECT_RENDER_ICB |
                            A840_ROUTE_P4_SPARSE |
                            A840_ROUTE_P4_MSAA_RESOLVE |
                            A840_ROUTE_P4_TESS_AMPLIFY |
                            A840_ROUTE_P4_COMPILER_DAEMON
};

/* Verbatim strings from contracts/DualPathPredicates.tsv (16 rows). */
static const char *const k_predicate_strings[A840_PRED_COUNT + 1] = {
    NULL,
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

/* Matches the exact ordinary uncompressed color formats handled by a840_emit_blit. */
static bool is_supported_2d_blit_color_fmt(uint32_t fmt_id, uint32_t *out_class)
{
    /* class: 0 = float/unorm/snorm, 1 = uint, 2 = sint */
    switch (fmt_id) {
    case A840_FMT6_A8_UNORM:
    case A840_FMT6_8_UNORM:
    case A840_FMT6_8_SNORM:
    case A840_FMT6_8_8_UNORM:
    case A840_FMT6_8_8_SNORM:
    case A840_FMT6_16_UNORM:
    case A840_FMT6_16_SNORM:
    case A840_FMT6_16_FLOAT:
    case A840_FMT6_8_8_8_8_UNORM:
    case A840_FMT6_8_8_8_X8_UNORM:
    case A840_FMT6_4_4_4_4_UNORM:
    case A840_FMT6_5_5_5_1_UNORM:
    case A840_FMT6_1_5_5_5_UNORM:
    case A840_FMT6_5_6_5_UNORM:
    case A840_FMT6_8_8_8_8_SNORM:
    case A840_FMT6_9_9_9_E5_FLOAT:
    case A840_FMT6_10_10_10_2_UNORM:
    case A840_FMT6_10_10_10_2_UNORM_DEST:
    case A840_FMT6_11_11_10_FLOAT:
    case A840_FMT6_16_16_UNORM:
    case A840_FMT6_16_16_SNORM:
    case A840_FMT6_16_16_FLOAT:
    case A840_FMT6_32_FLOAT:
    case A840_FMT6_16_16_16_16_UNORM:
    case A840_FMT6_16_16_16_16_SNORM:
    case A840_FMT6_16_16_16_16_FLOAT:
    case A840_FMT6_32_32_FLOAT:
    case A840_FMT6_32_32_32_32_FLOAT:
        if (out_class) {
            *out_class = 0u;
        }
        return true;
    case A840_FMT6_8_UINT:
    case A840_FMT6_8_8_UINT:
    case A840_FMT6_16_UINT:
    case A840_FMT6_8_8_8_8_UINT:
    case A840_FMT6_10_10_10_2_UINT:
    case A840_FMT6_16_16_UINT:
    case A840_FMT6_32_UINT:
    case A840_FMT6_16_16_16_16_UINT:
    case A840_FMT6_32_32_UINT:
    case A840_FMT6_32_32_32_32_UINT:
        if (out_class) {
            *out_class = 1u;
        }
        return true;
    case A840_FMT6_8_SINT:
    case A840_FMT6_8_8_SINT:
    case A840_FMT6_16_SINT:
    case A840_FMT6_8_8_8_8_SINT:
    case A840_FMT6_16_16_SINT:
    case A840_FMT6_32_SINT:
    case A840_FMT6_16_16_16_16_SINT:
    case A840_FMT6_32_32_SINT:
    case A840_FMT6_32_32_32_32_SINT:
        if (out_class) {
            *out_class = 2u;
        }
        return true;
    default:
        return false;
    }
}

static A840RouteDecision record_raw(A840DualPathRouter *router, uint32_t cat_idx)
{
    A840RouteDecision d;
    d.path = A840_ROUTE_PATH_RAW;
    d.predicate_id = A840_PRED_NONE;
    d.predicate = NULL;
    if (router) {
        router->raw_hits++;
        if (cat_idx < 6u) {
            router->raw_hits_by_category[cat_idx]++;
        }
    }
    return d;
}

static A840RouteDecision record_fallback(A840DualPathRouter *router,
                                         A840FallbackPredicate pred)
{
    A840RouteDecision d;
    if (pred < A840_PRED_UNIMPLEMENTED_GLOBAL || pred > A840_PRED_COUNT) {
        pred = A840_PRED_UNIMPLEMENTED_GLOBAL;
    }
    d.path = A840_ROUTE_PATH_FALLBACK;
    d.predicate_id = pred;
    d.predicate = k_predicate_strings[pred];
    if (router) {
        router->fallback_total++;
        router->predicate_counts[pred]++;
    }
    return d;
}

void a840_router_init(A840DualPathRouter *router, uint32_t initial_categories)
{
    if (!router) {
        return;
    }
    memset(router, 0, sizeof(*router));
    router->enabled_categories = initial_categories & (uint32_t)A840_ROUTE_VALID_MASK;
    router->gmem_engine_enabled = (router->enabled_categories & A840_ROUTE_P3_GMEM) != 0u;
}

void a840_router_set_categories(A840DualPathRouter *router, uint32_t categories)
{
    if (!router) {
        return;
    }
    router->enabled_categories = categories & (uint32_t)A840_ROUTE_VALID_MASK;
}

void a840_router_enable_category(A840DualPathRouter *router, uint32_t category_bits)
{
    if (!router) {
        return;
    }
    router->enabled_categories |= (category_bits & (uint32_t)A840_ROUTE_VALID_MASK);
}

void a840_router_disable_category(A840DualPathRouter *router, uint32_t category_bits)
{
    if (!router) {
        return;
    }
    router->enabled_categories &= ~(category_bits & (uint32_t)A840_ROUTE_VALID_MASK);
}

void a840_router_set_gmem_engine(A840DualPathRouter *router, bool enabled)
{
    if (!router) {
        return;
    }
    router->gmem_engine_enabled = enabled;
}

void a840_router_reset_stats(A840DualPathRouter *router)
{
    if (!router) {
        return;
    }
    router->raw_hits = 0u;
    memset(router->raw_hits_by_category, 0, sizeof(router->raw_hits_by_category));
    router->fallback_total = 0u;
    memset(router->predicate_counts, 0, sizeof(router->predicate_counts));
}

const char *a840_router_predicate_name(A840FallbackPredicate pred)
{
    if (pred < A840_PRED_UNIMPLEMENTED_GLOBAL || pred > A840_PRED_COUNT) {
        return NULL;
    }
    return k_predicate_strings[pred];
}

A840FallbackPredicate a840_router_lookup_predicate(const char *predicate_name)
{
    int i;
    if (!predicate_name || !predicate_name[0]) {
        return A840_PRED_NONE;
    }
    for (i = (int)A840_PRED_UNIMPLEMENTED_GLOBAL; i <= (int)A840_PRED_COUNT; ++i) {
        if (strcmp(k_predicate_strings[i], predicate_name) == 0) {
            return (A840FallbackPredicate)i;
        }
    }
    return A840_PRED_NONE;
}

uint64_t a840_router_predicate_count(const A840DualPathRouter *router,
                                     A840FallbackPredicate pred)
{
    if (!router || pred < A840_PRED_UNIMPLEMENTED_GLOBAL || pred > A840_PRED_COUNT) {
        return 0u;
    }
    return router->predicate_counts[pred];
}

A840RouteDecision a840_router_eval_blit(A840DualPathRouter *router,
                                        const A840BlitRouteQuery *query)
{
    const A840FormatCaps *dst_caps;
    const A840FormatCaps *src_caps = NULL;
    uint32_t dst_class = 0u;
    uint32_t src_class = 0u;
    bool ubwc_ready;

    if (!router || !query || (router->enabled_categories & (uint32_t)A840_ROUTE_BLIT) == 0u) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if (query->is_buffer_copy) {
        return record_raw(router, A840_CAT_IDX_BLIT);
    }

    dst_caps = a840_format_caps_lookup(query->dst_mtl_format);
    if (!query->is_clear) {
        src_caps = a840_format_caps_lookup(query->src_mtl_format);
    }

    ubwc_ready = (router->enabled_categories & A840_ROUTE_P3_UBWC) != 0u;
    if (query->tex_type == A840_TEX_3D ||
        (dst_caps && (dst_caps->flags & A840_FORMAT_COMPRESSED) != 0u) ||
        (src_caps && (src_caps->flags & A840_FORMAT_COMPRESSED) != 0u)) {
        return record_fallback(router, A840_PRED_BLIT_3D_OR_COMPRESSED);
    }
    if ((query->dst_tile_mode == A840_TILE_UBWC &&
         (!ubwc_ready || !dst_caps || !dst_caps->ubwc_supported)) ||
        (!query->is_clear && query->src_tile_mode == A840_TILE_UBWC &&
         (!ubwc_ready || !src_caps || !src_caps->ubwc_supported))) {
        return record_fallback(router, A840_PRED_BLIT_3D_OR_COMPRESSED);
    }

    if (query->is_ycbcr || !dst_caps ||
        (dst_caps->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_REQUIRES_XR_CODEC)) != 0u ||
        !is_supported_2d_blit_color_fmt(dst_caps->color_fmt_id, &dst_class)) {
        return record_fallback(router, A840_PRED_BLIT_UNSUPPORTED_FORMAT);
    }

    if (!query->is_clear) {
        if (!src_caps ||
            (src_caps->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_REQUIRES_XR_CODEC)) != 0u ||
            !is_supported_2d_blit_color_fmt(src_caps->color_fmt_id, &src_class)) {
            return record_fallback(router, A840_PRED_BLIT_UNSUPPORTED_FORMAT);
        }
        if (src_class != dst_class ||
            (query->filter == A840_FILTER_LINEAR && src_class != 0u)) {
            return record_fallback(router, A840_PRED_BLIT_UNSUPPORTED_FORMAT);
        }
    }

    return record_raw(router, A840_CAT_IDX_BLIT);
}

A840RouteDecision a840_router_eval_compute(A840DualPathRouter *router,
                                           const A840ComputeRouteQuery *query)
{
    if (!router || !query || (router->enabled_categories & (uint32_t)A840_ROUTE_COMPUTE) == 0u) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if ((query->is_indirect_dispatch || query->uses_icb) &&
        (router->enabled_categories & A840_ROUTE_P4_INDIRECT_COMPUTE) == 0u) {
        return record_fallback(router, A840_PRED_COMPUTE_INDIRECT_DISPATCH);
    }
    if (!query->has_compiled_kernel || !query->has_valid_local_size) {
        if (query->requires_cold_compile) {
            bool daemon_ready = query->compiler_daemon_available ||
                                ((router->enabled_categories & A840_ROUTE_P4_COMPILER_DAEMON) != 0u);
            if (!daemon_ready) {
                return record_fallback(router, A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
            }
            if (query->has_valid_local_size) {
                return record_raw(router, A840_CAT_IDX_COMPUTE);
            }
        }
        return record_fallback(router, A840_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }
    return record_raw(router, A840_CAT_IDX_COMPUTE);
}

A840RouteDecision a840_router_eval_render(A840DualPathRouter *router,
                                          const A840RenderRouteQuery *query)
{
    uint32_t i;
    bool gmem_ready;
    bool ubwc_ready;
    bool fbfetch_ready;
    bool xr10_ycbcr_ready;

    if (!router || !query ||
        (router->enabled_categories & ((uint32_t)A840_ROUTE_RENDER_SYSMEM | A840_ROUTE_P3_GMEM)) == 0u) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if (query->color_attachment_count > 8u) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }

    gmem_ready = router->gmem_engine_enabled ||
                 ((router->enabled_categories & A840_ROUTE_P3_GMEM) != 0u);
    ubwc_ready = (router->enabled_categories & A840_ROUTE_P3_UBWC) != 0u;
    fbfetch_ready = gmem_ready &&
                    ((router->enabled_categories & A840_ROUTE_P3_FBFETCH) != 0u);
    xr10_ycbcr_ready = (router->enabled_categories & A840_ROUTE_P3_XR10_YCBCR) != 0u;

    /* 1. Multi-plane YCbCr or XR10 stand-in formats. */
    if (query->has_ycbcr_attachment && !xr10_ycbcr_ready) {
        return record_fallback(router, A840_PRED_RENDER_YCBCR_OR_XR10);
    }
    for (i = 0; i < query->color_attachment_count; ++i) {
        if (query->color_mtl_formats[i] != 0u) {
            const A840FormatCaps *caps = a840_format_caps_lookup(query->color_mtl_formats[i]);
            if (caps && (caps->flags & A840_FORMAT_REQUIRES_XR_CODEC) != 0u && !xr10_ycbcr_ready) {
                return record_fallback(router, A840_PRED_RENDER_YCBCR_OR_XR10);
            }
        }
    }

    /* 2. UBWC 6.0 compressed attachments (enabled by A840_ROUTE_UBWC). */
    for (i = 0; i < query->color_attachment_count; ++i) {
        if (query->color_mtl_formats[i] != 0u &&
            query->color_tile_modes[i] == A840_TILE_UBWC) {
            const A840FormatCaps *caps = a840_format_caps_lookup(query->color_mtl_formats[i]);
            if (!ubwc_ready || !caps || !caps->ubwc_supported) {
                return record_fallback(router, A840_PRED_RENDER_UBWC_ATTACHMENT);
            }
        }
    }
    if (query->depth_mtl_format != 0u && query->depth_tile_mode == A840_TILE_UBWC) {
        const A840FormatCaps *dcaps = a840_format_caps_lookup(query->depth_mtl_format);
        if (!ubwc_ready || !dcaps || !dcaps->ubwc_supported) {
            return record_fallback(router, A840_PRED_RENDER_UBWC_ATTACHMENT);
        }
    }

    /* 3. TBDR GMEM requirement (memoryless / fbfetch / MSAA resolve). */
    if (!gmem_ready &&
        (query->has_memoryless_attachment || query->uses_fbfetch || query->has_msaa_resolve)) {
        return record_fallback(router, A840_PRED_RENDER_GMEM_REQUIRED);
    }

    /* 4. Framebuffer fetch / programmable blending (enabled by GMEM + FBFETCH). */
    if ((query->uses_fbfetch || query->uses_programmable_blend) && !fbfetch_ready) {
        return record_fallback(router, A840_PRED_RENDER_FBFETCH_BLEND);
    }

    /* 5. Multisample render / resolve pass (Phase 4). */
    if (query->has_msaa_resolve || query->sample_count > 1u) {
        bool msaa_ready = (router->enabled_categories & A840_ROUTE_P4_MSAA_RESOLVE) != 0u;
        bool valid_samples = (query->sample_count <= 1u ||
                              query->sample_count == 2u ||
                              query->sample_count == 4u);
        if (!msaa_ready || !valid_samples) {
            return record_fallback(router, A840_PRED_RENDER_MSAA_RESOLVE);
        }
    }

    /* 6. Tessellation or vertex amplification (Phase 4). */
    if ((query->uses_tessellation || query->uses_vertex_amplification) &&
        (router->enabled_categories & A840_ROUTE_P4_TESS_AMPLIFY) == 0u) {
        return record_fallback(router, A840_PRED_RENDER_TESS_OR_AMPLIFY);
    }

    /* 7. Indirect draw or Indirect Command Buffer (Phase 4). */
    if ((query->uses_indirect_draw || query->uses_icb) &&
        (router->enabled_categories & A840_ROUTE_P4_INDIRECT_RENDER_ICB) == 0u) {
        return record_fallback(router, A840_PRED_RENDER_INDIRECT_OR_ICB);
    }

    /* 8. Validate color & depth formats are known and renderable in Adreno840Caps.h. */
    for (i = 0; i < query->color_attachment_count; ++i) {
        if (query->color_mtl_formats[i] != 0u) {
            const A840FormatCaps *caps = a840_format_caps_lookup(query->color_mtl_formats[i]);
            bool xr_ok = xr10_ycbcr_ready && caps &&
                         ((caps->flags & A840_FORMAT_REQUIRES_XR_CODEC) != 0u);
            if (!caps || (!caps->renderable && !xr_ok) ||
                (caps->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_COMPRESSED)) != 0u) {
                return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
            }
        }
    }
    if (query->depth_mtl_format != 0u) {
        const A840FormatCaps *dcaps = a840_format_caps_lookup(query->depth_mtl_format);
        if (!dcaps || !dcaps->renderable || (dcaps->flags & A840_FORMAT_DEPTH) == 0u ||
            query->depth_tile_mode == A840_TILE_LINEAR) {
            return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
        }
    }

    return record_raw(router, A840_CAT_IDX_RENDER);
}

A840RouteDecision a840_router_eval_resource(A840DualPathRouter *router,
                                            const A840ResourceRouteQuery *query)
{
    if (!router || !query || router->enabled_categories == A840_ROUTE_NONE) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if (query->uses_sparse_texture &&
        (router->enabled_categories & A840_ROUTE_P4_SPARSE) == 0u) {
        return record_fallback(router, A840_PRED_RESOURCE_SPARSE_TEXTURE);
    }
    if (query->uses_heap_aliasing &&
        (router->enabled_categories & A840_ROUTE_P4_HEAP_ALIASING) == 0u) {
        return record_fallback(router, A840_PRED_RESOURCE_HEAP_ALIASING);
    }
    return record_raw(router, A840_CAT_IDX_RESOURCE);
}

A840RouteDecision a840_router_eval_compiler(A840DualPathRouter *router,
                                            const A840CompilerRouteQuery *query)
{
    if (!router || !query || router->enabled_categories == A840_ROUTE_NONE) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if (!query->precompiled_available && !query->daemon_available &&
        (router->enabled_categories & A840_ROUTE_P4_COMPILER_DAEMON) == 0u) {
        return record_fallback(router, A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
    }
    return record_raw(router, A840_CAT_IDX_COMPILER);
}

A840RouteDecision a840_router_eval_sync(A840DualPathRouter *router,
                                        const A840SyncRouteQuery *query)
{
    if (!router || !query || router->enabled_categories == A840_ROUTE_NONE) {
        return record_fallback(router, A840_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if ((query->cross_process_shared_event || query->uses_writer_promise) &&
        !query->abi_v3_supported) {
        return record_fallback(router, A840_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
    }
    return record_raw(router, A840_CAT_IDX_SYNC);
}

size_t a840_router_format_decision(const A840RouteDecision *decision,
                                   char *buf,
                                   size_t buf_bytes)
{
    int written;
    if (!decision || !buf || buf_bytes == 0u) {
        return 0u;
    }
    if (decision->path == A840_ROUTE_PATH_RAW) {
        written = snprintf(buf, buf_bytes, "path=raw");
    } else {
        const char *pred = decision->predicate ? decision->predicate
                                               : k_predicate_strings[A840_PRED_UNIMPLEMENTED_GLOBAL];
        written = snprintf(buf, buf_bytes, "fallback:%s", pred);
    }
    return written > 0 ? (size_t)written : 0u;
}
