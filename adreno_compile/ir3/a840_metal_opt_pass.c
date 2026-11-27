/* SPDX-License-Identifier: MIT */
/* Wave 10 / Lane 2: Adreno 840 ir3 Metal Hotspot Specialization Passes. */

#include "a840_metal_opt_pass.h"

#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define A840_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define A840_TLS __thread
#else
#define A840_TLS
#endif

static A840_TLS uint32_t g_a840_metal_opt_mask = A840_METAL_OPT_ALL;
static A840_TLS bool g_a840_metal_opt_has_report = false;
static A840_TLS A840MetalOptReport g_a840_metal_opt_last_report;

A840MetalOptConfig
a840_metal_opt_default_config(uint32_t effective_flags)
{
   A840MetalOptConfig cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.pass_mask = g_a840_metal_opt_mask & A840_METAL_OPT_ALL;
   cfg.preserve_xr10 = true;
   cfg.respect_half_rtz = (effective_flags & A840_COMPILE_FLAG_HALF_ROUND_RTZ) != 0u;
   cfg.preserve_push_layout = true;
   cfg.sink_pressure_thresh = A840_METAL_OPT_DEFAULT_SINK_THRESH;
   return cfg;
}

void
a840_metal_opt_set_thread_mask(uint32_t pass_mask)
{
   g_a840_metal_opt_mask = pass_mask & A840_METAL_OPT_ALL;
}

uint32_t
a840_metal_opt_get_thread_mask(void)
{
   return g_a840_metal_opt_mask & A840_METAL_OPT_ALL;
}

void
a840_metal_opt_reset_thread_mask(void)
{
   g_a840_metal_opt_mask = A840_METAL_OPT_ALL;
}

bool
a840_metal_opt_get_last_report(A840MetalOptReport *out_report)
{
   if (out_report == NULL || !g_a840_metal_opt_has_report)
      return false;
   *out_report = g_a840_metal_opt_last_report;
   return true;
}

void
a840_metal_opt_record_last_report(const A840MetalOptReport *report)
{
   if (report == NULL) {
      g_a840_metal_opt_has_report = false;
      memset(&g_a840_metal_opt_last_report, 0, sizeof(g_a840_metal_opt_last_report));
      return;
   }
   g_a840_metal_opt_last_report = *report;
   g_a840_metal_opt_has_report = true;
}

#ifdef A840_BUILD_MESA_IR3_BACKEND

#include <math.h>
#include <stdlib.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/half_float.h"

/* =========================================================================
 * Common Helpers: ALU counting & Apple XR10 Wide-Color Boundary Detection
 * ========================================================================= */

static uint32_t
a840_count_shader_alu(nir_shader *nir)
{
   uint32_t count = 0u;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu)
               count++;
         }
      }
   }
   return count;
}

/* Detect Apple XR10 wide-color clamp boundaries ([-0.752941, 1.25098]) so
 * FP16 precision narrowing never alters extended-range color math.
 */
static bool
a840_detect_xr10_shader(nir_shader *nir)
{
   bool seen_xr10_lo = false;
   bool seen_xr10_hi = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_load_const)
               continue;
            nir_load_const_instr *lc = nir_instr_as_load_const(instr);
            if (lc->def.bit_size != 32u)
               continue;
            for (unsigned c = 0u; c < lc->def.num_components; ++c) {
               float v = lc->value[c].f32;
               if (!isfinite(v))
                  continue;
               /* XR10 lower bound: -192/255 = -0.752941176... */
               if (v >= -0.760f && v <= -0.745f)
                  seen_xr10_lo = true;
               /* XR10 upper bound: 319/255 = 1.250980392... */
               if (v >= 1.245f && v <= 1.260f)
                  seen_xr10_hi = true;
            }
         }
      }
   }
   return seen_xr10_lo && seen_xr10_hi;
}

/* =========================================================================
 * Pass 1: Apple FP16 Rounding Alignment & Redundant f2f16/f2f32 Elimination
 * ========================================================================= */

static bool
a840_is_f2f16_op(nir_op op)
{
   return op == nir_op_f2f16 ||
          op == nir_op_f2f16_rtne ||
          op == nir_op_f2f16_rtz;
}

static bool
a840_alu_is_strict(const nir_alu_instr *alu)
{
   return (alu->fp_math_ctrl & (nir_fp_no_transform | nir_fp_preserve_sz_inf_nan)) != 0u;
}

static bool
a840_op_is_narrowable_f32_alu(nir_op op)
{
   switch (op) {
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_flrp:
   case nir_op_fabs:
   case nir_op_fneg:
   case nir_op_fsat:
   case nir_op_mov:
   case nir_op_bcsel:
      return true;
   default:
      return false;
   }
}

static bool
a840_f32_fits_f16_cleanly(float v, uint16_t *out_half)
{
   if (!isfinite(v))
      return false;
   if (v == 0.0f) {
      if (out_half != NULL)
         *out_half = _mesa_float_to_half(v);
      return true;
   }
   float av = fabsf(v);
   if (av > 65504.0f || av < 6.103515625e-5f)
      return false;
   uint16_t h = _mesa_float_to_half(v);
   float rt = _mesa_half_to_float(h);
   if (!isfinite(rt) || rt == 0.0f)
      return false;
   float err = fabsf(v - rt);
   if (err > av * 1.0e-3f + 1.0e-6f)
      return false;
   if (out_half != NULL)
      *out_half = h;
   return true;
}

static bool
a840_load_const_fits_f16(const nir_load_const_instr *lc,
                         const uint8_t swizzle[NIR_MAX_VEC_COMPONENTS],
                         unsigned num_components)
{
   if (lc->def.bit_size != 32u)
      return false;
   for (unsigned c = 0u; c < num_components; ++c) {
      unsigned sc = swizzle[c];
      if (sc >= lc->def.num_components)
         return false;
      if (!a840_f32_fits_f16_cleanly(lc->value[sc].f32, NULL))
         return false;
   }
   return true;
}

static nir_def *
a840_build_swizzled_copy(nir_builder *b,
                         nir_def *src_def,
                         const uint8_t swizzle[NIR_MAX_VEC_COMPONENTS],
                         unsigned num_components)
{
   bool identity = (src_def->num_components == num_components);
   if (identity) {
      for (unsigned c = 0u; c < num_components; ++c) {
         if (swizzle[c] != c) {
            identity = false;
            break;
         }
      }
   }
   if (identity)
      return src_def;

   nir_alu_src asrc;
   memset(&asrc, 0, sizeof(asrc));
   asrc.src = nir_src_for_ssa(src_def);
   for (unsigned c = 0u; c < num_components; ++c)
      asrc.swizzle[c] = swizzle[c];
   return nir_mov_alu(b, asrc, num_components);
}

