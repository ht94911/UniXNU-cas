/* SPDX-License-Identifier: MIT */
#ifndef A840_EMIT_3D_H
#define A840_EMIT_3D_H

#include "a840_emit_compute.h"
#include "a840_emit_ops.h"

/* Packet limits include headers; bound indirect buffers are counted separately. */
#define A840_MAX_VERTEX_INPUTS 32u
#define A840_MAX_VIEWPORTS 16u
#define A840_MAX_COLOR_TARGETS 8u
#define A840_EMIT_3D_PROGRAM_MAX_DWORDS (2u * A840_EMIT_SHADER_BIND_DWORDS + 161u)
/* Marker, transforms/clamps/scissors, raster/depth/stencil, attachments and blend. */
#define A840_EMIT_3D_RENDER_STATE_MAX_DWORDS (452u)
#define A840_EMIT_3D_DRAW_MAX_DWORDS 11u
#define A840_EMIT_SDS_GROUP_MAX_DWORDS (4u * ADRENO840_MAX_SDS_SLOTS)

typedef struct A840VertexInputDesc {
    uint8_t regid, component_mask;
} A840VertexInputDesc;

/* Ordinary VS -> FS link, in packed FS input order. The caller supplies
 * compiler/linker IO which is not carried by A840CompiledStage v1. */
typedef enum A840VaryingMode {
    A840_VARYING_NORMAL = 0u,
    A840_VARYING_POINT_COORD = 1u, /* VARYING_SLOT_PNTC: VPC_VARYING_REPLACE_MODE PS_REPL_S/T */
    A840_VARYING_ZERO = 2u         /* Unmatched FS varying: VPC_VARYING_INTERP_MODE INTERP_ZERO */
} A840VaryingMode;

typedef struct A840VaryingDesc {
    uint8_t vs_regid, component_mask;
    bool flat;
    uint8_t fs_inloc;    /* Used when explicit_inloc is true (exact ir3 fs->inputs[i].inloc) */
    uint8_t mode;        /* A840VaryingMode */
    bool explicit_inloc;
} A840VaryingDesc;

typedef struct A840FragmentInputs {
    uint8_t face, sample_id, sample_mask, center_rhw;
    uint8_t persp_pixel, linear_pixel, persp_centroid, linear_centroid;
    uint8_t persp_sample, linear_sample, frag_coord, shading_rate;
    uint8_t frag_coord_mask;
} A840FragmentInputs;

typedef struct A840FragmentOutput {
    uint8_t regid, component_mask;
    bool half_precision;
} A840FragmentOutput;

typedef struct A840GraphicsProgramDesc {
    A840ShaderCode vs, fs;
    uint32_t vertex_input_count;
    A840VertexInputDesc vertex_inputs[A840_MAX_VERTEX_INPUTS];
    uint8_t vertex_id, instance_id, primitive_id, view_id;
    uint8_t position, point_size;
    uint32_t varying_count;
    A840VaryingDesc varyings[ADRENO_COMPILE_MAX_VARYINGS];
    A840FragmentInputs fs_inputs;
    uint8_t depth_output, sample_mask_output, stencil_output;
    A840FragmentOutput colors[A840_MAX_COLOR_TARGETS];
    bool pixel_lod, full_quad, fs_thread128;
} A840GraphicsProgramDesc;

/* Depth is in Metal's [0,1] clip interval. Negative height flips Y. */
typedef struct A840Viewport {
    float x, y, width, height, min_depth, max_depth;
} A840Viewport;

typedef struct A840AttachmentStorage {
    A840GpuAddress data, flag_buffer;
    uint32_t pitch_bytes, flag_pitch_bytes;
    uint64_t layer_stride_bytes, flag_layer_stride_bytes;
    uint32_t tile_mode; /* A840_TILE_* from a840_emit_ops.h. */
} A840AttachmentStorage;

/* Factors and operations are the Mesa adreno_rb_blend_factor and
 * a3xx_rb_blend_opcode enums (A840_FACTOR_* / A840_BLEND_* in a840_bitfields.h). */
typedef struct A840BlendDesc {
    bool enabled;
    uint8_t write_mask;
    uint8_t src_rgb, dst_rgb, op_rgb;
    uint8_t src_alpha, dst_alpha, op_alpha;
} A840BlendDesc;

typedef struct A840MrtDesc {
    A840AttachmentStorage storage;
    uint32_t format, swap; /* A840_FMT6_* and A840_{WZYX,WXYZ,ZYXW,XYZW}. */
    bool srgb;
    A840BlendDesc blend;
} A840MrtDesc;

typedef struct A840StencilFace {
    uint8_t compare, fail, pass, depth_fail;
    uint8_t reference, read_mask, write_mask;
} A840StencilFace;

typedef struct A840RenderStateDesc {
    uint32_t viewport_count, scissor_count;
    A840Viewport viewports[A840_MAX_VIEWPORTS];
    A840Rect2D scissors[A840_MAX_VIEWPORTS];
    float line_width;
    bool cull_front, cull_back, front_clockwise;
    bool depth_test, depth_write, depth_clamp, stencil_test;
    uint8_t depth_compare;
    A840StencilFace front, back;
    uint16_t sample_mask;
    float blend_color[4];
    A840MrtDesc colors[A840_MAX_COLOR_TARGETS]; /* data.gpu_va == 0 unbinds a slot */
    uint32_t depth_format; /* A840_DEPTH_NONE / _16 / _24_8 / _32 */
    A840AttachmentStorage depth, stencil; /* stencil is a separate S8 plane */
} A840RenderStateDesc;

typedef enum A840IndexType {
    A840_INDEX_NONE = 0,
    A840_INDEX_UINT16 = 2,
    A840_INDEX_UINT32 = 4
} A840IndexType;

typedef struct A840DrawDesc {
    uint32_t primitive; /* A840_PRIM_* in a840_bitfields.h. */
    A840IndexType index_type;
    bool use_visibility;
    uint32_t count, instance_count, first_instance;
    uint32_t first_vertex; /* non-indexed */
    int32_t base_vertex;   /* indexed */
    uint32_t first_index, index_capacity;
    A840GpuAddress index_buffer;
} A840DrawDesc;

/* Pre-shifted CP_SET_DRAW_STATE enable bits as in the frozen CS ABI. */
#define A840_SDS_BINNING (UINT32_C(1) << 20)
#define A840_SDS_GMEM (UINT32_C(1) << 21)
#define A840_SDS_SYSMEM (UINT32_C(1) << 22)
typedef struct A840SdsGroupDesc {
    uint32_t group_id, enable_mask, state_dwords;
    A840GpuAddress state;
} A840SdsGroupDesc;

#ifdef __cplusplus
extern "C" {
#endif

/* Ordinary VS+FS and sysmem rendering. Unused shader register IDs are
 * A840_REGID_UNUSED. Vertex fetch buffers and global GPU setup are supplied
 * by the caller. All errors preserve the complete output stream. */
A840EmitStatus a840_emit_3d_program(A840CmdStream *cs, const A840GraphicsProgramDesc *desc);
A840EmitStatus a840_emit_3d_render_state(A840CmdStream *cs, const A840RenderStateDesc *desc);
A840EmitStatus a840_emit_3d_draw(A840CmdStream *cs, const A840DrawDesc *desc);
A840EmitStatus a840_emit_sds_group(A840CmdStream *cs, const A840SdsGroupDesc *groups, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* A840_EMIT_3D_H */
