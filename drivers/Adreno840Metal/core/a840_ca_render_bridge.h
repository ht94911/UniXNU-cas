/* SPDX-License-Identifier: MIT */
#ifndef A840_CA_RENDER_BRIDGE_H
#define A840_CA_RENDER_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a840_dualpath_router.h"
#include "a840_pack_cache_rt.h"
#include "a840_render_pass_gmem.h"
#include "a840_ubwc_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 3 capability flags extending A840RouteCategory (bits 6..9).
 * Keeping bits 0..5 untouched preserves 100% Phase 2 router compatibility. */
enum {
    A840_ROUTE_GMEM = 1u << 6,
    A840_ROUTE_UBWC = 1u << 7,
    A840_ROUTE_FBFETCH = 1u << 8,
    A840_ROUTE_XR10_YCBCR = 1u << 9,
    A840_ROUTE_PHASE3_ALL = A840_ROUTE_GMEM | A840_ROUTE_UBWC |
                            A840_ROUTE_FBFETCH | A840_ROUTE_XR10_YCBCR
};

enum {
    A840_CA_MAX_GLASS_PASSES = 8u
};

/* One on-chip GMEM subpass inside a Core Animation / Liquid Glass frame. */
typedef struct A840CaGlassPassDesc {
    A840PipelineKey pipeline_key;
    bool uses_fbfetch;    /* Reads previous on-chip GMEM color (programmable blend) */
    bool depth_test;
    bool depth_write;
    bool blend_enabled;
    uint8_t depth_compare; /* Hardware compare func 0..7 */
    uint32_t vertex_count; /* Non-indexed vertex count (e.g. 6 for full-screen quad) */
    uint32_t instance_count;
    bool emit_sds;
    uint32_t sds_group_id; /* 0..ADRENO840_MAX_SDS_SLOTS-1 */
} A840CaGlassPassDesc;

typedef struct A840CaFrameDesc {
    A840UbwcTexture *color_target;
    A840UbwcTexture *depth_target; /* Optional Memoryless Depth32Float */
    A840PackCacheRt *pack_cache;
    A840DualPathRouter *router;    /* NULL uses gmem_q->device->router */
    A840GmemLoad color_load;
    A840GmemStore color_store;
    uint32_t clear_color[4];
    uint32_t clear_depth;
    uint32_t pass_count;           /* 1..A840_CA_MAX_GLASS_PASSES */
    A840CaGlassPassDesc passes[A840_CA_MAX_GLASS_PASSES];
    const A840GmemVsc *vsc;        /* NULL for unconditional per-tile replay */
} A840CaFrameDesc;

typedef struct A840CaFrameStats {
    A840RouteDecision route_decision;
    A840GmemPlan gmem_plan;
    uint32_t passes_encoded;
    uint32_t fbfetch_barriers_emitted;
    uint32_t sds_groups_emitted;
    uint32_t draw_ib_dwords;
    uint32_t draw_ib_relocs;
    uint32_t draw_ib_wfi_count;
    uint32_t memoryless_reloc_count;
    uint32_t tier0_hits;
    uint32_t tier1_hits;
} A840CaFrameStats;

typedef struct A840CaFrameFlight {
    A840RtGmemFlight gmem_flight;
    A840CaFrameStats stats;
} A840CaFrameFlight;

/* Enable Phase 3 native GMEM/UBWC/FBFetch/XR10 capabilities on a router. */
void a840_ca_bridge_enable_phase3_router(A840DualPathRouter *router);

/* Evaluate dual-path routing for a Core Animation / Liquid Glass frame. */
A840RouteDecision a840_ca_bridge_eval_frame_route(A840DualPathRouter *router,
                                                  const A840CaFrameDesc *frame);

/* Encode all on-chip FBFetch / Liquid Glass subpasses into draw_cs with
 * zero-copy pack.bin GPU VAs, zero superfluous WFI, and zero Memoryless
 * main-memory relocations. */
A840RtStatus a840_ca_bridge_encode_tile_draws(const A840CaFrameDesc *frame,
                                              A840CmdStream *draw_cs,
                                              A840CaFrameStats *out_stats);

/* Submit a complete Phase 3 GMEM frame (Memoryless Depth + UBWC 6.0 Color +
 * multi-pass on-chip FBFetch) through a840_rt_gmem_submit. */
A840RtStatus a840_ca_bridge_submit_frame(A840RtGmemQueue *gmem_q,
                                         const A840CaFrameDesc *frame,
                                         A840RtBuffer *main_ib,
                                         A840RtBuffer *draw_ib,
                                         A840Reloc *scratch,
                                         size_t scratch_cap,
                                         A840RtBuffer *const extra_resident[],
                                         size_t extra_count,
                                         A840CaFrameFlight *out_flight);

A840RtStatus a840_ca_bridge_retire_frame(A840CaFrameFlight *flight,
                                         uint64_t timeout_ns);

/* Audit a PM4 stream for WFI packet count, indirect buffer count, and zero
 * main-memory relocations to a forbidden (e.g. Memoryless) BO handle. */
A840RtStatus a840_ca_bridge_audit_pm4(const uint32_t *words,
                                      size_t dwords,
                                      const A840Reloc *relocs,
                                      size_t reloc_count,
                                      uint32_t *out_wfi_count,
                                      uint32_t *out_indirect_count,
                                      uint32_t *out_zero_bo_relocs);

#ifdef __cplusplus
}
#endif

#endif /* A840_CA_RENDER_BRIDGE_H */
