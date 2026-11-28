/* SPDX-License-Identifier: MIT */
#ifdef A840_BUILD_MESA_IR3_BACKEND

#include "AdrenoCompileABI.h"
#include "a840_metal_opt_pass.h"
#include "a840_robustness_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "util/ralloc.h"
#include "util/u_math.h"

__attribute__((visibility("hidden")))
nir_shader *a840_ir3_spirv_to_lowered_nir(struct ir3_compiler *compiler,
                                          const A840StageInput *input,
                                          uint32_t effective_flags,
                                          struct ir3_shader_options *out_shader_opts,
                                          char *err_buf,
                                          size_t err_buf_bytes,
                                          A840CompileStatus *out_status);

struct a840_spirv_err_ctx {
   char *err_buf;
   size_t err_buf_bytes;
   bool has_error;
};

static void
a840_spirv_debug_cb(void *private_data,
                    enum nir_spirv_debug_level level,
                    size_t spirv_offset,
                    const char *message)
{
   struct a840_spirv_err_ctx *ctx = (struct a840_spirv_err_ctx *)private_data;
   if (ctx == NULL || ctx->err_buf == NULL || ctx->err_buf_bytes == 0)
      return;
   if (level == NIR_SPIRV_DEBUG_LEVEL_ERROR || !ctx->has_error) {
      (void)snprintf(ctx->err_buf, ctx->err_buf_bytes,
                     "spirv@%zu: %s", spirv_offset,
                     message ? message : "unknown SPIR-V error");
      if (level == NIR_SPIRV_DEBUG_LEVEL_ERROR)
         ctx->has_error = true;
   }
}

static mesa_shader_stage
a840_stage_to_mesa(uint32_t stage)
{
   switch (stage) {
   case A840_STAGE_VERTEX:   return MESA_SHADER_VERTEX;
   case A840_STAGE_FRAGMENT: return MESA_SHADER_FRAGMENT;
   case A840_STAGE_COMPUTE:  return MESA_SHADER_COMPUTE;
   default:                  return MESA_SHADER_NONE;
   }
}

static struct spirv_capabilities
a840_build_spirv_caps(void)
{
   struct spirv_capabilities c;
   memset(&c, 0, sizeof(c));

   /* Core Vulkan 1.2 + Adreno 840 (Gen8) + metal2vulkan capabilities */
   c.Matrix = true;
   c.Shader = true;
   c.Float16 = true;
   c.Float16Buffer = true;
   c.Int8 = true;
   c.Int16 = true;
   c.Int64 = true;
   c.Int64Atomics = true;

   c.ClipDistance = true;
   c.CullDistance = true;
   c.DrawParameters = true;
   c.MultiView = true;
   c.MultiViewport = true;
   c.ShaderLayer = true;
   c.ShaderViewportIndex = true;
   c.ShaderViewportIndexLayerEXT = true;

   c.DerivativeControl = true;
   c.InterpolationFunction = true;
   c.SampleRateShading = true;
   c.SampleMaskPostDepthCoverage = true;
   c.StencilExportEXT = true;
   c.DemoteToHelperInvocation = true;
   c.FragmentBarycentricKHR = true;
   c.InputAttachment = true;

   c.Sampled1D = true;
   c.Image1D = true;
   c.SampledBuffer = true;
   c.ImageBuffer = true;
   c.SampledCubeArray = true;
   c.ImageCubeArray = true;
   c.ImageMSArray = true;
   c.ImageQuery = true;
   c.ImageGatherExtended = true;
   c.MinLod = true;
   c.StorageImageExtendedFormats = true;
   c.StorageImageMultisample = true;
   c.StorageImageReadWithoutFormat = true;
   c.StorageImageWriteWithoutFormat = true;
   c.Int64ImageEXT = true;

   c.UniformBufferArrayDynamicIndexing = true;
   c.SampledImageArrayDynamicIndexing = true;
   c.StorageBufferArrayDynamicIndexing = true;
   c.StorageImageArrayDynamicIndexing = true;
   c.InputAttachmentArrayDynamicIndexing = true;
   c.UniformTexelBufferArrayDynamicIndexing = true;
   c.StorageTexelBufferArrayDynamicIndexing = true;
   c.RuntimeDescriptorArray = true;
   c.ShaderNonUniform = true;
   c.UniformBufferArrayNonUniformIndexing = true;
   c.SampledImageArrayNonUniformIndexing = true;
   c.StorageBufferArrayNonUniformIndexing = true;
   c.StorageImageArrayNonUniformIndexing = true;
   c.InputAttachmentArrayNonUniformIndexing = true;
   c.UniformTexelBufferArrayNonUniformIndexing = true;
   c.StorageTexelBufferArrayNonUniformIndexing = true;

   c.StorageBuffer16BitAccess = true;
   c.UniformAndStorageBuffer16BitAccess = true;
   c.StoragePushConstant16 = true;
   c.StorageInputOutput16 = true;
   c.StorageBuffer8BitAccess = true;
   c.UniformAndStorageBuffer8BitAccess = true;
   c.StoragePushConstant8 = true;

   c.PhysicalStorageBufferAddresses = true;
   c.VulkanMemoryModel = true;
   c.VulkanMemoryModelDeviceScope = true;
   c.VariablePointers = true;
   c.VariablePointersStorageBuffer = true;
   c.WorkgroupMemoryExplicitLayoutKHR = true;
   c.WorkgroupMemoryExplicitLayout8BitAccessKHR = true;
   c.WorkgroupMemoryExplicitLayout16BitAccessKHR = true;

   c.GroupNonUniform = true;
   c.GroupNonUniformVote = true;
   c.GroupNonUniformArithmetic = true;
   c.GroupNonUniformBallot = true;
   c.GroupNonUniformShuffle = true;
   c.GroupNonUniformShuffleRelative = true;
   c.GroupNonUniformClustered = true;
   c.GroupNonUniformQuad = true;
   c.SubgroupBallotKHR = true;
   c.SubgroupVoteKHR = true;

   c.DenormPreserve = true;
   c.DenormFlushToZero = true;
   c.SignedZeroInfNanPreserve = true;
   c.RoundingModeRTE = true;
   c.RoundingModeRTZ = true;
   c.ComputeDerivativeGroupLinearKHR = true;
   c.ComputeDerivativeGroupQuadsKHR = true;
   c.AtomicFloat32MinMaxEXT = true;
   c.AtomicFloat16MinMaxEXT = true;

   return c;
}

