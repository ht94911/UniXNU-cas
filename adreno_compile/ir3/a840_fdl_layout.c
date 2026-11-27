/* SPDX-License-Identifier: MIT */
#ifdef A840_BUILD_MESA_IR3_BACKEND

#include "AdrenoCompileABI.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/fd6_hw.h"
#include "common/freedreno_dev_info.h"
#include "fdl/fd6_format_table.h"
#include "fdl/freedreno_layout.h"
#include "util/format/u_format.h"
#include "util/u_math.h"

#define A840_KAANAPALI_CHIP_ID_U64 UINT64_C(0x44050a31)
#define A840_DEFAULT_HBB 16u
#define A840_DEFAULT_UBWC_SWIZZLE 6u
#define A840_DEFAULT_MACROTILE_MODE 1u

enum {
   A840_TEX_FLAG_IS_3D       = 1u << 0,
   A840_TEX_FLAG_IS_CUBE     = 1u << 1,
   A840_TEX_FLAG_FOR_SCANOUT = 1u << 2,
   A840_TEX_FLAG_PLANE_SHIFT = 8u,
   A840_TEX_FLAG_PLANE_MASK  = 0xffu << 8,
   A840_TEX_FLAG_KNOWN_MASK  = A840_TEX_FLAG_IS_3D |
                               A840_TEX_FLAG_IS_CUBE |
                               A840_TEX_FLAG_FOR_SCANOUT |
                               A840_TEX_FLAG_PLANE_MASK,
   A840_MAX_IMAGE_DIM        = 16384u,
   A840_MAX_IMAGE_DEPTH      = 2048u,
   A840_MAX_IMAGE_LAYERS     = 2048u,
};

enum {
   A840_MTL_FMT_YCBCR_NV12_VIDEO = 500u,
   A840_MTL_FMT_YCBCR_NV12_FULL  = 505u,
   A840_MTL_FMT_YCBCR_P010_VIDEO = 508u,
   A840_MTL_FMT_YCBCR_P010_FULL  = 509u,
   A840_MTL_FMT_BGRA10_XR        = 552u,
   A840_MTL_FMT_BGRA10_XR_SRGB   = 553u,
   A840_MTL_FMT_BGR10_XR         = 554u,
   A840_MTL_FMT_BGR10_XR_SRGB    = 555u,
   A840_CV_FMT_420V              = 0x34323076u, /* '420v' NV12 video range */
   A840_CV_FMT_420F              = 0x34323066u, /* '420f' NV12 full range */
   A840_CV_FMT_X420              = 0x78343230u, /* 'x420' P010 video range */
   A840_CV_FMT_XF20              = 0x78663230u, /* 'xf20' P010 full range */
};

__attribute__((visibility("hidden")))
A840CompileStatus a840_ir3_init_dev_info(const A840CompileTarget *target,
                                         struct fd_dev_id *out_dev_id,
                                         struct fd_dev_info *out_info,
                                         char *err_buf,
                                         size_t err_buf_bytes);

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

__attribute__((visibility("hidden")))
A840CompileStatus
a840_ir3_init_dev_info(const A840CompileTarget *target,
                       struct fd_dev_id *out_dev_id,
                       struct fd_dev_info *out_info,
                       char *err_buf,
                       size_t err_buf_bytes)
{
   if (target == NULL) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "target is required");
   }
   if (target->abi_version != ADRENO_COMPILE_ABI_VERSION) {
      return report_err(A840_COMPILE_ABI_MISMATCH, err_buf, err_buf_bytes,
                        "target ABI version mismatch");
   }
   if (target->chip_id != A840_KAANAPALI_CHIP_ID_U64) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "chip_id must identify the Adreno 840 target");
   }

   uint32_t hbb = target->highest_bank_bit != 0u
                     ? target->highest_bank_bit
                     : A840_DEFAULT_HBB;
   if (hbb != 15u && hbb != 16u) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "highest_bank_bit must be 15 or 16");
   }

   uint32_t ubwc_swizzle = target->ubwc_swizzle != 0u
                              ? target->ubwc_swizzle
                              : A840_DEFAULT_UBWC_SWIZZLE;
   if (ubwc_swizzle > 7u) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "ubwc_swizzle out of range");
   }

   if (target->macrotile_mode > 1u) {
      return report_err(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                        "macrotile_mode must be 0 or 1");
   }

   struct fd_dev_id dev_id = {
      .gpu_id = 0,
      .chip_id = target->chip_id,
   };
   const struct fd_dev_info *raw = fd_dev_info_raw(&dev_id);
   if (raw == NULL || raw->chip != 8) {
      return report_err(A840_COMPILE_IR3_ERROR, err_buf, err_buf_bytes,
                        "Mesa fd_dev_info_raw failed to resolve Adreno 840 (Gen8)");
   }

   struct fd_dev_info info = *raw;
   /* Explicitly override Mesa static catalog HBB (15) with Kaanapali target HBB (16). */
   info.highest_bank_bit = hbb;
   info.ubwc_swizzle = ubwc_swizzle;
   info.macrotile_mode = target->macrotile_mode;
   if (target->num_slices != 0u)
      info.num_slices = target->num_slices;
   if (target->num_ccu != 0u)
      info.num_ccu = target->num_ccu;

   *out_dev_id = dev_id;
   *out_info = info;
   return A840_COMPILE_OK;
}

