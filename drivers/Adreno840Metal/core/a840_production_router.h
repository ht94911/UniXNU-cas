/* SPDX-License-Identifier: MIT */
/* Adreno 840 (Gen8 / A8XX) Phase 6 Zero-Fallback Production Router.
 *
 * Pure C11, allocation-free, zero fallback branches, zero legacy bridge or fallback
 * symbol dependencies. All 16/16 predicates from contracts/DualPathPredicates.tsv
 * route unconditionally to A840_PROD_ROUTE_NATIVE; any out-of-range/unknown
 * predicate or explicit attempt to enable fallback fails closed with
 * A840_PROD_ERR_UNSUPPORTED or A840_PROD_ERR_FALLBACK_FORBIDDEN.
 */
#ifndef A840_PRODUCTION_ROUTER_H
#define A840_PRODUCTION_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Permanently compiled-out fallback lock (cannot be overridden at runtime). */
#define A840_PRODUCTION_FALLBACK_COMPILED_OUT 1u
#define A840_PROD_ROUTER_ABI_VERSION 1u
#define A840_PROD_PREDICATE_COUNT 16u
#define A840_PROD_CATEGORY_COUNT 7u
#define A840_PROD_MAX_COLOR_ATTACHMENTS 8u

typedef enum A840ProdRouteTarget {
    A840_PROD_ROUTE_REJECT = 0,
    A840_PROD_ROUTE_NATIVE = 1
} A840ProdRouteTarget;

typedef enum A840ProdStatus {
    A840_PROD_OK = 0,
    A840_PROD_ERR_INVALID_ARG = 1,
    A840_PROD_ERR_UNSUPPORTED = 2,
    A840_PROD_ERR_FALLBACK_FORBIDDEN = 3
} A840ProdStatus;

typedef enum A840ProdCategory {
    A840_PROD_CAT_ROUTER = 0,
    A840_PROD_CAT_BLIT = 1,
    A840_PROD_CAT_COMPUTE = 2,
    A840_PROD_CAT_RENDER = 3,
    A840_PROD_CAT_RESOURCE = 4,
    A840_PROD_CAT_COMPILER = 5,
    A840_PROD_CAT_SYNC = 6
} A840ProdCategory;

/* Exact 1..16 predicate IDs aligned 1:1 with contracts/DualPathPredicates.tsv. */
typedef enum A840ProdPredicateId {
    A840_PROD_PRED_NONE = 0,
    A840_PROD_PRED_UNIMPLEMENTED_GLOBAL = 1,
    A840_PROD_PRED_BLIT_UNSUPPORTED_FORMAT = 2,
    A840_PROD_PRED_BLIT_3D_OR_COMPRESSED = 3,
    A840_PROD_PRED_COMPUTE_UNCOMPILED_KERNEL = 4,
    A840_PROD_PRED_COMPUTE_INDIRECT_DISPATCH = 5,
    A840_PROD_PRED_RENDER_GMEM_REQUIRED = 6,
    A840_PROD_PRED_RENDER_UBWC_ATTACHMENT = 7,
    A840_PROD_PRED_RENDER_YCBCR_OR_XR10 = 8,
    A840_PROD_PRED_RENDER_FBFETCH_BLEND = 9,
    A840_PROD_PRED_RENDER_TESS_OR_AMPLIFY = 10,
    A840_PROD_PRED_RENDER_INDIRECT_OR_ICB = 11,
    A840_PROD_PRED_RENDER_MSAA_RESOLVE = 12,
    A840_PROD_PRED_RESOURCE_HEAP_ALIASING = 13,
    A840_PROD_PRED_RESOURCE_SPARSE_TEXTURE = 14,
    A840_PROD_PRED_COMPILER_DAEMON_UNAVAILABLE = 15,
    A840_PROD_PRED_SYNC_SHARED_EVENT_CROSS_PROC = 16
} A840ProdPredicateId;

typedef struct A840ProdPredicateInfo {
    A840ProdPredicateId id;
    const char *name;
    A840ProdCategory category;
    const char *owner_lane;
    uint32_t closed_phase;
    A840ProdRouteTarget target;
} A840ProdPredicateInfo;

typedef struct A840ProdDecision {
    A840ProdStatus status;
    A840ProdRouteTarget target;
    A840ProdPredicateId predicate_id;
    const char *predicate_name;
} A840ProdDecision;

typedef struct A840ProdBlitQuery {
    bool is_buffer_copy;
    bool is_clear;
    bool is_3d_or_compressed;
    bool is_ycbcr;
    bool request_fallback;
    uint32_t src_mtl_format;
    uint32_t dst_mtl_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} A840ProdBlitQuery;

