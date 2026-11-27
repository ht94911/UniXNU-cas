/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "a840_system_pack_builder.h"

#include "Adreno840Regs.h"
#include "../ir3/a840_robustness_policy.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------------- Minimal Deterministic SPIR-V 1.3 Emitter ---------------- */

enum {
    SPV_OP_MEMORY_MODEL          = 14u,
    SPV_OP_ENTRY_POINT           = 15u,
    SPV_OP_EXECUTION_MODE        = 16u,
    SPV_OP_CAPABILITY            = 17u,
    SPV_OP_TYPE_VOID             = 19u,
    SPV_OP_TYPE_INT              = 21u,
    SPV_OP_TYPE_FLOAT            = 22u,
    SPV_OP_TYPE_VECTOR           = 23u,
    SPV_OP_TYPE_IMAGE            = 25u,
    SPV_OP_TYPE_RUNTIME_ARRAY    = 29u,
    SPV_OP_TYPE_STRUCT           = 30u,
    SPV_OP_TYPE_POINTER          = 32u,
    SPV_OP_TYPE_FUNCTION         = 33u,
    SPV_OP_CONSTANT              = 43u,
    SPV_OP_CONSTANT_COMPOSITE    = 44u,
    SPV_OP_FUNCTION              = 54u,
    SPV_OP_FUNCTION_END          = 56u,
    SPV_OP_VARIABLE              = 59u,
    SPV_OP_LOAD                  = 61u,
    SPV_OP_STORE                 = 62u,
    SPV_OP_ACCESS_CHAIN          = 65u,
    SPV_OP_DECORATE              = 71u,
    SPV_OP_MEMBER_DECORATE       = 72u,
    SPV_OP_COMPOSITE_EXTRACT     = 81u,
    SPV_OP_IMAGE_READ            = 98u,
    SPV_OP_FCONVERT              = 115u,
    SPV_OP_IADD                  = 128u,
    SPV_OP_FADD                  = 129u,
    SPV_OP_FSUB                  = 131u,
    SPV_OP_FMUL                  = 133u,
    SPV_OP_LABEL                 = 248u,
    SPV_OP_RETURN                = 253u,

    SPV_CAP_SHADER               = 1u,
    SPV_CAP_FLOAT16              = 9u,
    SPV_CAP_INPUT_ATTACHMENT     = 40u,
    SPV_CAP_STORAGE_IO16         = 4436u,

    SPV_EXEC_VERTEX              = 0u,
    SPV_EXEC_FRAGMENT            = 4u,
    SPV_EXEC_GLCOMPUTE           = 5u,

    SPV_MODE_ORIGIN_UPPER_LEFT   = 7u,
    SPV_MODE_LOCAL_SIZE          = 17u,

    SPV_SC_UNIFORM_CONSTANT      = 0u,
    SPV_SC_INPUT                 = 1u,
    SPV_SC_OUTPUT                = 3u,
    SPV_SC_STORAGE_BUFFER        = 12u,

    SPV_DEC_BLOCK                = 2u,
    SPV_DEC_ARRAY_STRIDE         = 6u,
    SPV_DEC_BUILTIN              = 11u,
    SPV_DEC_LOCATION             = 30u,
    SPV_DEC_BINDING              = 33u,
    SPV_DEC_DESC_SET             = 34u,
    SPV_DEC_OFFSET               = 35u,
    SPV_DEC_INPUT_ATT_IDX        = 43u,

    SPV_BUILTIN_POSITION         = 0u,
    SPV_BUILTIN_GLOBAL_INVOC_ID  = 28u
};

typedef struct SpvBuf {
    uint32_t *words;
    size_t len;
    size_t cap;
    bool oom;
} SpvBuf;

