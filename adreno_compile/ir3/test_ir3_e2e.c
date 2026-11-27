/* SPDX-License-Identifier: MIT */
#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/disasm.h"
#include "common/fd6_hw.h"
#include "common/freedreno_dev_info.h"
#include "fdl/freedreno_layout.h"
#include "ir3-isa.h"

static const char COMPUTE_METALLIB[] =
   "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib";
static const char RENDER_METALLIB[] =
   "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib";

static void
require_cond(bool cond, const char *what, const char *detail)
{
   if (!cond) {
      fprintf(stderr, "FAIL: %s%s%s\n", what,
              (detail && detail[0]) ? ": " : "",
              (detail && detail[0]) ? detail : "");
      exit(EXIT_FAILURE);
   }
}

static uint8_t *
read_file_bytes(const char *path, size_t *out_len)
{
   FILE *f = fopen(path, "rb");
   require_cond(f != NULL, path, strerror(errno));
   require_cond(fseek(f, 0, SEEK_END) == 0, "fseek end", path);
   long sz = ftell(f);
   require_cond(sz > 0, "file size", path);
   require_cond(fseek(f, 0, SEEK_SET) == 0, "fseek set", path);
   uint8_t *buf = malloc((size_t)sz);
   require_cond(buf != NULL, "malloc", path);
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   require_cond(got == (size_t)sz, "fread", path);
   *out_len = got;
   return buf;
}

static void
disasm_pre_instr_cb(void *d, unsigned n, void *instr)
{
   FILE *out = (FILE *)d;
   const uint32_t *dw = (const uint32_t *)instr;
   fprintf(out, "%4u[%08x_%08x] ", n, dw[1], dw[0]);
}

static char *
disassemble_a840_isa(const uint32_t *isa_words, uint32_t isa_dwords,
                     struct shader_stats *out_stats)
{
   /* Run disasm_a3xx_stat for Gen8 (gpu_id=800) to get real instruction stats. */
   char *stat_text = NULL;
   size_t stat_len = 0;
   FILE *stat_fp = open_memstream(&stat_text, &stat_len);
   require_cond(stat_fp != NULL, "open_memstream stat", NULL);
   int rc = disasm_a3xx_stat(isa_words, (int)isa_dwords, 0, stat_fp, 800, out_stats);
   fclose(stat_fp);
   free(stat_text);
   require_cond(rc == 0, "disasm_a3xx_stat rc", NULL);

   /* Also run ir3_isa_disasm directly from libir3decode.a for clean hex+asm dump. */
   char *asm_text = NULL;
   size_t asm_len = 0;
   FILE *asm_fp = open_memstream(&asm_text, &asm_len);
   require_cond(asm_fp != NULL, "open_memstream asm", NULL);
   struct isa_decode_options opts = {
      .gpu_id = 800,
      .show_errors = true,
      .max_errors = 5,
      .branch_labels = true,
      .pre_instr_cb = disasm_pre_instr_cb,
      .cbdata = asm_fp,
   };
   ir3_isa_disasm((void *)isa_words, (int)(isa_dwords * sizeof(uint32_t)), asm_fp, &opts);
   fclose(asm_fp);
   require_cond(asm_text != NULL && asm_len > 0, "ir3_isa_disasm produced empty output", NULL);
   return asm_text;
}

static void
dump_stage_artifacts(const char *dump_dir, const char *label,
                     const A840CompiledStage *compiled, const char *disasm)
{
   if (dump_dir == NULL || dump_dir[0] == '\0')
      return;

   char path[4096];
   int n = snprintf(path, sizeof(path), "%s/%s.asm", dump_dir, label);
   require_cond(n > 0 && (size_t)n < sizeof(path), "asm path", label);
   FILE *f = fopen(path, "wb");
   require_cond(f != NULL, "open asm dump", path);
   size_t len = strlen(disasm);
   require_cond(fwrite(disasm, 1, len, f) == len, "write asm dump", path);
   fclose(f);

   n = snprintf(path, sizeof(path), "%s/%s.bin", dump_dir, label);
   require_cond(n > 0 && (size_t)n < sizeof(path), "bin path", label);
   f = fopen(path, "wb");
   require_cond(f != NULL, "open bin dump", path);
   size_t bytes = (size_t)compiled->isa_dwords * sizeof(uint32_t);
   require_cond(fwrite(compiled->isa_words, 1, bytes, f) == bytes, "write bin dump", path);
   fclose(f);
}