static void
a840_shared_type_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   unsigned comp_size =
      glsl_type_is_boolean(type) ? 4u : glsl_get_bit_size(type) / 8u;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length;
   *align = comp_size;
}

struct a840_push_scan {
   bool has_push;
   uint32_t lo_bytes;
   uint32_t hi_bytes;
};

static bool
scan_push_const_cb(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)b;
   if (intr->intrinsic != nir_intrinsic_load_push_constant)
      return false;

   struct a840_push_scan *scan = (struct a840_push_scan *)data;
   uint32_t base = nir_intrinsic_base(intr);
   uint32_t range = nir_intrinsic_range(intr);
   uint32_t elem_bytes = intr->num_components * (intr->def.bit_size / 8u);
   uint32_t lo = base;
   uint32_t hi = base + (range > elem_bytes ? range : elem_bytes);

   if (nir_src_is_const(intr->src[0])) {
      uint32_t off = nir_src_as_uint(intr->src[0]);
      lo = base + off;
      hi = lo + elem_bytes;
   }

   if (!scan->has_push) {
      scan->has_push = true;
      scan->lo_bytes = lo;
      scan->hi_bytes = hi;
   } else {
      if (lo < scan->lo_bytes)
         scan->lo_bytes = lo;
      if (hi > scan->hi_bytes)
         scan->hi_bytes = hi;
   }
   return false;
}

struct a840_desc_lower_state {
   struct ir3_compiler *compiler;
   const A840StageInput *input;
   uint32_t push_const_dst_dwords;
   uint32_t push_const_lo_bytes;
   bool failed;
   char err_msg[256];
};

static void
lower_load_push_constant_a840(nir_builder *b, nir_intrinsic_instr *intr,
                              struct a840_desc_lower_state *st)
{
   /* Fold constant push words when the caller supplies const_push_mask bits. */
   if (st->input->const_push_mask != 0u &&
       intr->def.bit_size == 32u &&
       nir_src_is_const(intr->src[0])) {
      uint32_t byte_off = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]);
      if ((byte_off % 4u) == 0u) {
         uint32_t dword_off = byte_off / 4u;
         bool all_known = true;
         for (unsigned c = 0; c < intr->num_components; ++c) {
            if (dword_off + c >= 32u ||
                (st->input->const_push_mask & (1u << (dword_off + c))) == 0u) {
               all_known = false;
               break;
            }
         }
         if (all_known) {
            nir_const_value vals[NIR_MAX_VEC_COMPONENTS];
            for (unsigned c = 0; c < intr->num_components; ++c)
               vals[c] = nir_const_value_for_uint(
                  st->input->const_push_dwords[dword_off + c], 32);
            nir_def *imm = nir_build_imm(b, intr->num_components, 32, vals);
            nir_def_replace(&intr->def, imm);
            return;
         }
      }
   }

   uint32_t base = nir_intrinsic_base(intr);
   uint32_t dst_base = st->push_const_dst_dwords;
   nir_def *word_off = NULL;
   if (nir_src_is_const(intr->src[0])) {
      /* Fold base + const_offset before subtracting push_const_lo_bytes so
       * SPIR-V push loads with base=0, offset=64 never unsigned-underflow. */
      uint32_t byte_off = base + nir_src_as_uint(intr->src[0]);
      uint32_t rel_bytes = (byte_off >= st->push_const_lo_bytes)
                              ? (byte_off - st->push_const_lo_bytes)
                              : 0u;
      dst_base += rel_bytes / 4u;
      word_off = nir_imm_int(b, 0);
   } else {
      int32_t delta = (int32_t)base - (int32_t)st->push_const_lo_bytes;
      nir_def *adj = (delta != 0)
                        ? nir_iadd_imm(b, intr->src[0].ssa, delta)
                        : intr->src[0].ssa;
      word_off = nir_ushr_imm(b, adj, 2);
   }
   nir_def *load = nir_load_const_ir3(b, intr->num_components,
                                      intr->def.bit_size, word_off,
                                      .base = dst_base);
   nir_def_replace(&intr->def, load);
}

static bool
lower_ssbo_ubo_a840(nir_builder *b, nir_intrinsic_instr *intr,
                    struct a840_desc_lower_state *st)
{
   unsigned buffer_src = (intr->intrinsic == nir_intrinsic_store_ssbo) ? 1u : 0u;
   if (intr->src[buffer_src].ssa->num_components == 1u)
      return false;

   nir_scalar set_scalar = nir_scalar_resolved(intr->src[buffer_src].ssa, 0);
   nir_def *desc_idx = nir_channel(b, intr->src[buffer_src].ssa, 1);

   if (nir_scalar_is_const(set_scalar)) {
      unsigned set = nir_scalar_as_uint(set_scalar);
      if (set >= st->compiler->info->props.max_sets) {
         st->failed = true;
         (void)snprintf(st->err_msg, sizeof(st->err_msg),
                        "descriptor set %u exceeds A840 max_sets %u",
                        set, st->compiler->info->props.max_sets);
         return false;
      }
      nir_def *bindless = nir_bindless_resource_ir3(b, 32, desc_idx, .desc_set = set);
      nir_src_rewrite(&intr->src[buffer_src], bindless);
      return true;
   }