static enum pipe_format
mtl_pixel_format_to_pipe(uint32_t mtl_format)
{
   switch (mtl_format) {
   /* 8-bit single channel */
   case 1u:   return PIPE_FORMAT_A8_UNORM;           /* A8Unorm */
   case 10u:  return PIPE_FORMAT_R8_UNORM;           /* R8Unorm */
   case 11u:  return PIPE_FORMAT_R8_SRGB;            /* R8Unorm_sRGB */
   case 12u:  return PIPE_FORMAT_R8_SNORM;           /* R8Snorm */
   case 13u:  return PIPE_FORMAT_R8_UINT;            /* R8Uint */
   case 14u:  return PIPE_FORMAT_R8_SINT;            /* R8Sint */

   /* 16-bit single / dual / packed */
   case 20u:  return PIPE_FORMAT_R16_UNORM;          /* R16Unorm */
   case 22u:  return PIPE_FORMAT_R16_SNORM;          /* R16Snorm */
   case 23u:  return PIPE_FORMAT_R16_UINT;           /* R16Uint */
   case 24u:  return PIPE_FORMAT_R16_SINT;           /* R16Sint */
   case 25u:  return PIPE_FORMAT_R16_FLOAT;          /* R16Float */
   case 30u:  return PIPE_FORMAT_R8G8_UNORM;         /* RG8Unorm */
   case 31u:  return PIPE_FORMAT_R8G8_SRGB;          /* RG8Unorm_sRGB */
   case 32u:  return PIPE_FORMAT_R8G8_SNORM;         /* RG8Snorm */
   case 33u:  return PIPE_FORMAT_R8G8_UINT;          /* RG8Uint */
   case 34u:  return PIPE_FORMAT_R8G8_SINT;          /* RG8Sint */
   case 40u:  return PIPE_FORMAT_B5G6R5_UNORM;       /* B5G6R5Unorm */
   case 41u:  return PIPE_FORMAT_A1B5G5R5_UNORM;     /* A1BGR5Unorm */
   case 42u:  return PIPE_FORMAT_A4B4G4R4_UNORM;     /* ABGR4Unorm */
   case 43u:  return PIPE_FORMAT_B5G5R5A1_UNORM;     /* BGR5A1Unorm */

   /* 32-bit */
   case 53u:  return PIPE_FORMAT_R32_UINT;           /* R32Uint */
   case 54u:  return PIPE_FORMAT_R32_SINT;           /* R32Sint */
   case 55u:  return PIPE_FORMAT_R32_FLOAT;          /* R32Float */
   case 60u:  return PIPE_FORMAT_R16G16_UNORM;       /* RG16Unorm */
   case 62u:  return PIPE_FORMAT_R16G16_SNORM;       /* RG16Snorm */
   case 63u:  return PIPE_FORMAT_R16G16_UINT;        /* RG16Uint */
   case 64u:  return PIPE_FORMAT_R16G16_SINT;        /* RG16Sint */
   case 65u:  return PIPE_FORMAT_R16G16_FLOAT;       /* RG16Float */
   case 70u:  return PIPE_FORMAT_R8G8B8A8_UNORM;     /* RGBA8Unorm */
   case 71u:  return PIPE_FORMAT_R8G8B8A8_SRGB;      /* RGBA8Unorm_sRGB */
   case 72u:  return PIPE_FORMAT_R8G8B8A8_SNORM;     /* RGBA8Snorm */
   case 73u:  return PIPE_FORMAT_R8G8B8A8_UINT;      /* RGBA8Uint */
   case 74u:  return PIPE_FORMAT_R8G8B8A8_SINT;      /* RGBA8Sint */
   case 80u:  return PIPE_FORMAT_B8G8R8A8_UNORM;     /* BGRA8Unorm */
   case 81u:  return PIPE_FORMAT_B8G8R8A8_SRGB;      /* BGRA8Unorm_sRGB */
   case 90u:  return PIPE_FORMAT_R10G10B10A2_UNORM;  /* RGB10A2Unorm */
   case 91u:  return PIPE_FORMAT_R10G10B10A2_UINT;   /* RGB10A2Uint */
   case 92u:  return PIPE_FORMAT_R11G11B10_FLOAT;    /* RG11B10Float */
   case 93u:  return PIPE_FORMAT_R9G9B9E5_FLOAT;     /* RGB9E5Float */
   case 94u:  return PIPE_FORMAT_B10G10R10A2_UNORM;  /* BGR10A2Unorm */

   /* 64-bit */
   case 103u: return PIPE_FORMAT_R32G32_UINT;        /* RG32Uint */
   case 104u: return PIPE_FORMAT_R32G32_SINT;        /* RG32Sint */
   case 105u: return PIPE_FORMAT_R32G32_FLOAT;       /* RG32Float */
   case 110u: return PIPE_FORMAT_R16G16B16A16_UNORM; /* RGBA16Unorm */
   case 112u: return PIPE_FORMAT_R16G16B16A16_SNORM; /* RGBA16Snorm */
   case 113u: return PIPE_FORMAT_R16G16B16A16_UINT;  /* RGBA16Uint */
   case 114u: return PIPE_FORMAT_R16G16B16A16_SINT;  /* RGBA16Sint */
   case 115u: return PIPE_FORMAT_R16G16B16A16_FLOAT; /* RGBA16Float */

   /* 128-bit */
   case 123u: return PIPE_FORMAT_R32G32B32A32_UINT;  /* RGBA32Uint */
   case 124u: return PIPE_FORMAT_R32G32B32A32_SINT;  /* RGBA32Sint */
   case 125u: return PIPE_FORMAT_R32G32B32A32_FLOAT; /* RGBA32Float */

   /* Compressed ETC2 / ASTC */
   case 180u: return PIPE_FORMAT_ETC2_RGB8;          /* ETC2_RGB8 */
   case 181u: return PIPE_FORMAT_ETC2_SRGB8;         /* ETC2_RGB8_sRGB */
   case 182u: return PIPE_FORMAT_ETC2_RGB8A1;        /* ETC2_RGB8A1 */
   case 183u: return PIPE_FORMAT_ETC2_SRGB8A1;       /* ETC2_RGB8A1_sRGB */
   case 186u: return PIPE_FORMAT_ASTC_4x4_SRGB;      /* ASTC_4x4_sRGB */
   case 188u: return PIPE_FORMAT_ASTC_5x5_SRGB;      /* ASTC_5x5_sRGB */
   case 190u: return PIPE_FORMAT_ASTC_6x6_SRGB;      /* ASTC_6x6_sRGB */
   case 194u: return PIPE_FORMAT_ASTC_8x8_SRGB;      /* ASTC_8x8_sRGB */
   case 204u: return PIPE_FORMAT_ASTC_4x4;           /* ASTC_4x4_LDR */
   case 206u: return PIPE_FORMAT_ASTC_5x5;           /* ASTC_5x5_LDR */
   case 208u: return PIPE_FORMAT_ASTC_6x6;           /* ASTC_6x6_LDR */
   case 212u: return PIPE_FORMAT_ASTC_8x8;           /* ASTC_8x8_LDR */
   case 222u: return PIPE_FORMAT_ASTC_4x4_FLOAT;     /* ASTC_4x4_HDR */

   /* Depth / Stencil */
   case 250u: return PIPE_FORMAT_Z16_UNORM;          /* Depth16Unorm */
   case 252u: return PIPE_FORMAT_Z32_FLOAT;          /* Depth32Float */
   case 253u: return PIPE_FORMAT_S8_UINT;            /* Stencil8 */
   case 255u: return PIPE_FORMAT_Z24_UNORM_S8_UINT;  /* Depth24Unorm_Stencil8 */
   case 260u: return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT; /* Depth32Float_Stencil8 */

   /* Extended-Range (Apple XR10 backing storage, matching Adreno840Caps) */
   case A840_MTL_FMT_BGRA10_XR:      return PIPE_FORMAT_R16G16B16A16_UNORM;
   case A840_MTL_FMT_BGRA10_XR_SRGB: return PIPE_FORMAT_R16G16B16A16_UNORM;
   case A840_MTL_FMT_BGR10_XR:       return PIPE_FORMAT_B10G10R10A2_UNORM;
   case A840_MTL_FMT_BGR10_XR_SRGB:  return PIPE_FORMAT_B10G10R10A2_UNORM;

   default:
      return PIPE_FORMAT_NONE;
   }
}

