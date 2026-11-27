/* SPDX-License-Identifier: MIT */
/* Adreno 840 (A8XX / Gen8) native PM4 emitters for:
 *   - 2x / 4x MSAA state, standard/custom sample locations & GMEM->Sysmem MSAA Resolve
 *   - Depth/Stencil variants (depth bounds, separate stencil, read-only depth) &
 *     Visibility / Occlusion Queries (Counting & Boolean, ZPASS_DONE / CP_EVENT_WRITE7)
 *   - Tessellation (PC_HS_PARAM_0/1, PC_DS_PARAM, PC_TESS_BASE, CP_SET_SUBDRAW_SIZE,
 *     patch draw) & Vertex Amplification / Multiview (GRAS_SU_CNTL, GRAS_SU_STEREO_CNTL,
 *     VFD/VPC/PC stereo controls + multi-viewport array mapping)
 *
 * Pure C11, allocation-free, transactional rollback on any error, zero raw hex
 * register/opcode literals.
 */
#ifndef A840_EMIT_MSAA_TESS_VIS_H
#define A840_EMIT_MSAA_TESS_VIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_emit_3d.h"
#include "a840_emit_compute.h"
#include "a840_emit_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum packet lengths in dwords, including headers. */
#define A840_EMIT_MSAA_STATE_MAX_DWORDS 26u
#define A840_EMIT_MSAA_RESOLVE_BLIT_MAX_DWORDS 64u
#define A840_EMIT_DEPTH_STENCIL_VARIANT_MAX_DWORDS 24u
#define A840_EMIT_VIS_QUERY_BEGIN_MAX_DWORDS 16u
#define A840_EMIT_VIS_QUERY_END_MAX_DWORDS 20u
#define A840_EMIT_TESS_STATE_MAX_DWORDS 24u
#define A840_EMIT_TESS_DRAW_MAX_DWORDS 18u
#define A840_EMIT_VERTEX_AMP_STATE_MAX_DWORDS 48u

enum {
    A840_MSAA_MAX_SAMPLES = 4u,
    A840_VIS_QUERY_SLOT_STRIDE_BYTES = 64u,
    A840_VIS_QUERY_SLOT_ALIGN_BYTES = 64u,
    A840_TESS_MAX_CONTROL_POINTS = 32u,
    A840_VERTEX_AMP_MAX_VIEWS = 2u
};

/* ---------- 1. MSAA State & GMEM->Sysmem Resolve ---------- */

typedef struct A840SampleLocation {
    float x; /* [0.0, 1.0), quantized to 1/16 pixel grid (snorm offset from 0.5) */
    float y;
} A840SampleLocation;

typedef struct A840MsaaStateDesc {
    uint32_t sample_count;               /* 1, 2, or 4 (8x and others rejected) */
    bool sample_rate_shading;            /* Per-sample FS execution when samples > 1 */
    bool custom_sample_locations;        /* False -> standard Metal/D3D/Vulkan locations */
    A840SampleLocation locations[A840_MSAA_MAX_SAMPLES];
} A840MsaaStateDesc;

typedef enum A840MsaaResolveMode {
    A840_MSAA_RESOLVE_AVERAGE = 0,       /* Float/UNORM box average across samples */
    A840_MSAA_RESOLVE_SAMPLE0 = 1        /* Take sample 0 (required for integer/depth) */
} A840MsaaResolveMode;

typedef struct A840MsaaResolveBlitDesc {
    uint32_t src_samples;                /* 2 or 4 */
    uint32_t gmem_offset;                /* 4 KiB-aligned GMEM offset (relative to GMEM base) */
    uint32_t gmem_pitch;                 /* Tile row pitch in bytes (64B aligned) */
    uint32_t slot_index;                 /* Color attachment slot 0..7 */
    uint32_t tile_w;                     /* Tile width (multiple of 16) */
    uint32_t tile_h;                     /* Tile height (multiple of 4) */
    A840Rect2D resolve_rect;             /* Half-open rectangle within dst */
    A840Surface2D dst;                   /* Single-sample sysmem resolve target (Linear/Tiled/UBWC) */
    A840MsaaResolveMode mode;
    bool use_2d_engine;                  /* Force A2D resolve path (auto-selected for wide/RG8/SNORM) */
    bool emit_ccu_uche_barriers;         /* Emit CCU/UCHE flush+invalidate barriers around resolve */
} A840MsaaResolveBlitDesc;

/* ---------- 2. Depth/Stencil Variants & Visibility / Occlusion Query ---------- */

typedef enum A840DepthPlaneMode {
    A840_ZMODE_EARLY_Z = 0,
    A840_ZMODE_LATE_Z = 1,
    A840_ZMODE_EARLY_Z_LATE_Z = 2
} A840DepthPlaneMode;

typedef struct A840DepthStencilVariantDesc {
    bool depth_test;
    bool depth_write;
    bool depth_clamp;
    bool depth_bounds_test;
    bool read_only_depth;
    uint8_t depth_compare;               /* Hardware adreno_compare_func 0..7 */
    float depth_bounds_min;              /* [0.0, 1.0], min <= max when bounds enabled */
    float depth_bounds_max;
    A840DepthPlaneMode z_mode;
    bool stencil_test;
    bool separate_stencil_faces;
    A840StencilFace front;
    A840StencilFace back;
} A840DepthStencilVariantDesc;