static void
compile_and_verify_stage(const A840CompileTarget *target,
                         const NmbSpirvResult *spv_res,
                         uint32_t stage_idx,
                         A840ShaderStage stage,
                         const char *label,
                         const char *dump_dir)
{
   size_t spv_bytes = 0;
   const uint32_t *spv_words = nmb_spirv_words(spv_res, stage_idx, &spv_bytes);
   require_cond(spv_words != NULL && spv_bytes >= 20 && (spv_bytes % 4) == 0,
                "valid translated SPIR-V", label);
   const char *entry = nmb_spirv_entry_point(spv_res, stage_idx);
   require_cond(entry != NULL && entry[0] != '\0', "valid SPIR-V entry", label);

   A840StageInput input;
   memset(&input, 0, sizeof(input));
   input.abi_version = ADRENO_COMPILE_ABI_VERSION;
   input.stage = stage;
   input.spirv_words = spv_words;
   input.spirv_bytes = spv_bytes;
   input.entry_point = entry;
   input.color_rt_mask = (stage == A840_STAGE_FRAGMENT) ? 1u : 0u;
   input.sample_count = 1u;
   if (stage == A840_STAGE_COMPUTE) {
      input.local_size[0] = 8u;
      input.local_size[1] = 8u;
      input.local_size[2] = 1u;
   }
   input.flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;

   A840CompiledStage compiled;
   memset(&compiled, 0x5a, sizeof(compiled));
   char err[512] = { 0 };

   A840CompileStatus st = a840_compile_stage(target, &input, &compiled, err, sizeof(err));
   require_cond(st == A840_COMPILE_OK, label, err);
   require_cond(compiled.abi_version == ADRENO_COMPILE_ABI_VERSION, "abi_version", label);
   require_cond(compiled.stage == (uint32_t)stage, "stage echo", label);
   require_cond(compiled.isa_words != NULL && compiled.isa_dwords > 0u, "isa non-empty", label);
   require_cond((compiled.isa_dwords % 2u) == 0u, "ir3 64-bit instruction alignment", label);
   require_cond(compiled.instruction_groups > 0u, "instruction_groups > 0", label);
   require_cond(compiled.owner != NULL, "owner non-null", label);
   require_cond(compiled.max_waves > 0u, "max_waves > 0", label);
   if (stage == A840_STAGE_COMPUTE) {
      require_cond(compiled.local_size[0] == 8u &&
                   compiled.local_size[1] == 8u &&
                   compiled.local_size[2] == 1u,
                   "compute specialized local_size [8,8,1]", label);
   }

   struct shader_stats stats;
   memset(&stats, 0, sizeof(stats));
   char *disasm = disassemble_a840_isa(compiled.isa_words, compiled.isa_dwords, &stats);
   uint32_t decoded_instrs = 0u;
   for (unsigned c = 0; c < 8u; ++c)
      decoded_instrs += stats.instrs_per_cat[c];
   require_cond(decoded_instrs > 0u, "disassembled instruction count > 0", label);
   require_cond(strstr(disasm, "end") != NULL, "disassembly contains 'end' opcode", label);
   require_cond(strstr(disasm, "no match") == NULL && strstr(disasm, "ERROR") == NULL,
                "disassembly has zero decode errors", label);

   dump_stage_artifacts(dump_dir, label, &compiled, disasm);

   printf("\n[STAGE %s] spirv=%zuB -> A840 Gen8 ISA: %u dwords (%u bytes), "
          "groups=%u, instrs=%u (cat0=%u cat1=%u cat2=%u cat3=%u cat4=%u cat5=%u cat6=%u cat7=%u), "
          "full_reg=%d, half_reg=%d, branchstack=%u, "
          "constlen=%u vec4, const_data=%u dw, pvt=%uB, slm=%uB, "
          "local=[%u,%u,%u], flags=0x%x, ubo_push=%u, vpc_varyings=%u, "
          "spills=%u, fills=%u, max_waves=%u\n",
          label, spv_bytes, compiled.isa_dwords, compiled.isa_dwords * 4u,
          compiled.instruction_groups, decoded_instrs,
          stats.instrs_per_cat[0], stats.instrs_per_cat[1],
          stats.instrs_per_cat[2], stats.instrs_per_cat[3],
          stats.instrs_per_cat[4], stats.instrs_per_cat[5],
          stats.instrs_per_cat[6], stats.instrs_per_cat[7],
          compiled.max_full_reg, compiled.max_half_reg, compiled.branchstack,
          compiled.constlen_vec4, compiled.const_data_dwords,
          compiled.private_mem_bytes, compiled.shared_mem_bytes,
          compiled.local_size[0], compiled.local_size[1], compiled.local_size[2],
          compiled.flags_out, compiled.ubo_push_count, compiled.vpc_varying_count,
          compiled.reg_spills, compiled.reg_fills, compiled.max_waves);

   /* Print first 16 lines of real A840 Gen8 disassembly. */
   const char *p = disasm;
   int line = 0;
   while (*p != '\0' && line < 16) {
      const char *nl = strchr(p, '\n');
      if (nl == NULL) {
         printf("  %s\n", p);
         break;
      }
      printf("  %.*s\n", (int)(nl - p), p);
      p = nl + 1;
      ++line;
   }

   free(disasm);
   a840_compile_stage_free(&compiled);
   require_cond(compiled.isa_words == NULL && compiled.owner == NULL && compiled.isa_dwords == 0u,
                "free cleared pointers", label);
   /* Double free must be a safe no-op. */
   a840_compile_stage_free(&compiled);
}

