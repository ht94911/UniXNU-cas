/* SPDX-License-Identifier: MIT */
#ifndef A840_DUALPATH_ROUTER_H
#define A840_DUALPATH_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum A840RoutePath {
    A840_ROUTE_PATH_RAW = 0,
    A840_ROUTE_PATH_FALLBACK = 1
} A840RoutePath;

/* Dynamic per-category native enable bitmask. */
typedef enum A840RouteCategory {
    A840_ROUTE_NONE = 0u,
    A840_ROUTE_BLIT = 1u << 0,
    A840_ROUTE_COMPUTE = 1u << 1,
    A840_ROUTE_RENDER_SYSMEM = 1u << 2,
    A840_ROUTE_RESOURCE = 1u << 3,
    A840_ROUTE_COMPILER = 1u << 4,
    A840_ROUTE_SYNC = 1u << 5,
    A840_ROUTE_PHASE2_ALL = A840_ROUTE_BLIT | A840_ROUTE_COMPUTE | A840_ROUTE_RENDER_SYSMEM,
    A840_ROUTE_ALL = A840_ROUTE_BLIT | A840_ROUTE_COMPUTE | A840_ROUTE_RENDER_SYSMEM |
                     A840_ROUTE_RESOURCE | A840_ROUTE_COMPILER | A840_ROUTE_SYNC
} A840RouteCategory;

/* Strictly bound to all 16 rows of contracts/DualPathPredicates.tsv. */
typedef enum A840FallbackPredicate {
    A840_PRED_NONE = 0,
    A840_PRED_UNIMPLEMENTED_GLOBAL = 1,
    A840_PRED_BLIT_UNSUPPORTED_FORMAT = 2,
    A840_PRED_BLIT_3D_OR_COMPRESSED = 3,
    A840_PRED_COMPUTE_UNCOMPILED_KERNEL = 4,
    A840_PRED_COMPUTE_INDIRECT_DISPATCH = 5,
    A840_PRED_RENDER_GMEM_REQUIRED = 6,
    A840_PRED_RENDER_UBWC_ATTACHMENT = 7,
    A840_PRED_RENDER_YCBCR_OR_XR10 = 8,
    A840_PRED_RENDER_FBFETCH_BLEND = 9,
    A840_PRED_RENDER_TESS_OR_AMPLIFY = 10,
    A840_PRED_RENDER_INDIRECT_OR_ICB = 11,
    A840_PRED_RENDER_MSAA_RESOLVE = 12,
    A840_PRED_RESOURCE_HEAP_ALIASING = 13,
    A840_PRED_RESOURCE_SPARSE_TEXTURE = 14,
    A840_PRED_COMPILER_DAEMON_UNAVAILABLE = 15,
    A840_PRED_SYNC_SHARED_EVENT_CROSS_PROC = 16,
    A840_PRED_COUNT = 16
} A840FallbackPredicate;

typedef struct A840RouteDecision {
    A840RoutePath path;
    A840FallbackPredicate predicate_id;
    const char *predicate; /* NULL on RAW; exact DualPathPredicates.tsv string on FALLBACK */
} A840RouteDecision;

typedef struct A840BlitRouteQuery {
    bool is_buffer_copy;
    bool is_clear;
    bool is_ycbcr;
    uint32_t src_mtl_format;
    uint32_t dst_mtl_format;
    uint32_t tex_type;      /* A840_TEX_* */
    uint32_t src_tile_mode; /* A840_TILE_* */
    uint32_t dst_tile_mode; /* A840_TILE_* */
    uint32_t filter;        /* A840_FILTER_* */
} A840BlitRouteQuery;

typedef struct A840ComputeRouteQuery {
    bool has_compiled_kernel;
    bool has_valid_local_size;
    bool is_indirect_dispatch;
    bool uses_icb;
    bool requires_cold_compile;
    bool compiler_daemon_available;
} A840ComputeRouteQuery;

typedef struct A840RenderRouteQuery {
    uint32_t color_attachment_count; /* 0..8 */
    uint32_t color_mtl_formats[8];
    uint32_t color_tile_modes[8];
    uint32_t depth_mtl_format;       /* 0 if unbound */
    uint32_t depth_tile_mode;
    uint32_t sample_count;           /* 0 or 1 for single-sample */
    bool has_memoryless_attachment;
    bool has_msaa_resolve;
    bool uses_fbfetch;
    bool uses_programmable_blend;
    bool uses_tessellation;
    bool uses_vertex_amplification;
    bool uses_indirect_draw;
    bool uses_icb;
    bool has_ycbcr_attachment;
} A840RenderRouteQuery;

typedef struct A840ResourceRouteQuery {
    bool uses_heap_aliasing;
    bool uses_sparse_texture;
} A840ResourceRouteQuery;

typedef struct A840CompilerRouteQuery {
    bool precompiled_available;
    bool daemon_available;
} A840CompilerRouteQuery;

typedef struct A840SyncRouteQuery {
    bool cross_process_shared_event;
    bool uses_writer_promise;
    bool abi_v3_supported;
} A840SyncRouteQuery;

typedef struct A840DualPathRouter {
    uint32_t enabled_categories;
    bool gmem_engine_enabled;
    uint64_t raw_hits;
    uint64_t raw_hits_by_category[6];
    uint64_t fallback_total;
    uint64_t predicate_counts[A840_PRED_COUNT + 1];
} A840DualPathRouter;

void a840_router_init(A840DualPathRouter *router, uint32_t initial_categories);
void a840_router_set_categories(A840DualPathRouter *router, uint32_t categories);
void a840_router_enable_category(A840DualPathRouter *router, uint32_t category_bits);
void a840_router_disable_category(A840DualPathRouter *router, uint32_t category_bits);
void a840_router_set_gmem_engine(A840DualPathRouter *router, bool enabled);
void a840_router_reset_stats(A840DualPathRouter *router);

const char *a840_router_predicate_name(A840FallbackPredicate pred);
A840FallbackPredicate a840_router_lookup_predicate(const char *predicate_name);
uint64_t a840_router_predicate_count(const A840DualPathRouter *router,
                                     A840FallbackPredicate pred);

A840RouteDecision a840_router_eval_blit(A840DualPathRouter *router,
                                        const A840BlitRouteQuery *query);
A840RouteDecision a840_router_eval_compute(A840DualPathRouter *router,
                                           const A840ComputeRouteQuery *query);
A840RouteDecision a840_router_eval_render(A840DualPathRouter *router,
                                          const A840RenderRouteQuery *query);
A840RouteDecision a840_router_eval_resource(A840DualPathRouter *router,
                                            const A840ResourceRouteQuery *query);
A840RouteDecision a840_router_eval_compiler(A840DualPathRouter *router,
                                            const A840CompilerRouteQuery *query);
A840RouteDecision a840_router_eval_sync(A840DualPathRouter *router,
                                        const A840SyncRouteQuery *query);

/* Formats "path=raw" or "fallback:<predicate>" for NezhaGpuTest / logs. */
size_t a840_router_format_decision(const A840RouteDecision *decision,
                                   char *buf,
                                   size_t buf_bytes);

#ifdef __cplusplus
}
#endif

#endif /* A840_DUALPATH_ROUTER_H */
