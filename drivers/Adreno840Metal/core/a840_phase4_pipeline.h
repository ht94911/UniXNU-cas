/* SPDX-License-Identifier: MIT */
/* Phase 4 Pipeline Orchestrator & 16/16 Router Closure (Wave 8 Lane 3):
 *   - Remaining 3 Phase 4 router capability bits (bits 14..16) closing all 16/16
 *     predicates in contracts/DualPathPredicates.tsv
 *   - Native MSAA 4x Resolve Pass (Memoryless MSAA color + 1x Resolve target)
 *   - Native Visibility / Occlusion Query Pass (Counting & Boolean)
 *   - Native Tessellation & Vertex Amplification Pass
 *   - Native Camera 155 (NV12/P010 YCbCr preview + 4096x3072 BGRA8 photo) /
 *     VideoToolbox / Core Image (separable Gaussian compute) / MPS (GEMM compute)
 *     encoding and Mock Transport submission loop
 *   - Out-of-process NezhaShaderCompiler client bridge (fail-closed to
 *     "raw:compiler.daemon_unavailable" when circuit-open or unreachable)
 */
#ifndef A840_PHASE4_PIPELINE_H
#define A840_PHASE4_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "AdrenoCompileABI.h"
#include "a840_argbuf_icb.h"
#include "a840_ca_render_bridge.h"
#include "a840_dualpath_router.h"
#include "a840_emit_compute.h"
#include "a840_emit_msaa_tess_vis.h"
#include "a840_render_pass_gmem.h"
#include "a840_runtime_cmd.h"
#include "a840_runtime_device.h"
#include "a840_ubwc_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Remaining 3 Phase 4 capability bits (continuing W07-L3 bits 10..13):
 *   bit 14 -> raw:render.msaa_resolve
 *   bit 15 -> raw:render.tess_or_amplify
 *   bit 16 -> raw:compiler.daemon_unavailable
 * Note: a840_argbuf_icb.h already defines enum A840_ROUTE_PHASE4_ALL for bits
 * 10..13 (immutable outside our whitelist). Here we expose both the W08 bits
 * and a macro A840_ROUTE_PHASE4_FULL_ALL plus an explicit override macro
 * A840_ROUTE_PHASE4_ALL_16 (and redefine A840_ROUTE_PHASE4_ALL to the complete
 * Phase 2 + Phase 3 + Phase 4 16/16 closure mask when this header is included). */
enum {
    A840_ROUTE_P4_MSAA_RESOLVE = 1u << 14,
    A840_ROUTE_P4_TESS_AMPLIFY = 1u << 15,
    A840_ROUTE_P4_COMPILER_DAEMON = 1u << 16,
    A840_ROUTE_P4_W08_BITS = A840_ROUTE_P4_MSAA_RESOLVE |
                             A840_ROUTE_P4_TESS_AMPLIFY |
                             A840_ROUTE_P4_COMPILER_DAEMON,
    A840_ROUTE_PHASE4_FULL_ALL = (uint32_t)A840_ROUTE_ALL |
                                 (uint32_t)A840_ROUTE_PHASE3_ALL |
                                 (uint32_t)A840_ROUTE_P4_HEAP_ALIASING |
                                 (uint32_t)A840_ROUTE_P4_INDIRECT_COMPUTE |
                                 (uint32_t)A840_ROUTE_P4_INDIRECT_RENDER_ICB |
                                 (uint32_t)A840_ROUTE_P4_SPARSE |
                                 (uint32_t)A840_ROUTE_P4_MSAA_RESOLVE |
                                 (uint32_t)A840_ROUTE_P4_TESS_AMPLIFY |
                                 (uint32_t)A840_ROUTE_P4_COMPILER_DAEMON
};

#ifndef A840_PHASE4_KEEP_W07_PHASE4_ALL
#undef A840_ROUTE_PHASE4_ALL
#define A840_ROUTE_PHASE4_ALL A840_ROUTE_PHASE4_FULL_ALL
#endif

/* ---------- Out-of-process Compiler Daemon Bridge (zero hardcoded paths) ---------- */

typedef enum A840Phase4CompilerState {
    A840_P4_COMPILER_OK = 0,
    A840_P4_COMPILER_DAEMON_UNAVAILABLE = 1,
    A840_P4_COMPILER_COMPILE_FAILED = 2,
    A840_P4_COMPILER_INVALID_ARG = 3
} A840Phase4CompilerState;