static void
test_real_metallibs(const A840CompileTarget *target, const char *dump_dir)
{
   NmbDevice *dev = nmb_device_create_a840(0);
   require_cond(dev != NULL, "nmb_device_create_a840", nmb_last_error());

   /* 1. Real 24A446 Compute: MattingV2.metallib -> copyTexture */
   size_t comp_len = 0;
   uint8_t *comp_bytes = read_file_bytes(COMPUTE_METALLIB, &comp_len);
   NmbLibrary *comp_lib = nmb_library_parse(dev, comp_bytes, comp_len);
   free(comp_bytes);
   require_cond(comp_lib != NULL, "parse MattingV2.metallib", nmb_last_error());

   NmbSpirvResult *comp_spv = nmb_compile_compute_spirv(dev, comp_lib, "copyTexture");
   nmb_library_destroy(comp_lib);
   require_cond(comp_spv != NULL, "translate MattingV2 copyTexture", nmb_last_error());
   require_cond(nmb_spirv_stage_count(comp_spv) == 1u, "compute stage count", NULL);

   compile_and_verify_stage(target, comp_spv, 0u, A840_STAGE_COMPUTE,
                            "matting_copyTexture_cs", dump_dir);
   nmb_spirv_result_free(comp_spv);

   /* 2. Real 24A446 Render: NTKKaleidoscopeShaders.original.metallib -> VS + FS */
   size_t rend_len = 0;
   uint8_t *rend_bytes = read_file_bytes(RENDER_METALLIB, &rend_len);
   NmbLibrary *rend_lib = nmb_library_parse(dev, rend_bytes, rend_len);
   free(rend_bytes);
   require_cond(rend_lib != NULL, "parse NTKKaleidoscopeShaders.original.metallib", nmb_last_error());

   enum { MTL_RGBA8_UNORM = 70u };
   NmbSpirvResult *rend_spv = nmb_compile_render_spirv(
      dev, rend_lib, "kal_fullscreen_vert", "kal_rosette_frag", MTL_RGBA8_UNORM);
   nmb_library_destroy(rend_lib);
   require_cond(rend_spv != NULL, "translate NTKKaleidoscope VS+FS", nmb_last_error());
   require_cond(nmb_spirv_stage_count(rend_spv) == 2u, "render stage count", NULL);

   compile_and_verify_stage(target, rend_spv, 0u, A840_STAGE_VERTEX,
                            "kal_fullscreen_vert_vs", dump_dir);
   compile_and_verify_stage(target, rend_spv, 1u, A840_STAGE_FRAGMENT,
                            "kal_rosette_frag_fs", dump_dir);
   nmb_spirv_result_free(rend_spv);

   nmb_device_destroy(dev);
}

