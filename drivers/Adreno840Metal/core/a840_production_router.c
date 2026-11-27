/* SPDX-License-Identifier: MIT */
#include "a840_production_router.h"

#include <stdio.h>
#include <string.h>

#include "Adreno840Caps.h"

#if !defined(A840_PRODUCTION_FALLBACK_COMPILED_OUT) || (A840_PRODUCTION_FALLBACK_COMPILED_OUT != 1u)
#error "A840_PRODUCTION_FALLBACK_COMPILED_OUT must remain locked to 1"
#endif

/* Complete 16/16 predicate table aligned verbatim with contracts/DualPathPredicates.tsv.
 * Every single predicate is permanently closed to A840_PROD_ROUTE_NATIVE in Phase 6. */
static const A840ProdPredicateInfo k_prod_predicates[A840_PROD_PREDICATE_COUNT] = {
    {
        A840_PROD_PRED_UNIMPLEMENTED_GLOBAL,
        "raw:unimplemented.global",
        A840_PROD_CAT_ROUTER,
        "L3",
        6u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT,
        "raw:blit.unsupported_format",
        A840_PROD_CAT_BLIT,
        "L3",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_BLIT_3D_OR_COMPRESSED,
        "raw:blit.3d_or_compressed",
        A840_PROD_CAT_BLIT,
        "L3",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL,
        "raw:compute.uncompiled_kernel",
        A840_PROD_CAT_COMPUTE,
        "L2",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_COMPUTE_INDIRECT_DISPATCH,
        "raw:compute.indirect_dispatch",
        A840_PROD_CAT_COMPUTE,
        "L3",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_GMEM_REQUIRED,
        "raw:render.gmem_required",
        A840_PROD_CAT_RENDER,
        "L3",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_UBWC_ATTACHMENT,
        "raw:render.ubwc_attachment",
        A840_PROD_CAT_RENDER,
        "L3",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_YCBCR_OR_XR10,
        "raw:render.ycbcr_or_xr10",
        A840_PROD_CAT_RENDER,
        "L3",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_FBFETCH_BLEND,
        "raw:render.fbfetch_blend",
        A840_PROD_CAT_RENDER,
        "L2",
        3u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_TESS_OR_AMPLIFY,
        "raw:render.tess_or_amplify",
        A840_PROD_CAT_RENDER,
        "L3",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_INDIRECT_OR_ICB,
        "raw:render.indirect_or_icb",
        A840_PROD_CAT_RENDER,
        "L3",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RENDER_MSAA_RESOLVE,
        "raw:render.msaa_resolve",
        A840_PROD_CAT_RENDER,
        "L3",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RESOURCE_HEAP_ALIASING,
        "raw:resource.heap_aliasing",
        A840_PROD_CAT_RESOURCE,
        "L3",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_RESOURCE_SPARSE_TEXTURE,
        "raw:resource.sparse_texture",
        A840_PROD_CAT_RESOURCE,
        "L1",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_COMPILER_DAEMON_UNAVAILABLE,
        "raw:compiler.daemon_unavailable",
        A840_PROD_CAT_COMPILER,
        "L2",
        4u,
        A840_PROD_ROUTE_NATIVE
    },
    {
        A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC,
        "raw:sync.shared_event_cross_proc",
        A840_PROD_CAT_SYNC,
        "L1",
        3u,
        A840_PROD_ROUTE_NATIVE
    }
};

static bool predicate_id_valid(A840ProdPredicateId pred)
{
    return pred >= A840_PROD_PRED_UNIMPLEMENTED_GLOBAL &&
           pred <= A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC;
}

static A840ProdDecision make_native(A840ProdRouter *router,
                                    A840ProdCategory cat,
                                    A840ProdPredicateId pred)
{
    A840ProdDecision d;
    d.status = A840_PROD_OK;
    d.target = A840_PROD_ROUTE_NATIVE;
    d.predicate_id = pred;
    d.predicate_name = predicate_id_valid(pred)
                           ? k_prod_predicates[(uint32_t)pred - 1u].name
                           : NULL;
    if (router) {
        router->native_hits++;
        if ((uint32_t)cat < A840_PROD_CATEGORY_COUNT) {
            router->native_hits_by_category[(uint32_t)cat]++;
        }
        if (predicate_id_valid(pred)) {
            router->predicate_native_hits[(uint32_t)pred]++;
        }
    }
    return d;
}