typedef struct A840ProdComputeQuery {
    bool has_compiled_kernel;
    bool has_valid_local_size;
    bool is_indirect_dispatch;
    bool uses_icb;
    bool uses_daemon_compile;
    bool request_fallback;
} A840ProdComputeQuery;

typedef struct A840ProdRenderQuery {
    uint32_t color_attachment_count;
    uint32_t sample_count; /* 0 or 1, 2, or 4 */
    bool uses_gmem;
    bool has_memoryless_attachment;
    bool has_ubwc_attachment;
    bool has_ycbcr_or_xr10;
    bool uses_fbfetch_or_prog_blend;
    bool uses_tess_or_amplify;
    bool uses_indirect_or_icb;
    bool has_msaa_resolve;
    bool request_fallback;
} A840ProdRenderQuery;

typedef struct A840ProdResourceQuery {
    bool uses_heap_aliasing;
    bool uses_sparse_texture;
    bool request_fallback;
} A840ProdResourceQuery;

typedef struct A840ProdCompilerQuery {
    bool precompiled_available;
    bool daemon_available;
    bool request_fallback;
} A840ProdCompilerQuery;

typedef struct A840ProdSyncQuery {
    bool cross_process_shared_event;
    bool uses_writer_promise;
    bool abi_v3_supported;
    bool request_fallback;
} A840ProdSyncQuery;

typedef struct A840ProdRouter {
    uint32_t abi_version;
    uint32_t fallback_compiled_out;
    uint64_t native_hits;
    uint64_t native_hits_by_category[A840_PROD_CATEGORY_COUNT];
    uint64_t predicate_native_hits[A840_PROD_PREDICATE_COUNT + 1u];
    uint64_t fallback_denied_count;
    uint64_t unsupported_rejected_count;
} A840ProdRouter;

typedef struct A840ProdBundleFootprint {
    uint32_t abi_version;
    uint32_t fallback_compiled_out;
    uint32_t native_predicate_count;
    uint32_t fallback_predicate_count;
    uint32_t objc_exported_class_count;
    uint32_t static_fallback_table_bytes;
    uint32_t dualpath_symbol_dependencies;
    uint32_t nb_symbol_dependencies;
    uint32_t cp_protect_violation_count;
    const char *bundle_version;
} A840ProdBundleFootprint;

A840ProdStatus a840_prod_router_init(A840ProdRouter *router);
void a840_prod_router_reset_stats(A840ProdRouter *router);

/* Explicitly rejects any attempt to turn on fallback; always returns
 * A840_PROD_ERR_FALLBACK_FORBIDDEN. */
A840ProdStatus a840_prod_route_request_fallback(A840ProdRouter *router);

/* Evaluate any of the 16/16 predicates by ID or exact TSV name. */
A840ProdDecision a840_prod_route_eval_predicate(A840ProdRouter *router,
                                                A840ProdPredicateId pred,
                                                bool request_fallback);
A840ProdDecision a840_prod_route_eval_by_name(A840ProdRouter *router,
                                              const char *predicate_name,
                                              bool request_fallback);

/* Domain evaluators: strictly native or fail-closed rejection (never fallback). */
A840ProdDecision a840_prod_route_blit(A840ProdRouter *router,
                                      const A840ProdBlitQuery *query);
A840ProdDecision a840_prod_route_compute(A840ProdRouter *router,
                                         const A840ProdComputeQuery *query);
A840ProdDecision a840_prod_route_render(A840ProdRouter *router,
                                        const A840ProdRenderQuery *query);
A840ProdDecision a840_prod_route_resource(A840ProdRouter *router,
                                          const A840ProdResourceQuery *query);
A840ProdDecision a840_prod_route_compiler(A840ProdRouter *router,
                                          const A840ProdCompilerQuery *query);
A840ProdDecision a840_prod_route_sync(A840ProdRouter *router,
                                      const A840ProdSyncQuery *query);

const char *a840_prod_predicate_name(A840ProdPredicateId pred);
A840ProdPredicateId a840_prod_lookup_predicate(const char *predicate_name);
bool a840_prod_predicate_info(A840ProdPredicateId pred, A840ProdPredicateInfo *out);
uint64_t a840_prod_predicate_hits(const A840ProdRouter *router, A840ProdPredicateId pred);

/* Formats "path=native", "reject:fallback_forbidden", or "reject:unsupported". */
size_t a840_prod_format_decision(const A840ProdDecision *decision,
                                 char *buf,
                                 size_t buf_bytes);

/* Minimal production bundle footprint & zero-fallback audit metadata. */
A840ProdBundleFootprint a840_prod_bundle_footprint(void);

#ifdef __cplusplus
}
#endif

#endif /* A840_PRODUCTION_ROUTER_H */