static void
test_fdl_layouts(const A840CompileTarget *target)
{
   /* Verify Mesa static catalog reports HBB=15 while Kaanapali target overrides to 16. */
   struct fd_dev_id raw_id = { .gpu_id = 0, .chip_id = target->chip_id };
   const struct fd_dev_info *raw_info = fd_dev_info_raw(&raw_id);
   require_cond(raw_info != NULL && raw_info->chip == 8, "Mesa raw A840 info", NULL);
   require_cond(raw_info->highest_bank_bit == 15u, "Mesa static A840 HBB expected 15", NULL);
   require_cond(target->highest_bank_bit == 16u, "Kaanapali target HBB expected 16", NULL);

   /* Verify bank_shift responds to HBB=16 vs HBB=15 in libfreedreno_layout.a. */
   struct fdl_ubwc_config cfg16 = {
      .highest_bank_bit = 16u,
      .bank_swizzle_levels = 6u,
      .macrotile_mode = FDL_MACROTILE_8_CHANNEL,
   };
   struct fdl_ubwc_config cfg15 = {
      .highest_bank_bit = 15u,
      .bank_swizzle_levels = 6u,
      .macrotile_mode = FDL_MACROTILE_8_CHANNEL,
   };
   require_cond(fdl6_get_bank_shift(&cfg16) == 15u && fdl6_get_bank_shift(&cfg15) == 14u,
                "HBB 16 vs 15 bank_shift distinction", NULL);

   /* Xiaomi 17 Ultra primary display: 1200x2608 BGRA8Unorm(80) across linear/tiled/UBWC */
   A840TextureLayoutQuery q;
   memset(&q, 0, sizeof(q));
   q.abi_version = ADRENO_COMPILE_ABI_VERSION;
   q.mtl_pixel_format = 80u; /* MTLPixelFormatBGRA8Unorm */
   q.width = 1200u;
   q.height = 2608u;
   q.depth = 1u;
   q.array_layers = 1u;
   q.mip_levels = 1u;
   q.sample_count = 1u;
   q.flags = 4u; /* for_scanout */

   A840TextureLayoutResult res_lin, res_tiled, res_ubwc, res_ubwc_hbb15;
   q.tile_mode = 0u;
   require_cond(a840_compute_texture_layout(target, &q, &res_lin) == A840_COMPILE_OK,
                "1200x2608 BGRA8Unorm linear", NULL);
   require_cond(res_lin.ubwc_enabled == 0u && res_lin.total_bytes > 0u &&
                res_lin.pitch0_bytes >= 1200u * 4u,
                "linear layout invariants", NULL);

   q.tile_mode = 1u;
   require_cond(a840_compute_texture_layout(target, &q, &res_tiled) == A840_COMPILE_OK,
                "1200x2608 BGRA8Unorm tiled", NULL);
   require_cond(res_tiled.ubwc_enabled == 0u && res_tiled.base_align == 4096u &&
                res_tiled.total_bytes > 0u && res_tiled.pitch0_bytes >= 1200u * 4u,
                "tiled layout invariants", NULL);

   q.tile_mode = 2u;
   require_cond(a840_compute_texture_layout(target, &q, &res_ubwc) == A840_COMPILE_OK,
                "1200x2608 BGRA8Unorm UBWC (HBB=16)", NULL);
   require_cond(res_ubwc.ubwc_enabled == 1u && res_ubwc.ubwc_meta_pitch > 0u &&
                res_ubwc.base_align == 4096u &&
                res_ubwc.total_bytes > res_tiled.total_bytes &&
                (res_ubwc.tex_const_template[4] & A8XX_TEX_MEMOBJ_4_FLAG) != 0u,
                "UBWC layout invariants", NULL);

   /* Compare HBB=16 vs HBB=15 legal validation, and reject invalid HBB=14/17. */
   A840CompileTarget t_hbb15 = *target;
   t_hbb15.highest_bank_bit = 15u;
   require_cond(a840_compute_texture_layout(&t_hbb15, &q, &res_ubwc_hbb15) == A840_COMPILE_OK,
                "1200x2608 BGRA8Unorm UBWC (HBB=15)", NULL);
   require_cond(res_ubwc_hbb15.ubwc_enabled == 1u &&
                res_ubwc_hbb15.total_bytes == res_ubwc.total_bytes,
                "HBB=15 legal geometry check", NULL);

   A840CompileTarget t_bad_hbb = *target;
   t_bad_hbb.highest_bank_bit = 14u;
   A840TextureLayoutResult sentinel;
   memset(&sentinel, 0x3c, sizeof(sentinel));
   A840TextureLayoutResult sentinel_copy = sentinel;
   require_cond(a840_compute_texture_layout(&t_bad_hbb, &q, &sentinel) == A840_COMPILE_INVALID_ARG &&
                memcmp(&sentinel, &sentinel_copy, sizeof(sentinel)) == 0,
                "reject invalid HBB=14 without mutating out_layout", NULL);
   t_bad_hbb.highest_bank_bit = 17u;
   require_cond(a840_compute_texture_layout(&t_bad_hbb, &q, &sentinel) == A840_COMPILE_INVALID_ARG &&
                memcmp(&sentinel, &sentinel_copy, sizeof(sentinel)) == 0,
                "reject invalid HBB=17 without mutating out_layout", NULL);

   printf("\n[FDL 1200x2608 BGRA8Unorm (MTLPixelFormat=80, Kaanapali HBB=16 vs Mesa static 15)]\n");
   printf("  linear: total=%" PRIu64 " B, align=%u, pitch0=%u, slice_stride=%u, ubwc=%u\n",
          res_lin.total_bytes, res_lin.base_align, res_lin.pitch0_bytes,
          res_lin.slice_stride_bytes, res_lin.ubwc_enabled);
   printf("  tiled:  total=%" PRIu64 " B, align=%u, pitch0=%u, slice_stride=%u, ubwc=%u\n",
          res_tiled.total_bytes, res_tiled.base_align, res_tiled.pitch0_bytes,
          res_tiled.slice_stride_bytes, res_tiled.ubwc_enabled);
   printf("  ubwc16: total=%" PRIu64 " B, align=%u, pitch0=%u, slice_stride=%u, "
          "ubwc=%u, meta_offset=%" PRIu64 ", meta_pitch=%u, bank_shift=%u\n",
          res_ubwc.total_bytes, res_ubwc.base_align, res_ubwc.pitch0_bytes,
          res_ubwc.slice_stride_bytes, res_ubwc.ubwc_enabled,
          res_ubwc.ubwc_meta_offset, res_ubwc.ubwc_meta_pitch,
          fdl6_get_bank_shift(&cfg16));
   printf("  ubwc16 A8XX_TEX_MEMOBJ[0..8]: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
          res_ubwc.tex_const_template[0], res_ubwc.tex_const_template[1],
          res_ubwc.tex_const_template[2], res_ubwc.tex_const_template[3],
          res_ubwc.tex_const_template[4], res_ubwc.tex_const_template[5],
          res_ubwc.tex_const_template[6], res_ubwc.tex_const_template[7],
          res_ubwc.tex_const_template[8]);

   /* Test 24 core MTLPixelFormats across linear/tiled/ubwc. */
   const uint32_t core_formats[] = {
      1u, 10u, 11u, 25u, 30u, 40u, 53u, 55u, 65u, 70u, 71u, 80u, 81u,
      90u, 92u, 115u, 125u, 186u, 204u, 212u, 250u, 252u, 253u, 260u,
   };
   for (size_t i = 0; i < sizeof(core_formats) / sizeof(core_formats[0]); ++i) {
      A840TextureLayoutQuery fq = {
         .abi_version = ADRENO_COMPILE_ABI_VERSION,
         .mtl_pixel_format = core_formats[i],
         .width = 256u,
         .height = 256u,
         .depth = 1u,
         .array_layers = 1u,
         .mip_levels = 4u,
         .sample_count = 1u,
         .tile_mode = 2u,
         .flags = 0u,
      };
      A840TextureLayoutResult fres;
      A840CompileStatus fst = a840_compute_texture_layout(target, &fq, &fres);
      require_cond(fst == A840_COMPILE_OK && fres.total_bytes > 0u &&
                   fres.pitch0_bytes > 0u && fres.base_align > 0u,
                   "core MTLPixelFormat layout", NULL);
   }
   printf("  verified %zu core MTLPixelFormats (256x256, 4 mips) via fdl6_layout_image\n",
          sizeof(core_formats) / sizeof(core_formats[0]));
}