/* Verify that every consumer of a candidate 32-bit ALU def stays inside a
 * narrowable pure-half cone (no external 32-bit observer such as a 32-bit RT
 * store_output, tex coord, or SSBO/UBO address/store).
 */
static bool
a840_def_uses_only_narrowable_or_f2f16(nir_def *def, unsigned depth)
{
   if (depth > 16u)
      return false;
   if (list_is_empty(&def->uses))
      return false;

   nir_foreach_use(use_src, def) {
      nir_instr *use_instr = nir_src_use_instr(use_src);
      if (use_instr->type != nir_instr_type_alu)
         return false;
      nir_alu_instr *use_alu = nir_instr_as_alu(use_instr);
      if (a840_alu_is_strict(use_alu))
         return false;
      if (a840_is_f2f16_op(use_alu->op))
         continue;
      if (!a840_op_is_narrowable_f32_alu(use_alu->op) ||
          use_alu->def.bit_size != 32u) {
         return false;
      }
      /* For bcsel, def must not be the boolean condition (src[0]). */
      if (use_alu->op == nir_op_bcsel && use_src == &use_alu->src[0].src)
         return false;
      if (!a840_def_uses_only_narrowable_or_f2f16(&use_alu->def, depth + 1u))
         return false;
   }
   return true;
}

/* Recursively verify that a 32-bit float expression cone originates strictly
 * from 16-bit values widened by non-strict f2f32 (plus FP16-clean constants),
 * with at least one genuine 16-bit leaf so pure-32-bit arithmetic is never
 * unexpectedly altered.
 */