/* Transport-agnostic adapter callbacks (bound by caller to A840ScClient). */
typedef A840Phase4CompilerState (*A840Phase4CompilerCompileFn)(
    void *client_handle,
    const uint8_t pipeline_key[32],
    const void *payload,
    size_t payload_bytes,
    uint32_t timeout_ms,
    uint8_t **out_record_blob,
    size_t *out_record_bytes);

typedef bool (*A840Phase4CompilerHealthFn)(void *client_handle);
typedef void (*A840Phase4CompilerFreeFn)(void *ptr);

typedef struct A840Phase4CompilerBridge {
    void *client_handle;                      /* Opaque handle (e.g. A840ScClient*) */
    A840Phase4CompilerCompileFn compile_sync; /* Cold compile callback */
    A840Phase4CompilerHealthFn is_available;  /* Health / circuit-breaker check */
    A840Phase4CompilerFreeFn free_blob;       /* Optional custom free (NULL -> free) */
    uint32_t default_timeout_ms;
} A840Phase4CompilerBridge;

typedef struct A840Phase4ColdCompileResult {
    A840RouteDecision route;
    A840Phase4CompilerState compiler_state;
    uint8_t *record_blob;                     /* Caller owns (free via bridge or free()) */
    size_t record_bytes;
} A840Phase4ColdCompileResult;

/* ---------- 1. MSAA 4x Resolve + Visibility Query Pass ---------- */

typedef struct A840Phase4MsaaVisPassDesc {
    A840UbwcTexture *msaa_color_target;       /* 2x/4x Memoryless or Backing MSAA color */
    A840UbwcTexture *resolve_target;          /* 1x Linear/Tiled/UBWC resolve destination */
    A840UbwcTexture *depth_target;            /* Optional 2x/4x Memoryless depth */
    A840RtBuffer *visibility_buffer;          /* Optional 64B-aligned visibility buffer */
    uint32_t visibility_slot;                 /* Query slot index */
    A840VisQueryMode visibility_mode;
    bool enable_visibility_query;
    bool sample_rate_shading;
    bool custom_sample_locations;
    A840SampleLocation sample_locations[A840_MSAA_MAX_SAMPLES];
    A840DepthStencilVariantDesc depth_stencil;
    bool emit_depth_stencil_variant;
    A840MsaaResolveMode resolve_mode;
    bool force_2d_resolve;
    A840GraphicsProgramDesc program;
    A840DrawDesc draw;
    uint32_t clear_color[4];
} A840Phase4MsaaVisPassDesc;

typedef struct A840Phase4PassStats {
    A840RouteDecision route;
    size_t emitted_dwords;
    size_t emitted_relocs;
    uint32_t tile_count;
    uint32_t submit_seq;
} A840Phase4PassStats;

/* ---------- 2. Tessellation & Vertex Amplification Pass ---------- */

typedef struct A840Phase4TessAmpPassDesc {
    A840RtTexture *color_target;              /* 1x color target (Linear or Tiled) */
    A840RtBuffer *tess_factor_buffer;         /* 32B-aligned tess factor/param buffer */
    A840TessStateDesc tess_state;
    A840TessDrawDesc tess_draw;
    bool enable_tessellation;
    A840VertexAmpStateDesc vertex_amp;
    bool enable_vertex_amp;
    A840GraphicsProgramDesc program;
    A840DrawDesc fallback_draw;               /* Used when amp-only without tessellation */
    A840DepthStencilVariantDesc depth_stencil;
    bool emit_depth_stencil_variant;
} A840Phase4TessAmpPassDesc;

/* ---------- 3. Camera 155 / VideoToolbox / Core Image / MPS Native Flows ---------- */

typedef struct A840Phase4Camera155Desc {
    A840UbwcTexture *preview_ycbcr;           /* NV12 or P010 multi-plane preview surface */
    A840UbwcTexture *photo_bgra8;             /* e.g. 4096x3072 BGRA8 UBWC/Tiled photo target */
    A840RtBuffer *yuv_to_rgb_consts;          /* Color-matrix constants buffer (16B aligned) */
    A840RtBuffer *bindless_table;             /* Bindless descriptor table BO */
    A840ComputePipelineDesc yuv_convert_pipe; /* Native YCbCr->BGRA8 compute pipeline */
    bool emit_photo_clear_and_blit;           /* Emit 2D photo pipeline sync/blit */
} A840Phase4Camera155Desc;