   /* Dynamic descriptor set index: emit constant-base if-ladder. */
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];
   unsigned max_sets = st->compiler->info->props.max_sets;
   if (max_sets > ADRENO_COMPILE_MAX_BIND_SETS)
      max_sets = ADRENO_COMPILE_MAX_BIND_SETS;
   nir_def *results[ADRENO_COMPILE_MAX_BIND_SETS] = { NULL };
   nir_def *base_idx = nir_mov_scalar(b, set_scalar);

   for (unsigned i = 0; i < max_sets; ++i) {
      nir_if *nif = nir_push_if(b, nir_ieq_imm(b, base_idx, (int)i));
      nir_def *bindless = nir_bindless_resource_ir3(b, 32, desc_idx, .desc_set = i);

      nir_intrinsic_instr *copy = nir_intrinsic_instr_create(b->shader, intr->intrinsic);
      copy->num_components = intr->num_components;
      for (unsigned s = 0; s < info->num_srcs; ++s) {
         copy->src[s] = nir_src_for_ssa(s == buffer_src ? bindless : intr->src[s].ssa);
      }
      for (unsigned idx = 0; idx < info->num_indices; ++idx)
         copy->const_index[idx] = intr->const_index[idx];
      if (info->has_dest) {
         nir_def_init(&copy->instr, &copy->def,
                      intr->def.num_components, intr->def.bit_size);
         results[i] = &copy->def;
      }
      nir_builder_instr_insert(b, &copy->instr);
      nir_push_else(b, nif);
   }

   nir_def *res = info->has_dest
                     ? nir_undef(b, intr->def.num_components, intr->def.bit_size)
                     : NULL;
   for (int i = (int)max_sets - 1; i >= 0; --i) {
      nir_pop_if(b, NULL);
      if (info->has_dest)
         res = nir_if_phi(b, results[i], res);
   }
   if (info->has_dest)
      nir_def_rewrite_uses(&intr->def, res);
   nir_instr_remove(&intr->instr);
   return true;
}

static nir_def *
build_bindless_from_deref(nir_builder *b, nir_deref_instr *deref,
                          unsigned sub_offset, unsigned stride,
                          struct a840_desc_lower_state *st)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var == NULL) {
      st->failed = true;
      (void)snprintf(st->err_msg, sizeof(st->err_msg),
                     "descriptor deref has no backing variable");
      return nir_bindless_resource_ir3(b, 32, nir_imm_int(b, 0), .desc_set = 0);
   }

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   if (set >= st->compiler->info->props.max_sets) {
      st->failed = true;
      (void)snprintf(st->err_msg, sizeof(st->err_msg),
                     "descriptor set %u exceeds A840 max_sets %u",
                     set, st->compiler->info->props.max_sets);
      return nir_bindless_resource_ir3(b, 32, nir_imm_int(b, 0), .desc_set = 0);
   }

   nir_def *arr_idx = NULL;
   for (nir_deref_instr *d = deref; d != NULL && d->deref_type != nir_deref_type_var;
        d = nir_deref_instr_parent(d)) {
      if (d->deref_type == nir_deref_type_array) {
         nir_def *idx = d->arr.index.ssa;
         if (idx->bit_size != 32u)
            idx = nir_u2u32(b, idx);
         unsigned elem_count = glsl_get_aoa_size(d->type);
         if (elem_count > 1u)
            idx = nir_imul_imm(b, idx, elem_count);
         arr_idx = (arr_idx == NULL) ? idx : nir_iadd(b, arr_idx, idx);
      }
   }

   nir_def *slot = nir_imm_int(b, (int)(binding + sub_offset));
   if (arr_idx != NULL) {
      if (stride > 1u)
         arr_idx = nir_imul_imm(b, arr_idx, stride);
      slot = nir_iadd(b, slot, arr_idx);
   }
   return nir_bindless_resource_ir3(b, 32, slot, .desc_set = set);
}

static bool
lower_tex_a840(nir_builder *b, nir_tex_instr *tex, struct a840_desc_lower_state *st)
{
   bool progress = false;
   int tex_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   int samp_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);

   nir_variable *tex_var = NULL;
   nir_variable *samp_var = NULL;
   if (tex_idx >= 0) {
      nir_deref_instr *td = nir_src_as_deref(tex->src[tex_idx].src);
      if (td != NULL)
         tex_var = nir_deref_instr_get_variable(td);
   }
   if (samp_idx >= 0) {
      nir_deref_instr *sd = nir_src_as_deref(tex->src[samp_idx].src);
      if (sd != NULL)
         samp_var = nir_deref_instr_get_variable(sd);
   }
   bool combined = (tex_var != NULL && tex_var == samp_var);

   if (tex_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_idx].src);
      nir_def *bindless = build_bindless_from_deref(
         b, deref, 0u, combined ? 2u : 1u, st);
      nir_src_rewrite(&tex->src[tex_idx].src, bindless);
      tex->src[tex_idx].src_type = nir_tex_src_texture_handle;
      tex->texture_index = 0;
      progress = true;
   }

   samp_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (samp_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[samp_idx].src);
      nir_def *bindless = build_bindless_from_deref(
         b, deref, combined ? 1u : 0u, combined ? 2u : 1u, st);
      nir_src_rewrite(&tex->src[samp_idx].src, bindless);
      tex->src[samp_idx].src_type = nir_tex_src_sampler_handle;
      tex->sampler_index = 0;
      progress = true;
   }

   return progress;
}