static bool
a840_cone_is_pure_half(nir_def *def,
                       const uint8_t swizzle[NIR_MAX_VEC_COMPONENTS],
                       unsigned num_components,
                       unsigned depth,
                       bool *out_has_half_leaf)
{
   if (def == NULL || def->bit_size != 32u || depth > 16u)
      return false;

   nir_instr *parent = nir_def_instr(def);
   if (parent->type == nir_instr_type_load_const) {
      return a840_load_const_fits_f16(nir_instr_as_load_const(parent),
                                      swizzle, num_components);
   }
   if (parent->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(parent);
   if (a840_alu_is_strict(alu))
      return false;

   if (alu->op == nir_op_f2f32) {
      if (alu->src[0].src.ssa->bit_size == 16u) {
         *out_has_half_leaf = true;
         return true;
      }
      return false;
   }

   if (!a840_op_is_narrowable_f32_alu(alu->op))
      return false;

   if (!a840_def_uses_only_narrowable_or_f2f16(&alu->def, 0u))
      return false;

   unsigned num_inputs = nir_op_infos[alu->op].num_inputs;
   for (unsigned i = 0u; i < num_inputs; ++i) {
      if (alu->op == nir_op_bcsel && i == 0u) {
         /* Condition must be boolean (1-bit); it stays 1-bit after narrowing. */
         if (alu->src[0].src.ssa->bit_size != 1u)
            return false;
         continue;
      }
      uint8_t child_swiz[NIR_MAX_VEC_COMPONENTS];
      for (unsigned c = 0u; c < num_components; ++c)
         child_swiz[c] = alu->src[i].swizzle[swizzle[c]];
      if (!a840_cone_is_pure_half(alu->src[i].src.ssa, child_swiz,
                                  num_components, depth + 1u,
                                  out_has_half_leaf)) {
         return false;
      }
   }
   return true;
}

static nir_def *
a840_build_f2f16_with_op(nir_builder *b, nir_op conv_op, nir_def *src32)
{
   switch (conv_op) {
   case nir_op_f2f16_rtne:
      return nir_f2f16_rtne(b, src32);
   case nir_op_f2f16_rtz:
      return nir_f2f16_rtz(b, src32);
   default:
      return nir_f2f16(b, src32);
   }
}

static nir_def *
a840_build_narrowed_alu16(nir_builder *b,
                          nir_op op,
                          nir_def *srcs[NIR_ALU_MAX_INPUTS])
{
   switch (op) {
   case nir_op_fadd:  return nir_fadd(b, srcs[0], srcs[1]);
   case nir_op_fsub:  return nir_fsub(b, srcs[0], srcs[1]);
   case nir_op_fmul:  return nir_fmul(b, srcs[0], srcs[1]);
   case nir_op_ffma:  return nir_ffma(b, srcs[0], srcs[1], srcs[2]);
   case nir_op_fmin:  return nir_fmin(b, srcs[0], srcs[1]);
   case nir_op_fmax:  return nir_fmax(b, srcs[0], srcs[1]);
   case nir_op_flrp:  return nir_flrp(b, srcs[0], srcs[1], srcs[2]);
   case nir_op_fabs:  return nir_fabs(b, srcs[0]);
   case nir_op_fneg:  return nir_fneg(b, srcs[0]);
   case nir_op_fsat:  return nir_fsat(b, srcs[0]);
   case nir_op_mov:   return nir_mov(b, srcs[0]);
   case nir_op_bcsel: return nir_bcsel(b, srcs[0], srcs[1], srcs[2]);
   default:           return NULL;
   }
}

struct a840_fp16_pass_ctx {
   const A840MetalOptConfig *cfg;
   A840MetalOptReport *report;
   bool shader_rtz_fp16;
   bool xr10_guarded;
};

static bool
a840_fp16_elide_roundtrips_cb(nir_builder *b, nir_alu_instr *alu, void *data)
{
   struct a840_fp16_pass_ctx *ctx = (struct a840_fp16_pass_ctx *)data;
   if (!a840_is_f2f16_op(alu->op))
      return false;

   if (a840_alu_is_strict(alu)) {
      ctx->report->fp16_xr10_guarded++;
      return false;
   }

   nir_def *src_def = alu->src[0].src.ssa;
   if (src_def->bit_size != 32u)
      return false;
   nir_instr *parent = nir_def_instr(src_def);
   if (parent->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *inner = nir_instr_as_alu(parent);
   if (a840_alu_is_strict(inner)) {
      ctx->report->fp16_xr10_guarded++;
      return false;
   }

   /* Pattern 1A: f2f16*(f2f32(h16)) -> h16 (exact bit-preserving identity). */
   if (inner->op == nir_op_f2f32 && inner->src[0].src.ssa->bit_size == 16u) {
      unsigned ncomp = alu->def.num_components;
      uint8_t swiz[NIR_MAX_VEC_COMPONENTS];
      memset(swiz, 0, sizeof(swiz));
      for (unsigned c = 0u; c < ncomp; ++c)
         swiz[c] = inner->src[0].swizzle[alu->src[0].swizzle[c]];

      b->cursor = nir_before_instr(&alu->instr);
      nir_def *rep = a840_build_swizzled_copy(b, inner->src[0].src.ssa, swiz, ncomp);
      nir_def_replace(&alu->def, rep);
      ctx->report->fp16_roundtrip_elided++;
      return true;
   }

   /* Pattern 1B: Pull f2f16* through pure-half 32-bit float ALU cones. */
   if (!a840_op_is_narrowable_f32_alu(inner->op))
      return false;

   if (ctx->xr10_guarded && ctx->cfg->preserve_xr10) {
      ctx->report->fp16_xr10_guarded++;
      return false;
   }

   /* Respect explicit f2f16_rtz when shader is not executing FP16 in RTZ mode. */
   if (alu->op == nir_op_f2f16_rtz &&
       ctx->cfg->respect_half_rtz &&
       !ctx->shader_rtz_fp16) {
      ctx->report->fp16_xr10_guarded++;
      return false;
   }

   unsigned ncomp = alu->def.num_components;
   uint8_t outer_swiz[NIR_MAX_VEC_COMPONENTS];
   memset(outer_swiz, 0, sizeof(outer_swiz));
   for (unsigned c = 0u; c < ncomp; ++c)
      outer_swiz[c] = alu->src[0].swizzle[c];

   bool has_half_leaf = false;
   if (!a840_cone_is_pure_half(&inner->def, outer_swiz, ncomp, 0u, &has_half_leaf) ||
       !has_half_leaf) {
      return false;
   }

   b->cursor = nir_before_instr(&alu->instr);
   unsigned num_inputs = nir_op_infos[inner->op].num_inputs;
   nir_def *narrow_srcs[NIR_ALU_MAX_INPUTS];
   memset(narrow_srcs, 0, sizeof(narrow_srcs));

   for (unsigned i = 0u; i < num_inputs; ++i) {
      uint8_t child_swiz[NIR_MAX_VEC_COMPONENTS];
      memset(child_swiz, 0, sizeof(child_swiz));
      for (unsigned c = 0u; c < ncomp; ++c)
         child_swiz[c] = inner->src[i].swizzle[outer_swiz[c]];

      if (inner->op == nir_op_bcsel && i == 0u) {
         narrow_srcs[i] = a840_build_swizzled_copy(b, inner->src[0].src.ssa,
                                                   child_swiz, ncomp);
         continue;
      }

      nir_def *op_def = inner->src[i].src.ssa;
      nir_instr *op_instr = nir_def_instr(op_def);

      if (op_instr->type == nir_instr_type_load_const) {
         nir_load_const_instr *lc = nir_instr_as_load_const(op_instr);
         nir_const_value vals[NIR_MAX_VEC_COMPONENTS];
         for (unsigned c = 0u; c < ncomp; ++c) {
            uint16_t h = _mesa_float_to_half(lc->value[child_swiz[c]].f32);
            vals[c] = nir_const_value_for_raw_uint(h, 16);
         }
         narrow_srcs[i] = nir_build_imm(b, ncomp, 16, vals);
      } else if (op_instr->type == nir_instr_type_alu &&
                 nir_instr_as_alu(op_instr)->op == nir_op_f2f32 &&
                 nir_instr_as_alu(op_instr)->src[0].src.ssa->bit_size == 16u &&
                 !a840_alu_is_strict(nir_instr_as_alu(op_instr))) {
         nir_alu_instr *up = nir_instr_as_alu(op_instr);
         uint8_t leaf_swiz[NIR_MAX_VEC_COMPONENTS];
         memset(leaf_swiz, 0, sizeof(leaf_swiz));
         for (unsigned c = 0u; c < ncomp; ++c)
            leaf_swiz[c] = up->src[0].swizzle[child_swiz[c]];
         narrow_srcs[i] = a840_build_swizzled_copy(b, up->src[0].src.ssa,
                                                   leaf_swiz, ncomp);
         ctx->report->fp16_roundtrip_elided++;
      } else {
         nir_def *extracted = a840_build_swizzled_copy(b, op_def, child_swiz, ncomp);
         narrow_srcs[i] = a840_build_f2f16_with_op(b, alu->op, extracted);
      }
   }

   nir_def *rep16 = a840_build_narrowed_alu16(b, inner->op, narrow_srcs);
   if (rep16 == NULL)
      return false;

   nir_def_replace(&alu->def, rep16);
   ctx->report->fp16_alu_narrowed++;
   return true;
}

static bool
a840_fp16_align_rtz_cb(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)b;
   struct a840_fp16_pass_ctx *ctx = (struct a840_fp16_pass_ctx *)data;
   if (!ctx->shader_rtz_fp16 || !ctx->cfg->respect_half_rtz)
      return false;
   if (alu->op != nir_op_f2f16)
      return false;
   if (a840_alu_is_strict(alu))
      return false;

   alu->op = nir_op_f2f16_rtz;
   ctx->report->fp16_rtz_aligned++;
   return true;
}

static bool
a840_pass1_fp16_round_fold(nir_shader *nir,
                           const A840MetalOptConfig *cfg,
                           A840MetalOptReport *report)
{
   struct a840_fp16_pass_ctx ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.cfg = cfg;
   ctx.report = report;
   ctx.shader_rtz_fp16 =
      (nir->info.float_controls_execution_mode & FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16) != 0u;
   ctx.xr10_guarded = report->xr10_detected;

   bool total_progress = false;

   /* Fixed-point loop pulls f2f16* up through multi-level half->float->half
    * liquid-glass blur/blend cones and cancels inner f2f32(h16) conversions.
    */
   for (unsigned iter = 0u; iter < 12u; ++iter) {
      bool step = nir_shader_alu_pass(nir, a840_fp16_elide_roundtrips_cb,
                                      nir_metadata_control_flow, &ctx);
      if (!step)
         break;
      total_progress = true;
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_dce);
   }

   /* Align remaining generic f2f16 ops to Apple Metal RTZ when requested. */
   if (ctx.shader_rtz_fp16 && cfg->respect_half_rtz) {
      if (nir_shader_alu_pass(nir, a840_fp16_align_rtz_cb,
                              nir_metadata_control_flow, &ctx)) {
         total_progress = true;
      }
   }

   if (total_progress) {
      NIR_PASS(_, nir, nir_opt_algebraic);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return total_progress;
}

/* =========================================================================
 * Pass 2: Uniform / Push-Constant Folding & Redundant Scalar Read Coalescing
 * ========================================================================= */

static bool
a840_fold_offset_expr(nir_builder *b, nir_src *offset_src, A840MetalOptReport *report)
{
   nir_def *off = offset_src->ssa;
   if (off == NULL || off->bit_size != 32u || off->num_components != 1u)
      return false;

   nir_instr *parent = nir_def_instr(off);
   if (parent->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(parent);
   if (alu->op != nir_op_iadd)
      return false;

   /* Fold (x + 0) -> x and ((x + c1) + c2) -> (x + (c1 + c2)). */
   int const_idx = -1;
   if (nir_src_is_const(alu->src[1].src))
      const_idx = 1;
   else if (nir_src_is_const(alu->src[0].src))
      const_idx = 0;
   if (const_idx < 0)
      return false;

   uint32_t c2 = nir_src_as_uint(alu->src[const_idx].src);
   nir_def *other = alu->src[1 - const_idx].src.ssa;
   if (c2 == 0u) {
      nir_src_rewrite(offset_src, other);
      report->push_const_offsets_folded++;
      return true;
   }

   if (nir_src_is_const(alu->src[1 - const_idx].src)) {
      uint32_t c1 = nir_src_as_uint(alu->src[1 - const_idx].src);
      nir_def *sum = nir_imm_int(b, (int)(c1 + c2));
      nir_src_rewrite(offset_src, sum);
      report->push_const_offsets_folded++;
      return true;
   }

   nir_instr *other_instr = nir_def_instr(other);
   if (other_instr->type == nir_instr_type_alu) {
      nir_alu_instr *inner = nir_instr_as_alu(other_instr);
      if (inner->op == nir_op_iadd) {
         int inner_c = -1;
         if (nir_src_is_const(inner->src[1].src))
            inner_c = 1;
         else if (nir_src_is_const(inner->src[0].src))
            inner_c = 0;
         if (inner_c >= 0) {
            uint32_t c1 = nir_src_as_uint(inner->src[inner_c].src);
            nir_def *base_dyn = inner->src[1 - inner_c].src.ssa;
            nir_def *folded = nir_iadd_imm(b, base_dyn, (int)(c1 + c2));
            nir_src_rewrite(offset_src, folded);
            report->push_const_offsets_folded++;
            return true;
         }
      }
   }
   return false;
}

typedef struct A840SeenConstLoad {
   nir_intrinsic_instr *intr;
   nir_block *block;
   bool is_push;
   bool const_off;
   nir_def *ubo_desc;
   uint32_t base_or_off;
   nir_def *dyn_off;
   uint32_t bit_size;
   uint32_t num_components;
} A840SeenConstLoad;

/* Iteratively decompose an offset SSA def into (dyn_base, const_delta) so that
 * nested `iadd`, `ior` (disjoint low bits over `ishl`/`imul`/`amul`) chains
 * normalize to a common dynamic base + byte delta.
 */
static void
a840_decompose_dyn_offset(nir_def *off,
                          nir_def **out_dyn_base,
                          uint32_t *out_const_delta)
{
   nir_def *cur = off;
   uint32_t delta = 0u;

   for (unsigned depth = 0u; depth < 8u; ++depth) {
      if (cur == NULL || cur->bit_size != 32u || cur->num_components != 1u)
         break;

      nir_instr *parent = nir_def_instr(cur);
      if (parent->type != nir_instr_type_alu)
         break;
      nir_alu_instr *alu = nir_instr_as_alu(parent);

      if (alu->op == nir_op_iadd) {
         if (nir_src_is_const(alu->src[1].src)) {
            delta += nir_src_as_uint(alu->src[1].src);
            cur = alu->src[0].src.ssa;
            continue;
         }
         if (nir_src_is_const(alu->src[0].src)) {
            delta += nir_src_as_uint(alu->src[0].src);
            cur = alu->src[1].src.ssa;
            continue;
         }
         break;
      }

      if (alu->op == nir_op_ior) {
         int cidx = -1;
         if (nir_src_is_const(alu->src[1].src))
            cidx = 1;
         else if (nir_src_is_const(alu->src[0].src))
            cidx = 0;
         if (cidx < 0)
            break;
         uint32_t cval = nir_src_as_uint(alu->src[cidx].src);
         nir_def *other = alu->src[1 - cidx].src.ssa;
         nir_instr *oinstr = nir_def_instr(other);
         bool disjoint = false;
         if (oinstr->type == nir_instr_type_alu) {
            nir_alu_instr *oalu = nir_instr_as_alu(oinstr);
            if (oalu->op == nir_op_ishl && nir_src_is_const(oalu->src[1].src)) {
               uint32_t sh = nir_src_as_uint(oalu->src[1].src);
               if (sh > 0u && sh < 32u && cval < (1u << sh))
                  disjoint = true;
            } else if ((oalu->op == nir_op_imul || oalu->op == nir_op_amul) &&
                       nir_src_is_const(oalu->src[1].src)) {
               uint32_t mul = nir_src_as_uint(oalu->src[1].src);
               if (mul > 0u && (mul & (mul - 1u)) == 0u && cval < mul)
                  disjoint = true;
            }
         }
         if (disjoint) {
            delta += cval;
            cur = other;
            continue;
         }
         break;
      }

      break;
   }

   *out_dyn_base = cur;
   *out_const_delta = delta;
}

static bool
a840_pass2_uniform_push_fold(nir_shader *nir,
                             const A840MetalOptConfig *cfg,
                             A840MetalOptReport *report)
{
   (void)cfg;
   bool progress = false;
   bool has_const_or_ubo = false;

   /* Step 2.1: Simplify push-constant / UBO offset chains before coalescing. */
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_push_constant) {
               has_const_or_ubo = true;
               b.cursor = nir_before_instr(instr);
               if (a840_fold_offset_expr(&b, &intr->src[0], report))
                  progress = true;
            } else if (intr->intrinsic == nir_intrinsic_load_ubo) {
               has_const_or_ubo = true;
               b.cursor = nir_before_instr(instr);
               if (a840_fold_offset_expr(&b, &intr->src[1], report))
                  progress = true;
            }
         }
      }
      if (progress)
         nir_progress(true, impl, nir_metadata_control_flow);
   }

   if (!has_const_or_ubo) {
      report->push_layout_preserved = true;
      return false;
   }

   /* Unify duplicate explicit-IO offset expressions (e.g. repeated amul/ishl
    * emitted by nir_lower_explicit_io) before dominance-based coalescing.
    */
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_cse);
   if (progress) {
      NIR_PASS(_, nir, nir_opt_dce);
   }

   /* Step 2.2: Dominance-based deduplication & sub-vector extraction for
    * load_push_constant (normalizing base + const_off!) and load_ubo.
    * Strictly leaves surviving load_push_constant base values intact so
    * push_lo_bytes / push_dst_dwords contracts in a840_spirv_lower.c hold.
    */
   nir_foreach_function_impl(impl, nir) {
      nir_metadata_require(impl, nir_metadata_dominance);
      nir_builder b = nir_builder_create(impl);

      size_t cap = 64u;
      size_t len = 0u;
      A840SeenConstLoad *seen =
         (A840SeenConstLoad *)malloc(cap * sizeof(A840SeenConstLoad));
      if (seen == NULL)
         continue;

      bool impl_progress = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            bool is_push = (intr->intrinsic == nir_intrinsic_load_push_constant);
            bool is_ubo = (intr->intrinsic == nir_intrinsic_load_ubo);
            if (!is_push && !is_ubo)
               continue;

            unsigned off_src_idx = is_push ? 0u : 1u;
            bool const_off = nir_src_is_const(intr->src[off_src_idx]);
            uint32_t eff_off = 0u;
            nir_def *dyn_off = NULL;
            if (is_push) {
               if (const_off) {
                  eff_off = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]);
               } else {
                  uint32_t delta = 0u;
                  a840_decompose_dyn_offset(intr->src[0].ssa, &dyn_off, &delta);
                  eff_off = nir_intrinsic_base(intr) + delta;
               }
            } else {
               if (const_off) {
                  eff_off = nir_src_as_uint(intr->src[1]);
               } else {
                  uint32_t delta = 0u;
                  a840_decompose_dyn_offset(intr->src[1].ssa, &dyn_off, &delta);
                  eff_off = delta;
               }
            }

            nir_def *ubo_desc = is_ubo ? intr->src[0].ssa : NULL;
            uint32_t bs = intr->def.bit_size;
            uint32_t ncomp = intr->def.num_components;
            uint32_t elem_bytes = bs / 8u;
            if (elem_bytes == 0u)
               continue;

            bool replaced = false;
            for (size_t i = 0u; i < len; ++i) {
               const A840SeenConstLoad *prev = &seen[i];
               if (prev->is_push != is_push || prev->bit_size != bs)
                  continue;
               if (is_ubo && prev->ubo_desc != ubo_desc)
                  continue;
               if (prev->block != block && !nir_block_dominates(prev->block, block))
                  continue;

               bool same_base = (const_off && prev->const_off) ||
                                (!const_off && !prev->const_off && prev->dyn_off == dyn_off);
               if (same_base) {
                  uint32_t prev_end = prev->base_or_off + prev->num_components * elem_bytes;
                  uint32_t cur_end = eff_off + ncomp * elem_bytes;
                  if (eff_off >= prev->base_or_off &&
                      cur_end <= prev_end &&
                      ((eff_off - prev->base_or_off) % elem_bytes) == 0u) {
                     uint32_t first_ch = (eff_off - prev->base_or_off) / elem_bytes;
                     b.cursor = nir_before_instr(instr);
                     nir_def *rep = NULL;
                     if (first_ch == 0u && ncomp == prev->num_components) {
                        rep = &prev->intr->def;
                     } else if (ncomp == 1u) {
                        rep = nir_channel(&b, &prev->intr->def, first_ch);
                     } else {
                        uint8_t swiz[NIR_MAX_VEC_COMPONENTS];
                        memset(swiz, 0, sizeof(swiz));
                        for (unsigned c = 0u; c < ncomp; ++c)
                           swiz[c] = (uint8_t)(first_ch + c);
                        rep = a840_build_swizzled_copy(&b, &prev->intr->def, swiz, ncomp);
                     }
                     nir_def_replace(&intr->def, rep);
                     report->push_const_loads_merged++;
                     impl_progress = true;
                     replaced = true;
                     break;
                  }
               }
            }

            if (replaced)
               continue;

            if (len == cap) {
               size_t new_cap = cap * 2u;
               A840SeenConstLoad *grown =
                  (A840SeenConstLoad *)realloc(seen, new_cap * sizeof(A840SeenConstLoad));
               if (grown == NULL)
                  continue;
               seen = grown;
               cap = new_cap;
            }
            seen[len].intr = intr;
            seen[len].block = block;
            seen[len].is_push = is_push;
            seen[len].const_off = const_off;
            seen[len].ubo_desc = ubo_desc;
            seen[len].base_or_off = eff_off;
            seen[len].dyn_off = dyn_off;
            seen[len].bit_size = bs;
            seen[len].num_components = ncomp;
            len++;
         }
      }

      free(seen);
      if (impl_progress) {
         progress = true;
         nir_progress(true, impl, nir_metadata_control_flow);
      }
   }

   /* Step 2.3: Eliminate redundant integer/float widens on uniform loads and
    * shrink unused trailing vector components so scan_push_const_cb sees the
    * tightest live push-constant footprint.
    */
   nir_foreach_function_impl(impl, nir) {
      bool widen_progress = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if ((alu->op == nir_op_u2u16 || alu->op == nir_op_i2i16) &&
                alu->src[0].src.ssa->bit_size == 32u) {
               nir_instr *p = nir_def_instr(alu->src[0].src.ssa);
               if (p->type == nir_instr_type_alu) {
                  nir_alu_instr *pa = nir_instr_as_alu(p);
                  if ((pa->op == nir_op_u2u32 || pa->op == nir_op_i2i32) &&
                      pa->src[0].src.ssa->bit_size == 16u) {
                     nir_builder b = nir_builder_at(nir_before_instr(instr));
                     uint8_t swiz[NIR_MAX_VEC_COMPONENTS];
                     memset(swiz, 0, sizeof(swiz));
                     for (unsigned c = 0u; c < alu->def.num_components; ++c)
                        swiz[c] = pa->src[0].swizzle[alu->src[0].swizzle[c]];
                     nir_def *rep = a840_build_swizzled_copy(
                        &b, pa->src[0].src.ssa, swiz, alu->def.num_components);
                     nir_def_replace(&alu->def, rep);
                     report->uniform_widens_elided++;
                     widen_progress = true;
                  }
               }
            }
         }
      }
      if (widen_progress) {
         progress = true;
         nir_progress(true, impl, nir_metadata_control_flow);
      }
   }

   if (progress) {
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_cse);
      NIR_PASS(_, nir, nir_opt_algebraic);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_opt_shrink_vectors, true);
      NIR_PASS(_, nir, nir_opt_dce);
   }

   report->push_layout_preserved = true;
   return progress;
}

