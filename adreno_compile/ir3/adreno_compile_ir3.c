/* SPDX-License-Identifier: MIT */
#ifdef A840_BUILD_MESA_IR3_BACKEND

#include "AdrenoCompileABI.h"
#include "a840_ir3_io_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "common/fd6_hw.h"
#include "fdl/fd6_format_table.h"
#include "fdl/freedreno_layout.h"
#include "freedreno_dev_info.h"
#include "util/format/u_format.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "util/ralloc.h"
#include "util/u_math.h"

#define A840_SPIRV_MAGIC UINT32_C(0x07230203)
#define A840_KAANAPALI_UCHE_TRAP_BASE UINT64_C(0x1fffffffff000)

enum {
   A840_OUT_FLAG_EARLY_Z        = 1u << 0,
   A840_OUT_FLAG_WRITES_DEPTH   = 1u << 1,
   A840_OUT_FLAG_USES_FBFETCH   = 1u << 2,
   A840_OUT_FLAG_MERGED_REGS    = 1u << 3,
   A840_OUT_FLAG_EARLY_PREAMBLE = 1u << 4
};

__attribute__((visibility("hidden")))
A840CompileStatus a840_ir3_init_dev_info(const A840CompileTarget *target,
                                         struct fd_dev_id *out_dev_id,
                                         struct fd_dev_info *out_info,
                                         char *err_buf,
                                         size_t err_buf_bytes);

__attribute__((visibility("hidden")))
nir_shader *a840_ir3_spirv_to_lowered_nir(struct ir3_compiler *compiler,
                                          const A840StageInput *input,
                                          uint32_t effective_flags,
                                          struct ir3_shader_options *out_shader_opts,
                                          char *err_buf,
                                          size_t err_buf_bytes,
                                          A840CompileStatus *out_status);

#define A840_STAGE_OWNER_MAGIC UINT32_C(0x4138494f)

typedef struct A840CompiledStageOwner {
   uint32_t magic;
   uint32_t *isa_words;
   uint32_t *const_data_words;
   A840Ir3StageIoInfo io_info;
} A840CompiledStageOwner;

static uint8_t
clamp_regid(unsigned r)
{
   return r >= A840_IR3_REGID_UNUSED ? A840_IR3_REGID_UNUSED : (uint8_t)r;
}