static bool
lower_desc_instr_cb(nir_builder *b, nir_instr *instr, void *data)
{
   struct a840_desc_lower_state *st = (struct a840_desc_lower_state *)data;
   b->cursor = nir_before_instr(instr);

   if (instr->type == nir_instr_type_tex)
      return lower_tex_a840(b, nir_instr_as_tex(instr), st);

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_push_constant:
      lower_load_push_constant_a840(b, intr, st);
      return true;

   case nir_intrinsic_vulkan_resource_index: {
      unsigned set = nir_intrinsic_desc_set(intr);
      unsigned binding = nir_intrinsic_binding(intr);
      nir_def *idx = intr->src[0].ssa;
      nir_def *def = nir_vec3(b, nir_imm_int(b, (int)set),
                              nir_iadd(b, nir_imm_int(b, (int)binding), idx),
                              nir_imm_int(b, 0));
      nir_def_replace(&intr->def, def);
      return true;
   }

   case nir_intrinsic_vulkan_resource_reindex: {
      nir_def *old_idx = intr->src[0].ssa;
      nir_def *delta = intr->src[1].ssa;
      nir_def *shift = nir_channel(b, old_idx, 2);
      nir_def *def = nir_vec3(b, nir_channel(b, old_idx, 0),
                              nir_iadd(b, nir_channel(b, old_idx, 1),
                                       nir_ishl(b, delta, shift)),
                              shift);
      nir_def_replace(&intr->def, def);
      return true;
   }

   case nir_intrinsic_load_vulkan_descriptor: {
      nir_def *old_idx = intr->src[0].ssa;
      nir_def *def = nir_vec3(b, nir_channel(b, old_idx, 0),
                              nir_channel(b, old_idx, 1),
                              nir_imm_int(b, 0));
      nir_def_replace(&intr->def, def);
      return true;
   }

   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_uav_ir3:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_get_ssbo_size:
      return lower_ssbo_ubo_a840(b, intr, st);

   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      nir_def *bindless = build_bindless_from_deref(b, deref, 0u, 1u, st);
      nir_rewrite_image_intrinsic(intr, bindless, nir_image_intrinsic_type_bindless);
      return true;
   }

   case nir_intrinsic_load_view_index:
      nir_def_replace(&intr->def, nir_imm_int(b, 0));
      return true;

   default:
      return false;
   }
}

static struct nir_spirv_specialization *
a840_build_workgroup_spec(const uint32_t *words, size_t word_count,
                          const uint32_t local_size[3])
{
   if (word_count < 5u || local_size[0] == 0u || local_size[1] == 0u || local_size[2] == 0u)
      return NULL;

   /* Pass 1: find ID decorated with BuiltIn WorkgroupSize (25). */
   uint32_t wg_id = 0u;
   for (size_t i = 5u; i < word_count; ) {
      uint32_t wc = words[i] >> 16;
      uint32_t op = words[i] & 0xffffu;
      if (wc == 0u || i + wc > word_count)
         return NULL;
      if (op == SpvOpDecorate && wc >= 4u &&
          words[i + 2u] == SpvDecorationBuiltIn &&
          words[i + 3u] == SpvBuiltInWorkgroupSize) {
         wg_id = words[i + 1u];
         break;
      }
      i += wc;
   }
   if (wg_id == 0u)
      return NULL;

   /* Pass 2: find OpSpecConstantComposite defining wg_id to get its 3 constituent IDs. */
   uint32_t comp_ids[3] = { 0u, 0u, 0u };
   bool found_comp = false;
   for (size_t i = 5u; i < word_count; ) {
      uint32_t wc = words[i] >> 16;
      uint32_t op = words[i] & 0xffffu;
      if (wc == 0u || i + wc > word_count)
         return NULL;
      if (op == SpvOpSpecConstantComposite && wc == 6u && words[i + 2u] == wg_id) {
         comp_ids[0] = words[i + 3u];
         comp_ids[1] = words[i + 4u];
         comp_ids[2] = words[i + 5u];
         found_comp = true;
         break;
      }
      i += wc;
   }
   if (!found_comp)
      return NULL;

   /* Pass 3: find SpecId decorations on each constituent ID. */
   uint32_t spec_ids[3] = { 0u, 0u, 0u };
   bool has_spec[3] = { false, false, false };
   for (size_t i = 5u; i < word_count; ) {
      uint32_t wc = words[i] >> 16;
      uint32_t op = words[i] & 0xffffu;
      if (wc == 0u || i + wc > word_count)
         return NULL;
      if (op == SpvOpDecorate && wc >= 4u && words[i + 2u] == SpvDecorationSpecId) {
         for (unsigned k = 0; k < 3u; ++k) {
            if (words[i + 1u] == comp_ids[k]) {
               spec_ids[k] = words[i + 3u];
               has_spec[k] = true;
            }
         }
      }
      i += wc;
   }

   uint32_t count = (has_spec[0] ? 1u : 0u) + (has_spec[1] ? 1u : 0u) + (has_spec[2] ? 1u : 0u);
   if (count == 0u)
      return NULL;

   struct nir_spirv_specialization *spec = vtn_alloc_specialization(count);
   if (spec == NULL)
      return NULL;

   uint32_t slot = 0u;
   for (unsigned k = 0; k < 3u; ++k) {
      if (!has_spec[k])
         continue;
      if (!vtn_add_specialization_entry(spec, slot++, spec_ids[k],
                                        sizeof(uint32_t), &local_size[k], true)) {
         vtn_free_specialization(spec);
         return NULL;
      }
   }
   return spec;
}

/* Wave 9 / Lane 2: NIR pass helpers for A840 robustness policy.
 *
 * 1. Scan NIR shader for image/fbfetch usage so the policy engine can always
 *    preserve Apple XR10 wide-color and image/FBFetch OOB semantics.
 * 2. When ROBUST_OFF is granted (backed by the Phase 2 shared 64 KiB RO zero
 *    page), mark UBO/SSBO loads with ACCESS_IN_BOUNDS | ACCESS_CAN_SPECULATE
 *    and eliminate redundant software buffer bounds clamps/checks against
 *    nir_intrinsic_get_ssbo_size (while strictly leaving image/tex/XR paths
 *    untouched).
 */
struct a840_robust_scan {
   bool has_storage_images;
   bool has_fbfetch;
};

static bool
a840_scan_robust_features_cb(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)b;
   struct a840_robust_scan *st = (struct a840_robust_scan *)data;
   switch (intr->intrinsic) {
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_image_size:
   case nir_intrinsic_image_samples:
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_sparse_load:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
   case nir_intrinsic_bindless_image_size:
   case nir_intrinsic_bindless_image_samples:
      st->has_storage_images = true;
      break;
   case nir_intrinsic_load_frag_coord_gmem_ir3:
      st->has_fbfetch = true;
      break;
   default:
      break;
   }
   return false;
}