/* =========================================================================
 * Pass 2B (Post-Finalize): Adreno 840 load_const_ir3 Offset & Shift Folding
 *
 * After ir3_finalize_nir, push-constant accesses lowered to load_const_ir3
 * often retain byte-to-dword address arithmetic of the form:
 *   load_const_ir3(ushr(iadd(ishl(idx, s), C_bytes), 2), base = B)
 * When C_bytes is 4-byte aligned and the dynamic term is 4-byte aligned
 * (e.g. ishl(idx, s>=2) or imul(idx, 4*k)), hoist C_bytes/4 into .base and
 * fold (ishl(idx, s) >> 2) -> ishl(idx, s-2). Multiple dynamic push-constant
 * loads then share a single a0.x address register, eliminating redundant
 * add.u / shr.b / cov.u32s16 / mova a0.x stalls.
 * ========================================================================= */

static bool
a840_scalar_is_aligned4_and_shr2(nir_builder *b,
                                 nir_scalar s,
                                 nir_def **out_shr2)
{
   s = nir_scalar_resolved(s.def, s.comp);
   if (nir_scalar_is_const(s)) {
      uint32_t v = (uint32_t)nir_scalar_as_uint(s);
      if ((v & 3u) != 0u)
         return false;
      *out_shr2 = nir_imm_int(b, (int)(v >> 2));
      return true;
   }
   if (!nir_scalar_is_alu(s))
      return false;

   nir_op op = nir_scalar_alu_op(s);
   if (op == nir_op_ishl) {
      nir_scalar sh_s = nir_scalar_chase_alu_src(s, 1u);
      if (nir_scalar_is_const(sh_s)) {
         uint32_t sh = (uint32_t)nir_scalar_as_uint(sh_s);
         if (sh >= 2u && sh < 32u) {
            nir_scalar base_s = nir_scalar_chase_alu_src(s, 0u);
            nir_def *base_def = nir_mov_scalar(b, base_s);
            *out_shr2 = (sh == 2u) ? base_def : nir_ishl_imm(b, base_def, sh - 2u);
            return true;
         }
      }
   } else if (op == nir_op_imul) {
      nir_scalar m_s = nir_scalar_chase_alu_src(s, 1u);
      if (nir_scalar_is_const(m_s)) {
         uint32_t m = (uint32_t)nir_scalar_as_uint(m_s);
         if (m >= 4u && (m & 3u) == 0u) {
            nir_scalar base_s = nir_scalar_chase_alu_src(s, 0u);
            nir_def *base_def = nir_mov_scalar(b, base_s);
            uint32_t m4 = m >> 2;
            *out_shr2 = (m4 == 1u) ? base_def : nir_imul_imm(b, base_def, m4);
            return true;
         }
      }
   }
   return false;
}