static bool
is_xr10_mtl_format(uint32_t mtl_format)
{
   return mtl_format == A840_MTL_FMT_BGRA10_XR ||
          mtl_format == A840_MTL_FMT_BGRA10_XR_SRGB ||
          mtl_format == A840_MTL_FMT_BGR10_XR ||
          mtl_format == A840_MTL_FMT_BGR10_XR_SRGB;
}

static bool
is_ycbcr_nv12_format(uint32_t mtl_format)
{
   return mtl_format == A840_MTL_FMT_YCBCR_NV12_VIDEO ||
          mtl_format == A840_MTL_FMT_YCBCR_NV12_FULL ||
          mtl_format == A840_CV_FMT_420V ||
          mtl_format == A840_CV_FMT_420F;
}

static bool
is_ycbcr_p010_format(uint32_t mtl_format)
{
   return mtl_format == A840_MTL_FMT_YCBCR_P010_VIDEO ||
          mtl_format == A840_MTL_FMT_YCBCR_P010_FULL ||
          mtl_format == A840_CV_FMT_X420 ||
          mtl_format == A840_CV_FMT_XF20;
}

static A840CompileStatus
resolve_ycbcr_plane(uint32_t mtl_format,
                    uint32_t plane,
                    enum pipe_format *out_pfmt,
                    uint32_t *out_div_x,
                    uint32_t *out_div_y)
{
   if (plane > 1u)
      return A840_COMPILE_INVALID_ARG;

   if (is_ycbcr_nv12_format(mtl_format)) {
      if (plane == 0u) {
         *out_pfmt = PIPE_FORMAT_R8_UNORM;
         *out_div_x = 1u;
         *out_div_y = 1u;
      } else {
         *out_pfmt = PIPE_FORMAT_R8G8_UNORM;
         *out_div_x = 2u;
         *out_div_y = 2u;
      }
      return A840_COMPILE_OK;
   }

   if (is_ycbcr_p010_format(mtl_format)) {
      if (plane == 0u) {
         *out_pfmt = PIPE_FORMAT_R16_UNORM;
         *out_div_x = 1u;
         *out_div_y = 1u;
      } else {
         *out_pfmt = PIPE_FORMAT_R16G16_UNORM;
         *out_div_x = 2u;
         *out_div_y = 2u;
      }
      return A840_COMPILE_OK;
   }

   return A840_COMPILE_UNSUPPORTED_FEATURE;
}