static void
populate_stage_io_info(const struct ir3_shader_variant *v,
                       uint32_t stage,
                       bool const_is_immediates,
                       A840Ir3StageIoInfo *io)
{
   memset(io, 0, sizeof(*io));
   io->stage = stage;
   io->bindless_tex = v->bindless_tex ? 1u : 0u;
   io->bindless_samp = v->bindless_samp ? 1u : 0u;
   io->bindless_uav = v->bindless_ibo ? 1u : 0u;
   io->bindless_ubo = v->bindless_ubo ? 1u : 0u;
   io->imm_offset_vec4 = UINT32_MAX;
   if (!v->compiler->info->props.load_shader_consts_via_preamble && v->imm_state.count > 0u) {
      const struct ir3_const_state *imm_cs = ir3_const_state(v);
      if (imm_cs != NULL)
         io->imm_offset_vec4 = imm_cs->allocs.max_const_offset_vec4;
   }
   io->driver_params_offset_vec4 = UINT32_MAX;
   io->ubo_ptrs_offset_vec4 = UINT32_MAX;
   io->const_data_is_immediates = const_is_immediates ? 1u : 0u;

   const struct ir3_const_state *cs = ir3_const_state(v);
   if (cs != NULL) {
      if (cs->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].size_vec4 > 0u)
         io->driver_params_offset_vec4 = cs->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
      if (cs->allocs.consts[IR3_CONST_ALLOC_UBO_PTRS].size_vec4 > 0u)
         io->ubo_ptrs_offset_vec4 = cs->allocs.consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4;
   }

   io->cs_local_id_regid = A840_IR3_REGID_UNUSED;
   io->cs_wg_id_regid = A840_IR3_REGID_UNUSED;
   io->cs_subgroup_id_regid = A840_IR3_REGID_UNUSED;
   io->vs_vertex_id_regid = A840_IR3_REGID_UNUSED;
   io->vs_instance_id_regid = A840_IR3_REGID_UNUSED;
   io->vs_primitive_id_regid = A840_IR3_REGID_UNUSED;
   io->vs_view_id_regid = A840_IR3_REGID_UNUSED;
   io->vs_pos_regid = A840_IR3_REGID_UNUSED;
   io->vs_psize_regid = A840_IR3_REGID_UNUSED;
   io->fs_face_regid = A840_IR3_REGID_UNUSED;
   io->fs_sample_id_regid = A840_IR3_REGID_UNUSED;
   io->fs_sample_mask_regid = A840_IR3_REGID_UNUSED;
   io->fs_center_rhw_regid = A840_IR3_REGID_UNUSED;
   io->fs_persp_pixel_regid = A840_IR3_REGID_UNUSED;
   io->fs_linear_pixel_regid = A840_IR3_REGID_UNUSED;
   io->fs_persp_centroid_regid = A840_IR3_REGID_UNUSED;
   io->fs_linear_centroid_regid = A840_IR3_REGID_UNUSED;
   io->fs_persp_sample_regid = A840_IR3_REGID_UNUSED;
   io->fs_linear_sample_regid = A840_IR3_REGID_UNUSED;
   io->fs_frag_coord_regid = A840_IR3_REGID_UNUSED;
   io->fs_shading_rate_regid = A840_IR3_REGID_UNUSED;
   io->fs_depth_out_regid = A840_IR3_REGID_UNUSED;
   io->fs_stencil_out_regid = A840_IR3_REGID_UNUSED;
   io->fs_sample_mask_out_regid = A840_IR3_REGID_UNUSED;
   for (unsigned i = 0; i < A840_IR3_MAX_COLOR_OUTPUTS; ++i) {
      io->fs_colors[i].slot = (uint8_t)(FRAG_RESULT_DATA0 + i);
      io->fs_colors[i].regid = A840_IR3_REGID_UNUSED;
   }

   if (v->type == MESA_SHADER_COMPUTE) {
      io->cs_local_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID));
      io->cs_wg_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID));
      io->cs_subgroup_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_SUBGROUP_ID));
   } else if (v->type == MESA_SHADER_VERTEX) {
      io->vs_vertex_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_VERTEX_ID));
      io->vs_instance_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_INSTANCE_ID));
      io->vs_primitive_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_PRIMITIVE_ID));
      io->vs_view_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_VIEW_INDEX));
      io->vs_pos_regid = clamp_regid(ir3_find_output_regid(v, VARYING_SLOT_POS));
      io->vs_psize_regid = clamp_regid(ir3_find_output_regid(v, VARYING_SLOT_PSIZ));
   } else if (v->type == MESA_SHADER_FRAGMENT) {
      io->fs_face_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_FRONT_FACE));
      io->fs_sample_id_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_SAMPLE_ID));
      io->fs_sample_mask_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_SAMPLE_MASK_IN));
      io->fs_center_rhw_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTER_RHW));
      io->fs_persp_pixel_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL));
      io->fs_linear_pixel_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL));
      io->fs_persp_centroid_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID));
      io->fs_linear_centroid_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID));
      io->fs_persp_sample_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE));
      io->fs_linear_sample_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE));
      io->fs_frag_coord_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_FRAG_COORD));
      io->fs_frag_coord_mask = v->fragcoord_compmask;
      io->fs_shading_rate_regid = clamp_regid(ir3_find_sysval_regid(v, SYSTEM_VALUE_FRAG_SHADING_RATE));
      io->fs_depth_out_regid = clamp_regid(ir3_find_output_regid(v, FRAG_RESULT_DEPTH));
      io->fs_stencil_out_regid = clamp_regid(ir3_find_output_regid(v, FRAG_RESULT_STENCIL));
      io->fs_sample_mask_out_regid = clamp_regid(ir3_find_output_regid(v, FRAG_RESULT_SAMPLE_MASK));
      io->fs_pixel_lod = v->need_pixlod ? 1u : 0u;
      io->fs_full_quad = v->need_full_quad ? 1u : 0u;
      io->fs_thread128 = v->info.double_threadsize ? 1u : 0u;
      for (unsigned i = 0; i < v->outputs_count; ++i) {
         unsigned s = v->outputs[i].slot;
         if (s >= FRAG_RESULT_DATA0 && s < FRAG_RESULT_DATA0 + A840_IR3_MAX_COLOR_OUTPUTS) {
            unsigned rt = s - FRAG_RESULT_DATA0;
            io->fs_colors[rt].slot = (uint8_t)s;
            io->fs_colors[rt].regid = clamp_regid(v->outputs[i].regid);
            io->fs_colors[rt].compmask = 0xfu;
            io->fs_colors[rt].half_precision = v->outputs[i].half ? 1u : 0u;
         }
      }
   }

   for (unsigned i = 0; i < v->inputs_count && io->input_count < A840_IR3_MAX_IO_SLOTS; ++i) {
      A840Ir3InputSlot *dst = &io->inputs[io->input_count++];
      dst->slot = v->inputs[i].slot;
      dst->regid = clamp_regid(v->inputs[i].regid);
      dst->compmask = v->inputs[i].compmask;
      dst->inloc = v->inputs[i].inloc;
      dst->flat = v->inputs[i].flat ? 1u : 0u;
      dst->half_precision = v->inputs[i].half ? 1u : 0u;
      dst->bary = v->inputs[i].bary ? 1u : 0u;
      dst->sysval = v->inputs[i].sysval ? 1u : 0u;
   }

   for (unsigned i = 0; i < v->outputs_count && io->output_count < A840_IR3_MAX_IO_SLOTS; ++i) {
      A840Ir3OutputSlot *dst = &io->outputs[io->output_count++];
      dst->slot = v->outputs[i].slot;
      dst->regid = clamp_regid(v->outputs[i].regid);
      dst->compmask = 0xfu;
      dst->half_precision = v->outputs[i].half ? 1u : 0u;
   }
}