static bool
a840_def_depends_on_ssbo_size(nir_def *def, unsigned depth)
{
   if (def == NULL || depth > 8u)
      return false;
   nir_instr *parent = nir_def_instr(def);
   if (parent->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(parent);
      return intr->intrinsic == nir_intrinsic_get_ssbo_size ||
             intr->intrinsic == nir_intrinsic_get_ubo_size ||
             intr->intrinsic == nir_intrinsic_deref_buffer_array_length;
   }
   if (parent->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(parent);
      unsigned num_srcs = nir_op_infos[alu->op].num_inputs;
      for (unsigned i = 0; i < num_srcs; ++i) {
         if (a840_def_depends_on_ssbo_size(alu->src[i].src.ssa, depth + 1u))
            return true;
      }
   }
   return false;
}

static bool
a840_def_is_const_or_const_vec(nir_def *def, unsigned depth)
{
   if (def == NULL || depth > 4u)
      return false;
   nir_instr *parent = nir_def_instr(def);
   if (parent->type == nir_instr_type_load_const)
      return true;
   if (parent->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(parent);
      if (nir_op_is_vec(alu->op) || alu->op == nir_op_mov) {
         unsigned num_srcs = nir_op_infos[alu->op].num_inputs;
         for (unsigned i = 0; i < num_srcs; ++i) {
            if (!a840_def_is_const_or_const_vec(alu->src[i].src.ssa, depth + 1u))
               return false;
         }
         return true;
      }
   }
   return false;
}

static nir_def *
a840_strip_ssbo_size_clamp_rec(nir_def *def, unsigned depth)
{
   if (def == NULL || depth > 4u)
      return NULL;
   nir_instr *instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu)
      return NULL;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   /* Pattern A: umin/imin(offset, expr(get_ssbo_size)) -> offset */
   if (alu->op == nir_op_umin || alu->op == nir_op_imin) {
      nir_def *s0 = alu->src[0].src.ssa;
      nir_def *s1 = alu->src[1].src.ssa;
      bool d0 = a840_def_depends_on_ssbo_size(s0, 0u);
      bool d1 = a840_def_depends_on_ssbo_size(s1, 0u);
      if (d0 != d1)
         return d0 ? s1 : s0;
   }

   /* Pattern B: bcsel(cmp(offset, expr(get_ssbo_size)), a, b) -> non-const branch.
    * Also handles when one branch is a clamped size expression (e.g. size - 1).
    */
   if (alu->op == nir_op_bcsel) {
      nir_def *cond = alu->src[0].src.ssa;
      if (a840_def_depends_on_ssbo_size(cond, 0u)) {
         nir_def *tval = alu->src[1].src.ssa;
         nir_def *fval = alu->src[2].src.ssa;
         bool t_clamp = a840_def_is_const_or_const_vec(tval, 0u) ||
                        a840_def_depends_on_ssbo_size(tval, 0u);
         bool f_clamp = a840_def_is_const_or_const_vec(fval, 0u) ||
                        a840_def_depends_on_ssbo_size(fval, 0u);
         if (t_clamp && !f_clamp)
            return fval;
         if (f_clamp && !t_clamp)
            return tval;
      }
   }

   /* Pattern C: bit-masking a clamped offset (e.g. iand(mask_from_ssbo_size, offset)) */
   if (alu->op == nir_op_iand) {
      nir_def *s0 = alu->src[0].src.ssa;
      nir_def *s1 = alu->src[1].src.ssa;
      bool d0 = a840_def_depends_on_ssbo_size(s0, 0u);
      bool d1 = a840_def_depends_on_ssbo_size(s1, 0u);
      if (d0 != d1)
         return d0 ? s1 : s0;
   }

   /* Pattern D: rewrite inside address arithmetic (ishl / ishr / ushr / imul / iadd / mov / u2u / i2i) */
   if (alu->op == nir_op_ishl || alu->op == nir_op_imul ||
       alu->op == nir_op_amul || alu->op == nir_op_iadd ||
       alu->op == nir_op_isub || alu->op == nir_op_ushr ||
       alu->op == nir_op_ishr || alu->op == nir_op_mov) {
      unsigned n = nir_op_infos[alu->op].num_inputs;
      for (unsigned i = 0; i < n; ++i) {
         nir_def *inner = a840_strip_ssbo_size_clamp_rec(alu->src[i].src.ssa, depth + 1u);
         if (inner != NULL) {
            nir_src_rewrite(&alu->src[i].src, inner);
            return def;
         }
      }
   }

   return NULL;
}