static A840ProdDecision make_reject(A840ProdRouter *router,
                                    A840ProdStatus status,
                                    A840ProdPredicateId pred)
{
    A840ProdDecision d;
    d.status = status;
    d.target = A840_PROD_ROUTE_REJECT;
    d.predicate_id = predicate_id_valid(pred) ? pred : A840_PROD_PRED_NONE;
    d.predicate_name = predicate_id_valid(pred)
                           ? k_prod_predicates[(uint32_t)pred - 1u].name
                           : NULL;
    if (router) {
        if (status == A840_PROD_ERR_FALLBACK_FORBIDDEN) {
            router->fallback_denied_count++;
        } else {
            router->unsupported_rejected_count++;
        }
    }
    return d;
}

A840ProdStatus a840_prod_router_init(A840ProdRouter *router)
{
    if (!router) {
        return A840_PROD_ERR_INVALID_ARG;
    }
    memset(router, 0, sizeof(*router));
    router->abi_version = A840_PROD_ROUTER_ABI_VERSION;
    router->fallback_compiled_out = A840_PRODUCTION_FALLBACK_COMPILED_OUT;
    return A840_PROD_OK;
}

void a840_prod_router_reset_stats(A840ProdRouter *router)
{
    if (!router) {
        return;
    }
    router->native_hits = 0u;
    memset(router->native_hits_by_category, 0, sizeof(router->native_hits_by_category));
    memset(router->predicate_native_hits, 0, sizeof(router->predicate_native_hits));
    router->fallback_denied_count = 0u;
    router->unsupported_rejected_count = 0u;
}

A840ProdStatus a840_prod_route_request_fallback(A840ProdRouter *router)
{
    if (router) {
        router->fallback_denied_count++;
    }
    return A840_PROD_ERR_FALLBACK_FORBIDDEN;
}

const char *a840_prod_predicate_name(A840ProdPredicateId pred)
{
    if (!predicate_id_valid(pred)) {
        return NULL;
    }
    return k_prod_predicates[(uint32_t)pred - 1u].name;
}

A840ProdPredicateId a840_prod_lookup_predicate(const char *predicate_name)
{
    uint32_t i;
    if (!predicate_name || predicate_name[0] == '\0') {
        return A840_PROD_PRED_NONE;
    }
    for (i = 0u; i < A840_PROD_PREDICATE_COUNT; ++i) {
        if (strcmp(k_prod_predicates[i].name, predicate_name) == 0) {
            return k_prod_predicates[i].id;
        }
    }
    return A840_PROD_PRED_NONE;
}

bool a840_prod_predicate_info(A840ProdPredicateId pred, A840ProdPredicateInfo *out)
{
    if (!predicate_id_valid(pred) || !out) {
        return false;
    }
    *out = k_prod_predicates[(uint32_t)pred - 1u];
    return true;
}

uint64_t a840_prod_predicate_hits(const A840ProdRouter *router, A840ProdPredicateId pred)
{
    if (!router || !predicate_id_valid(pred)) {
        return 0u;
    }
    return router->predicate_native_hits[(uint32_t)pred];
}

A840ProdDecision a840_prod_route_eval_predicate(A840ProdRouter *router,
                                                A840ProdPredicateId pred,
                                                bool request_fallback)
{
    if (request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN, pred);
    }
    if (!predicate_id_valid(pred)) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED, A840_PROD_PRED_NONE);
    }
    return make_native(router, k_prod_predicates[(uint32_t)pred - 1u].category, pred);
}