static A840CompileStatus
report_err(A840CompileStatus status, char *err_buf, size_t err_buf_bytes,
           const char *msg)
{
   if (err_buf != NULL && err_buf_bytes != 0)
      (void)snprintf(err_buf, err_buf_bytes, "%s", msg);
   return status;
}

static bool
valid_sample_count(uint32_t sample_count)
{
   return sample_count == 1u || sample_count == 2u || sample_count == 4u;
}

static uint32_t
extract_vpc_varyings(const struct ir3_shader_variant *v,
                     uint32_t out_varyings[ADRENO_COMPILE_MAX_VARYINGS])
{
   uint32_t count = 0u;
   if (v->type == MESA_SHADER_VERTEX) {
      for (unsigned i = 0; i < v->outputs_count && count < ADRENO_COMPILE_MAX_VARYINGS; ++i) {
         uint8_t slot = v->outputs[i].slot;
         if (slot == VARYING_SLOT_POS || slot == VARYING_SLOT_PSIZ ||
             slot == VARYING_SLOT_LAYER || slot == VARYING_SLOT_VIEWPORT) {
            continue;
         }
         out_varyings[count++] =
            (uint32_t)slot |
            ((uint32_t)v->outputs[i].regid << 8) |
            ((uint32_t)(v->outputs[i].half ? 1u : 0u) << 16);
      }
   } else if (v->type == MESA_SHADER_FRAGMENT) {
      for (int i = ir3_next_varying(v, -1);
           i < (int)v->inputs_count && count < ADRENO_COMPILE_MAX_VARYINGS;
           i = ir3_next_varying(v, i)) {
         out_varyings[count++] =
            (uint32_t)v->inputs[i].slot |
            ((uint32_t)v->inputs[i].inloc << 8) |
            ((uint32_t)v->inputs[i].compmask << 16) |
            ((uint32_t)(v->inputs[i].flat ? 1u : 0u) << 24) |
            ((uint32_t)(v->inputs[i].half ? 1u : 0u) << 25);
      }
   }
   return count;
}