static bool
a840_pass2_post_fold_const_ir3_cb(nir_builder *b,
                                  nir_intrinsic_instr *intr,
                                  void *data)
{
   A840MetalOptReport *report = (A840MetalOptReport *)data;
   if (intr->intrinsic != nir_intrinsic_load_const_ir3)
      return false;

   nir_scalar off = nir_scalar_resolved(intr->src[0].ssa, 0u);
   if (nir_scalar_is_const(off) || !nir_scalar_is_alu(off))
      return false;

   uint32_t cur_base = nir_intrinsic_base(intr);
   nir_op op = nir_scalar_alu_op(off);

   /* Pattern A: load_const_ir3(iadd(dyn_dwords, c_dwords), base=B) */
   if (op == nir_op_iadd) {
      nir_scalar s0 = nir_scalar_chase_alu_src(off, 0u);
      nir_scalar s1 = nir_scalar_chase_alu_src(off, 1u);
      int cidx = -1;
      if (nir_scalar_is_const(s1))
         cidx = 1;
      else if (nir_scalar_is_const(s0))
         cidx = 0;
      if (cidx >= 0) {
         uint32_t c_dw = (uint32_t)nir_scalar_as_uint(cidx == 1 ? s1 : s0);
         if (c_dw <= 255u && cur_base + c_dw <= 255u) {
            b->cursor = nir_before_instr(&intr->instr);
            nir_def *dyn_def = nir_mov_scalar(b, cidx == 1 ? s0 : s1);
            nir_intrinsic_set_base(intr, cur_base + c_dw);
            nir_src_rewrite(&intr->src[0], dyn_def);
            report->push_const_offsets_folded++;
            return true;
         }
      }
      return false;
   }

   /* Pattern B: load_const_ir3(ushr(expr, 2), base=B) where expr is
    *   iadd(aligned4_dyn, c_bytes)  OR  aligned4_dyn
    */
   if (op == nir_op_ushr) {
      nir_scalar sh_s = nir_scalar_chase_alu_src(off, 1u);
      if (!nir_scalar_is_const(sh_s) || nir_scalar_as_uint(sh_s) != 2u)
         return false;

      nir_scalar in_s = nir_scalar_chase_alu_src(off, 0u);
      in_s = nir_scalar_resolved(in_s.def, in_s.comp);
      if (!nir_scalar_is_alu(in_s))
         return false;

      b->cursor = nir_before_instr(&intr->instr);
      if (nir_scalar_alu_op(in_s) == nir_op_iadd) {
         nir_scalar a0 = nir_scalar_chase_alu_src(in_s, 0u);
         nir_scalar a1 = nir_scalar_chase_alu_src(in_s, 1u);
         int cidx = -1;
         if (nir_scalar_is_const(a1))
            cidx = 1;
         else if (nir_scalar_is_const(a0))
            cidx = 0;
         if (cidx >= 0) {
            uint32_t c_bytes = (uint32_t)nir_scalar_as_uint(cidx == 1 ? a1 : a0);
            if ((c_bytes & 3u) == 0u) {
               uint32_t c_dw = c_bytes >> 2;
               if (c_dw <= 255u && cur_base + c_dw <= 255u) {
                  nir_def *dyn_dw = NULL;
                  if (a840_scalar_is_aligned4_and_shr2(b, cidx == 1 ? a0 : a1, &dyn_dw)) {
                     nir_intrinsic_set_base(intr, cur_base + c_dw);
                     nir_src_rewrite(&intr->src[0], dyn_dw);
                     report->push_const_offsets_folded++;
                     return true;
                  }
               }
            }
         }
      } else {
         nir_def *dyn_dw = NULL;
         if (a840_scalar_is_aligned4_and_shr2(b, in_s, &dyn_dw)) {
            nir_src_rewrite(&intr->src[0], dyn_dw);
            report->push_const_offsets_folded++;
            return true;
         }
      }
   }

   return false;
}

