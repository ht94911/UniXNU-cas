/* SPDX-License-Identifier: MIT */
#ifndef A840_IR3_IO_INFO_H
#define A840_IR3_IO_INFO_H

#include <stdbool.h>
#include <stdint.h>

#include "AdrenoCompileABI.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A840_IR3_REGID_UNUSED UINT8_C(252)
#define A840_IR3_MAX_IO_SLOTS 32u
#define A840_IR3_MAX_COLOR_OUTPUTS 8u

typedef struct A840Ir3InputSlot {
    uint8_t slot;
    uint8_t regid;
    uint8_t compmask;
    uint8_t inloc;
    uint8_t flat;
    uint8_t half_precision;
    uint8_t bary;
    uint8_t sysval;
} A840Ir3InputSlot;

typedef struct A840Ir3OutputSlot {
    uint8_t slot;
    uint8_t regid;
    uint8_t compmask;
    uint8_t half_precision;
} A840Ir3OutputSlot;

typedef struct A840Ir3StageIoInfo {
    uint32_t stage;

    /* Bindless enable flags reported directly by ir3_shader_variant. */
    uint8_t bindless_tex;
    uint8_t bindless_samp;
    uint8_t bindless_uav;
    uint8_t bindless_ubo;

    /* Constant file offsets in vec4 units (UINT32_MAX when unused). */
    uint32_t imm_offset_vec4;
    uint32_t driver_params_offset_vec4;
    uint32_t ubo_ptrs_offset_vec4;
    uint32_t const_data_is_immediates;
    /* Rebased push-constant window actually read by this stage's ir3 ISA. */
    uint32_t push_const_base_bytes;
    uint32_t push_const_dst_vec4;
    uint32_t push_const_size_vec4;

    /* Compute system values (A840_IR3_REGID_UNUSED when unused). */
    uint8_t cs_local_id_regid;
    uint8_t cs_wg_id_regid;
    uint8_t cs_subgroup_id_regid;
    uint8_t reserved0;

    /* Vertex shader system values & fixed outputs. */
    uint8_t vs_vertex_id_regid;
    uint8_t vs_instance_id_regid;
    uint8_t vs_primitive_id_regid;
    uint8_t vs_view_id_regid;
    uint8_t vs_pos_regid;
    uint8_t vs_psize_regid;
    uint8_t reserved1[2];

    /* Fragment shader system values, barycentrics & outputs. */
    uint8_t fs_face_regid;
    uint8_t fs_sample_id_regid;
    uint8_t fs_sample_mask_regid;
    uint8_t fs_center_rhw_regid;
    uint8_t fs_persp_pixel_regid;
    uint8_t fs_linear_pixel_regid;
    uint8_t fs_persp_centroid_regid;
    uint8_t fs_linear_centroid_regid;
    uint8_t fs_persp_sample_regid;
    uint8_t fs_linear_sample_regid;
    uint8_t fs_frag_coord_regid;
    uint8_t fs_frag_coord_mask;
    uint8_t fs_shading_rate_regid;
    uint8_t fs_depth_out_regid;
    uint8_t fs_stencil_out_regid;
    uint8_t fs_sample_mask_out_regid;
    uint8_t fs_pixel_lod;
    uint8_t fs_full_quad;
    uint8_t fs_thread128;
    uint8_t reserved2;
    A840Ir3OutputSlot fs_colors[A840_IR3_MAX_COLOR_OUTPUTS];

    /* General VS inputs/outputs and FS varying inputs. */
    uint32_t input_count;
    A840Ir3InputSlot inputs[A840_IR3_MAX_IO_SLOTS];
    uint32_t output_count;
    A840Ir3OutputSlot outputs[A840_IR3_MAX_IO_SLOTS];
} A840Ir3StageIoInfo;

/* Query exact ir3 hardware IO/register assignments for a stage compiled by
 * libadreno_compile_ir3.a without modifying the frozen AdrenoCompileABI.h. */
bool a840_ir3_get_stage_io_info(const A840CompiledStage *stage,
                                A840Ir3StageIoInfo *out);

/* Encode genuine A8XX 64-byte bindless buffer (SSBO/storage) and UBO views
 * directly using pinned Mesa 85fda3b36405 fdl/fd6_hw.h bitfields. */
void a840_fdl_encode_buffer_view_a8xx(uint64_t gpu_va,
                                      uint32_t size_bytes,
                                      uint32_t elem_bytes,
                                      uint32_t out_words[16]);

void a840_fdl_encode_ubo_view_a8xx(uint64_t gpu_va,
                                   uint32_t size_bytes,
                                   uint32_t out_words[16]);

#ifdef __cplusplus
}
#endif

#endif /* A840_IR3_IO_INFO_H */