A840CompileStatus
a840_compile_stage(const A840CompileTarget *target,
                   const A840StageInput *input,
                   A840CompiledStage *out_stage,
                   char *err_buf,
                   size_t err_buf_bytes)
{
   if (err_buf == NULL && err_buf_bytes != 0)
      return A840_COMPILE_INVALID_ARG;
   if (err_buf != NULL && err_buf_bytes != 0)
      err_buf[0] = '\0';
   if (input == NULL || out_stage == NULL) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "input and out_stage are required");
   }

   struct fd_dev_id dev_id;
   struct fd_dev_info dev_info;
   A840CompileStatus status = a840_ir3_init_dev_info(
      target, &dev_id, &dev_info, err_buf, err_buf_bytes);
   if (status != A840_COMPILE_OK)
      return status;

   if (input->abi_version != ADRENO_COMPILE_ABI_VERSION) {
      return report_err(A840_COMPILE_ABI_MISMATCH, err_buf, err_buf_bytes,
                        "stage input ABI version mismatch");
   }
   if (input->stage > A840_STAGE_COMPUTE) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "invalid shader stage");
   }
   if (input->entry_point == NULL || input->entry_point[0] == '\0') {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "entry_point is required");
   }
   if (!valid_sample_count(input->sample_count)) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "sample_count must be 1, 2, or 4");
   }
   if (input->spirv_words == NULL || input->spirv_bytes < 4u ||
       (input->spirv_bytes % 4u) != 0u ||
       ((uintptr_t)input->spirv_words % 4u) != 0u) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "SPIR-V requires a 4-byte-aligned pointer and a whole number of words");
   }
   if (input->spirv_words[0] != A840_SPIRV_MAGIC) {
      return report_err(A840_COMPILE_SPIRV_ERROR, err_buf, err_buf_bytes,
                        "invalid SPIR-V magic");
   }
   if (input->spirv_bytes < 20u) {
      return report_err(A840_COMPILE_SPIRV_ERROR, err_buf, err_buf_bytes,
                        "SPIR-V binary truncated (fewer than 5 header words)");
   }

   const uint32_t effective_flags = target->flags | input->flags;
   if ((effective_flags & A840_COMPILE_FLAG_EARLY_PREAMBLE) != 0u) {
      dev_info.props.has_early_preamble = true;
   }

   const bool fbfetch_gmem =
      (effective_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u;

   const struct ir3_compiler_options comp_opts = {
      .push_ubo_with_preamble = true,
      .disable_cache = true,
      .bindless_fb_read_descriptor = fbfetch_gmem ? 0 : -1,
      .bindless_fb_read_slot = fbfetch_gmem ? 0 : -1,
      .storage_16bit = dev_info.props.storage_16bit,
      .storage_8bit = dev_info.props.storage_8bit,
      .lower_base_vertex = false,
      .shared_push_consts = false,
      .dual_color_blend_by_location = false,
      .uche_trap_base = A840_KAANAPALI_UCHE_TRAP_BASE,
   };

   struct ir3_compiler *compiler = ir3_compiler_create(NULL, &dev_id, &dev_info, &comp_opts);
   if (compiler == NULL) {
      return report_err(A840_COMPILE_IR3_ERROR, err_buf, err_buf_bytes,
                        "ir3_compiler_create failed for Adreno 840");
   }
   if ((effective_flags & A840_COMPILE_FLAG_DISABLE_MERGED_REGS) != 0u) {
      compiler->mergedregs = false;
   }

   glsl_type_singleton_init_or_ref();

   struct ir3_shader_options shader_opts;
   A840CompileStatus lower_status = A840_COMPILE_OK;
   nir_shader *nir = a840_ir3_spirv_to_lowered_nir(
      compiler, input, effective_flags, &shader_opts,
      err_buf, err_buf_bytes, &lower_status);
   if (nir == NULL) {
      glsl_type_singleton_decref();
      ir3_compiler_destroy(compiler);
      return lower_status != A840_COMPILE_OK ? lower_status : A840_COMPILE_SPIRV_ERROR;
   }

   /* ir3_shader_from_nir takes ownership of nir. */
   struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &shader_opts);
   nir = NULL;
   if (shader == NULL) {
      glsl_type_singleton_decref();
      ir3_compiler_destroy(compiler);
      return report_err(A840_COMPILE_IR3_ERROR, err_buf, err_buf_bytes,
                        "ir3_shader_from_nir failed");
   }

   struct ir3_shader_key key;
   memset(&key, 0, sizeof(key));
   if (input->stage == A840_STAGE_FRAGMENT && input->sample_count > 1u)
      key.msaa = 1u;

   struct ir3_shader_variant *v =
      ir3_shader_get_variant(shader, &key, false, false, NULL, NULL);
   if (v == NULL || v->bin == NULL || v->info.sizedwords == 0u ||
       v->info.size < (uint32_t)v->info.sizedwords * sizeof(uint32_t)) {
      ir3_shader_destroy(shader);
      glsl_type_singleton_decref();
      ir3_compiler_destroy(compiler);
      return report_err(A840_COMPILE_IR3_ERROR, err_buf, err_buf_bytes,
                        "ir3_shader_get_variant failed to emit Adreno 840 ISA");
   }

   A840CompiledStageOwner *owner = calloc(1, sizeof(*owner));
   if (owner == NULL) {
      ir3_shader_destroy(shader);
      glsl_type_singleton_decref();
      ir3_compiler_destroy(compiler);
      return report_err(A840_COMPILE_OOM, err_buf, err_buf_bytes, "OOM allocating stage owner");
   }

   uint32_t isa_dwords = v->info.sizedwords;
   owner->isa_words = malloc((size_t)isa_dwords * sizeof(uint32_t));
   if (owner->isa_words == NULL) {
      free(owner);
      ir3_shader_destroy(shader);
      glsl_type_singleton_decref();
      ir3_compiler_destroy(compiler);
      return report_err(A840_COMPILE_OOM, err_buf, err_buf_bytes, "OOM allocating ISA buffer");
   }
   memcpy(owner->isa_words, v->bin, (size_t)isa_dwords * sizeof(uint32_t));

   uint32_t const_data_dwords = 0u;
   if (v->constant_data_size > 0u &&
       v->info.constant_data_offset + v->constant_data_size <= v->info.size) {
      const_data_dwords = DIV_ROUND_UP(v->constant_data_size, 4u);
      owner->const_data_words = calloc(const_data_dwords, sizeof(uint32_t));
      if (owner->const_data_words == NULL) {
         free(owner->isa_words);
         free(owner);
         ir3_shader_destroy(shader);
         glsl_type_singleton_decref();
         ir3_compiler_destroy(compiler);
         return report_err(A840_COMPILE_OOM, err_buf, err_buf_bytes,
                           "OOM allocating constant data buffer");
      }
      memcpy(owner->const_data_words,
             (const uint8_t *)v->bin + v->info.constant_data_offset,
             v->constant_data_size);
   } else if (v->imm_state.size > 0u && v->imm_state.values != NULL) {
      const_data_dwords = v->imm_state.size;
      owner->const_data_words = malloc((size_t)const_data_dwords * sizeof(uint32_t));
      if (owner->const_data_words == NULL) {
         free(owner->isa_words);
         free(owner);
         ir3_shader_destroy(shader);
         glsl_type_singleton_decref();
         ir3_compiler_destroy(compiler);
         return report_err(A840_COMPILE_OOM, err_buf, err_buf_bytes,
                           "OOM allocating immediate constant buffer");
      }
      memcpy(owner->const_data_words, v->imm_state.values,
             (size_t)const_data_dwords * sizeof(uint32_t));
   }

   A840CompiledStage result;
   memset(&result, 0, sizeof(result));
   result.abi_version = ADRENO_COMPILE_ABI_VERSION;
   result.stage = input->stage;
   result.isa_words = owner->isa_words;
   result.isa_dwords = isa_dwords;
   /* AdrenoCompileABI::instruction_groups counts 16-byte groups (4 dwords).
    * Mesa ir3 v->instrlen counts 128-byte units (8 groups = 32 dwords), and
    * ir3.c guarantees info->sizedwords >= v->instrlen * 32. */
   uint32_t instr_groups = v->instrlen > 0u ? (v->instrlen * 8u) : (isa_dwords / 4u);
   if (instr_groups == 0u)
      instr_groups = 1u;
   if ((uint64_t)instr_groups * 4u > (uint64_t)isa_dwords)
      instr_groups = isa_dwords / 4u;
   result.instruction_groups = instr_groups;
   result.max_full_reg = v->info.max_reg;
   result.max_half_reg = v->info.max_half_reg;
   result.branchstack = ir3_shader_branchstack_hw(v);
   result.constlen_vec4 = v->constlen;
   result.const_data_dwords = const_data_dwords;
   result.const_data_words = owner->const_data_words;
   result.private_mem_bytes = v->pvtmem_size;
   result.shared_mem_bytes = v->shared_size;
   result.local_size[0] = v->local_size[0];
   result.local_size[1] = v->local_size[1];
   result.local_size[2] = v->local_size[2];

   uint32_t flags_out = 0u;
   if (input->stage == A840_STAGE_FRAGMENT) {
      if (v->fs.early_fragment_tests || !v->no_earlyz)
         flags_out |= A840_OUT_FLAG_EARLY_Z;
      if (v->writes_pos)
         flags_out |= A840_OUT_FLAG_WRITES_DEPTH;
      if (v->fs.uses_fbfetch_output || v->fb_read)
         flags_out |= A840_OUT_FLAG_USES_FBFETCH;
   }
   if (v->mergedregs)
      flags_out |= A840_OUT_FLAG_MERGED_REGS;
   if (v->early_preamble || v->info.early_preamble)
      flags_out |= A840_OUT_FLAG_EARLY_PREAMBLE;
   result.flags_out = flags_out;

   const struct ir3_const_state *cs = ir3_const_state(v);
   if (cs != NULL) {
      uint32_t n = cs->ubo_state.num_enabled;
      if (n > ADRENO_COMPILE_MAX_UBO_PUSH)
         n = ADRENO_COMPILE_MAX_UBO_PUSH;
      result.ubo_push_count = n;
      for (uint32_t i = 0; i < n; ++i) {
         const struct ir3_ubo_range *r = &cs->ubo_state.range[i];
         result.ubo_push[i].set_index = r->ubo.bindless ? r->ubo.bindless_base : 0u;
         result.ubo_push[i].binding_index = r->ubo.block;
         result.ubo_push[i].src_offset_vec4 = r->start / 16u;
         result.ubo_push[i].dst_const_vec4 = r->offset / 16u;
         result.ubo_push[i].size_vec4 =
            r->end > r->start ? (r->end - r->start) / 16u : 0u;
         result.ubo_push[i].reserved = 0u;
      }
   }

   result.vpc_varying_count = extract_vpc_varyings(v, result.vpc_varyings);
   result.sds_state_words = NULL;
   result.sds_state_dwords = 0u;
   result.reg_spills = v->info.stp_count;
   result.reg_fills = v->info.ldp_count;
   result.max_waves = v->info.max_waves > 0 ? (uint32_t)v->info.max_waves : 0u;
   owner->magic = A840_STAGE_OWNER_MAGIC;
   populate_stage_io_info(v, input->stage,
                          !(v->constant_data_size > 0u && v->info.constant_data_offset > 0u) &&
                             (v->imm_state.size > 0u && v->imm_state.values != NULL),
                          &owner->io_info);
   result.owner = owner;

   ir3_shader_destroy(shader);
   glsl_type_singleton_decref();
   ir3_compiler_destroy(compiler);

   *out_stage = result;
   return A840_COMPILE_OK;
}