static bool
a840_pass2_post_fold_const_ir3(nir_shader *nir, A840MetalOptReport *report)
{
   bool progress = nir_shader_intrinsics_pass(
      nir, a840_pass2_post_fold_const_ir3_cb, nir_metadata_control_flow, report);
   if (progress) {
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_cse);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return progress;
}

/* =========================================================================
 * Pass 3: Register Pressure Reduction & Anti-Spill Scheduling
 * ========================================================================= */

static bool
a840_instr_has_mem_or_ctrl_side_effect(const nir_instr *instr)
{
   if (instr->type == nir_instr_type_jump)
      return true;
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_barrier:
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if:
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_store_ssbo_ir3:
   case nir_intrinsic_store_shared:
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_global_ir3:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_ssbo_atomic_ir3:
   case nir_intrinsic_ssbo_atomic_swap_ir3:
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
   case nir_intrinsic_load_frag_coord_gmem_ir3:
      return true;
   default:
      return false;
   }
}

/* Estimate peak live register pressure in a single basic block in 32-bit vec4
 * equivalents (1 full vec4 = 8 half-scalars; 1 half vec4 = 4 half-scalars).
 */
static uint32_t
a840_estimate_block_peak_live_vec4(nir_block *block)
{
   unsigned n = 0u;
   nir_foreach_instr(instr, block) {
      instr->index = n++;
   }
   if (n == 0u)
      return 0u;

   int32_t *delta = (int32_t *)calloc((size_t)n + 2u, sizeof(int32_t));
   if (delta == NULL)
      return 0u;

   const nir_if *iff = nir_block_get_following_if(block);
   const nir_instr *if_cond = iff ? nir_def_instr(iff->condition.ssa) : NULL;

   nir_foreach_instr(instr, block) {
      nir_def *def = nir_instr_def(instr);
      if (def == NULL || list_is_empty(&def->uses))
         continue;
      if (instr->type == nir_instr_type_load_const ||
          instr->type == nir_instr_type_undef) {
         continue;
      }

      unsigned half_units = (def->bit_size <= 16u)
         ? def->num_components
         : def->num_components * (def->bit_size / 16u);
      unsigned start = instr->index;
      unsigned end = start;
      if (instr == if_cond)
         end = n;

      nir_foreach_use(use, def) {
         nir_instr *user = nir_src_use_instr(use);
         if (user->block != block || user->type == nir_instr_type_phi) {
            end = n;
            break;
         }
         if (user->index > end)
            end = user->index;
      }

      if (end > start) {
         delta[start] += (int32_t)half_units;
         delta[end] -= (int32_t)half_units;
      }
   }

   int32_t cur = 0;
   int32_t peak = 0;
   for (unsigned i = 0u; i <= n; ++i) {
      cur += delta[i];
      if (cur > peak)
         peak = cur;
   }
   free(delta);
   return peak > 0 ? (uint32_t)((peak + 7) / 8) : 0u;
}