A840ProdDecision a840_prod_route_eval_by_name(A840ProdRouter *router,
                                              const char *predicate_name,
                                              bool request_fallback)
{
    const A840ProdPredicateId pred = a840_prod_lookup_predicate(predicate_name);
    if (request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN, pred);
    }
    if (!predicate_id_valid(pred)) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED, A840_PROD_PRED_NONE);
    }
    return make_native(router, k_prod_predicates[(uint32_t)pred - 1u].category, pred);
}

A840ProdDecision a840_prod_route_blit(A840ProdRouter *router,
                                      const A840ProdBlitQuery *query)
{
    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT);
    }
    if (query->is_buffer_copy) {
        return make_native(router, A840_PROD_CAT_BLIT, A840_PROD_PRED_UNIMPLEMENTED_GLOBAL);
    }
    if (query->width == 0u || query->height == 0u || query->depth == 0u) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED, A840_PROD_PRED_NONE);
    }
    if (!query->is_ycbcr) {
        const A840FormatCaps *dst_caps = a840_format_caps_lookup(query->dst_mtl_format);
        if (!dst_caps) {
            return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                               A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT);
        }
        if (!query->is_clear) {
            const A840FormatCaps *src_caps = a840_format_caps_lookup(query->src_mtl_format);
            if (!src_caps) {
                return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                                   A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT);
            }
        }
    }
    if (query->is_3d_or_compressed || query->depth > 1u) {
        return make_native(router, A840_PROD_CAT_BLIT, A840_PROD_PRED_BLIT_3D_OR_COMPRESSED);
    }
    return make_native(router, A840_PROD_CAT_BLIT, A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT);
}

A840ProdDecision a840_prod_route_compute(A840ProdRouter *router,
                                         const A840ProdComputeQuery *query)
{
    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }
    if (!query->has_compiled_kernel && !query->uses_daemon_compile) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                           A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }
    if (!query->has_valid_local_size) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                           A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }
    if (query->is_indirect_dispatch || query->uses_icb) {
        return make_native(router, A840_PROD_CAT_COMPUTE,
                           A840_PROD_PRED_COMPUTE_INDIRECT_DISPATCH);
    }
    return make_native(router, A840_PROD_CAT_COMPUTE,
                       A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL);
}

A840ProdDecision a840_prod_route_render(A840ProdRouter *router,
                                        const A840ProdRenderQuery *query)
{
    A840ProdPredicateId pred = A840_PROD_PRED_UNIMPLEMENTED_GLOBAL;

    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_RENDER_GMEM_REQUIRED);
    }
    if (query->color_attachment_count > A840_PROD_MAX_COLOR_ATTACHMENTS) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED, A840_PROD_PRED_NONE);
    }
    /* Adreno 840 hardware MSAA supports 1x, 2x, 4x (0 defaults to 1x); reject 8x/others. */
    if (query->sample_count != 0u && query->sample_count != 1u &&
        query->sample_count != 2u && query->sample_count != 4u) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                           A840_PROD_PRED_RENDER_MSAA_RESOLVE);
    }

    if (query->has_msaa_resolve || query->sample_count > 1u) {
        pred = A840_PROD_PRED_RENDER_MSAA_RESOLVE;
    } else if (query->uses_tess_or_amplify) {
        pred = A840_PROD_PRED_RENDER_TESS_OR_AMPLIFY;
    } else if (query->uses_indirect_or_icb) {
        pred = A840_PROD_PRED_RENDER_INDIRECT_OR_ICB;
    } else if (query->uses_fbfetch_or_prog_blend) {
        pred = A840_PROD_PRED_RENDER_FBFETCH_BLEND;
    } else if (query->has_ycbcr_or_xr10) {
        pred = A840_PROD_PRED_RENDER_YCBCR_OR_XR10;
    } else if (query->has_ubwc_attachment) {
        pred = A840_PROD_PRED_RENDER_UBWC_ATTACHMENT;
    } else if (query->uses_gmem || query->has_memoryless_attachment) {
        pred = A840_PROD_PRED_RENDER_GMEM_REQUIRED;
    }
    return make_native(router, A840_PROD_CAT_RENDER, pred);
}