static enum a6xx_tex_type
a840_fdl_tex_type(const A840TextureLayoutQuery *query)
{
   if (query->flags & 1u)
      return A6XX_TEX_3D;
   if (query->flags & 2u)
      return A6XX_TEX_CUBE;
   return A6XX_TEX_2D;
}

static void
a840_fill_tex_const_template(const struct fd_dev_info *info,
                             const struct fdl_layout *layout,
                             const A840TextureLayoutQuery *query,
                             uint32_t plane_width,
                             uint32_t plane_height,
                             uint32_t descriptor[16])
{
   memset(descriptor, 0, sizeof(uint32_t) * 16u);

   enum pipe_format fmt = layout->format;
   enum a6xx_tile_mode tm = (enum a6xx_tile_mode)fdl_tile_mode(layout, 0);
   enum a6xx_format tex_fmt = fd6_texture_format(fmt, tm, layout->is_mutable);
   enum a3xx_color_swap swap = fd6_texture_swap(fmt, tm, layout->is_mutable);
   bool ubwc_on = fdl_ubwc_enabled(layout, 0);

   unsigned char swiz[4] = {
      PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W
   };
   fdl6_format_swiz(fmt, info->props.has_z24uint_s8uint, swiz);

   uint64_t base_offset = fdl_surface_offset(layout, 0, 0);
   uint64_t ubwc_offset = ubwc_on ? fdl_ubwc_offset(layout, 0, 0) : 0u;
   uint32_t pitch = fdl_pitch(layout, 0);
   uint32_t layer_size = fdl_layer_stride(layout, 0);
   uint32_t depth = (query->flags & 1u) ? query->depth : query->array_layers;

   /* Template encodes relative byte offsets (base_iova == 0); runtime ORs base IOVA. */
   descriptor[0] = A8XX_TEX_MEMOBJ_0_BASE_LO((uint32_t)base_offset);
   descriptor[1] = A8XX_TEX_MEMOBJ_1_BASE_HI((uint32_t)(base_offset >> 32)) |
                   A8XX_TEX_MEMOBJ_1_TYPE(a840_fdl_tex_type(query)) |
                   A8XX_TEX_MEMOBJ_1_DEPTH(depth);
   descriptor[2] = A8XX_TEX_MEMOBJ_2_WIDTH(plane_width) |
                   A8XX_TEX_MEMOBJ_2_HEIGHT(plane_height) |
                   A8XX_TEX_MEMOBJ_2_SAMPLES(
                      (enum a3xx_msaa_samples)util_logbase2(layout->nr_samples));
   descriptor[3] = A8XX_TEX_MEMOBJ_3_FMT(tex_fmt) |
                   A8XX_TEX_MEMOBJ_3_SWAP(swap) |
                   A8XX_TEX_MEMOBJ_3_SWIZ_X(fdl8_swiz(swiz[0])) |
                   A8XX_TEX_MEMOBJ_3_SWIZ_Y(fdl8_swiz(swiz[1])) |
                   A8XX_TEX_MEMOBJ_3_SWIZ_Z(fdl8_swiz(swiz[2])) |
                   A8XX_TEX_MEMOBJ_3_SWIZ_W(fdl8_swiz(swiz[3]));
   descriptor[4] = A8XX_TEX_MEMOBJ_4_TILE_MODE(tm) |
                   (layout->tile_all ? A8XX_TEX_MEMOBJ_4_TILE_ALL : 0u) |
                   (util_format_is_srgb(fmt) ? A8XX_TEX_MEMOBJ_4_SRGB : 0u);
   descriptor[5] = 0u;
   descriptor[6] = A8XX_TEX_MEMOBJ_6_TEX_LINE_OFFSET(pitch * 8u) |
                   A8XX_TEX_MEMOBJ_6_MIN_LINE_OFFSET(layout->pitchalign - 6u) |
                   A8XX_TEX_MEMOBJ_6_MIPLVLS(query->mip_levels - 1u);
   if ((layer_size & 0xfffu) == 0u)
      descriptor[7] = A8XX_TEX_MEMOBJ_7_ARRAY_SLICE_OFFSET(layer_size);
   if (ubwc_on) {
      uint32_t bw = 0u, bh = 0u;
      fdl6_get_ubwc_blockwidth(layout, &bw, &bh);
      uint32_t ubwc_pitch = fdl_ubwc_pitch(layout, 0);
      descriptor[4] |= A8XX_TEX_MEMOBJ_4_FLAG |
                       A8XX_TEX_MEMOBJ_4_FLAG_LO((uint32_t)ubwc_offset);
      descriptor[5] |= A8XX_TEX_MEMOBJ_5_FLAG_HI((uint32_t)(ubwc_offset >> 32)) |
                       A8XX_TEX_MEMOBJ_5_FLAG_BUFFER_PITCH(ubwc_pitch);
      if (bw > 0u && bh > 0u && (layout->ubwc_layer_size & 0xfffu) == 0u) {
         descriptor[8] = A8XX_TEX_MEMOBJ_8_FLAG_ARRAY_PITCH((uint32_t)layout->ubwc_layer_size) |
                         A8XX_TEX_MEMOBJ_8_FLAG_BUFFER_LOGW(
                            util_logbase2_ceil(DIV_ROUND_UP(plane_width, bw))) |
                         A8XX_TEX_MEMOBJ_8_FLAG_BUFFER_LOGH(
                            util_logbase2_ceil(DIV_ROUND_UP(plane_height, bh)));
      }
   }
}