typedef struct A840Phase4CoreImageGaussianDesc {
    A840RtTexture *input_tex;
    A840RtTexture *intermediate_tex;          /* Horizontal pass output -> vertical input */
    A840RtTexture *output_tex;
    A840RtBuffer *weights_buffer;             /* Gaussian kernel tap weights (16B aligned) */
    A840RtBuffer *bindless_table;
    A840ComputePipelineDesc horiz_pipe;
    A840ComputePipelineDesc vert_pipe;
    uint32_t kernel_radius;                   /* 1..31 */
} A840Phase4CoreImageGaussianDesc;

typedef struct A840Phase4MpsGemmDesc {
    A840RtBuffer *matrix_a;
    A840RtBuffer *matrix_b;
    A840RtBuffer *matrix_c;
    A840RtBuffer *uniform_params;             /* {M, N, K, alpha, beta} */
    A840RtBuffer *bindless_table;
    A840ComputePipelineDesc gemm_pipe;
    uint32_t m;
    uint32_t n;
    uint32_t k;
    uint32_t tile_m;                          /* e.g. 16 or 32 */
    uint32_t tile_n;                          /* e.g. 16 or 32 */
} A840Phase4MpsGemmDesc;

/* ---------- Public API ---------- */

void a840_phase4_enable_full_router(A840DualPathRouter *router);

/* Evaluate cold shader compile through the DualPathRouter + optional
 * NezhaShaderCompiler client bridge. Fail-closed to
 * "raw:compiler.daemon_unavailable" whenever precompiled is absent and the
 * background compiler service is unreachable or circuit-open. */
A840RtStatus a840_phase4_resolve_cold_compile(
    A840DualPathRouter *router,
    const A840Phase4CompilerBridge *bridge,
    const uint8_t pipeline_key[32],
    const void *payload,
    size_t payload_bytes,
    bool precompiled_available,
    A840Phase4ColdCompileResult *out_result);

/* Encode & optionally submit MSAA 2x/4x Resolve + Visibility Query pass. */
A840RtStatus a840_phase4_encode_msaa_vis_pass(
    A840DualPathRouter *router,
    const A840Phase4MsaaVisPassDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats);

A840RtStatus a840_phase4_submit_msaa_vis_pass(
    A840RtDevice *device,
    A840RtQueue *queue,
    const A840Phase4MsaaVisPassDesc *desc,
    A840Phase4PassStats *out_stats);

/* Encode & optionally submit Tessellation + Vertex Amplification pass. */
A840RtStatus a840_phase4_encode_tess_amp_pass(
    A840DualPathRouter *router,
    const A840Phase4TessAmpPassDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats);

A840RtStatus a840_phase4_submit_tess_amp_pass(
    A840RtDevice *device,
    A840RtQueue *queue,
    const A840Phase4TessAmpPassDesc *desc,
    A840Phase4PassStats *out_stats);

/* Encode native Camera 155 (NV12/P010 preview + 4096x3072 BGRA8 photo),
 * Core Image separable Gaussian, and MPS GEMM compute passes. */
A840RtStatus a840_phase4_encode_camera155_flow(
    A840DualPathRouter *router,
    const A840Phase4Camera155Desc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats);

A840RtStatus a840_phase4_encode_coreimage_gaussian(
    A840DualPathRouter *router,
    const A840Phase4CoreImageGaussianDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats);

A840RtStatus a840_phase4_encode_mps_gemm(
    A840DualPathRouter *router,
    const A840Phase4MpsGemmDesc *desc,
    A840CmdStream *cs,
    A840Phase4PassStats *out_stats);

/* Submit an already-encoded Phase 4 command stream through the runtime queue
 * and Mock/KMD transport with explicit residency list. */
A840RtStatus a840_phase4_submit_cs(
    A840RtDevice *device,
    A840RtQueue *queue,
    const uint32_t *words,
    size_t dwords,
    A840RtBuffer *const *resident_buffers,
    size_t resident_count,
    uint32_t *out_submit_seq);

#ifdef __cplusplus
}
#endif

#endif /* A840_PHASE4_PIPELINE_H */