bool
a840_ir3_get_stage_io_info(const A840CompiledStage *stage,
                           A840Ir3StageIoInfo *out)
{
   if (stage == NULL || out == NULL || stage->owner == NULL)
      return false;
   const A840CompiledStageOwner *owner = (const A840CompiledStageOwner *)stage->owner;
   if (owner->magic != A840_STAGE_OWNER_MAGIC)
      return false;
   *out = owner->io_info;
   return true;
}

void
a840_fdl_encode_buffer_view_a8xx(uint64_t gpu_va,
                                 uint32_t size_bytes,
                                 uint32_t elem_bytes,
                                 uint32_t out_words[16])
{
   if (out_words == NULL)
      return;
   memset(out_words, 0, sizeof(uint32_t) * 16u);
   if (gpu_va == 0u || size_bytes == 0u)
      return;

   enum pipe_format fmt = PIPE_FORMAT_R32_UINT;
   uint32_t step = 4u;
   if (elem_bytes == 1u) {
      fmt = PIPE_FORMAT_R8_UINT;
      step = 1u;
   } else if (elem_bytes == 2u) {
      fmt = PIPE_FORMAT_R16_UINT;
      step = 2u;
   }
   uint32_t elements = size_bytes / step;
   enum a6xx_format tex_fmt = fd6_texture_format(fmt, TILE6_LINEAR, false);
   enum a3xx_color_swap swap = fd6_texture_swap(fmt, TILE6_LINEAR, false);

   out_words[0] = A8XX_TEX_MEMOBJ_0_INSTANCE_DESC_BASE_LO((uint32_t)gpu_va);
   out_words[1] = A8XX_TEX_MEMOBJ_1_BASE_HI((uint32_t)(gpu_va >> 32)) |
                  A8XX_TEX_MEMOBJ_1_TYPE(A6XX_TEX_BUFFER) |
                  A8XX_TEX_MEMOBJ_1_STRUCTSIZETEXELS(1u);
   out_words[2] = A8XX_TEX_MEMOBJ_2_WIDTH(elements & ((1u << 15) - 1u)) |
                  A8XX_TEX_MEMOBJ_2_HEIGHT(elements >> 15) |
                  A8XX_TEX_MEMOBJ_2_SAMPLES(MSAA_ONE);
   out_words[3] = A8XX_TEX_MEMOBJ_3_FMT(tex_fmt) |
                  A8XX_TEX_MEMOBJ_3_SWAP(swap) |
                  A8XX_TEX_MEMOBJ_3_SWIZ_X(fdl8_swiz(PIPE_SWIZZLE_X)) |
                  A8XX_TEX_MEMOBJ_3_SWIZ_Y(fdl8_swiz(PIPE_SWIZZLE_Y)) |
                  A8XX_TEX_MEMOBJ_3_SWIZ_Z(fdl8_swiz(PIPE_SWIZZLE_Z)) |
                  A8XX_TEX_MEMOBJ_3_SWIZ_W(fdl8_swiz(PIPE_SWIZZLE_W));
   out_words[4] = A8XX_TEX_MEMOBJ_4_TILE_MODE(TILE6_LINEAR);
}