static uint32_t f32_bits(float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static void spv_init(SpvBuf *b, uint32_t bound)
{
    b->cap = 512u;
    b->words = (uint32_t *)malloc(b->cap * sizeof(uint32_t));
    b->len = 0u;
    b->oom = (b->words == NULL);
    if (!b->oom) {
        b->words[b->len++] = 0x07230203u;
        b->words[b->len++] = 0x00010300u; /* SPIR-V 1.3 */
        b->words[b->len++] = 0u;
        b->words[b->len++] = bound;
        b->words[b->len++] = 0u;
    }
}

static void spv_push(SpvBuf *b, uint32_t w)
{
    if (b->oom)
        return;
    if (b->len >= b->cap) {
        size_t ncap = b->cap * 2u;
        uint32_t *nw = (uint32_t *)realloc(b->words, ncap * sizeof(uint32_t));
        if (!nw) {
            b->oom = true;
            return;
        }
        b->words = nw;
        b->cap = ncap;
    }
    b->words[b->len++] = w;
}

static void spv_op(SpvBuf *b, uint16_t opcode, const uint32_t *ops, size_t nops)
{
    uint32_t wc = (uint32_t)(nops + 1u);
    spv_push(b, (wc << 16) | (uint32_t)opcode);
    for (size_t i = 0u; i < nops; ++i)
        spv_push(b, ops[i]);
}

static void spv_entry(SpvBuf *b, uint32_t exec_model, uint32_t fn_id,
    const char *name, const uint32_t *ifaces, size_t nifaces)
{
    size_t slen = strlen(name) + 1u;
    size_t sw = (slen + 3u) / 4u;
    uint32_t buf[64];
    if (2u + sw + nifaces > 64u) {
        b->oom = true;
        return;
    }
    buf[0] = exec_model;
    buf[1] = fn_id;
    memset(&buf[2], 0, sw * sizeof(uint32_t));
    for (size_t i = 0u; i < slen; ++i)
        buf[2u + i / 4u] |= ((uint32_t)(unsigned char)name[i]) << ((i % 4u) * 8u);
    for (size_t i = 0u; i < nifaces; ++i)
        buf[2u + sw + i] = ifaces[i];
    spv_op(b, SPV_OP_ENTRY_POINT, buf, 2u + sw + nifaces);
}

#define SPV_OP(b, op, ...)                                                   \
    do {                                                                     \
        const uint32_t _ops[] = { __VA_ARGS__ };                             \
        spv_op((b), (op), _ops, sizeof(_ops) / sizeof(_ops[0]));             \
    } while (0)

#define SPV_OP0(b, op) spv_op((b), (op), NULL, 0u)

/* ---------------- 6-Domain System Shader Catalog (42 Shaders) ---------------- */

typedef enum ShaderTemplateKind {
    TMPL_RENDER_FBFETCH_XR10 = 0,
    TMPL_RENDER_FP16_BLUR4   = 1,
    TMPL_RENDER_FP32_MIX3    = 2,
    TMPL_COMPUTE_SSBO_KERNEL = 3
} ShaderTemplateKind;

typedef struct SystemShaderSpec {
    A840SystemShaderDesc pub;
    ShaderTemplateKind tmpl;
    float c0;
    float c1;
    float c2;
    uint32_t taps;
} SystemShaderSpec;

#define FLAG_EP      A840_COMPILE_FLAG_EARLY_PREAMBLE
#define FLAG_EP_FB   (A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_FBFETCH_GMEM)
#define FLAG_EP_RTZ  (A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_HALF_ROUND_RTZ)
#define FLAG_EP_ROFF (A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_ROBUST_OFF)

static const SystemShaderSpec k_catalog[] = {
    /* Domain 0: CoreAnimation Liquid Glass (7 shaders) */
    { {  0u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_fbfetch_tint_xr10",
         true, true, true, FLAG_EP_FB, 1u, 0u, 1u, {0,0,0}, "vs_glass_tint", "fs_glass_tint" },
      TMPL_RENDER_FBFETCH_XR10, 0.82f, 0.18f, 0.04f, 2u },
    { {  1u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_fbfetch_frosted_xr10",
         true, true, true, FLAG_EP_FB, 1u, 0u, 1u, {0,0,0}, "vs_glass_frost", "fs_glass_frost" },
      TMPL_RENDER_FBFETCH_XR10, 0.68f, 0.32f, 0.09f, 3u },
    { {  2u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_blur_horiz_fp16_xr10",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_blur_h", "fs_blur_h" },
      TMPL_RENDER_FP16_BLUR4, 0.25f, 0.35f, 0.15f, 4u },
    { {  3u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_blur_vert_fp16_xr10",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_blur_v", "fs_blur_v" },
      TMPL_RENDER_FP16_BLUR4, 0.28f, 0.32f, 0.12f, 4u },
    { {  4u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_rim_refract_xr10",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_glass_rim", "fs_glass_rim" },
      TMPL_RENDER_FP16_BLUR4, 0.45f, 0.30f, 0.22f, 4u },
    { {  5u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_backdrop_composite_xr10",
         true, false, true, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_glass_comp", "fs_glass_comp" },
      TMPL_RENDER_FP32_MIX3, 0.74f, 0.26f, 0.05f, 3u },
    { {  6u, A840_SYSDOMAIN_CA_LIQUID_GLASS, "ca_liquid_glass_kawase_downsample_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_glass_kawase", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.25f, 0.50f, 4u },

    /* Domain 1: SpringBoard Icons & Animations (7 shaders) */
    { {  7u, A840_SYSDOMAIN_SPRINGBOARD, "sb_icon_squircle_mask_xr10",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_sb_squircle", "fs_sb_squircle" },
      TMPL_RENDER_FP16_BLUR4, 0.50f, 0.25f, 0.10f, 4u },
    { {  8u, A840_SYSDOMAIN_SPRINGBOARD, "sb_folder_backdrop_blur_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_sb_folder_blur", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.30f, 0.40f, 0.30f, 3u },
    { {  9u, A840_SYSDOMAIN_SPRINGBOARD, "sb_folder_spring_zoom_vsfs",
         true, false, true, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_sb_zoom", "fs_sb_zoom" },
      TMPL_RENDER_FP32_MIX3, 0.90f, 0.10f, 0.02f, 3u },
    { { 10u, A840_SYSDOMAIN_SPRINGBOARD, "sb_wallpaper_parallax_vignette_xr10",
         true, false, true, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_sb_wall", "fs_sb_wall" },
      TMPL_RENDER_FP32_MIX3, 0.85f, 0.12f, 0.03f, 3u },
    { { 11u, A840_SYSDOMAIN_SPRINGBOARD, "sb_badge_glyph_premul_fp16",
         true, false, false, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_sb_badge", "fs_sb_badge" },
      TMPL_RENDER_FP16_BLUR4, 0.60f, 0.20f, 0.08f, 4u },
    { { 12u, A840_SYSDOMAIN_SPRINGBOARD, "sb_dock_frosted_fbfetch_xr10",
         true, true, true, FLAG_EP_FB, 1u, 0u, 1u, {0,0,0}, "vs_sb_dock", "fs_sb_dock" },
      TMPL_RENDER_FBFETCH_XR10, 0.72f, 0.28f, 0.06f, 2u },
    { { 13u, A840_SYSDOMAIN_SPRINGBOARD, "sb_page_scroll_tile_blit_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_sb_page_blit", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.50f, 0.35f, 0.15f, 3u },

    /* Domain 2: UIKit Scroll Lists & Text (7 shaders) */
    { { 14u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_sdf_text_glyph_fp16",
         true, false, false, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_ui_text", "fs_ui_text" },
      TMPL_RENDER_FP16_BLUR4, 0.40f, 0.40f, 0.05f, 4u },
    { { 15u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_rounded_rect_clip_fp16",
         true, false, false, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_ui_rrect", "fs_ui_rrect" },
      TMPL_RENDER_FP16_BLUR4, 0.55f, 0.25f, 0.07f, 4u },
    { { 16u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_list_cell_fill_vsfs",
         true, false, false, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_ui_cell", "fs_ui_cell" },
      TMPL_RENDER_FP32_MIX3, 0.95f, 0.05f, 0.01f, 3u },
    { { 17u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_shadow_9slice_vsfs",
         true, false, false, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_ui_shadow", "fs_ui_shadow" },
      TMPL_RENDER_FP32_MIX3, 0.33f, 0.33f, 0.34f, 3u },
    { { 18u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_settings_scroll_blit_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {8,8,1}, "cs_ui_settings", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.70f, 0.20f, 0.10f, 2u },
    { { 19u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_p3_xr10_tint_overlay",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_ui_p3", "fs_ui_p3" },
      TMPL_RENDER_FP16_BLUR4, 0.42f, 0.38f, 0.11f, 4u },
    { { 20u, A840_SYSDOMAIN_UIKIT_SCROLL, "uikit_applib_section_fade_vsfs",
         true, false, true, FLAG_EP_RTZ, 1u, 1u, 1u, {0,0,0}, "vs_ui_applib", "fs_ui_applib" },
      TMPL_RENDER_FP16_BLUR4, 0.62f, 0.24f, 0.09f, 4u },

    /* Domain 3: Metal System Builtins (7 shaders) */
    { { 21u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_blit_buffer_copy_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {64,1,1}, "cs_mtl_bufcopy", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 1.00f, 0.00f, 0.00f, 1u },
    { { 22u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_blit_tex_clear_bgra8_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {8,8,1}, "cs_mtl_clear8", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.00f, 0.50f, 0.50f, 1u },
    { { 23u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_blit_tex_scale_bilinear_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_mtl_scale", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.50f, 0.25f, 4u },
    { { 24u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_mipmap_box2x2_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_mtl_mip2x2", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.25f, 0.25f, 4u },
    { { 25u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_msaa4x_resolve_avg_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_mtl_msaa4x", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.25f, 0.50f, 4u },
    { { 26u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_indirect_arg_prefix_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {32,1,1}, "cs_mtl_prefix", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.50f, 0.50f, 0.00f, 2u },
    { { 27u, A840_SYSDOMAIN_METAL_BUILTIN, "mtl_blit_tex_clear_xr10_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_mtl_clearxr", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.12f, 0.64f, 0.24f, 2u },

    /* Domain 4: Camera 155 Preview & Capture Pipeline (7 shaders) */
    { { 28u, A840_SYSDOMAIN_CAMERA155, "cam155_nv12_yuv_to_bgra_bt709_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {8,8,1}, "cs_cam_nv12", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 1.164f, 1.793f, -0.533f, 3u },
    { { 29u, A840_SYSDOMAIN_CAMERA155, "cam155_yuv_to_xr10_preview_vsfs",
         true, false, true, FLAG_EP, 1u, 0u, 1u, {0,0,0}, "vs_cam_prev", "fs_cam_prev" },
      TMPL_RENDER_FP32_MIX3, 1.164f, 1.596f, -0.391f, 3u },
    { { 30u, A840_SYSDOMAIN_CAMERA155, "cam155_rotate90_crop_scale_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_cam_rot90", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.50f, 0.25f, 0.25f, 3u },
    { { 31u, A840_SYSDOMAIN_CAMERA155, "cam155_raw10_bayer_demosaic_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_cam_bayer", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.50f, 0.25f, 4u },
    { { 32u, A840_SYSDOMAIN_CAMERA155, "cam155_ae_25pt_luma_reduce_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {16,16,1}, "cs_cam_ae25", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.2126f, 0.7152f, 0.0722f, 2u },
    { { 33u, A840_SYSDOMAIN_CAMERA155, "cam155_still_tonemap_sharpen_xr10_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_cam_tone", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 1.20f, -0.15f, -0.05f, 4u },
    { { 34u, A840_SYSDOMAIN_CAMERA155, "cam155_p010_hdr_bt2020_xr10_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_cam_p010", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 1.168f, 1.474f, -0.571f, 3u },

    /* Domain 5: CoreImage & MPS (7 shaders) */
    { { 35u, A840_SYSDOMAIN_COREIMAGE_MPS, "ci_conv3x3_separable_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_ci_conv3", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.25f, 0.50f, 0.25f, 3u },
    { { 36u, A840_SYSDOMAIN_COREIMAGE_MPS, "ci_gaussian5x5_filter_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_ci_gauss5", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.0625f, 0.25f, 0.375f, 4u },
    { { 37u, A840_SYSDOMAIN_COREIMAGE_MPS, "ci_color_matrix_p3_xr10_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_ci_cmat", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.8225f, 0.1775f, 0.0332f, 3u },
    { { 38u, A840_SYSDOMAIN_COREIMAGE_MPS, "ci_unsharp_mask_exposure_xr10_cs",
         false, false, true, FLAG_EP, 0u, 0u, 1u, {8,8,1}, "cs_ci_unsharp", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 1.35f, -0.25f, -0.10f, 3u },
    { { 39u, A840_SYSDOMAIN_COREIMAGE_MPS, "mps_gemm_tile8x8_fma_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {8,8,1}, "cs_mps_gemm", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.75f, 0.50f, 0.125f, 4u },
    { { 40u, A840_SYSDOMAIN_COREIMAGE_MPS, "mps_conv2d_depthwise_relu_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {8,8,1}, "cs_mps_conv2d", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.60f, 0.30f, 0.10f, 4u },
    { { 41u, A840_SYSDOMAIN_COREIMAGE_MPS, "mps_layernorm_reduce_cs",
         false, false, false, FLAG_EP_ROFF, 0u, 0u, 1u, {32,1,1}, "cs_mps_lnorm", NULL },
      TMPL_COMPUTE_SSBO_KERNEL, 0.50f, 0.25f, 0.25f, 3u }
};

#define CATALOG_LEN (sizeof(k_catalog) / sizeof(k_catalog[0]))

/* ---------------- SPIR-V Synthesis per Template ---------------- */

static SpvBuf emit_vertex_spv(const SystemShaderSpec *spec)
{
    SpvBuf b;
    spv_init(&b, 64u);

    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_SHADER);
    SPV_OP(&b, SPV_OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u, 13u };
    spv_entry(&b, SPV_EXEC_VERTEX, 3u, spec->pub.entry_name, ifaces, 4u);

    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_LOCATION, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_LOCATION, 1u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_BUILTIN, SPV_BUILTIN_POSITION);
    SPV_OP(&b, SPV_OP_DECORATE, 13u, SPV_DEC_LOCATION, 0u);

    SPV_OP(&b, SPV_OP_TYPE_VOID, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 6u, SPV_SC_INPUT, 5u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 7u, SPV_SC_OUTPUT, 5u);

    SPV_OP(&b, SPV_OP_VARIABLE, 6u, 10u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 6u, 11u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 7u, 12u, SPV_SC_OUTPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 7u, 13u, SPV_SC_OUTPUT);

    float scale = 1.0f + (float)(spec->pub.index % 7u) * 0.01f;
    float bias = (float)(spec->pub.index % 5u) * 0.002f;
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 20u, f32_bits(scale));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 21u, 20u, 20u, 20u, 20u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 22u, f32_bits(bias));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 23u, 22u, 22u, 22u, 22u);

    SPV_OP(&b, SPV_OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, SPV_OP_LABEL, 30u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 32u, 11u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 33u, 31u, 21u);
    SPV_OP(&b, SPV_OP_FADD, 5u, 34u, 33u, 23u);
    SPV_OP(&b, SPV_OP_STORE, 12u, 34u);
    SPV_OP(&b, SPV_OP_STORE, 13u, 32u);
    SPV_OP0(&b, SPV_OP_RETURN);
    SPV_OP0(&b, SPV_OP_FUNCTION_END);
    return b;
}

static SpvBuf emit_fbfetch_frag_spv(const SystemShaderSpec *spec)
{
    SpvBuf b;
    spv_init(&b, 64u);

    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_SHADER);
    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_INPUT_ATTACHMENT);
    SPV_OP(&b, SPV_OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u };
    spv_entry(&b, SPV_EXEC_FRAGMENT, 3u, spec->pub.fs_entry_name, ifaces, 3u);
    SPV_OP(&b, SPV_OP_EXECUTION_MODE, 3u, SPV_MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_DESC_SET, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_BINDING, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_INPUT_ATT_IDX, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_LOCATION, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_LOCATION, 0u);

    SPV_OP(&b, SPV_OP_TYPE_VOID, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, SPV_OP_TYPE_INT, 6u, 32u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 7u, 6u, 2u);
    /* OpTypeImage float SubpassData(6) depth=0 arrayed=0 ms=0 sampled=2 fmt=Unknown(0) */
    SPV_OP(&b, SPV_OP_TYPE_IMAGE, 8u, 4u, 6u, 0u, 0u, 0u, 2u, 0u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 9u, SPV_SC_UNIFORM_CONSTANT, 8u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 13u, SPV_SC_INPUT, 5u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 14u, SPV_SC_OUTPUT, 5u);

    SPV_OP(&b, SPV_OP_VARIABLE, 9u, 10u, SPV_SC_UNIFORM_CONSTANT);
    SPV_OP(&b, SPV_OP_VARIABLE, 13u, 11u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 14u, 12u, SPV_SC_OUTPUT);

    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 20u, 0u);
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 7u, 21u, 20u, 20u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 22u, f32_bits(spec->c0));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 23u, 22u, 22u, 22u, 22u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 24u, f32_bits(spec->c1));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 25u, 24u, 24u, 24u, 24u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 26u, f32_bits(spec->c2));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 27u, 26u, 26u, 26u, 26u);

    SPV_OP(&b, SPV_OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, SPV_OP_LABEL, 30u);
    SPV_OP(&b, SPV_OP_LOAD, 8u, 31u, 10u);
    SPV_OP(&b, SPV_OP_IMAGE_READ, 5u, 32u, 31u, 21u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 33u, 11u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 34u, 32u, 23u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 35u, 33u, 25u);
    SPV_OP(&b, SPV_OP_FADD, 5u, 36u, 34u, 35u);
    SPV_OP(&b, SPV_OP_FADD, 5u, 37u, 36u, 27u);
    SPV_OP(&b, SPV_OP_STORE, 12u, 37u);
    SPV_OP0(&b, SPV_OP_RETURN);
    SPV_OP0(&b, SPV_OP_FUNCTION_END);
    return b;
}

static SpvBuf emit_fp16_blur_frag_spv(const SystemShaderSpec *spec)
{
    SpvBuf b;
    spv_init(&b, 80u);

    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_SHADER);
    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_FLOAT16);
    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_STORAGE_IO16);
    SPV_OP(&b, SPV_OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u, 13u, 14u };
    spv_entry(&b, SPV_EXEC_FRAGMENT, 3u, spec->pub.fs_entry_name, ifaces, 5u);
    SPV_OP(&b, SPV_OP_EXECUTION_MODE, 3u, SPV_MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_LOCATION, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_LOCATION, 1u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_LOCATION, 2u);
    SPV_OP(&b, SPV_OP_DECORATE, 13u, SPV_DEC_LOCATION, 3u);
    SPV_OP(&b, SPV_OP_DECORATE, 14u, SPV_DEC_LOCATION, 0u);

    SPV_OP(&b, SPV_OP_TYPE_VOID, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 4u, 16u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 8u, SPV_SC_INPUT, 5u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 9u, SPV_SC_OUTPUT, 5u);

    SPV_OP(&b, SPV_OP_VARIABLE, 8u, 10u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 8u, 11u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 8u, 12u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 8u, 13u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 9u, 14u, SPV_SC_OUTPUT);

    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 20u, f32_bits(spec->c0));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 7u, 21u, 20u, 20u, 20u, 20u);
    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 22u, f32_bits(spec->c1));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 7u, 23u, 22u, 22u, 22u, 22u);
    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 24u, f32_bits(spec->c2));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 7u, 25u, 24u, 24u, 24u, 24u);

    SPV_OP(&b, SPV_OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, SPV_OP_LABEL, 30u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 32u, 11u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 33u, 12u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 34u, 13u);
    SPV_OP(&b, SPV_OP_FCONVERT, 7u, 35u, 31u);
    SPV_OP(&b, SPV_OP_FCONVERT, 7u, 36u, 32u);
    SPV_OP(&b, SPV_OP_FCONVERT, 7u, 37u, 33u);
    SPV_OP(&b, SPV_OP_FCONVERT, 7u, 38u, 34u);
    SPV_OP(&b, SPV_OP_FMUL, 7u, 40u, 35u, 21u);
    SPV_OP(&b, SPV_OP_FMUL, 7u, 41u, 36u, 23u);
    SPV_OP(&b, SPV_OP_FADD, 7u, 42u, 40u, 41u);
    SPV_OP(&b, SPV_OP_FMUL, 7u, 43u, 37u, 25u);
    SPV_OP(&b, SPV_OP_FADD, 7u, 44u, 42u, 43u);
    SPV_OP(&b, SPV_OP_FMUL, 7u, 45u, 38u, 25u);
    SPV_OP(&b, SPV_OP_FADD, 7u, 46u, 44u, 45u);
    SPV_OP(&b, SPV_OP_FCONVERT, 5u, 47u, 46u);
    SPV_OP(&b, SPV_OP_STORE, 14u, 47u);
    SPV_OP0(&b, SPV_OP_RETURN);
    SPV_OP0(&b, SPV_OP_FUNCTION_END);
    return b;
}

static SpvBuf emit_fp32_mix_frag_spv(const SystemShaderSpec *spec)
{
    SpvBuf b;
    spv_init(&b, 64u);

    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_SHADER);
    SPV_OP(&b, SPV_OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u, 13u };
    spv_entry(&b, SPV_EXEC_FRAGMENT, 3u, spec->pub.fs_entry_name, ifaces, 4u);
    SPV_OP(&b, SPV_OP_EXECUTION_MODE, 3u, SPV_MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, SPV_OP_DECORATE, 10u, SPV_DEC_LOCATION, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_LOCATION, 1u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_LOCATION, 2u);
    SPV_OP(&b, SPV_OP_DECORATE, 13u, SPV_DEC_LOCATION, 0u);

    SPV_OP(&b, SPV_OP_TYPE_VOID, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 6u, SPV_SC_INPUT, 5u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 7u, SPV_SC_OUTPUT, 5u);

    SPV_OP(&b, SPV_OP_VARIABLE, 6u, 10u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 6u, 11u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 6u, 12u, SPV_SC_INPUT);
    SPV_OP(&b, SPV_OP_VARIABLE, 7u, 13u, SPV_SC_OUTPUT);

    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 20u, f32_bits(spec->c0));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 21u, 20u, 20u, 20u, 20u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 22u, f32_bits(spec->c1));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 23u, 22u, 22u, 22u, 22u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 24u, f32_bits(spec->c2));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 25u, 24u, 24u, 24u, 24u);

    SPV_OP(&b, SPV_OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, SPV_OP_LABEL, 30u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 32u, 11u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 33u, 12u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 34u, 31u, 21u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 35u, 32u, 23u);
    SPV_OP(&b, SPV_OP_FADD, 5u, 36u, 34u, 35u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 37u, 33u, 25u);
    SPV_OP(&b, SPV_OP_FADD, 5u, 38u, 36u, 37u);
    SPV_OP(&b, SPV_OP_STORE, 13u, 38u);
    SPV_OP0(&b, SPV_OP_RETURN);
    SPV_OP0(&b, SPV_OP_FUNCTION_END);
    return b;
}

static SpvBuf emit_compute_ssbo_spv(const SystemShaderSpec *spec)
{
    SpvBuf b;
    spv_init(&b, 96u);

    SPV_OP(&b, SPV_OP_CAPABILITY, SPV_CAP_SHADER);
    SPV_OP(&b, SPV_OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 16u };
    spv_entry(&b, SPV_EXEC_GLCOMPUTE, 3u, spec->pub.entry_name, ifaces, 1u);
    SPV_OP(&b, SPV_OP_EXECUTION_MODE, 3u, SPV_MODE_LOCAL_SIZE,
           spec->pub.local_size[0], spec->pub.local_size[1], spec->pub.local_size[2]);

    SPV_OP(&b, SPV_OP_DECORATE, 8u, SPV_DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, SPV_OP_DECORATE, 9u, SPV_DEC_BLOCK);
    SPV_OP(&b, SPV_OP_MEMBER_DECORATE, 9u, 0u, SPV_DEC_OFFSET, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_DESC_SET, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 11u, SPV_DEC_BINDING, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_DESC_SET, 0u);
    SPV_OP(&b, SPV_OP_DECORATE, 12u, SPV_DEC_BINDING, 1u);
    SPV_OP(&b, SPV_OP_DECORATE, 16u, SPV_DEC_BUILTIN, SPV_BUILTIN_GLOBAL_INVOC_ID);

    SPV_OP(&b, SPV_OP_TYPE_VOID, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, SPV_OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 5u, 4u, 4u);    /* vec4 */
    SPV_OP(&b, SPV_OP_TYPE_INT, 6u, 32u, 0u);      /* uint */
    SPV_OP(&b, SPV_OP_TYPE_VECTOR, 7u, 6u, 3u);    /* uvec3 */
    SPV_OP(&b, SPV_OP_TYPE_RUNTIME_ARRAY, 8u, 5u); /* vec4[] */
    SPV_OP(&b, SPV_OP_TYPE_STRUCT, 9u, 8u);        /* struct { vec4[] } */
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 10u, SPV_SC_STORAGE_BUFFER, 9u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 13u, SPV_SC_STORAGE_BUFFER, 5u);
    SPV_OP(&b, SPV_OP_TYPE_POINTER, 15u, SPV_SC_INPUT, 7u);

    SPV_OP(&b, SPV_OP_VARIABLE, 10u, 11u, SPV_SC_STORAGE_BUFFER); /* in_buf */
    SPV_OP(&b, SPV_OP_VARIABLE, 10u, 12u, SPV_SC_STORAGE_BUFFER); /* out_buf */
    SPV_OP(&b, SPV_OP_VARIABLE, 15u, 16u, SPV_SC_INPUT);          /* gl_GlobalInvocationID */

    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 20u, 0u);
    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 21u, 1u);
    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 22u, 2u);
    SPV_OP(&b, SPV_OP_CONSTANT, 6u, 23u, 3u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 24u, f32_bits(spec->c0));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 25u, 24u, 24u, 24u, 24u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 26u, f32_bits(spec->c1));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 27u, 26u, 26u, 26u, 26u);
    SPV_OP(&b, SPV_OP_CONSTANT, 4u, 28u, f32_bits(spec->c2));
    SPV_OP(&b, SPV_OP_CONSTANT_COMPOSITE, 5u, 29u, 28u, 28u, 28u, 28u);

    SPV_OP(&b, SPV_OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, SPV_OP_LABEL, 30u);

    SPV_OP(&b, SPV_OP_LOAD, 7u, 31u, 16u);
    SPV_OP(&b, SPV_OP_COMPOSITE_EXTRACT, 6u, 32u, 31u, 0u); /* idx = gid.x */

    /* Tap 0 */
    SPV_OP(&b, SPV_OP_ACCESS_CHAIN, 13u, 33u, 11u, 20u, 32u);
    SPV_OP(&b, SPV_OP_LOAD, 5u, 34u, 33u);
    SPV_OP(&b, SPV_OP_FMUL, 5u, 35u, 34u, 25u);

    uint32_t acc = 35u;
    if (spec->taps >= 2u) {
        SPV_OP(&b, SPV_OP_IADD, 6u, 36u, 32u, 21u);
        SPV_OP(&b, SPV_OP_ACCESS_CHAIN, 13u, 37u, 11u, 20u, 36u);
        SPV_OP(&b, SPV_OP_LOAD, 5u, 38u, 37u);
        SPV_OP(&b, SPV_OP_FMUL, 5u, 39u, 38u, 27u);
        SPV_OP(&b, SPV_OP_FADD, 5u, 40u, acc, 39u);
        acc = 40u;
    }
    if (spec->taps >= 3u) {
        SPV_OP(&b, SPV_OP_IADD, 6u, 41u, 32u, 22u);
        SPV_OP(&b, SPV_OP_ACCESS_CHAIN, 13u, 42u, 11u, 20u, 41u);
        SPV_OP(&b, SPV_OP_LOAD, 5u, 43u, 42u);
        SPV_OP(&b, SPV_OP_FMUL, 5u, 44u, 43u, 29u);
        SPV_OP(&b, SPV_OP_FADD, 5u, 45u, acc, 44u);
        acc = 45u;
    }
    if (spec->taps >= 4u) {
        SPV_OP(&b, SPV_OP_IADD, 6u, 46u, 32u, 23u);
        SPV_OP(&b, SPV_OP_ACCESS_CHAIN, 13u, 47u, 11u, 20u, 46u);
        SPV_OP(&b, SPV_OP_LOAD, 5u, 48u, 47u);
        SPV_OP(&b, SPV_OP_FMUL, 5u, 49u, 48u, 29u);
        SPV_OP(&b, SPV_OP_FADD, 5u, 50u, acc, 49u);
        acc = 50u;
    }

    SPV_OP(&b, SPV_OP_ACCESS_CHAIN, 13u, 51u, 12u, 20u, 32u);
    SPV_OP(&b, SPV_OP_STORE, 51u, acc);
    SPV_OP0(&b, SPV_OP_RETURN);
    SPV_OP0(&b, SPV_OP_FUNCTION_END);
    return b;
}

/* ---------------- Pre-encoded Static SDS PM4 Block (Zero BO VAs) ---------------- */

static uint32_t pm4_odd_parity(uint32_t v)
{
    uint32_t p = 1u;
    while (v != 0u) {
        p ^= (v & 1u);
        v >>= 1;
    }
    return p;
}

static uint32_t pm4_type4_hdr(uint32_t reg_offset, uint32_t count)
{
    return 0x40000000u |
           (count & 0x7fu) |
           (pm4_odd_parity(count & 0x7fu) << 7) |
           ((reg_offset & 0x7ffffu) << 8) |
           (pm4_odd_parity(reg_offset & 0x7ffffu) << 27);
}

static void encode_static_sds_block(const A840CompiledStage *st, uint32_t out_words[6])
{
    uint32_t reg_cfg = A840_REG_SP_CS_CONFIG;
    uint32_t reg_sz  = A840_REG_SP_CS_INSTR_SIZE;
    uint32_t reg_cst = A840_REG_SP_CS_CONST_CONFIG;
    if (st->stage == A840_STAGE_VERTEX) {
        reg_cfg = A840_REG_SP_VS_CONFIG;
        reg_sz  = A840_REG_SP_VS_INSTR_SIZE;
        reg_cst = A840_REG_SP_VS_CONST_CONFIG;
    } else if (st->stage == A840_STAGE_FRAGMENT) {
        reg_cfg = A840_REG_SP_PS_CONFIG;
        reg_sz  = A840_REG_SP_PS_INSTR_SIZE;
        reg_cst = A840_REG_SP_PS_CONST_CONFIG;
    }

    /* Static non-VA config words: stage enabled + bindless + instruction groups + constlen */
    uint32_t cfg_val = (1u << 8) | ((st->flags_out & 0x1fu) << 16);
    uint32_t sz_val  = st->instruction_groups;
    uint32_t cst_val = st->constlen_vec4;

    out_words[0] = pm4_type4_hdr(reg_cfg, 1u);
    out_words[1] = cfg_val;
    out_words[2] = pm4_type4_hdr(reg_sz, 1u);
    out_words[3] = sz_val;
    out_words[4] = pm4_type4_hdr(reg_cst, 1u);
    out_words[5] = cst_val;
}

/* ---------------- Path / Timing Helpers ---------------- */

static bool starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        ++s;
        ++prefix;
    }
    return true;
}

static bool path_is_forbidden(const char *path)
{
    if (!path || !path[0])
        return true;
    for (const char *p = path; *p; ++p) {
        if (*p == '\\')
            return true;
    }
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        return true;
    if (starts_with_ci(path, "/mnt/c") && (path[6] == '\0' || path[6] == '/'))
        return true;
    if (strcmp(path, "/tmp") == 0 || strncmp(path, "/tmp/", 5u) == 0)
        return true;
    if (starts_with_ci(path, "/private/var/ngpu") &&
        (path[17] == '\0' || path[17] == '/'))
        return true;
    return false;
}

static uint64_t now_monotonic_ns(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

static bool stage_has_unbound_ubo_block1(const A840CompiledStage *st)
{
    if (!st)
        return false;
    uint32_t n = st->ubo_push_count;
    if (n > ADRENO_COMPILE_MAX_UBO_PUSH)
        n = ADRENO_COMPILE_MAX_UBO_PUSH;
    for (uint32_t i = 0u; i < n; ++i) {
        if (st->ubo_push[i].set_index == 0u && st->ubo_push[i].binding_index == 1u)
            return true;
    }
    return false;
}

static bool verify_view_and_bench(
    const A840PackV1View *view,
    uint32_t bench_iters,
    bool *out_isa_aligned,
    bool *out_sds_present,
    bool *out_instrlen_valid,
    uint32_t *out_unbound_ubo1,
    double *out_mean_us,
    double *out_p50_us,
    double *out_p99_us)
{
    if (!view || view->entry_count == 0u)
        return false;

    bool all_aligned = true;
    bool all_sds = true;
    bool all_instrlen = true;
    uint32_t unbound_ubo1 = 0u;

    A840PipelineKey *keys = (A840PipelineKey *)calloc(view->entry_count, sizeof(A840PipelineKey));
    if (!keys)
        return false;

    for (uint32_t i = 0u; i < view->entry_count; ++i) {
        A840PackV1Entry e;
        memset(&e, 0, sizeof(e));
        if (a840_pack_v1_entry_at(view, i, &e) != A840_PACK_OK) {
            free(keys);
            return false;
        }
        keys[i] = *e.pipeline_key;
        for (uint32_t s = 0u; s < e.stage_count; ++s) {
            const A840PackV1StageView *sv = &e.stages[s];
            if (((uintptr_t)sv->isa_words % A840_PACK_V1_ISA_ALIGNMENT) != 0u)
                all_aligned = false;
            if (sv->sds_state_dwords == 0u || sv->sds_state_words == NULL)
                all_sds = false;
            if (sv->instruction_groups == 0u ||
                (uint64_t)sv->instruction_groups * 4u > (uint64_t)sv->isa_dwords)
                all_instrlen = false;
        }
        A840PipelineRecord rec;
        memset(&rec, 0, sizeof(rec));
        if (a840_pack_v1_entry_record(view, &e, &rec) == A840_PACK_OK) {
            for (uint32_t s = 0u; s < rec.stage_count; ++s) {
                if (stage_has_unbound_ubo_block1(&rec.stages[s]))
                    ++unbound_ubo1;
            }
        }
    }

    uint32_t iters = bench_iters ? bench_iters : 200u;
    size_t total_samples = (size_t)iters * (size_t)view->entry_count;
    if (total_samples > 32768u) {
        iters = (uint32_t)(32768u / view->entry_count);
        if (iters == 0u)
            iters = 1u;
        total_samples = (size_t)iters * (size_t)view->entry_count;
    }

    /* Warmup pass to fault pages before timing zero-copy lookup. */
    for (uint32_t i = 0u; i < view->entry_count; ++i) {
        A840PackV1Entry e;
        if (a840_pack_v1_lookup(view, &keys[i], &e) != A840_PACK_OK) {
            free(keys);
            return false;
        }
    }

    uint64_t *samples_ns = (uint64_t *)malloc(total_samples * sizeof(uint64_t));
    if (!samples_ns) {
        free(keys);
        return false;
    }

    size_t sample_idx = 0u;
    uint64_t sum_ns = 0u;
    for (uint32_t r = 0u; r < iters; ++r) {
        for (uint32_t i = 0u; i < view->entry_count; ++i) {
            A840PackV1Entry e;
            uint64_t t0 = now_monotonic_ns();
            A840PackV1Status st = a840_pack_v1_lookup(view, &keys[i], &e);
            uint64_t t1 = now_monotonic_ns();
            if (st != A840_PACK_OK) {
                free(samples_ns);
                free(keys);
                return false;
            }
            uint64_t dt = (t1 >= t0) ? (t1 - t0) : 0u;
            samples_ns[sample_idx++] = dt;
            sum_ns += dt;
        }
    }

    qsort(samples_ns, sample_idx, sizeof(uint64_t), compare_u64);
    size_t idx50 = (sample_idx > 0u) ? ((sample_idx - 1u) * 50u) / 100u : 0u;
    size_t idx99 = (sample_idx > 0u) ? ((sample_idx - 1u) * 99u) / 100u : 0u;

    if (out_isa_aligned)   *out_isa_aligned = all_aligned;
    if (out_sds_present)   *out_sds_present = all_sds;
    if (out_instrlen_valid) *out_instrlen_valid = all_instrlen;
    if (out_unbound_ubo1)  *out_unbound_ubo1 = unbound_ubo1;
    if (out_mean_us)       *out_mean_us = ((double)sum_ns / (double)sample_idx) / 1000.0;
    if (out_p50_us)        *out_p50_us = (double)samples_ns[idx50] / 1000.0;
    if (out_p99_us)        *out_p99_us = (double)samples_ns[idx99] / 1000.0;

    free(samples_ns);
    free(keys);
    return true;
}

/* ---------------- Public API ---------------- */

const char *a840_system_pack_status_string(A840SystemPackStatus status)
{
    switch (status) {
    case A840_SYSPACK_OK:               return "OK";
    case A840_SYSPACK_INVALID_ARG:      return "INVALID_ARG";
    case A840_SYSPACK_COMPILE_ERROR:    return "COMPILE_ERROR";
    case A840_SYSPACK_XR10_VIOLATION:   return "XR10_VIOLATION";
    case A840_SYSPACK_INSTRLEN_ERROR:   return "INSTRLEN_ERROR";
    case A840_SYSPACK_UBO_BLOCK1_ERROR: return "UBO_BLOCK1_ERROR";
    case A840_SYSPACK_PACK_ERROR:       return "PACK_ERROR";
    case A840_SYSPACK_VERIFY_ERROR:     return "VERIFY_ERROR";
    case A840_SYSPACK_IO_ERROR:         return "IO_ERROR";
    case A840_SYSPACK_OOM:              return "OOM";
    case A840_SYSPACK_PATH_FORBIDDEN:   return "PATH_FORBIDDEN";
    default:                            return "UNKNOWN";
    }
}

const char *a840_system_pack_domain_name(A840SystemPackDomain domain)
{
    switch (domain) {
    case A840_SYSDOMAIN_CA_LIQUID_GLASS: return "ca_liquid_glass";
    case A840_SYSDOMAIN_SPRINGBOARD:     return "springboard";
    case A840_SYSDOMAIN_UIKIT_SCROLL:    return "uikit_scroll";
    case A840_SYSDOMAIN_METAL_BUILTIN:   return "metal_builtin";
    case A840_SYSDOMAIN_CAMERA155:       return "camera155";
    case A840_SYSDOMAIN_COREIMAGE_MPS:   return "coreimage_mps";
    default:                             return "unknown";
    }
}

size_t a840_system_pack_catalog_count(void)
{
    return CATALOG_LEN;
}

const A840SystemShaderDesc *a840_system_pack_catalog_get(size_t index)
{
    if (index >= CATALOG_LEN)
        return NULL;
    return &k_catalog[index].pub;
}

A840SystemPackStatus a840_system_pack_entry_spirv(
    size_t index,
    uint32_t stage_idx,
    uint32_t **out_words,
    size_t *out_bytes)
{
    if (index >= CATALOG_LEN || !out_words || !out_bytes)
        return A840_SYSPACK_INVALID_ARG;

    const SystemShaderSpec *spec = &k_catalog[index];
    uint32_t max_stages = spec->pub.is_render ? 2u : 1u;
    if (stage_idx >= max_stages)
        return A840_SYSPACK_INVALID_ARG;

    SpvBuf buf;
    if (!spec->pub.is_render) {
        buf = emit_compute_ssbo_spv(spec);
    } else if (stage_idx == 0u) {
        buf = emit_vertex_spv(spec);
    } else {
        switch (spec->tmpl) {
        case TMPL_RENDER_FBFETCH_XR10: buf = emit_fbfetch_frag_spv(spec); break;
        case TMPL_RENDER_FP16_BLUR4:   buf = emit_fp16_blur_frag_spv(spec); break;
        case TMPL_RENDER_FP32_MIX3:    buf = emit_fp32_mix_frag_spv(spec); break;
        default:                       return A840_SYSPACK_INVALID_ARG;
        }
    }

    if (buf.oom || !buf.words || buf.len == 0u) {
        free(buf.words);
        return A840_SYSPACK_OOM;
    }

    *out_words = buf.words;
    *out_bytes = buf.len * sizeof(uint32_t);
    return A840_SYSPACK_OK;
}

static void canonical_spirv_sha256(const uint32_t *words, size_t bytes, uint8_t out[32])
{
    uint8_t *le = (uint8_t *)malloc(bytes);
    if (!le) {
        memset(out, 0, 32u);
        return;
    }
    for (size_t i = 0u; i < bytes; ++i)
        le[i] = (uint8_t)(words[i / 4u] >> ((i % 4u) * 8u));
    (void)a840_record_sha256(le, bytes, out);
    free(le);
}

typedef struct CompiledEntryState {
    A840PipelineRecord record;
    A840BindingPlan plan;
    uint32_t sds_words[2][6];
    bool stage_compiled[2];
} CompiledEntryState;

static void free_compiled_entries(CompiledEntryState *entries, size_t count,
    A840SystemPackStageFreeFn free_fn, void *user_ctx)
{
    if (!entries)
        return;
    for (size_t i = 0u; i < count; ++i) {
        for (uint32_t s = 0u; s < entries[i].record.stage_count && s < 2u; ++s) {
            if (!entries[i].stage_compiled[s])
                continue;
            entries[i].record.stages[s].sds_state_words = NULL;
            entries[i].record.stages[s].sds_state_dwords = 0u;
            if (free_fn)
                free_fn(user_ctx, &entries[i].record.stages[s]);
            else
                a840_compile_stage_free(&entries[i].record.stages[s]);
        }
    }
    free(entries);
}

A840SystemPackStatus a840_system_pack_build_memory(
    const A840SystemPackBuildConfig *cfg,
    uint8_t **out_pack_buf,
    size_t *out_pack_bytes,
    A840SystemPackBuildReport *out_report)
{
    if (!cfg || !out_pack_buf || !out_pack_bytes || cfg->stack_key == 0u)
        return A840_SYSPACK_INVALID_ARG;
    if (cfg->tier > 1u)
        return A840_SYSPACK_INVALID_ARG;

    const uint64_t expected_abi = a840_compile_abi_fingerprint();
    const uint64_t abi_fp = cfg->abi_fingerprint ? cfg->abi_fingerprint : expected_abi;
    if (abi_fp != expected_abi)
        return A840_SYSPACK_INVALID_ARG;

    const uint32_t domain_mask = cfg->domain_mask ? (cfg->domain_mask & A840_SYSDOMAIN_ALL_MASK)
                                                  : A840_SYSDOMAIN_ALL_MASK;
    if (domain_mask == 0u)
        return A840_SYSPACK_INVALID_ARG;

    size_t selected_indices[A840_SYSTEM_PACK_MAX_CATALOG];
    size_t selected_count = 0u;
    for (size_t i = 0u; i < CATALOG_LEN; ++i) {
        uint32_t dbit = 1u << (uint32_t)k_catalog[i].pub.domain;
        if ((domain_mask & dbit) != 0u)
            selected_indices[selected_count++] = i;
    }
    if (selected_count == 0u)
        return A840_SYSPACK_INVALID_ARG;

    CompiledEntryState *compiled = (CompiledEntryState *)calloc(selected_count, sizeof(*compiled));
    if (!compiled)
        return A840_SYSPACK_OOM;

    A840CompileTarget target;
    memset(&target, 0, sizeof(target));
    target.abi_version = ADRENO_COMPILE_ABI_VERSION;
    target.chip_id = A840_PACK_V1_CHIP_ID;
    target.num_slices = 3u;
    target.num_ccu = 6u;
    target.highest_bank_bit = A840_PACK_V1_HBB;
    target.ubwc_swizzle = 6u;
    target.macrotile_mode = 1u;
    target.flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;

    A840SystemPackBuildReport rep;
    memset(&rep, 0, sizeof(rep));

    A840SystemPackStatus err_st = A840_SYSPACK_OK;

    for (size_t idx = 0u; idx < selected_count; ++idx) {
        const size_t cat_i = selected_indices[idx];
        const SystemShaderSpec *spec = &k_catalog[cat_i];
        CompiledEntryState *ce = &compiled[idx];

        uint32_t stage_count = spec->pub.is_render ? 2u : 1u;
        ce->record.abi_fingerprint = abi_fp;
        ce->record.stack_key = cfg->stack_key;
        ce->record.chip_id = A840_PACK_V1_CHIP_ID;
        ce->record.highest_bank_bit = A840_PACK_V1_HBB;
        ce->record.stage_count = stage_count;
        ce->record.binding_count = 0u;
        ce->record.flags = 0u;

        A840RecordKeyStage key_stages[2];
        memset(key_stages, 0, sizeof(key_stages));

        for (uint32_t s = 0u; s < stage_count; ++s) {
            uint32_t *spv_words = NULL;
            size_t spv_bytes = 0u;
            err_st = a840_system_pack_entry_spirv(cat_i, s, &spv_words, &spv_bytes);
            if (err_st != A840_SYSPACK_OK)
                goto fail;

            A840StageInput sinp;
            memset(&sinp, 0, sizeof(sinp));
            sinp.abi_version = ADRENO_COMPILE_ABI_VERSION;
            sinp.stage = spec->pub.is_render
                             ? (s == 0u ? (uint32_t)A840_STAGE_VERTEX
                                        : (uint32_t)A840_STAGE_FRAGMENT)
                             : (uint32_t)A840_STAGE_COMPUTE;
            sinp.spirv_words = spv_words;
            sinp.spirv_bytes = spv_bytes;
            sinp.entry_point = (s == 0u) ? spec->pub.entry_name : spec->pub.fs_entry_name;
            sinp.sample_count = spec->pub.sample_count ? spec->pub.sample_count : 1u;
            sinp.flags = spec->pub.compile_flags;
            if (sinp.stage == A840_STAGE_COMPUTE) {
                sinp.local_size[0] = spec->pub.local_size[0];
                sinp.local_size[1] = spec->pub.local_size[1];
                sinp.local_size[2] = spec->pub.local_size[2];
            } else if (sinp.stage == A840_STAGE_FRAGMENT) {
                sinp.color_rt_mask = spec->pub.color_rt_mask;
                sinp.half_rt_mask = spec->pub.half_rt_mask;
            }

            /* Evaluate W09 XR10 / robustness safety boundary before compiling. */
            A840RobustnessDecision rdec;
            memset(&rdec, 0, sizeof(rdec));
            uint32_t eff_flags = target.flags | sinp.flags;
            if (spec->pub.requires_xr10)
                eff_flags |= A840_COMPILE_FLAG_XR10_CLAMP;
            A840CompileStatus rpol = a840_robustness_decide_for_stage(
                &sinp, eff_flags, true, false,
                spec->pub.uses_fbfetch && sinp.stage == A840_STAGE_FRAGMENT, &rdec);
            if (rpol != A840_COMPILE_OK || (spec->pub.requires_xr10 && !rdec.preserve_xr_oob)) {
                free(spv_words);
                err_st = A840_SYSPACK_XR10_VIOLATION;
                goto fail;
            }

            key_stages[s].entry_point = sinp.entry_point;
            key_stages[s].stage = sinp.stage;
            canonical_spirv_sha256(spv_words, spv_bytes, key_stages[s].spirv_sha256);
            key_stages[s].color_rt_mask = sinp.color_rt_mask;
            key_stages[s].half_rt_mask = sinp.half_rt_mask;
            key_stages[s].sample_count = sinp.sample_count;
            key_stages[s].local_size[0] = sinp.local_size[0];
            key_stages[s].local_size[1] = sinp.local_size[1];
            key_stages[s].local_size[2] = sinp.local_size[2];
            key_stages[s].flags = sinp.flags;

            char err_buf[256];
            memset(err_buf, 0, sizeof(err_buf));
            A840CompileStatus cst = cfg->compile_stage_fn
                ? cfg->compile_stage_fn(cfg->compile_user_ctx, &target, &sinp,
                                        &ce->record.stages[s], err_buf, sizeof(err_buf))
                : a840_compile_stage(&target, &sinp, &ce->record.stages[s],
                                     err_buf, sizeof(err_buf));
            free(spv_words);

            if (cst != A840_COMPILE_OK) {
                err_st = A840_SYSPACK_COMPILE_ERROR;
                goto fail;
            }
            ce->stage_compiled[s] = true;

            /* Enforce probe006 instrlen*8 validity & zero unbound UBO block1. */
            const A840CompiledStage *cs = &ce->record.stages[s];
            if (cs->isa_dwords == 0u || cs->isa_words == NULL ||
                cs->instruction_groups == 0u ||
                (uint64_t)cs->instruction_groups * 4u > (uint64_t)cs->isa_dwords) {
                err_st = A840_SYSPACK_INSTRLEN_ERROR;
                goto fail;
            }
            ++rep.valid_instrlen_stages;

            if (stage_has_unbound_ubo_block1(cs)) {
                ++rep.unbound_ubo_block1_count;
                err_st = A840_SYSPACK_UBO_BLOCK1_ERROR;
                goto fail;
            }

            if (spec->pub.uses_fbfetch && sinp.stage == A840_STAGE_FRAGMENT)
                ++rep.fbfetch_stages;

            if (cfg->attach_sds_blocks) {
                encode_static_sds_block(cs, ce->sds_words[s]);
                ce->record.stages[s].sds_state_words = ce->sds_words[s];
                ce->record.stages[s].sds_state_dwords = 6u;
                ++rep.sds_attached_stages;
            }

            ++rep.total_stages;
        }

        if (a840_record_compute_key(key_stages, stage_count, &ce->record.pipeline_key) != A840_RECORD_OK) {
            err_st = A840_SYSPACK_PACK_ERROR;
            goto fail;
        }

        if (a840_binding_plan_build(NULL, 0u, ce->record.stages, stage_count, NULL,
                                    &ce->plan) != A840_BINDING_PLAN_OK) {
            err_st = A840_SYSPACK_PACK_ERROR;
            goto fail;
        }

        if (spec->pub.requires_xr10) {
            ++rep.xr10_checked_entries;
            ++rep.xr10_safe_entries;
        }
        ++rep.domain_counts[spec->pub.domain];
        ++rep.total_entries;
        if (rep.entry_key_count < A840_SYSTEM_PACK_MAX_CATALOG)
            rep.entry_keys[rep.entry_key_count++] = ce->record.pipeline_key;
    }

    /* Assemble deterministic A840PK01 pack.bin via a840_pack_v1_builder_*. */
    uint32_t bucket_cap = 16u;
    while (bucket_cap < (uint32_t)selected_count * 4u && bucket_cap < A840_PACK_V1_MAX_BUCKETS)
        bucket_cap *= 2u;

    A840PackV1BuilderItem *items = (A840PackV1BuilderItem *)calloc(selected_count, sizeof(*items));
    uint32_t *buckets = (uint32_t *)malloc((size_t)bucket_cap * sizeof(uint32_t));
    if (!items || !buckets) {
        free(items);
        free(buckets);
        err_st = A840_SYSPACK_OOM;
        goto fail;
    }

    A840PackV1Builder builder;
    memset(&builder, 0, sizeof(builder));
    A840PackV1Status pst = a840_pack_v1_builder_init(&builder, cfg->tier, cfg->stack_key,
        abi_fp, items, selected_count, buckets, bucket_cap);
    for (size_t i = 0u; pst == A840_PACK_OK && i < selected_count; ++i)
        pst = a840_pack_v1_builder_add(&builder, &compiled[i].record, &compiled[i].plan);

    size_t pack_bytes = 0u;
    if (pst == A840_PACK_OK)
        pst = a840_pack_v1_builder_measure(&builder, &pack_bytes);

    void *aligned_buf = NULL;
    if (pst == A840_PACK_OK) {
        if (posix_memalign(&aligned_buf, A840_PACK_V1_ISA_ALIGNMENT, pack_bytes) != 0) {
            free(items);
            free(buckets);
            err_st = A840_SYSPACK_OOM;
            goto fail;
        }
        memset(aligned_buf, 0, pack_bytes);
        pst = a840_pack_v1_builder_build(&builder, aligned_buf, pack_bytes, &pack_bytes);
    }

    free(items);
    free(buckets);

    if (pst != A840_PACK_OK) {
        free(aligned_buf);
        err_st = A840_SYSPACK_PACK_ERROR;
        goto fail;
    }

    if (a840_pack_v1_sha256(aligned_buf, pack_bytes, rep.pack_sha256) != A840_PACK_OK) {
        free(aligned_buf);
        err_st = A840_SYSPACK_VERIFY_ERROR;
        goto fail;
    }
    rep.pack_bytes = pack_bytes;

    if (cfg->verify_after_build) {
        A840PackV1View view;
        memset(&view, 0, sizeof(view));
        if (a840_pack_v1_open_memory(aligned_buf, pack_bytes, cfg->stack_key,
                                     abi_fp, &view) != A840_PACK_OK) {
            free(aligned_buf);
            err_st = A840_SYSPACK_VERIFY_ERROR;
            goto fail;
        }
        bool isa_aligned = false, sds_ok = false, instrlen_ok = false;
        uint32_t ubo1 = 0u;
        if (!verify_view_and_bench(&view, cfg->bench_iters, &isa_aligned, &sds_ok,
                                   &instrlen_ok, &ubo1, &rep.lookup_mean_us,
                                   &rep.lookup_p50_us, &rep.lookup_p99_us) ||
            !isa_aligned || !instrlen_ok || ubo1 != 0u ||
            (cfg->attach_sds_blocks && !sds_ok)) {
            free(aligned_buf);
            err_st = A840_SYSPACK_VERIFY_ERROR;
            goto fail;
        }
    }

    free_compiled_entries(compiled, selected_count, cfg->free_stage_fn, cfg->compile_user_ctx);
    *out_pack_buf = (uint8_t *)aligned_buf;
    *out_pack_bytes = pack_bytes;
    if (out_report)
        *out_report = rep;
    return A840_SYSPACK_OK;

fail:
    free_compiled_entries(compiled, selected_count, cfg->free_stage_fn, cfg->compile_user_ctx);
    return err_st;
}

A840SystemPackStatus a840_system_pack_build_to_file(
    const A840SystemPackBuildConfig *cfg,
    const char *output_path,
    A840SystemPackBuildReport *out_report)
{
    if (!cfg || !output_path || !output_path[0])
        return A840_SYSPACK_INVALID_ARG;
    if (path_is_forbidden(output_path))
        return A840_SYSPACK_PATH_FORBIDDEN;

    uint8_t *buf = NULL;
    size_t bytes = 0u;
    A840SystemPackBuildReport rep;
    memset(&rep, 0, sizeof(rep));

    A840SystemPackStatus st = a840_system_pack_build_memory(cfg, &buf, &bytes, &rep);
    if (st != A840_SYSPACK_OK)
        return st;

    const uint64_t abi_fp = cfg->abi_fingerprint ? cfg->abi_fingerprint
                                                 : a840_compile_abi_fingerprint();
    A840PackV1Status save_st = a840_pack_v1_save_file(output_path, buf, bytes,
                                                      cfg->stack_key, abi_fp);
    free(buf);

    if (save_st != A840_PACK_OK)
        return (save_st == A840_PACK_INVALID_ARG) ? A840_SYSPACK_PATH_FORBIDDEN
                                                  : A840_SYSPACK_IO_ERROR;

    if (cfg->verify_after_build) {
        A840SystemPackVerifyReport vrep;
        memset(&vrep, 0, sizeof(vrep));
        st = a840_system_pack_verify_file_mmap(output_path, cfg->stack_key, abi_fp,
                                               cfg->bench_iters, &vrep);
        if (st != A840_SYSPACK_OK)
            return st;
        if (memcmp(vrep.pack_sha256, rep.pack_sha256, 32u) != 0)
            return A840_SYSPACK_VERIFY_ERROR;
        rep.lookup_mean_us = vrep.lookup_mean_us;
        rep.lookup_p50_us = vrep.lookup_p50_us;
        rep.lookup_p99_us = vrep.lookup_p99_us;
    }

    if (out_report)
        *out_report = rep;
    return A840_SYSPACK_OK;
}

A840SystemPackStatus a840_system_pack_verify_file_mmap(
    const char *pack_path,
    uint64_t stack_key,
    uint64_t abi_fingerprint,
    uint32_t bench_iters,
    A840SystemPackVerifyReport *out_report)
{
    if (!pack_path || !pack_path[0] || stack_key == 0u || !out_report)
        return A840_SYSPACK_INVALID_ARG;
    if (path_is_forbidden(pack_path))
        return A840_SYSPACK_PATH_FORBIDDEN;

    const uint64_t abi_fp = abi_fingerprint ? abi_fingerprint
                                            : a840_compile_abi_fingerprint();
    int fd = open(pack_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return A840_SYSPACK_IO_ERROR;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)A840_PACK_V1_HEADER_BYTES) {
        (void)close(fd);
        return A840_SYSPACK_VERIFY_ERROR;
    }

    size_t map_bytes = (size_t)st.st_size;
    void *mapped = mmap(NULL, map_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    (void)close(fd);
    if (mapped == MAP_FAILED)
        return A840_SYSPACK_IO_ERROR;

    A840PackV1View view;
    memset(&view, 0, sizeof(view));
    if (a840_pack_v1_open_memory(mapped, map_bytes, stack_key, abi_fp, &view) != A840_PACK_OK) {
        (void)munmap(mapped, map_bytes);
        return A840_SYSPACK_VERIFY_ERROR;
    }

    A840SystemPackVerifyReport rep;
    memset(&rep, 0, sizeof(rep));
    rep.entry_count = view.entry_count;
    rep.tier = view.tier;
    rep.pack_bytes = map_bytes;

    if (a840_pack_v1_sha256(mapped, map_bytes, rep.pack_sha256) != A840_PACK_OK ||
        !verify_view_and_bench(&view, bench_iters, &rep.all_isa_128b_aligned,
                               &rep.all_sds_present, &rep.all_instrlen_valid,
                               &rep.unbound_ubo_block1_count, &rep.lookup_mean_us,
                               &rep.lookup_p50_us, &rep.lookup_p99_us)) {
        (void)munmap(mapped, map_bytes);
        return A840_SYSPACK_VERIFY_ERROR;
    }

    (void)munmap(mapped, map_bytes);
    *out_report = rep;
    return A840_SYSPACK_OK;
}