typedef enum A840VisQueryMode {
    A840_VIS_QUERY_COUNTING = 0,
    A840_VIS_QUERY_BOOLEAN = 1
} A840VisQueryMode;

typedef struct A840VisQueryDesc {
    A840GpuAddress query_buffer;         /* 64B-aligned visibility buffer base */
    uint64_t buffer_bytes;               /* Total visibility buffer size in bytes */
    uint32_t slot_index;                 /* Query slot index (each slot occupies 64 bytes) */
    A840VisQueryMode mode;
    bool inside_render_pass;             /* True when emitted inside an active render pass */
    bool emit_rb_sample_counter_base;    /* Also program RB_SAMPLE_COUNTER_BASE for A6XX/A7XX compat */
} A840VisQueryDesc;

/* ---------- 3. Tessellation & Vertex Amplification ---------- */

typedef enum A840TessDomain {
    A840_TESS_DOMAIN_QUAD = 0,           /* Matches A8XX TESS_QUADS */
    A840_TESS_DOMAIN_TRIANGLE = 1        /* Matches A8XX TESS_TRIANGLES */
} A840TessDomain;

typedef enum A840TessSpacing {
    A840_TESS_SPACING_EQUAL = 0,         /* TESS_EQUAL (0) */
    A840_TESS_SPACING_FRACTIONAL_ODD = 2,/* TESS_FRACTIONAL_ODD (2) */
    A840_TESS_SPACING_FRACTIONAL_EVEN = 3/* TESS_FRACTIONAL_EVEN (3) */
} A840TessSpacing;

typedef enum A840TessWinding {
    A840_TESS_WINDING_POINTS = 0,
    A840_TESS_WINDING_LINES = 1,
    A840_TESS_WINDING_CW = 2,
    A840_TESS_WINDING_CCW = 3
} A840TessWinding;

typedef struct A840TessStateDesc {
    A840TessDomain domain;
    A840TessSpacing spacing;
    A840TessWinding winding;
    uint32_t control_points;             /* 1..32 patch control points */
    uint32_t hs_output_control_points;   /* 1..32 HS output vertices (0 -> control_points) */
    uint32_t patch_local_mem_16b;        /* Patch local memory in 16B units (1..2047) */
    uint32_t subdraw_patch_limit;        /* Max patches per subdraw (1..65535) */
    bool d3d_vertex_ordering;            /* Metal/D3D ordering toggle in PC_CNTL / VPC_PC_CNTL */
    A840GpuAddress tess_factor_buffer;   /* 32B-aligned tess factor/param buffer */
    uint64_t tess_buffer_bytes;          /* >= 256 bytes */
} A840TessStateDesc;

typedef struct A840TessDrawDesc {
    A840TessDomain domain;
    uint32_t control_points;             /* 1..32 */
    uint32_t patch_count;                /* Number of patches (0 = no-op) */
    uint32_t instance_count;             /* Number of instances (0 = no-op) */
    uint32_t first_patch;
    uint32_t first_instance;
    A840IndexType index_type;            /* NONE, UINT16, or UINT32 */
    uint32_t index_capacity;
    A840GpuAddress index_buffer;
    bool use_visibility;
} A840TessDrawDesc;

typedef struct A840VertexAmpViewMapping {
    uint32_t viewport_array_index;       /* 0..15 */
    uint32_t render_target_array_index;  /* 0..15 */
} A840VertexAmpViewMapping;

typedef struct A840VertexAmpStateDesc {
    uint32_t amp_count;                  /* 1 or 2 views (Metal maxVertexAmplificationCount) */
    bool multi_pos_output;               /* False -> VS invoked per view with ViewID */
    bool increment_rt_index;             /* GRAS_SU_STEREO_CNTL.RENDERTARGETINDEXINCR */
    bool increment_viewport_index;       /* GRAS_SU_STEREO_CNTL.VIEWPORTINDEXINCR */
    A840VertexAmpViewMapping mappings[A840_VERTEX_AMP_MAX_VIEWS];
    uint32_t viewport_count;             /* 0 (skip viewport programming) or amp_count..16 */
    A840Viewport viewports[A840_VERTEX_AMP_MAX_VIEWS];
    A840Rect2D scissors[A840_VERTEX_AMP_MAX_VIEWS];
} A840VertexAmpStateDesc;

/* ---------- Public Emitters ---------- */

A840EmitStatus a840_emit_msaa_state(A840CmdStream *cs, const A840MsaaStateDesc *desc);

A840EmitStatus a840_emit_msaa_resolve_blit(A840CmdStream *cs,
                                           const A840MsaaResolveBlitDesc *desc);

A840EmitStatus a840_emit_depth_stencil_variant(A840CmdStream *cs,
                                               const A840DepthStencilVariantDesc *desc);

A840EmitStatus a840_emit_visibility_query_begin(A840CmdStream *cs,
                                                const A840VisQueryDesc *desc);

A840EmitStatus a840_emit_visibility_query_end(A840CmdStream *cs,
                                              const A840VisQueryDesc *desc);

A840EmitStatus a840_emit_tess_state(A840CmdStream *cs, const A840TessStateDesc *desc);

A840EmitStatus a840_emit_tess_draw(A840CmdStream *cs, const A840TessDrawDesc *desc);

A840EmitStatus a840_emit_vertex_amplification_state(A840CmdStream *cs,
                                                    const A840VertexAmpStateDesc *desc);

#ifdef __cplusplus
}
#endif

#endif /* A840_EMIT_MSAA_TESS_VIS_H */