static void
test_abi_boundaries_and_errors(const A840CompileTarget *target)
{
   /* Verify ABI fingerprint matches frozen stub formula. */
   const uint64_t expected_vals[] = {
      ADRENO_COMPILE_ABI_VERSION,
      sizeof(A840CompileTarget),
      sizeof(A840UboPushRange),
      sizeof(A840StageInput),
      sizeof(A840CompiledStage),
      sizeof(A840TextureLayoutQuery),
      sizeof(A840TextureLayoutResult),
   };
   uint64_t expected_fp = UINT64_C(14695981039346656037);
   for (size_t i = 0; i < sizeof(expected_vals) / sizeof(expected_vals[0]); ++i) {
      uint64_t v = expected_vals[i];
      for (unsigned b = 0; b < 8u; ++b) {
         expected_fp ^= (uint8_t)v;
         expected_fp *= UINT64_C(1099511628211);
         v >>= 8;
      }
   }
   uint64_t actual_fp = a840_compile_abi_fingerprint();
   require_cond(actual_fp == expected_fp, "a840_compile_abi_fingerprint match", NULL);

   /* Null / ABI mismatch / bad chip / bad stage / bad SPIR-V must not mutate out_stage. */
   A840CompiledStage out;
   memset(&out, 0xa5, sizeof(out));
   A840CompiledStage before = out;
   char err[256] = { 0 };

   const uint32_t bad_magic[] = { 0xdeadbeefu, 0x00010500u, 0u, 1u, 0u };
   const uint32_t empty_spv[] = { 0x07230203u, 0x00010500u, 0u, 1u, 0u };
   A840StageInput in;
   memset(&in, 0, sizeof(in));
   in.abi_version = ADRENO_COMPILE_ABI_VERSION;
   in.stage = A840_STAGE_COMPUTE;
   in.spirv_words = bad_magic;
   in.spirv_bytes = sizeof(bad_magic);
   in.entry_point = "main";
   in.sample_count = 1u;

   require_cond(a840_compile_stage(NULL, &in, &out, err, sizeof(err)) == A840_COMPILE_INVALID_ARG,
                "NULL target", err);
   require_cond(a840_compile_stage(target, NULL, &out, err, sizeof(err)) == A840_COMPILE_INVALID_ARG,
                "NULL input", err);
   require_cond(a840_compile_stage(target, &in, NULL, err, sizeof(err)) == A840_COMPILE_INVALID_ARG,
                "NULL out_stage", err);
   require_cond(a840_compile_stage(target, &in, &out, NULL, 16u) == A840_COMPILE_INVALID_ARG,
                "NULL err_buf with nonzero size", NULL);

   A840CompileTarget bad_t = *target;
   bad_t.abi_version = 99u;
   require_cond(a840_compile_stage(&bad_t, &in, &out, err, sizeof(err)) == A840_COMPILE_ABI_MISMATCH,
                "target ABI mismatch", err);
   bad_t = *target;
   bad_t.chip_id = UINT64_C(0x43050a01);
   require_cond(a840_compile_stage(&bad_t, &in, &out, err, sizeof(err)) == A840_COMPILE_INVALID_ARG,
                "non-A840 chip_id rejected", err);

   require_cond(a840_compile_stage(target, &in, &out, err, sizeof(err)) == A840_COMPILE_SPIRV_ERROR,
                "bad SPIR-V magic rejected", err);

   /* Valid magic but missing entry point -> SPIRV_ERROR, zero leak, zero crash. */
   in.spirv_words = empty_spv;
   in.spirv_bytes = sizeof(empty_spv);
   err[0] = '\0';
   require_cond(a840_compile_stage(target, &in, &out, err, sizeof(err)) == A840_COMPILE_SPIRV_ERROR &&
                err[0] != '\0',
                "empty SPIR-V rejected cleanly", err);

   require_cond(memcmp(&out, &before, sizeof(out)) == 0,
                "failed a840_compile_stage did not touch out_stage", NULL);

   a840_compile_stage_free(NULL);
   printf("\n[ABI] fingerprint=0x%016" PRIx64 ", boundary/error/NULL-free checks passed\n",
          actual_fp);
}

int
main(int argc, char **argv)
{
   const char *dump_dir = NULL;
   for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--dump-disasm") == 0 && i + 1 < argc) {
         dump_dir = argv[++i];
      } else {
         fprintf(stderr, "usage: %s [--dump-disasm <dir>]\n", argv[0]);
         return EXIT_FAILURE;
      }
   }

   const A840CompileTarget target = {
      .abi_version = ADRENO_COMPILE_ABI_VERSION,
      .reserved0 = 0u,
      .chip_id = UINT64_C(0x44050a31),
      .num_slices = 3u,
      .num_ccu = 6u,
      .highest_bank_bit = 16u,
      .ubwc_swizzle = 6u,
      .macrotile_mode = 1u,
      .flags = 0u,
   };

   test_abi_boundaries_and_errors(&target);
   test_fdl_layouts(&target);
   test_real_metallibs(&target, dump_dir);

   puts("\nPASS: test_ir3_e2e (real 24A446 Compute+Render A840 Gen8 ISA + FDL UBWC 6.0 + ABI)");
   return EXIT_SUCCESS;
}