static uint32_t
a840_estimate_shader_peak_live_vec4(nir_shader *nir,
                                    uint32_t *out_num_blocks,
                                    bool *out_has_side_effects)
{
   uint32_t peak = 0u;
   uint32_t blocks = 0u;
   bool side_effects = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         blocks++;
         uint32_t bp = a840_estimate_block_peak_live_vec4(block);
         if (bp > peak)
            peak = bp;
         nir_foreach_instr(instr, block) {
            if (a840_instr_has_mem_or_ctrl_side_effect(instr))
               side_effects = true;
         }
      }
   }

   if (out_num_blocks != NULL)
      *out_num_blocks = blocks;
   if (out_has_side_effects != NULL)
      *out_has_side_effects = side_effects;
   return peak;
}

static bool
a840_can_pressure_sink_instr(nir_instr *instr, bool allow_mem_and_tex)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      return true;
   case nir_instr_type_tex:
      if (!allow_mem_and_tex)
         return false;
      switch (nir_instr_as_tex(instr)->op) {
      case nir_texop_tex:
      case nir_texop_txb:
      case nir_texop_txl:
      case nir_texop_txd:
      case nir_texop_txf:
      case nir_texop_tg4:
         return true;
      default:
         return false;
      }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      /* Move copies, comparisons, single-use ALU, or non-expanding ALU. */
      if (nir_op_is_vec_or_mov(alu->op) || nir_alu_instr_is_comparison(alu))
         return true;
      if (list_is_singular(&alu->def.uses))
         return true;
      return false;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_const_ir3:
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_vec4:
      case nir_intrinsic_load_uniform:
      case nir_intrinsic_load_frag_coord:
      case nir_intrinsic_load_frag_coord_xy:
      case nir_intrinsic_load_frag_coord_unscaled_ir3:
         return true;
      case nir_intrinsic_load_ssbo:
      case nir_intrinsic_load_ssbo_ir3:
         return allow_mem_and_tex && nir_intrinsic_can_reorder(intr);
      default:
         return false;
      }
   }
   default:
      return false;
   }
}

/* Reverse-walk intra-block live-range sinker for high-pressure blocks.
 * Moves eligible loads/constants/single-use ALU immediately before their
 * earliest consumer while respecting memory/control barrier fences.
 */
static bool
a840_sink_high_pressure_block(nir_block *block,
                              bool allow_mem_and_tex,
                              uint32_t *out_moves)
{
   bool progress = false;
   nir_instr *last_instr = nir_block_ends_in_jump(block) ? nir_block_last_instr(block) : NULL;
   nir_instr *mem_fence = last_instr;
   const nir_if *iff = nir_block_get_following_if(block);
   const nir_instr *if_cond_instr = iff ? nir_def_instr(iff->condition.ssa) : NULL;

   /* Bottom-to-top reverse indexing (larger index = earlier in block). */
   unsigned index = 1u;
   nir_foreach_instr_reverse_safe(instr, block) {
      instr->index = index++;

      if (a840_instr_has_mem_or_ctrl_side_effect(instr)) {
         mem_fence = instr;
         continue;
      }

      if (!a840_can_pressure_sink_instr(instr, allow_mem_and_tex))
         continue;

      const nir_def *def = nir_instr_def(instr);
      if (def == NULL)
         continue;

      bool is_mem_or_tex =
         (instr->type == nir_instr_type_tex) ||
         (instr->type == nir_instr_type_intrinsic &&
          (nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_ssbo ||
           nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_ssbo_ir3));

      nir_instr *limit = is_mem_or_tex ? mem_fence : last_instr;
      nir_instr *first_user = (instr == if_cond_instr) ? NULL : limit;

      nir_foreach_use(use, def) {
         nir_instr *parent = nir_src_use_instr(use);
         if (parent->type == nir_instr_type_phi || parent->block != block)
            continue;
         if (first_user == NULL || parent->index > first_user->index)
            first_user = parent;
      }

      if (first_user == NULL)
         continue;

      /* Never sink a memory/tex load below an intervening barrier/store. */
      if (is_mem_or_tex && mem_fence != NULL && first_user->index < mem_fence->index)
         first_user = mem_fence;

      /* Preserve relative order among siblings sharing the same target user. */
      while (nir_instr_prev(first_user) != NULL &&
             nir_instr_prev(first_user)->index == first_user->index) {
         first_user = nir_instr_prev(first_user);
      }

      if (first_user == instr || nir_instr_prev(first_user) == instr)
         continue;

      exec_node_remove(&instr->node);
      instr->index = first_user->index;
      exec_node_insert_node_before(&first_user->node, &instr->node);
      (*out_moves)++;
      progress = true;
   }

   return progress;
}