static bool
a840_elide_buffer_bounds_cb(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)b;
   (void)data;
   unsigned offset_src = ~0u;
   bool is_load = false;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo:
      offset_src = 1u;
      is_load = true;
      break;
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_ssbo_ir3:
   case nir_intrinsic_load_uav_ir3:
      offset_src = 1u;
      is_load = true;
      break;
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_store_ssbo_ir3:
      offset_src = 2u;
      break;
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_ssbo_atomic_ir3:
   case nir_intrinsic_ssbo_atomic_swap_ir3:
      offset_src = 1u;
      break;
   default:
      return false;
   }

   bool progress = false;

   /* Mark UBO/SSBO reads in-bounds and speculatable when 64 KiB RO zero-page
    * guard backs OOB reads; this enables ir3 load/store vectorization and
    * preamble hoisting without software bounds checks.
    */
   if (is_load && nir_intrinsic_has_access(intr)) {
      enum gl_access_qualifier acc = nir_intrinsic_access(intr);
      enum gl_access_qualifier add = (enum gl_access_qualifier)(ACCESS_IN_BOUNDS | ACCESS_CAN_SPECULATE);
      if ((acc & add) != add) {
         nir_intrinsic_set_access(intr, (enum gl_access_qualifier)(acc | add));
         progress = true;
      }
   }

   /* Strip software buffer bounds clamps derived from get_ssbo_size/get_ubo_size
    * on the buffer offset operand(s), walking through address arithmetic (ishl/imul/iadd).
    * For ir3-lowered SSBO intrinsics (load_ssbo_ir3/store_ssbo_ir3), both byte-offset
    * and dword-offset operands are checked.
    */
   unsigned off_indices[2] = { offset_src, ~0u };
   if (intr->intrinsic == nir_intrinsic_load_ssbo_ir3 ||
       intr->intrinsic == nir_intrinsic_ssbo_atomic_ir3 ||
       intr->intrinsic == nir_intrinsic_ssbo_atomic_swap_ir3) {
      off_indices[1] = 2u;
   } else if (intr->intrinsic == nir_intrinsic_store_ssbo_ir3) {
      off_indices[1] = 3u;
   }
   for (unsigned oi = 0u; oi < 2u; ++oi) {
      if (off_indices[oi] == ~0u)
         continue;
      nir_def *off = intr->src[off_indices[oi]].ssa;
      nir_def *unclamped = a840_strip_ssbo_size_clamp_rec(off, 0u);
      if (unclamped != NULL) {
         if (unclamped != off)
            nir_src_rewrite(&intr->src[off_indices[oi]], unclamped);
         progress = true;
      }
   }

   /* If a buffer load result is masked by bcsel(cmp_against_ssbo_size, load, const)
    * (including scalarized or vector-composed fallback constants), simplify the
    * select directly to the load because the 64 KiB RO zero page prevents faults.
    */
   if (is_load) {
      nir_foreach_use_safe(use_src, &intr->def) {
         nir_instr *use_instr = nir_src_use_instr(use_src);
         if (use_instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *sel = nir_instr_as_alu(use_instr);
         if (sel->op == nir_op_bcsel &&
             a840_def_depends_on_ssbo_size(sel->src[0].src.ssa, 0u)) {
            nir_def *other = NULL;
            if (sel->src[1].src.ssa == &intr->def)
               other = sel->src[2].src.ssa;
            else if (sel->src[2].src.ssa == &intr->def)
               other = sel->src[1].src.ssa;
            if (a840_def_is_const_or_const_vec(other, 0u)) {
               nir_def_rewrite_uses(&sel->def, &intr->def);
               progress = true;
            }
         } else if (sel->op == nir_op_mov) {
            /* Handle component extract (mov load.c) feeding a scalarized bcsel */
            nir_foreach_use_safe(cuse, &sel->def) {
               nir_instr *cinstr = nir_src_use_instr(cuse);
               if (cinstr->type != nir_instr_type_alu)
                  continue;
               nir_alu_instr *csel = nir_instr_as_alu(cinstr);
               if (csel->op != nir_op_bcsel)
                  continue;
               if (!a840_def_depends_on_ssbo_size(csel->src[0].src.ssa, 0u))
                  continue;
               nir_def *other = NULL;
               if (csel->src[1].src.ssa == &sel->def)
                  other = csel->src[2].src.ssa;
               else if (csel->src[2].src.ssa == &sel->def)
                  other = csel->src[1].src.ssa;
               if (a840_def_is_const_or_const_vec(other, 0u)) {
                  nir_def_rewrite_uses(&csel->def, &sel->def);
                  progress = true;
               }
            }
         }
      }
   }

   return progress;
}

static bool
lower_frag_coord_gmem_ir3_cb(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_frag_coord_gmem_ir3)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *unscaled = nir_load_frag_coord_unscaled_ir3(b);
   nir_def_replace(&intr->def, unscaled);
   return true;
}