void
a840_fdl_encode_ubo_view_a8xx(uint64_t gpu_va,
                              uint32_t size_bytes,
                              uint32_t out_words[16])
{
   if (out_words == NULL)
      return;
   memset(out_words, 0, sizeof(uint32_t) * 16u);
   if (gpu_va == 0u || size_bytes == 0u)
      return;

   uint32_t range_vec4 = DIV_ROUND_UP(size_bytes, 16u);
   out_words[0] = A6XX_UBO_0_BASE_LO((uint32_t)gpu_va);
   out_words[1] = A6XX_UBO_1_BASE_HI((uint32_t)(gpu_va >> 32)) |
                  A6XX_UBO_1_SIZE(range_vec4);
}

void
a840_compile_stage_free(A840CompiledStage *stage)
{
   if (stage == NULL)
      return;
   if (stage->owner != NULL) {
      A840CompiledStageOwner *owner = (A840CompiledStageOwner *)stage->owner;
      free(owner->isa_words);
      free(owner->const_data_words);
      free(owner);
   }
   stage->isa_words = NULL;
   stage->isa_dwords = 0u;
   stage->const_data_words = NULL;
   stage->const_data_dwords = 0u;
   stage->sds_state_words = NULL;
   stage->sds_state_dwords = 0u;
   stage->owner = NULL;
}

uint64_t
a840_compile_abi_fingerprint(void)
{
   const uint64_t values[] = {
      ADRENO_COMPILE_ABI_VERSION,
      sizeof(A840CompileTarget),
      sizeof(A840UboPushRange),
      sizeof(A840StageInput),
      sizeof(A840CompiledStage),
      sizeof(A840TextureLayoutQuery),
      sizeof(A840TextureLayoutResult),
   };
   uint64_t hash = UINT64_C(14695981039346656037);
   for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      uint64_t value = values[i];
      for (unsigned int byte = 0; byte < 8u; ++byte) {
         hash ^= (uint8_t)value;
         hash *= UINT64_C(1099511628211);
         value >>= 8;
      }
   }
   return hash;
}

#else
typedef int a840_compile_ir3_stub_guard_t;
#endif