A840CompileStatus
a840_compute_texture_layout(const A840CompileTarget *target,
                            const A840TextureLayoutQuery *query,
                            A840TextureLayoutResult *out_layout)
{
   if (query == NULL || out_layout == NULL)
      return A840_COMPILE_INVALID_ARG;

   struct fd_dev_id dev_id;
   struct fd_dev_info dev_info;
   A840CompileStatus status = a840_ir3_init_dev_info(target, &dev_id, &dev_info, NULL, 0);
   if (status != A840_COMPILE_OK)
      return status;

   if (query->abi_version != ADRENO_COMPILE_ABI_VERSION)
      return A840_COMPILE_ABI_MISMATCH;
   if (query->mtl_pixel_format == 0u || query->width == 0u ||
       query->height == 0u || query->depth == 0u ||
       query->array_layers == 0u || query->mip_levels == 0u ||
       query->width > A840_MAX_IMAGE_DIM ||
       query->height > A840_MAX_IMAGE_DIM ||
       query->depth > A840_MAX_IMAGE_DEPTH ||
       query->array_layers > A840_MAX_IMAGE_LAYERS ||
       query->mip_levels > FDL_MAX_MIP_LEVELS ||
       !valid_sample_count(query->sample_count) || query->tile_mode > 2u ||
       (query->flags & ~A840_TEX_FLAG_KNOWN_MASK) != 0u) {
      return A840_COMPILE_INVALID_ARG;
   }

   const bool is_3d = (query->flags & A840_TEX_FLAG_IS_3D) != 0u;
   const bool is_cube = (query->flags & A840_TEX_FLAG_IS_CUBE) != 0u;
   if (is_3d && is_cube)
      return A840_COMPILE_INVALID_ARG;

   const uint32_t plane =
      (query->flags & A840_TEX_FLAG_PLANE_MASK) >> A840_TEX_FLAG_PLANE_SHIFT;
   const bool is_ycbcr = is_ycbcr_nv12_format(query->mtl_pixel_format) ||
                         is_ycbcr_p010_format(query->mtl_pixel_format);
   const bool is_xr10 = is_xr10_mtl_format(query->mtl_pixel_format);

   enum pipe_format pfmt = PIPE_FORMAT_NONE;
   uint32_t plane_width = query->width;
   uint32_t plane_height = query->height;

   if (is_ycbcr) {
      if (dev_info.highest_bank_bit != A840_DEFAULT_HBB)
         return A840_COMPILE_INVALID_ARG;
      if (query->tile_mode == 2u &&
          (dev_info.ubwc_swizzle != A840_DEFAULT_UBWC_SWIZZLE ||
           dev_info.macrotile_mode != A840_DEFAULT_MACROTILE_MODE)) {
         return A840_COMPILE_INVALID_ARG;
      }
      if (is_3d || is_cube || query->depth != 1u ||
          query->sample_count != 1u || query->mip_levels != 1u ||
          query->width < 2u || query->height < 2u ||
          (query->width & 1u) != 0u || (query->height & 1u) != 0u) {
         return A840_COMPILE_INVALID_ARG;
      }
      uint32_t div_x = 1u, div_y = 1u;
      A840CompileStatus pst = resolve_ycbcr_plane(
         query->mtl_pixel_format, plane, &pfmt, &div_x, &div_y);
      if (pst != A840_COMPILE_OK)
         return pst;
      plane_width = query->width / div_x;
      plane_height = query->height / div_y;
   } else {
      if (plane != 0u)
         return A840_COMPILE_INVALID_ARG;
      if (is_xr10) {
         if (dev_info.highest_bank_bit != A840_DEFAULT_HBB)
            return A840_COMPILE_INVALID_ARG;
         if (query->tile_mode == 2u &&
             (dev_info.ubwc_swizzle != A840_DEFAULT_UBWC_SWIZZLE ||
              dev_info.macrotile_mode != A840_DEFAULT_MACROTILE_MODE)) {
            return A840_COMPILE_INVALID_ARG;
         }
      }
      pfmt = mtl_pixel_format_to_pipe(query->mtl_pixel_format);
      if (pfmt == PIPE_FORMAT_NONE)
         return A840_COMPILE_UNSUPPORTED_FEATURE;
   }

   enum a6xx_tile_mode req_tile_mode =
      (query->tile_mode == 0u) ? TILE6_LINEAR : TILE6_3;
   bool req_ubwc = (query->tile_mode == 2u);

   /* Depth/stencil surfaces are always tiled on Adreno 6xx/7xx/8xx. */
   if (util_format_is_depth_or_stencil(pfmt) && req_tile_mode == TILE6_LINEAR)
      req_tile_mode = TILE6_3;

   /* Compressed formats (ASTC/ETC2) cannot use UBWC. */
   if (util_format_is_compressed(pfmt))
      req_ubwc = false;

   /* 128-bit and depth+stencil combined formats check: let fdl6_layout_image
    * handle hardware rules, except block UBWC on unsupported packed formats. */
   if (pfmt == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT ||
       pfmt == PIPE_FORMAT_S8_UINT ||
       pfmt == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      req_ubwc = false;
   }

   struct fdl_image_params params;
   memset(&params, 0, sizeof(params));
   params.format = pfmt;
   params.nr_samples = query->sample_count;
   params.width0 = plane_width;
   params.height0 = plane_height;
   params.depth0 = query->depth;
   params.mip_levels = query->mip_levels;
   params.array_size = query->array_layers;
   params.tile_mode = (uint32_t)req_tile_mode;
   params.ubwc = req_ubwc;
   params.is_3d = is_3d;
   params.is_mutable = false;
   params.plane = plane;

   struct fdl_layout layout;
   if (!fdl6_layout_image(&layout, &dev_info, &params, NULL))
      return A840_COMPILE_UNSUPPORTED_FEATURE;

   /* Validate UBWC bank configuration with the overridden Kaanapali HBB. */
   if (fdl_ubwc_enabled(&layout, 0)) {
      struct fdl_ubwc_config ubwc_cfg = {
         .highest_bank_bit = dev_info.highest_bank_bit,
         .bank_swizzle_levels = dev_info.ubwc_swizzle,
         .macrotile_mode = dev_info.macrotile_mode
                              ? FDL_MACROTILE_8_CHANNEL
                              : FDL_MACROTILE_4_CHANNEL,
      };
      uint32_t bank_shift = fdl6_get_bank_shift(&ubwc_cfg);
      if (bank_shift != dev_info.highest_bank_bit - 1u)
         return A840_COMPILE_IR3_ERROR;
   }

   A840TextureLayoutResult result;
   memset(&result, 0, sizeof(result));
   result.total_bytes = layout.size;
   result.base_align = layout.base_align;
   result.ubwc_enabled = fdl_ubwc_enabled(&layout, 0) ? 1u : 0u;
   result.pitch0_bytes = fdl_pitch(&layout, 0);
   result.slice_stride_bytes = fdl_layer_stride(&layout, 0);
   result.ubwc_meta_offset = layout.ubwc ? (uint64_t)fdl_ubwc_offset(&layout, 0, 0) : 0u;
   result.ubwc_meta_pitch = fdl_ubwc_pitch(&layout, 0);

   a840_fill_tex_const_template(&dev_info, &layout, query,
                                plane_width, plane_height,
                                result.tex_const_template);

   *out_layout = result;
   return A840_COMPILE_OK;
}

#else
typedef int a840_fdl_layout_stub_guard_t;
#endif