A840ProdDecision a840_prod_route_resource(A840ProdRouter *router,
                                          const A840ProdResourceQuery *query)
{
    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_RESOURCE_HEAP_ALIASING);
    }
    if (query->uses_sparse_texture) {
        return make_native(router, A840_PROD_CAT_RESOURCE,
                           A840_PROD_PRED_RESOURCE_SPARSE_TEXTURE);
    }
    return make_native(router, A840_PROD_CAT_RESOURCE,
                       A840_PROD_PRED_RESOURCE_HEAP_ALIASING);
}

A840ProdDecision a840_prod_route_compiler(A840ProdRouter *router,
                                          const A840ProdCompilerQuery *query)
{
    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_COMPILER_DAEMON_UNAVAILABLE);
    }
    if (!query->precompiled_available && !query->daemon_available) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                           A840_PROD_PRED_COMPILER_DAEMON_UNAVAILABLE);
    }
    return make_native(router, A840_PROD_CAT_COMPILER,
                       A840_PROD_PRED_COMPILER_DAEMON_UNAVAILABLE);
}

A840ProdDecision a840_prod_route_sync(A840ProdRouter *router,
                                      const A840ProdSyncQuery *query)
{
    if (!query) {
        return make_reject(router, A840_PROD_ERR_INVALID_ARG, A840_PROD_PRED_NONE);
    }
    if (query->request_fallback) {
        return make_reject(router, A840_PROD_ERR_FALLBACK_FORBIDDEN,
                           A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
    }
    if ((query->cross_process_shared_event || query->uses_writer_promise) &&
        !query->abi_v3_supported) {
        return make_reject(router, A840_PROD_ERR_UNSUPPORTED,
                           A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
    }
    return make_native(router, A840_PROD_CAT_SYNC,
                       A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
}

size_t a840_prod_format_decision(const A840ProdDecision *decision,
                                 char *buf,
                                 size_t buf_bytes)
{
    const char *text;
    size_t len;

    if (!decision) {
        return 0u;
    }
    if (decision->status == A840_PROD_OK && decision->target == A840_PROD_ROUTE_NATIVE) {
        text = "path=native";
    } else if (decision->status == A840_PROD_ERR_FALLBACK_FORBIDDEN) {
        text = "reject:fallback_forbidden";
    } else if (decision->status == A840_PROD_ERR_INVALID_ARG) {
        text = "reject:invalid_arg";
    } else {
        text = "reject:unsupported";
    }
    len = strlen(text);
    if (buf && buf_bytes > 0u) {
        size_t copy = len < (buf_bytes - 1u) ? len : (buf_bytes - 1u);
        memcpy(buf, text, copy);
        buf[copy] = '\0';
    }
    return len;
}

A840ProdBundleFootprint a840_prod_bundle_footprint(void)
{
    A840ProdBundleFootprint fp;
    fp.abi_version = A840_PROD_ROUTER_ABI_VERSION;
    fp.fallback_compiled_out = A840_PRODUCTION_FALLBACK_COMPILED_OUT;
    fp.native_predicate_count = A840_PROD_PREDICATE_COUNT;
    fp.fallback_predicate_count = 0u;
    /* Minimal production ObjC surface:
     * Principal, Device, Queue, CmdBuffer, Bindings, Encoder, BlitEncoder,
     * ComputeEncoder, RenderEncoder, Allocation, Buffer, Texture, Sampler,
     * Fence, ComputePipeline, RenderPipeline, Library, Function, PipelineProvider,
     * IokitTransport = 20 minimal native classes (zero fallback mirror/delegate classes). */
    fp.objc_exported_class_count = 20u;
    fp.static_fallback_table_bytes = 0u;
    fp.dualpath_symbol_dependencies = 0u;
    fp.nb_symbol_dependencies = 0u;
    fp.cp_protect_violation_count = 0u;
    fp.bundle_version = "Adreno840Metal-Phase6-Native-gen80200";
    return fp;
}