static bool
a840_pass3_reg_pressure_sink(nir_shader *nir,
                             const A840MetalOptConfig *cfg,
                             A840MetalOptReport *report)
{
   bool progress = false;
   uint32_t num_blocks = 0u;
   bool has_side_effects = false;
   uint32_t thresh = cfg->sink_pressure_thresh > 0u
      ? cfg->sink_pressure_thresh
      : A840_METAL_OPT_DEFAULT_SINK_THRESH;

   uint32_t peak_before =
      a840_estimate_shader_peak_live_vec4(nir, &num_blocks, &has_side_effects);
   report->peak_live_vec4_before = peak_before;

   /* Step 3.1: Cross-block sinking via Mesa nir_opt_sink (never sinks into loops). */
   if (num_blocks > 1u) {
      nir_move_options sink_opts = (nir_move_options)(
         nir_move_const_undef |
         nir_move_comparisons |
         nir_move_copies |
         nir_move_alu |
         nir_move_load_ubo |
         nir_move_load_uniform |
         nir_move_load_frag_coord);
      if (peak_before > thresh && !has_side_effects) {
         sink_opts = (nir_move_options)(sink_opts |
            nir_move_tex_sample |
            nir_move_load_ssbo);
      }
      if (nir_opt_sink(nir, sink_opts)) {
         report->sink_moves_applied++;
         progress = true;
      }
   }

   /* Step 3.2: Pressure-gated intra-block live-range sinking for high-pressure
    * multi-tap convolution / liquid-glass shaders (leaves low-pressure shaders
    * already achieving 16 waves/SP untouched to avoid disturbing optimal order).
    */
   if (peak_before > thresh) {
      bool allow_mem_and_tex = !has_side_effects;
      nir_foreach_function_impl(impl, nir) {
         bool impl_progress = false;
         nir_foreach_block(block, impl) {
            uint32_t bp = a840_estimate_block_peak_live_vec4(block);
            if (bp <= thresh)
               continue;
            for (unsigned round = 0u; round < 2u; ++round) {
               uint32_t moved = 0u;
               if (!a840_sink_high_pressure_block(block, allow_mem_and_tex, &moved))
                  break;
               report->sink_moves_applied += moved;
               impl_progress = true;
            }
         }
         if (impl_progress) {
            progress = true;
            nir_progress(true, impl, nir_metadata_control_flow);
         }
      }
   }

   report->peak_live_vec4_after =
      a840_estimate_shader_peak_live_vec4(nir, NULL, NULL);
   return progress;
}

/* =========================================================================
 * Public NIR Orchestration Entry Points
 * ========================================================================= */

bool
a840_metal_opt_run_nir_pre_push(struct nir_shader *nir,
                                const A840MetalOptConfig *cfg,
                                A840MetalOptReport *out_report)
{
   if (nir == NULL)
      return false;

   A840MetalOptConfig local_cfg;
   if (cfg != NULL) {
      local_cfg = *cfg;
   } else {
      local_cfg = a840_metal_opt_default_config(0u);
   }

   A840MetalOptReport rep;
   memset(&rep, 0, sizeof(rep));
   rep.active_pass_mask = local_cfg.pass_mask & A840_METAL_OPT_ALL;
   rep.xr10_detected = a840_detect_xr10_shader(nir);
   rep.xr10_safe = true;
   rep.push_layout_preserved = true;
   rep.pre_opt_alu_count = a840_count_shader_alu(nir);

   bool progress = false;

   if ((rep.active_pass_mask & A840_METAL_OPT_UNIFORM_PUSH_FOLD) != 0u) {
      if (a840_pass2_uniform_push_fold(nir, &local_cfg, &rep))
         progress = true;
   }

   if ((rep.active_pass_mask & A840_METAL_OPT_FP16_ROUND_FOLD) != 0u) {
      if (a840_pass1_fp16_round_fold(nir, &local_cfg, &rep))
         progress = true;
   }

   rep.post_opt_alu_count = a840_count_shader_alu(nir);

   if (out_report != NULL)
      *out_report = rep;
   a840_metal_opt_record_last_report(&rep);
   return progress;
}

bool
a840_metal_opt_run_nir_post_finalize(struct nir_shader *nir,
                                     const A840MetalOptConfig *cfg,
                                     A840MetalOptReport *inout_report)
{
   if (nir == NULL)
      return false;

   A840MetalOptConfig local_cfg;
   if (cfg != NULL) {
      local_cfg = *cfg;
   } else {
      local_cfg = a840_metal_opt_default_config(0u);
   }

   A840MetalOptReport rep;
   if (inout_report != NULL) {
      rep = *inout_report;
   } else {
      memset(&rep, 0, sizeof(rep));
      rep.active_pass_mask = local_cfg.pass_mask & A840_METAL_OPT_ALL;
      rep.xr10_detected = a840_detect_xr10_shader(nir);
      rep.xr10_safe = true;
      rep.push_layout_preserved = true;
      rep.pre_opt_alu_count = a840_count_shader_alu(nir);
   }

   bool progress = false;
   if ((local_cfg.pass_mask & A840_METAL_OPT_UNIFORM_PUSH_FOLD) != 0u) {
      if (a840_pass2_post_fold_const_ir3(nir, &rep))
         progress = true;
   }
   if ((local_cfg.pass_mask & A840_METAL_OPT_REG_PRESSURE_SINK) != 0u) {
      if (a840_pass3_reg_pressure_sink(nir, &local_cfg, &rep))
         progress = true;
   } else {
      uint32_t peak = a840_estimate_shader_peak_live_vec4(nir, NULL, NULL);
      rep.peak_live_vec4_before = peak;
      rep.peak_live_vec4_after = peak;
   }

   rep.post_opt_alu_count = a840_count_shader_alu(nir);
   if (inout_report != NULL)
      *inout_report = rep;
   a840_metal_opt_record_last_report(&rep);
   return progress;
}

bool
a840_metal_opt_run_nir(struct nir_shader *nir,
                       const A840MetalOptConfig *cfg,
                       A840MetalOptReport *out_report)
{
   A840MetalOptReport rep;
   memset(&rep, 0, sizeof(rep));
   bool p1 = a840_metal_opt_run_nir_pre_push(nir, cfg, &rep);
   bool p2 = a840_metal_opt_run_nir_post_finalize(nir, cfg, &rep);
   if (out_report != NULL)
      *out_report = rep;
   return p1 || p2;
}

#endif /* A840_BUILD_MESA_IR3_BACKEND */