__attribute__((visibility("hidden")))
nir_shader *
a840_ir3_spirv_to_lowered_nir(struct ir3_compiler *compiler,
                              const A840StageInput *input,
                              uint32_t effective_flags,
                              struct ir3_shader_options *out_shader_opts,
                              char *err_buf,
                              size_t err_buf_bytes,
                              A840CompileStatus *out_status)
{
   mesa_shader_stage stage = a840_stage_to_mesa(input->stage);
   if (stage == MESA_SHADER_NONE) {
      *out_status = A840_COMPILE_INVALID_ARG;
      if (err_buf != NULL && err_buf_bytes != 0)
         (void)snprintf(err_buf, err_buf_bytes, "invalid shader stage");
      return NULL;
   }

   struct spirv_capabilities caps = a840_build_spirv_caps();
   struct a840_spirv_err_ctx err_ctx = {
      .err_buf = err_buf,
      .err_buf_bytes = err_buf_bytes,
      .has_error = false,
   };

   struct spirv_to_nir_options spv_opts;
   memset(&spv_opts, 0, sizeof(spv_opts));
   spv_opts.environment = NIR_SPIRV_VULKAN;
   spv_opts.capabilities = &caps;
   spv_opts.mediump_16bit_alu = true;
   spv_opts.ubo_addr_format = nir_address_format_vec2_index_32bit_offset;
   spv_opts.ssbo_addr_format = nir_address_format_vec2_index_32bit_offset;
   spv_opts.phys_ssbo_addr_format = nir_address_format_64bit_global;
   spv_opts.push_const_addr_format = nir_address_format_logical;
   spv_opts.shared_addr_format = nir_address_format_32bit_offset;
   spv_opts.global_addr_format = nir_address_format_64bit_global;
   spv_opts.min_ubo_alignment = 64u;
   spv_opts.min_ssbo_alignment = 4u;
   spv_opts.skip_os_break_in_debug_build = true;
   spv_opts.ignore_replacement = true;
   spv_opts.debug.func = a840_spirv_debug_cb;
   spv_opts.debug.private_data = &err_ctx;

   const nir_shader_compiler_options *nir_compiler_opts =
      ir3_get_compiler_options(compiler);

   size_t word_count = input->spirv_bytes / sizeof(uint32_t);
   struct nir_spirv_specialization *spec =
      (stage == MESA_SHADER_COMPUTE)
         ? a840_build_workgroup_spec(input->spirv_words, word_count, input->local_size)
         : NULL;

   nir_shader *nir = spirv_to_nir(input->spirv_words,
                                  word_count,
                                  spec, stage, input->entry_point,
                                  &spv_opts, nir_compiler_opts);
   if (spec != NULL)
      vtn_free_specialization(spec);
   if (nir == NULL) {
      *out_status = A840_COMPILE_SPIRV_ERROR;
      if (err_buf != NULL && err_buf_bytes != 0 && !err_ctx.has_error && err_buf[0] == '\0')
         (void)snprintf(err_buf, err_buf_bytes,
                        "spirv_to_nir failed for entry point '%s'", input->entry_point);
      return NULL;
   }

   /* ir3 tracks bindful UBOs/SSBOs via num_ubos/num_ssbos; bindless descriptors start at 0. */
   nir->info.num_ubos = 0;
   nir->info.num_ssbos = 0;
   nir->info.internal = false;

   if ((effective_flags & A840_COMPILE_FLAG_HALF_ROUND_RTZ) != 0u) {
      nir->info.float_controls_execution_mode |= FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16;
   }

   /* Apply caller-supplied compute workgroup specialization if present. */
   if (stage == MESA_SHADER_COMPUTE &&
       input->local_size[0] > 0u &&
       input->local_size[1] > 0u &&
       input->local_size[2] > 0u) {
      if (nir->info.workgroup_size_variable || nir->info.workgroup_size[0] == 0u) {
         nir->info.workgroup_size[0] = (uint16_t)input->local_size[0];
         nir->info.workgroup_size[1] = (uint16_t)input->local_size[1];
         nir->info.workgroup_size[2] = (uint16_t)input->local_size[2];
         nir->info.workgroup_size_variable = false;
      }
   }

   /* Standard post-SPIR-V function inlining and variable normalization. */
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);

   NIR_PASS(_, nir, nir_lower_variable_initializers, (nir_variable_mode)~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   nir_gather_clip_cull_distance_sizes_from_vars(nir);
   NIR_PASS(_, nir, nir_merge_clip_cull_distance_vars);
   NIR_PASS(_, nir, nir_propagate_invariant, false);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_opt_find_array_copies);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_dce);

   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_lower_sysvals_to_varyings_options primid_opts = {
         .primitive_id = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &primid_opts);
   }

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_mediump_vars,
            nir_var_function_temp | nir_var_shader_temp | nir_var_mem_shared);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_combine_stores, nir_var_all);

   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_is_helper_invocation);

   struct ir3_optimize_options opt_opts;
   memset(&opt_opts, 0, sizeof(opt_opts));
   ir3_optimize_loop(compiler, &opt_opts, nir);

   const nir_opt_peephole_select_options peephole_opts = {
      .limit = 0,
      .discard_ok = true,
   };
   NIR_PASS(_, nir, nir_opt_peephole_select, &peephole_opts);

   const nir_opt_access_options access_opts = {
      .is_vulkan = true,
   };
   NIR_PASS(_, nir, nir_opt_access, &access_opts);

   if (stage == MESA_SHADER_FRAGMENT) {
      const bool fbfetch_gmem =
         (effective_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u;
      const nir_input_attachment_options att_opts = {
         .use_view_id_for_layer = false,
         .gmem_depth_stencil_ir3 = fbfetch_gmem,
         .gmem_input_attachment_ir3 = fbfetch_gmem ? ~0u : 0u,
      };
      NIR_PASS(_, nir, nir_lower_input_attachments, &att_opts);
      if (fbfetch_gmem) {
         NIR_PASS(_, nir, nir_shader_intrinsics_pass,
                  lower_frag_coord_gmem_ir3_cb,
                  nir_metadata_control_flow, NULL);
      }

      const nir_lower_sysvals_to_varyings_options fs_sysval_opts = {
         .point_coord = true,
         .layer_id = true,
         .view_index = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &fs_sysval_opts);
   }

   NIR_PASS(_, nir, nir_opt_acquire_release_barriers, SCOPE_QUEUE_FAMILY);
   ir3_nir_lower_io_vars_to_temporaries(nir);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_vec2_index_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);

   if (stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, a840_shared_type_info);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
               nir_address_format_32bit_offset);

      const struct nir_lower_compute_system_values_options cs_sysval_opts = {
         .has_base_workgroup_id = false,
      };
      NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_sysval_opts);
   }

   nir_assign_io_var_locations(nir, nir_var_shader_in);
   nir_assign_io_var_locations(nir, nir_var_shader_out);

   {
      nir_lower_mem_access_bit_sizes_options bit_opts = {
         .callback = ir3_mem_access_size_align,
         .modes = nir_var_mem_push_const,
      };
      NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &bit_opts);
   }

   ir3_nir_lower_io(nir);
   NIR_PASS(_, nir, nir_recompute_io_bases,
            nir_var_shader_in | nir_var_shader_out);

   /* Prepare ir3_shader_options and evaluate Wave 9 robustness safety policy.
    * Match Mesa Turnip tu_shader_key_subgroup_size() (tu_shader.cc:3761):
    * Adreno 840 (gen80200) has supports_double_threadsize = false (dual-wave
    * dispatch replaces THREAD128) and executes 64-lane waves across VS/FS/CS.
    * Never compile IR3_SINGLE_OR_DOUBLE on targets lacking double threadsize. */
   memset(out_shader_opts, 0, sizeof(*out_shader_opts));
   const enum ir3_wavesize_option wavesize_opt =
      (compiler != NULL && compiler->info != NULL &&
       compiler->info->props.supports_double_threadsize)
         ? IR3_SINGLE_OR_DOUBLE
         : IR3_SINGLE_ONLY;
   out_shader_opts->api_wavesize = wavesize_opt;
   out_shader_opts->real_wavesize = wavesize_opt;

   struct a840_robust_scan robust_scan;
   memset(&robust_scan, 0, sizeof(robust_scan));
   nir_shader_intrinsics_pass(nir, a840_scan_robust_features_cb,
                              nir_metadata_all, &robust_scan);
   if (stage == MESA_SHADER_FRAGMENT &&
       (effective_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u) {
      robust_scan.has_fbfetch = true;
   }

   /* Compile-time policy contract: Phase 2 shared 64 KiB RO zero page backs
    * ROBUST_OFF requests; Apple XR10 and image/FBFetch OOB rules are always
    * preserved. Without A840_COMPILE_FLAG_ROBUST_OFF, robust_modes stays
    * identical to Phase 1-4 defaults (nir_var_mem_ubo | nir_var_mem_ssbo).
    */
   A840RobustnessDecision robust_dec;
   A840CompileStatus pol_st = a840_robustness_decide_for_stage(
      input, effective_flags, true /* zero_page_ready */,
      robust_scan.has_storage_images, robust_scan.has_fbfetch,
      &robust_dec);
   if (pol_st != A840_COMPILE_OK) {
      *out_status = pol_st;
      if (err_buf != NULL && err_buf_bytes != 0)
         (void)snprintf(err_buf, err_buf_bytes, "robustness policy evaluation failed");
      ralloc_free(nir);
      return NULL;
   }

   nir_variable_mode resolved_robust_modes = (nir_variable_mode)0;
   if ((robust_dec.nir_robust_modes & A840_NIR_ROBUST_MODE_UBO) != 0u)
      resolved_robust_modes = (nir_variable_mode)(resolved_robust_modes | nir_var_mem_ubo);
   if ((robust_dec.nir_robust_modes & A840_NIR_ROBUST_MODE_SSBO) != 0u)
      resolved_robust_modes = (nir_variable_mode)(resolved_robust_modes | nir_var_mem_ssbo);
   out_shader_opts->nir_options.robust_modes = resolved_robust_modes;

   if (robust_dec.elide_buffer_bounds) {
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      bool elide_progress = false;
      for (unsigned pass = 0u; pass < 2u; ++pass) {
         if (!nir_shader_intrinsics_pass(
                nir, a840_elide_buffer_bounds_cb, nir_metadata_control_flow, NULL)) {
            break;
         }
         elide_progress = true;
         NIR_PASS(_, nir, nir_opt_copy_prop);
         NIR_PASS(_, nir, nir_opt_dce);
      }
      if (elide_progress) {
         NIR_PASS(_, nir, nir_opt_algebraic);
         NIR_PASS(_, nir, nir_opt_constant_folding);
         NIR_PASS(_, nir, nir_opt_dce);
      }
   }

   /* Wave 10 / Lane 2: Pre-push Metal hotspot specialization (Pass 2 uniform/
    * push-constant folding + Pass 1 Apple FP16 rounding & conversion folding).
    */
   const A840MetalOptConfig metal_opt_cfg =
      a840_metal_opt_default_config(effective_flags);
   A840MetalOptReport metal_opt_report;
   memset(&metal_opt_report, 0, sizeof(metal_opt_report));
   (void)a840_metal_opt_run_nir_pre_push(nir, &metal_opt_cfg, &metal_opt_report);

   struct a840_push_scan push_scan;
   memset(&push_scan, 0, sizeof(push_scan));
   nir_shader_intrinsics_pass(nir, scan_push_const_cb, nir_metadata_all, &push_scan);

   uint32_t push_lo_bytes = 0u;
   uint32_t push_dst_dwords = 0u;
   if (push_scan.has_push) {
      uint32_t unit_dwords = compiler->const_upload_unit * 4u;
      if (unit_dwords == 0u)
         unit_dwords = 4u;
      uint32_t lo_dwords = (push_scan.lo_bytes / 4u) & ~(unit_dwords - 1u);
      uint32_t hi_dwords = DIV_ROUND_UP(push_scan.hi_bytes, 4u);
      uint32_t span_dwords = hi_dwords > lo_dwords ? (hi_dwords - lo_dwords) : 4u;
      uint32_t push_vec4 = align(DIV_ROUND_UP(span_dwords, 4u),
                                 compiler->const_upload_unit);

      ir3_const_alloc(&out_shader_opts->const_allocs,
                      IR3_CONST_ALLOC_PUSH_CONSTS,
                      push_vec4, compiler->const_upload_unit);
      push_lo_bytes = lo_dwords * 4u;
      push_dst_dwords =
         out_shader_opts->const_allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4 * 4u;

      out_shader_opts->push_consts_type = IR3_PUSH_CONSTS_PER_STAGE;
      out_shader_opts->push_consts_base = lo_dwords;
      out_shader_opts->push_consts_dwords = push_vec4 * 4u;
   } else {
      out_shader_opts->push_consts_type = IR3_PUSH_CONSTS_NONE;
   }

   struct a840_desc_lower_state desc_st;
   memset(&desc_st, 0, sizeof(desc_st));
   desc_st.compiler = compiler;
   desc_st.input = input;
   desc_st.push_const_dst_dwords = push_dst_dwords;
   desc_st.push_const_lo_bytes = push_lo_bytes;

   bool desc_progress = nir_shader_instructions_pass(
      nir, lower_desc_instr_cb, nir_metadata_none, &desc_st);
   if (desc_st.failed) {
      *out_status = A840_COMPILE_IR3_ERROR;
      if (err_buf != NULL && err_buf_bytes != 0)
         (void)snprintf(err_buf, err_buf_bytes, "%s", desc_st.err_msg);
      ralloc_free(nir);
      return NULL;
   }
   if (desc_progress)
      NIR_PASS(_, nir, nir_opt_dce);

   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_uniform | nir_var_mem_ubo | nir_var_mem_ssbo, NULL);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   ir3_finalize_nir(compiler, &out_shader_opts->nir_options, nir);

   /* Wave 10 / Lane 2: Post-finalize register-pressure reduction & sinking. */
   (void)a840_metal_opt_run_nir_post_finalize(nir, &metal_opt_cfg, &metal_opt_report);

   *out_status = A840_COMPILE_OK;
   return nir;
}

#else
typedef int a840_spirv_lower_stub_guard_t;
#endif
