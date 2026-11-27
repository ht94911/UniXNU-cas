/* SPDX-License-Identifier: MIT */
#ifndef A840_BITFIELDS_H
#define A840_BITFIELDS_H

#include <stdint.h>

/* A8XX payload and descriptor fields at pinned Mesa 85fda3b36405. */
#define A840_A840_REGS_SUBSET_H

#define A840_EVENT_CACHE_FLUSH_TS UINT32_C(0x04) /* adreno_pm4.xml:17 */
#define A840_EVENT_RB_DONE_TS UINT32_C(0x16) /* adreno_pm4.xml:45 */
#define A840_EVENT_CCU_FLUSH_COLOR UINT32_C(0x1d) /* adreno_pm4.xml:146 */
#define A840_EVENT_CCU_FLUSH_DEPTH UINT32_C(0x1c) /* adreno_pm4.xml:145 */
#define A840_EVENT_CCU_INVALIDATE_COLOR UINT32_C(0x19) /* adreno_pm4.xml:143 */
#define A840_EVENT_CACHE_INVALIDATE7 UINT32_C(0x33) /* adreno_pm4.xml:154 */

#define A840_EV_WRITE_USER_64B UINT32_C(1) /* adreno_pm4.xml:1677 */
#define A840_EV_WRITE_ALWAYSON UINT32_C(3) /* adreno_pm4.xml:1680 */
#define A840_BLIT_OP_SCALE UINT32_C(3) /* adreno_pm4.xml:1740 */
#define A840_EVENT_WRITE_ENABLED (UINT32_C(1) << 27) /* adreno_pm4.xml:1709 */
#define A840_EVENT_WRITE_SRC_SHIFT 20u /* adreno_pm4.xml:1706 */


#define A840_TEX_0_BASE_LO_SHIFT 6u /* a8xx_descriptors.xml:45; a6xx.xml:15 */
#define A840_TEX_0_BASE_LO_MAX UINT32_C(0x3ffffff) /* a8xx_descriptors.xml:45 */
#define A840_TEX_0_BASE_LO_SHR 6u /* a8xx_descriptors.xml:45 */
#define A840_TEX_1_BASE_HI_SHIFT 0u /* a8xx_descriptors.xml:50; a6xx.xml:15 */
#define A840_TEX_1_BASE_HI_MAX UINT32_C(0x1ffff) /* a8xx_descriptors.xml:50 */
#define A840_TEX_1_TYPE_SHIFT 17u /* a8xx_descriptors.xml:52; a6xx.xml:15 */
#define A840_TEX_1_TYPE_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:52 */
#define A840_TEX_1_DEPTH_SHIFT 20u /* a8xx_descriptors.xml:53; a6xx.xml:15 */
#define A840_TEX_1_DEPTH_MAX UINT32_C(0xfff) /* a8xx_descriptors.xml:53 */
#define A840_TEX_2_WIDTH_SHIFT 0u /* a8xx_descriptors.xml:58; a6xx.xml:15 */
#define A840_TEX_2_WIDTH_MAX UINT32_C(0x7fff) /* a8xx_descriptors.xml:58 */
#define A840_TEX_2_HEIGHT_SHIFT 15u /* a8xx_descriptors.xml:59; a6xx.xml:15 */
#define A840_TEX_2_HEIGHT_MAX UINT32_C(0x7fff) /* a8xx_descriptors.xml:59 */
#define A840_TEX_2_SAMPLES_SHIFT 30u /* a8xx_descriptors.xml:60; a6xx.xml:15 */
#define A840_TEX_2_SAMPLES_MAX UINT32_C(0x3) /* a8xx_descriptors.xml:60 */
#define A840_TEX_3_FMT_SHIFT 0u /* a8xx_descriptors.xml:64; a6xx.xml:15 */
#define A840_TEX_3_FMT_MAX UINT32_C(0xff) /* a8xx_descriptors.xml:64 */
#define A840_TEX_3_SWAP_SHIFT 8u /* a8xx_descriptors.xml:65; a6xx.xml:15 */
#define A840_TEX_3_SWAP_MAX UINT32_C(0x3) /* a8xx_descriptors.xml:65 */
#define A840_TEX_3_SWIZ_X_SHIFT 10u /* a8xx_descriptors.xml:66; a6xx.xml:15 */
#define A840_TEX_3_SWIZ_X_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:66 */
#define A840_TEX_3_SWIZ_Y_SHIFT 13u /* a8xx_descriptors.xml:67; a6xx.xml:15 */
#define A840_TEX_3_SWIZ_Y_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:67 */
#define A840_TEX_3_SWIZ_Z_SHIFT 16u /* a8xx_descriptors.xml:68; a6xx.xml:15 */
#define A840_TEX_3_SWIZ_Z_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:68 */
#define A840_TEX_3_SWIZ_W_SHIFT 19u /* a8xx_descriptors.xml:69; a6xx.xml:15 */
#define A840_TEX_3_SWIZ_W_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:69 */
#define A840_TEX_4_TILE_MODE_SHIFT 0u /* a8xx_descriptors.xml:72; a6xx.xml:15 */
#define A840_TEX_4_TILE_MODE_MAX UINT32_C(0x3) /* a8xx_descriptors.xml:72 */
#define A840_TEX_4_FLAG (UINT32_C(1) << 2) /* a8xx_descriptors.xml:73; a6xx.xml:15 */
#define A840_TEX_4_TILE_ALL (UINT32_C(1) << 4) /* a8xx_descriptors.xml:75; a6xx.xml:15 */
#define A840_TEX_4_SRGB (UINT32_C(1) << 5) /* a8xx_descriptors.xml:76; a6xx.xml:15 */
#define A840_TEX_4_FLAG_LO_SHIFT 6u /* a8xx_descriptors.xml:77; a6xx.xml:15 */
#define A840_TEX_4_FLAG_LO_MAX UINT32_C(0x3ffffff) /* a8xx_descriptors.xml:77 */
#define A840_TEX_4_FLAG_LO_SHR 6u /* a8xx_descriptors.xml:77 */
#define A840_TEX_5_FLAG_HI_SHIFT 0u /* a8xx_descriptors.xml:82; a6xx.xml:15 */
#define A840_TEX_5_FLAG_HI_MAX UINT32_C(0x1ffff) /* a8xx_descriptors.xml:82 */
#define A840_TEX_5_FLAG_BUFFER_PITCH_SHIFT 17u /* a8xx_descriptors.xml:85; a6xx.xml:15 */
#define A840_TEX_5_FLAG_BUFFER_PITCH_MAX UINT32_C(0xff) /* a8xx_descriptors.xml:85 */
#define A840_TEX_5_FLAG_BUFFER_PITCH_SHR 6u /* a8xx_descriptors.xml:85 */
#define A840_TEX_6_TEX_LINE_OFFSET_SHIFT 0u /* a8xx_descriptors.xml:96; a6xx.xml:15 */
#define A840_TEX_6_TEX_LINE_OFFSET_MAX UINT32_C(0xffffff) /* a8xx_descriptors.xml:96 */
#define A840_TEX_6_MIN_LINE_OFFSET_SHIFT 24u /* a8xx_descriptors.xml:97; a6xx.xml:15 */
#define A840_TEX_6_MIN_LINE_OFFSET_MAX UINT32_C(0xf) /* a8xx_descriptors.xml:97 */
#define A840_TEX_6_MIPLVLS_SHIFT 28u /* a8xx_descriptors.xml:98; a6xx.xml:15 */
#define A840_TEX_6_MIPLVLS_MAX UINT32_C(0xf) /* a8xx_descriptors.xml:98 */
#define A840_TEX_7_ARRAY_SLICE_OFFSET_SHIFT 0u /* a8xx_descriptors.xml:101; a6xx.xml:15 */
#define A840_TEX_7_ARRAY_SLICE_OFFSET_MAX UINT32_C(0x7fffff) /* a8xx_descriptors.xml:101 */
#define A840_TEX_7_ARRAY_SLICE_OFFSET_SHR 12u /* a8xx_descriptors.xml:101 */
#define A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_SHIFT 24u /* a8xx_descriptors.xml:103; a6xx.xml:15 */
#define A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_MAX UINT32_C(0xf) /* a8xx_descriptors.xml:103 */
#define A840_TEX_7_MIN_ARRAY_SLIZE_OFFSET_SHR 12u /* a8xx_descriptors.xml:103 */
#define A840_TEX_8_FLAG_ARRAY_PITCH_SHIFT 0u /* a8xx_descriptors.xml:116; a6xx.xml:15 */
#define A840_TEX_8_FLAG_ARRAY_PITCH_MAX UINT32_C(0x7fff) /* a8xx_descriptors.xml:116 */
#define A840_TEX_8_FLAG_ARRAY_PITCH_SHR 12u /* a8xx_descriptors.xml:116 */
#define A840_TEX_8_FLAG_BUFFER_LOGW_SHIFT 24u /* a8xx_descriptors.xml:118; a6xx.xml:15 */
#define A840_TEX_8_FLAG_BUFFER_LOGW_MAX UINT32_C(0xf) /* a8xx_descriptors.xml:118 */
#define A840_TEX_8_FLAG_BUFFER_LOGH_SHIFT 28u /* a8xx_descriptors.xml:119; a6xx.xml:15 */
#define A840_TEX_8_FLAG_BUFFER_LOGH_MAX UINT32_C(0xf) /* a8xx_descriptors.xml:119 */
#define A840_SAMP_0_MIPFILTER_LINEAR_NEAR (UINT32_C(1) << 0) /* a8xx_descriptors.xml:14; a6xx.xml:15 */
#define A840_SAMP_0_MIPMAPING_DIS (UINT32_C(1) << 1) /* a8xx_descriptors.xml:15; a6xx.xml:15 */
#define A840_SAMP_0_XY_MAG_SHIFT 2u /* a8xx_descriptors.xml:16; a6xx.xml:15 */
#define A840_SAMP_0_XY_MAG_MAX UINT32_C(0x3) /* a8xx_descriptors.xml:16 */
#define A840_SAMP_0_XY_MIN_SHIFT 4u /* a8xx_descriptors.xml:17; a6xx.xml:15 */
#define A840_SAMP_0_XY_MIN_MAX UINT32_C(0x3) /* a8xx_descriptors.xml:17 */
#define A840_SAMP_0_WRAP_S_SHIFT 6u /* a8xx_descriptors.xml:18; a6xx.xml:15 */
#define A840_SAMP_0_WRAP_S_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:18 */
#define A840_SAMP_0_WRAP_T_SHIFT 9u /* a8xx_descriptors.xml:19; a6xx.xml:15 */
#define A840_SAMP_0_WRAP_T_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:19 */
#define A840_SAMP_0_WRAP_R_SHIFT 12u /* a8xx_descriptors.xml:20; a6xx.xml:15 */
#define A840_SAMP_0_WRAP_R_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:20 */
#define A840_SAMP_0_LOD_BIAS_SHIFT 16u /* a8xx_descriptors.xml:22; a6xx.xml:15 */
#define A840_SAMP_0_LOD_BIAS_MAX UINT32_C(0x1fff) /* a8xx_descriptors.xml:22 */
#define A840_SAMP_0_ANISO_SHIFT 29u /* a8xx_descriptors.xml:23; a6xx.xml:15 */
#define A840_SAMP_0_ANISO_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:23 */
#define A840_SAMP_1_MAX_LOD_SHIFT 0u /* a8xx_descriptors.xml:26; a6xx.xml:15 */
#define A840_SAMP_1_MAX_LOD_MAX UINT32_C(0xfff) /* a8xx_descriptors.xml:26 */
#define A840_SAMP_1_MIN_LOD_SHIFT 12u /* a8xx_descriptors.xml:27; a6xx.xml:15 */
#define A840_SAMP_1_MIN_LOD_MAX UINT32_C(0xfff) /* a8xx_descriptors.xml:27 */
#define A840_SAMP_1_COMPARE_FUNC_SHIFT 26u /* a8xx_descriptors.xml:29; a6xx.xml:15 */
#define A840_SAMP_1_COMPARE_FUNC_MAX UINT32_C(0x7) /* a8xx_descriptors.xml:29 */
#define A840_SAMP_1_UNNORM_COORDS (UINT32_C(1) << 31) /* a8xx_descriptors.xml:32; a6xx.xml:15 */
#define A840_SAMP_2_BCOLOR_SHIFT 7u /* a8xx_descriptors.xml:37; a6xx.xml:15 */
#define A840_SAMP_2_BCOLOR_MAX UINT32_C(0x1ffffff) /* a8xx_descriptors.xml:37 */

/* Shared 2D fields, directly from a6xx.xml. */
#define A840_2D_SOLID_COLOR (UINT32_C(1) << 7) /* a6xx.xml:2168 */
#define A840_2D_FORMAT_SHIFT 8u /* a6xx.xml:2169 */
#define A840_2D_MASK_SHIFT 20u /* a6xx.xml:2175 */
#define A840_2D_IFMT_SHIFT 24u /* a6xx.xml:2176 */
#define A840_2D_COORD_Y_SHIFT 16u /* a6xx.xml:1419 */
#define A840_2D_COORD_MAX UINT32_C(0x3fff) /* a6xx.xml:1418-1419 */
#define A840_2D_SRC_COORD_SHIFT 8u /* a6xx.xml:2201-2204 */
#define A840_2D_SRC_TILE_SHIFT 8u /* a6xx.xml:2781 */
#define A840_2D_SRC_SWAP_SHIFT 10u /* a6xx.xml:2782 */
#define A840_2D_SRC_FLAGS (UINT32_C(1) << 12) /* a6xx.xml:2783 */
#define A840_2D_SRC_SRGB (UINT32_C(1) << 13) /* a6xx.xml:2784 */
#define A840_2D_SRC_FILTER (UINT32_C(1) << 16) /* a6xx.xml:2786 */
#define A840_2D_SRC_UNK20 (UINT32_C(1) << 20) /* a6xx.xml:2790 */
#define A840_2D_SRC_UNK22 (UINT32_C(1) << 22) /* a6xx.xml:2792 */
#define A840_2D_SRC_HEIGHT_SHIFT 15u /* a6xx.xml:4525 */
#define A840_2D_SRC_PITCH_SHIFT 3u /* a6xx.xml:4535 */
#define A840_2D_SRC_PITCH_MAX UINT32_C(0x1fffff) /* a6xx.xml:4535 */
#define A840_2D_DST_TILE_SHIFT 8u /* a6xx.xml:2800 */
#define A840_2D_DST_SWAP_SHIFT 10u /* a6xx.xml:2801 */
#define A840_2D_DST_FLAGS (UINT32_C(1) << 12) /* a6xx.xml:2802 */
#define A840_2D_DST_SRGB (UINT32_C(1) << 13) /* a6xx.xml:2803 */
#define A840_2D_DST_PITCH_SHR 6u /* a6xx.xml:2811 */
#define A840_2D_DST_PITCH_MAX UINT32_C(0xffff) /* a6xx.xml:2811 */
/* FDL sends this common FLAG_BUFFER_PITCH image to both RB and TPL1.
 * Array pitch is supplied in dwords: fd6_view.cc:457-459, tu_image.cc:165-168.
 * Thus XML shr=7 consumes bytes >> 2 first, i.e. bytes >> 9. */
#define A840_2D_FLAG_PITCH_MAX UINT32_C(0x7f) /* a6xx.xml:2721 */
#define A840_2D_FLAG_PITCH_SHR 6u /* a6xx.xml:2721 */
#define A840_2D_FLAG_ARRAY_MAX UINT32_C(0x1ffff) /* a6xx.xml:2724 */
#define A840_2D_FLAG_ARRAY_SHR 9u /* a6xx.xml:2724; fd6_view.cc:459 (bytes/4) */
#define A840_2D_FLAG_ARRAY_SHIFT 11u /* a6xx.xml:2724 */
#define A840_2D_TPL1_TYPE_SHIFT 29u /* a6xx.xml:4566,4571 */
#define A840_2D_SP_TYPE_SHIFT 1u /* a6xx.xml:4353 */
#define A840_2D_SP_HALF_PRECISION UINT32_C(1) /* a6xx.xml:4352 */
#define A840_2D_SP_FORMAT_SHIFT 3u /* a6xx.xml:4356 */
#define A840_2D_SP_SRGB (UINT32_C(1) << 11) /* a6xx.xml:4358 */
#define A840_2D_SP_MASK_SHIFT 12u /* a6xx.xml:4360 */

#define A840_TILE6_LINEAR UINT32_C(0) /* a6xx_enums.xml:10; a6xx.xml:8 */
#define A840_TILE6_3 UINT32_C(3) /* a6xx_enums.xml:12; a6xx.xml:8 */
#define A840_A6XX_TEX_1D UINT32_C(0) /* a6xx_enums.xml:337; a6xx.xml:8 */
#define A840_A6XX_TEX_2D UINT32_C(1) /* a6xx_enums.xml:338; a6xx.xml:8 */
#define A840_A6XX_TEX_CUBE UINT32_C(2) /* a6xx_enums.xml:339; a6xx.xml:8 */
#define A840_A6XX_TEX_3D UINT32_C(3) /* a6xx_enums.xml:340; a6xx.xml:8 */
#define A840_A6XX_TEX_NEAREST UINT32_C(0) /* a6xx_enums.xml:387; a6xx.xml:8 */
#define A840_A6XX_TEX_LINEAR UINT32_C(1) /* a6xx_enums.xml:388; a6xx.xml:8 */
#define A840_A6XX_TEX_ANISO UINT32_C(2) /* a6xx_enums.xml:389; a6xx.xml:8 */
#define A840_R2D_UNORM8 UINT32_C(0x0) /* a6xx_enums.xml:330; a6xx.xml:8 */
#define A840_R2D_UNORM8_SRGB UINT32_C(0x1) /* a6xx_enums.xml:329; a6xx.xml:8 */
#define A840_R2D_SNORM8 UINT32_C(0x2) /* a6xx_enums.xml:328; a6xx.xml:8 */
#define A840_R2D_FLOAT16 UINT32_C(0x3) /* a6xx_enums.xml:327; a6xx.xml:8 */
#define A840_R2D_FLOAT32 UINT32_C(0x4) /* a6xx_enums.xml:326; a6xx.xml:8 */
#define A840_R2D_INT8 UINT32_C(0x5) /* a6xx_enums.xml:325; a6xx.xml:8 */
#define A840_R2D_INT16 UINT32_C(0x6) /* a6xx_enums.xml:324; a6xx.xml:8 */
#define A840_R2D_INT32 UINT32_C(0x7) /* a6xx_enums.xml:323; a6xx.xml:8 */
#define A840_OUTPUT_IFMT_2D_FLOAT UINT32_C(0) /* a6xx.xml:4346 */
#define A840_OUTPUT_IFMT_2D_SINT UINT32_C(1) /* a6xx.xml:4347 */
#define A840_OUTPUT_IFMT_2D_UINT UINT32_C(2) /* a6xx.xml:4348 */

/* Shader binding, dispatch, sysmem draw and attachment fields. */

/* a6xx.xml */
#define A840_XS_HALF_SHIFT 1u /* a6xx.xml:3650 */
#define A840_XS_HALF_MAX UINT32_C(63) /* a6xx.xml:3650 */
#define A840_XS_FULL_SHIFT 7u /* a6xx.xml:3651 */
#define A840_XS_FULL_MAX UINT32_C(63) /* a6xx.xml:3651 */
#define A840_XS_BRANCH_SHIFT 13u /* a6xx.xml:3653 */
#define A840_XS_BRANCH_MAX UINT32_C(127) /* a6xx.xml:3653 */
#define A840_XS_BINDLESS_TEX (UINT32_C(1) << 0) /* a6xx.xml:3665 */
#define A840_XS_BINDLESS_SAMP (UINT32_C(1) << 1) /* a6xx.xml:3666 */
#define A840_XS_BINDLESS_UAV (UINT32_C(1) << 2) /* a6xx.xml:3667 */
#define A840_XS_BINDLESS_UBO (UINT32_C(1) << 3) /* a6xx.xml:3668 */
#define A840_XS_ENABLE (UINT32_C(1) << 8) /* a6xx.xml:3670 */
#define A840_XS_NTEX_SHIFT 9u /* a6xx.xml:3675 */
#define A840_XS_NTEX_MAX UINT32_C(255) /* a6xx.xml:3675 */
#define A840_XS_NSAMP_SHIFT 17u /* a6xx.xml:3676 */
#define A840_XS_NSAMP_MAX UINT32_C(31) /* a6xx.xml:3676 */
#define A840_XS_NUAV_SHIFT 22u /* a6xx.xml:3677 */
#define A840_XS_NUAV_MAX UINT32_C(127) /* a6xx.xml:3677 */
#define A840_VS_MERGED (UINT32_C(1) << 20) /* a6xx.xml:3690 */
#define A840_VS_PREAMBLE (UINT32_C(1) << 21) /* a6xx.xml:3706 */
#define A840_PRIVATE_ITEM_SHIFT 0u /* a6xx.xml:3738 */
#define A840_PRIVATE_ITEM_MAX UINT32_C(255) /* a6xx.xml:3738 */
#define A840_PRIVATE_ITEM_SHR 9u /* a6xx.xml:3738 */
#define A840_PRIVATE_SIZE_SHIFT 0u /* a6xx.xml:3755 */
#define A840_PRIVATE_SIZE_MAX UINT32_C(262143) /* a6xx.xml:3755 */
#define A840_PRIVATE_SIZE_SHR 12u /* a6xx.xml:3755 */
#define A840_PRIVATE_PER_WAVE (UINT32_C(1) << 31) /* a6xx.xml:3756 */
#define A840_PRIVATE_STACK_SHIFT 0u /* a6xx.xml:3805 */
#define A840_PRIVATE_STACK_MAX UINT32_C(524287) /* a6xx.xml:3805 */
#define A840_PRIVATE_STACK_SHR 11u /* a6xx.xml:3805 */
#define A840_PS_THREADSIZE_SHIFT 20u /* a6xx.xml:3965 */
#define A840_PS_THREADSIZE_MAX UINT32_C(1) /* a6xx.xml:3965 */
#define A840_PS_VARYING (UINT32_C(1) << 22) /* a6xx.xml:3967 */
#define A840_PS_LOD_MASK (UINT32_C(1) << 23) /* a6xx.xml:3968 */
#define A840_PS_INOUT_OVERLAP (UINT32_C(1) << 24) /* a6xx.xml:3974 */
#define A840_PS_PIXEL_LOD (UINT32_C(1) << 26) /* a6xx.xml:3976 */
#define A840_PS_PREAMBLE (UINT32_C(1) << 28) /* a6xx.xml:3983 */
#define A840_PS_MERGED (UINT32_C(1) << 31) /* a6xx.xml:3984 */
#define A840_CS_THREADSIZE_SHIFT 20u /* a6xx.xml:4112 */
#define A840_CS_THREADSIZE_MAX UINT32_C(1) /* a6xx.xml:4112 */
#define A840_CS_ROUND_ROBIN (UINT32_C(1) << 22) /* a6xx.xml:4116 */
#define A840_CS_PREAMBLE (UINT32_C(1) << 23) /* a6xx.xml:4117 */
#define A840_CS_MERGED (UINT32_C(1) << 31) /* a6xx.xml:4118 */
#define A840_CS_SHARED_SHIFT 0u /* a6xx.xml:4130 */
#define A840_CS_SHARED_MAX UINT32_C(31) /* a6xx.xml:4130 */
#define A840_CS_CONST_MODE_SHIFT 5u /* a6xx.xml:4140 */
#define A840_CS_CONST_MODE_MAX UINT32_C(3) /* a6xx.xml:4140 */
#define A840_INSTR_SIZE_SHIFT 0u /* a6xx.xml:4157 */
#define A840_INSTR_SIZE_MAX UINT32_C(268435455) /* a6xx.xml:4157 */
#define A840_CS_WGID_SHIFT 0u /* a6xx.xml:4165 */
#define A840_CS_WGID_MAX UINT32_C(255) /* a6xx.xml:4165 */
#define A840_CS_WGSIZE_SHIFT 8u /* a6xx.xml:4166 */
#define A840_CS_WGSIZE_MAX UINT32_C(255) /* a6xx.xml:4166 */
#define A840_CS_WGOFFSET_SHIFT 16u /* a6xx.xml:4167 */
#define A840_CS_WGOFFSET_MAX UINT32_C(255) /* a6xx.xml:4167 */
#define A840_CS_LOCALID_SHIFT 24u /* a6xx.xml:4168 */
#define A840_CS_LOCALID_MAX UINT32_C(255) /* a6xx.xml:4168 */
#define A840_CS_WIE_THREAD_SHIFT 8u /* a6xx.xml:4198 */
#define A840_CS_WIE_THREAD_MAX UINT32_C(1) /* a6xx.xml:4198 */
#define A840_CS_WIE_ORDER_SHIFT 15u /* a6xx.xml:4203 */
#define A840_CS_WIE_ORDER_MAX UINT32_C(1) /* a6xx.xml:4203 */
#define A840_SP_CONST_DEMOTION (UINT32_C(1) << 0) /* a6xx.xml:4305 */
#define A840_SP_ISAM_SHIFT 1u /* a6xx.xml:4306 */
#define A840_SP_ISAM_MAX UINT32_C(3) /* a6xx.xml:4306 */
#define A840_XS_CONSTLEN_SHIFT 0u /* a6xx.xml:4613 */
#define A840_XS_CONSTLEN_MAX UINT32_C(255) /* a6xx.xml:4613 */
#define A840_XS_CONSTLEN_SHR 2u /* a6xx.xml:4613 */
#define A840_XS_CONST_ENABLE (UINT32_C(1) << 8) /* a6xx.xml:4614 */
#define A840_WAVE_VARYINGS (UINT32_C(1) << 1) /* a6xx.xml:4707 */
#define A840_NDRANGE_DIM_SHIFT 0u /* a6xx.xml:4778 */
#define A840_NDRANGE_DIM_MAX UINT32_C(3) /* a6xx.xml:4778 */
#define A840_NDRANGE_LOCAL_X_SHIFT 2u /* a6xx.xml:4780 */
#define A840_NDRANGE_LOCAL_X_MAX UINT32_C(1023) /* a6xx.xml:4780 */
#define A840_NDRANGE_LOCAL_Y_SHIFT 12u /* a6xx.xml:4781 */
#define A840_NDRANGE_LOCAL_Y_MAX UINT32_C(1023) /* a6xx.xml:4781 */
#define A840_NDRANGE_LOCAL_Z_SHIFT 22u /* a6xx.xml:4782 */
#define A840_NDRANGE_LOCAL_Z_MAX UINT32_C(1023) /* a6xx.xml:4782 */
#define A840_CS_WGE_THREAD_SHIFT 9u /* a6xx.xml:4818 */
#define A840_CS_WGE_THREAD_MAX UINT32_C(1) /* a6xx.xml:4818 */
#define A840_CS_WGE_Z_FIRST (UINT32_C(1) << 11) /* a6xx.xml:4824 */
#define A840_CS_WGE_TILE_W_SHIFT 20u /* a6xx.xml:4831 */
#define A840_CS_WGE_TILE_W_MAX UINT32_C(63) /* a6xx.xml:4831 */
#define A840_CS_WGE_TILE_H_SHIFT 26u /* a6xx.xml:4832 */
#define A840_CS_WGE_TILE_H_MAX UINT32_C(63) /* a6xx.xml:4832 */
#define A840_UPDATE_VS (UINT32_C(1) << 0) /* a6xx.xml:4945 */
#define A840_UPDATE_HS (UINT32_C(1) << 1) /* a6xx.xml:4946 */
#define A840_UPDATE_DS (UINT32_C(1) << 2) /* a6xx.xml:4947 */
#define A840_UPDATE_GS (UINT32_C(1) << 3) /* a6xx.xml:4948 */
#define A840_UPDATE_FS (UINT32_C(1) << 4) /* a6xx.xml:4949 */
#define A840_UPDATE_CS (UINT32_C(1) << 5) /* a6xx.xml:4950 */
#define A840_RB_PS_PIXEL_LOD (UINT32_C(1) << 0) /* a6xx.xml:2386 */
#define A840_RB_PS_LOD_MASK (UINT32_C(1) << 1) /* a6xx.xml:2387 */
#define A840_RB_PRIM_THRESHOLD_SHIFT 0u /* a6xx.xml:2457 */
#define A840_RB_PRIM_THRESHOLD_MAX UINT32_C(7) /* a6xx.xml:2457 */
#define A840_PC_VS_INSTR_SHIFT 0u /* a6xx.xml:3354 */
#define A840_PC_VS_INSTR_MAX UINT32_C(63) /* a6xx.xml:3354 */
#define A840_PC_VS_SIDEBAND_SHIFT 6u /* a6xx.xml:3355 */
#define A840_PC_VS_SIDEBAND_MAX UINT32_C(7) /* a6xx.xml:3355 */
#define A840_VFD_FETCH_SHIFT 0u /* a6xx.xml:3530 */
#define A840_VFD_FETCH_MAX UINT32_C(63) /* a6xx.xml:3530 */
#define A840_VFD_DECODE_SHIFT 8u /* a6xx.xml:3531 */
#define A840_VFD_DECODE_MAX UINT32_C(63) /* a6xx.xml:3531 */
#define A840_VFD_INST_SHIFT 8u /* a6xx.xml:3535 */
#define A840_VFD_INST_MAX UINT32_C(255) /* a6xx.xml:3535 */
#define A840_VFD_PRIM_SHIFT 16u /* a6xx.xml:3536 */
#define A840_VFD_PRIM_MAX UINT32_C(255) /* a6xx.xml:3536 */
#define A840_VFD_VIEW_SHIFT 24u /* a6xx.xml:3538 */
#define A840_VFD_VIEW_MAX UINT32_C(255) /* a6xx.xml:3538 */
#define A840_VFD_DEST_REG_SHIFT 4u /* a6xx.xml:3608 */
#define A840_VFD_DEST_REG_MAX UINT32_C(255) /* a6xx.xml:3608 */
#define A840_VS_OUT_MASK_SHIFT 8u /* a6xx.xml:3716 */
#define A840_VS_OUT_MASK_MAX UINT32_C(15) /* a6xx.xml:3716 */
#define A840_VS_OUT_NEXT_SHIFT 16u /* a6xx.xml:3717 */
#define A840_VS_OUT_NEXT_MAX UINT32_C(255) /* a6xx.xml:3717 */
#define A840_VS_DEST_NEXT_SHIFT 8u /* a6xx.xml:3731 */
#define A840_VS_DEST_NEXT_MAX UINT32_C(255) /* a6xx.xml:3731 */
#define A840_VPC_POSITION_SHIFT 8u /* a6xx.xml:3164 */
#define A840_VPC_POSITION_MAX UINT32_C(255) /* a6xx.xml:3164 */
#define A840_VPC_POINT_SIZE_SHIFT 16u /* a6xx.xml:3165 */
#define A840_VPC_POINT_SIZE_MAX UINT32_C(255) /* a6xx.xml:3165 */
#define A840_VPC_PRIM_ID_SHIFT 8u /* a6xx.xml:3186 */
#define A840_VPC_PRIM_ID_MAX UINT32_C(255) /* a6xx.xml:3186 */
#define A840_VPC_VARYING (UINT32_C(1) << 16) /* a6xx.xml:3187 */
#define A840_VPC_VIEW_ID_SHIFT 24u /* a6xx.xml:3188 */
#define A840_VPC_VIEW_ID_MAX UINT32_C(255) /* a6xx.xml:3188 */
#define A840_VPC_SIV_VIEW_SHIFT 8u /* a6xx.xml:2952 */
#define A840_VPC_SIV_VIEW_MAX UINT32_C(255) /* a6xx.xml:2952 */
#define A840_VPC_SIV_RATE_SHIFT 16u /* a6xx.xml:2953 */
#define A840_VPC_SIV_RATE_MAX UINT32_C(255) /* a6xx.xml:2953 */
#define A840_SP_PRIM_THRESHOLD_SHIFT 0u /* a6xx.xml:4710 */
#define A840_SP_PRIM_THRESHOLD_MAX UINT32_C(7) /* a6xx.xml:4710 */
#define A840_PROG_SAMPLE_SHIFT 8u /* a6xx.xml:4715 */
#define A840_PROG_SAMPLE_MAX UINT32_C(255) /* a6xx.xml:4715 */
#define A840_PROG_SAMPLE_MASK_SHIFT 16u /* a6xx.xml:4716 */
#define A840_PROG_SAMPLE_MASK_MAX UINT32_C(255) /* a6xx.xml:4716 */
#define A840_PROG_CENTER_SHIFT 24u /* a6xx.xml:4717 */
#define A840_PROG_CENTER_MAX UINT32_C(255) /* a6xx.xml:4717 */
#define A840_PROG_LINEAR_PIXEL_SHIFT 8u /* a6xx.xml:4671 */
#define A840_PROG_LINEAR_PIXEL_MAX UINT32_C(255) /* a6xx.xml:4671 */
#define A840_PROG_PERSP_CENTROID_SHIFT 16u /* a6xx.xml:4672 */
#define A840_PROG_PERSP_CENTROID_MAX UINT32_C(255) /* a6xx.xml:4672 */
#define A840_PROG_LINEAR_CENTROID_SHIFT 24u /* a6xx.xml:4673 */
#define A840_PROG_LINEAR_CENTROID_MAX UINT32_C(255) /* a6xx.xml:4673 */
#define A840_PROG_LINEAR_SAMPLE_SHIFT 8u /* a6xx.xml:4677 */
#define A840_PROG_LINEAR_SAMPLE_MAX UINT32_C(255) /* a6xx.xml:4677 */
#define A840_PROG_XY_SHIFT 16u /* a6xx.xml:4678 */
#define A840_PROG_XY_MAX UINT32_C(255) /* a6xx.xml:4678 */
#define A840_PROG_ZW_SHIFT 24u /* a6xx.xml:4679 */
#define A840_PROG_ZW_MAX UINT32_C(255) /* a6xx.xml:4679 */
#define A840_PROG_SHADING_RATE_SHIFT 8u /* a6xx.xml:4683 */
#define A840_PROG_SHADING_RATE_MAX UINT32_C(255) /* a6xx.xml:4683 */
#define A840_PS_DEFER_DISABLE (UINT32_C(1) << 8) /* a6xx.xml:4651 */
#define A840_PS_EVICT_MODE_SHIFT 9u /* a6xx.xml:4652 */
#define A840_PS_EVICT_MODE_MAX UINT32_C(3) /* a6xx.xml:4652 */
#define A840_INTERP_PERSP_PIXEL (UINT32_C(1) << 0) /* a6xx.xml:1455 */
#define A840_INTERP_PERSP_CENTROID (UINT32_C(1) << 1) /* a6xx.xml:1456 */
#define A840_INTERP_PERSP_SAMPLE (UINT32_C(1) << 2) /* a6xx.xml:1457 */
#define A840_INTERP_LINEAR_PIXEL (UINT32_C(1) << 3) /* a6xx.xml:1458 */
#define A840_INTERP_LINEAR_CENTROID (UINT32_C(1) << 4) /* a6xx.xml:1459 */
#define A840_INTERP_LINEAR_SAMPLE (UINT32_C(1) << 5) /* a6xx.xml:1460 */
#define A840_INTERP_COORD_SHIFT 6u /* a6xx.xml:1461 */
#define A840_INTERP_COORD_MAX UINT32_C(15) /* a6xx.xml:1461 */
#define A840_GRAS_FACE (UINT32_C(1) << 10) /* a6xx.xml:1463 */
#define A840_GRAS_CENTER (UINT32_C(1) << 11) /* a6xx.xml:1465 */
#define A840_RB_INTERP_ENABLE (UINT32_C(1) << 10) /* a6xx.xml:2289 */
#define A840_RB_INPUT_SAMPLE_MASK (UINT32_C(1) << 0) /* a6xx.xml:2293 */
#define A840_RB_INPUT_FACE (UINT32_C(1) << 2) /* a6xx.xml:2295 */
#define A840_RB_INPUT_SAMPLE (UINT32_C(1) << 3) /* a6xx.xml:2296 */
#define A840_RB_INPUT_CENTER (UINT32_C(1) << 6) /* a6xx.xml:2298 */
#define A840_RB_INPUT_RATE (UINT32_C(1) << 8) /* a6xx.xml:2300 */
#define A840_RB_OUTPUT_Z (UINT32_C(1) << 1) /* a6xx.xml:2305 */
#define A840_RB_OUTPUT_MASK (UINT32_C(1) << 2) /* a6xx.xml:2306 */
#define A840_RB_OUTPUT_STENCIL (UINT32_C(1) << 3) /* a6xx.xml:2307 */
#define A840_PS_OUTPUT_Z_SHIFT 8u /* a6xx.xml:4029 */
#define A840_PS_OUTPUT_Z_MAX UINT32_C(255) /* a6xx.xml:4029 */
#define A840_PS_OUTPUT_SAMPLE_SHIFT 16u /* a6xx.xml:4030 */
#define A840_PS_OUTPUT_SAMPLE_MAX UINT32_C(255) /* a6xx.xml:4030 */
#define A840_PS_OUTPUT_STENCIL_SHIFT 24u /* a6xx.xml:4031 */
#define A840_PS_OUTPUT_STENCIL_MAX UINT32_C(255) /* a6xx.xml:4031 */
#define A840_PS_OUTPUT_HALF (UINT32_C(1) << 8) /* a6xx.xml:4041 */
#define A840_PS_MRT_SINT (UINT32_C(1) << 8) /* a6xx.xml:4048 */
#define A840_PS_MRT_UINT (UINT32_C(1) << 9) /* a6xx.xml:4049 */
#define A840_PS_IJ_DISABLE (UINT32_C(1) << 3) /* a6xx.xml:4056 */
#define A840_PS_CONST_SLOT_SHIFT 6u /* a6xx.xml:4067 */
#define A840_PS_CONST_SLOT_MAX UINT32_C(511) /* a6xx.xml:4067 */
#define A840_PS_COORD_SLOT_SHIFT 16u /* a6xx.xml:4069 */
#define A840_PS_COORD_SLOT_MAX UINT32_C(511) /* a6xx.xml:4069 */
#define A840_GUARDBAND_H_SHIFT 0u /* a6xx.xml:1472 */
#define A840_GUARDBAND_H_MAX UINT32_C(511) /* a6xx.xml:1472 */
#define A840_GUARDBAND_V_SHIFT 10u /* a6xx.xml:1473 */
#define A840_GUARDBAND_V_MAX UINT32_C(511) /* a6xx.xml:1473 */
#define A840_RASTER_CULL_FRONT (UINT32_C(1) << 0) /* a6xx.xml:1571 */
#define A840_RASTER_CULL_BACK (UINT32_C(1) << 1) /* a6xx.xml:1572 */
#define A840_RASTER_CW (UINT32_C(1) << 2) /* a6xx.xml:1573 */
#define A840_RASTER_LINE_SHIFT 3u /* a6xx.xml:1574 */
#define A840_RASTER_LINE_MAX UINT32_C(255) /* a6xx.xml:1574 */
#define A840_SCISSOR_Y_SHIFT 16u /* a6xx.xml:1876 */
#define A840_SCISSOR_Y_MAX UINT32_C(65535) /* a6xx.xml:1876 */
#define A840_MRT_COLOR_BLEND (UINT32_C(1) << 0) /* a6xx.xml:2392 */
#define A840_MRT_ALPHA_BLEND (UINT32_C(1) << 1) /* a6xx.xml:2393 */
#define A840_MRT_MASK_SHIFT 7u /* a6xx.xml:2396 */
#define A840_MRT_MASK_MAX UINT32_C(15) /* a6xx.xml:2396 */
#define A840_MRT_RGB_SRC_SHIFT 0u /* a6xx.xml:2399 */
#define A840_MRT_RGB_SRC_MAX UINT32_C(31) /* a6xx.xml:2399 */
#define A840_MRT_RGB_OP_SHIFT 5u /* a6xx.xml:2400 */
#define A840_MRT_RGB_OP_MAX UINT32_C(7) /* a6xx.xml:2400 */
#define A840_MRT_RGB_DST_SHIFT 8u /* a6xx.xml:2401 */
#define A840_MRT_RGB_DST_MAX UINT32_C(31) /* a6xx.xml:2401 */
#define A840_MRT_ALPHA_SRC_SHIFT 16u /* a6xx.xml:2402 */
#define A840_MRT_ALPHA_SRC_MAX UINT32_C(31) /* a6xx.xml:2402 */
#define A840_MRT_ALPHA_OP_SHIFT 21u /* a6xx.xml:2403 */
#define A840_MRT_ALPHA_OP_MAX UINT32_C(7) /* a6xx.xml:2403 */
#define A840_MRT_ALPHA_DST_SHIFT 24u /* a6xx.xml:2404 */
#define A840_MRT_ALPHA_DST_MAX UINT32_C(31) /* a6xx.xml:2404 */
#define A840_MRT_TILE_SHIFT 8u /* a6xx.xml:2414 */
#define A840_MRT_TILE_MAX UINT32_C(3) /* a6xx.xml:2414 */
#define A840_MRT_UBWC (UINT32_C(1) << 11) /* a6xx.xml:2416 */
#define A840_MRT_SWAP_SHIFT 13u /* a6xx.xml:2417 */
#define A840_MRT_SWAP_MAX UINT32_C(3) /* a6xx.xml:2417 */
#define A840_MRT_PITCH_SHIFT 0u /* a6xx.xml:2424 */
#define A840_MRT_PITCH_MAX UINT32_C(65535) /* a6xx.xml:2424 */
#define A840_MRT_PITCH_SHR 6u /* a6xx.xml:2424 */
#define A840_MRT_ARRAY_PITCH_SHIFT 0u /* a6xx.xml:2425 */
#define A840_MRT_ARRAY_PITCH_MAX UINT32_C(536870911) /* a6xx.xml:2425 */
#define A840_MRT_ARRAY_PITCH_SHR 6u /* a6xx.xml:2425 */
#define A840_RB_INDEPENDENT_BLEND (UINT32_C(1) << 8) /* a6xx.xml:2450 */
#define A840_RB_BLEND_SAMPLE_SHIFT 16u /* a6xx.xml:2454 */
#define A840_RB_BLEND_SAMPLE_MAX UINT32_C(65535) /* a6xx.xml:2454 */
#define A840_SP_INDEPENDENT_BLEND (UINT32_C(1) << 8) /* a6xx.xml:3996 */
#define A840_SP_MRT_COLOR_BLEND (UINT32_C(1) << 0) /* a6xx.xml:4273 */
#define A840_SP_MRT_ALPHA_BLEND (UINT32_C(1) << 1) /* a6xx.xml:4274 */
#define A840_SP_MRT_MASK_SHIFT 7u /* a6xx.xml:4275 */
#define A840_SP_MRT_MASK_MAX UINT32_C(15) /* a6xx.xml:4275 */
#define A840_DEPTH_TEST (UINT32_C(1) << 0) /* a6xx.xml:2462 */
#define A840_DEPTH_WRITE (UINT32_C(1) << 1) /* a6xx.xml:2463 */
#define A840_DEPTH_FUNC_SHIFT 2u /* a6xx.xml:2464 */
#define A840_DEPTH_FUNC_MAX UINT32_C(7) /* a6xx.xml:2464 */
#define A840_DEPTH_CLAMP (UINT32_C(1) << 5) /* a6xx.xml:2465 */
#define A840_DEPTH_READ (UINT32_C(1) << 6) /* a6xx.xml:2470 */
#define A840_DEPTH_TILE_SHIFT 5u /* a6xx.xml:2481 */
#define A840_DEPTH_TILE_MAX UINT32_C(3) /* a6xx.xml:2481 */
#define A840_DEPTH_UBWC (UINT32_C(1) << 7) /* a6xx.xml:2482 */
#define A840_DEPTH_PITCH_SHIFT 0u /* a6xx.xml:2485 */
#define A840_DEPTH_PITCH_MAX UINT32_C(16383) /* a6xx.xml:2485 */
#define A840_DEPTH_PITCH_SHR 6u /* a6xx.xml:2485 */
#define A840_DEPTH_ARRAY_PITCH_SHIFT 0u /* a6xx.xml:2486 */
#define A840_DEPTH_ARRAY_PITCH_MAX UINT32_C(268435455) /* a6xx.xml:2486 */
#define A840_DEPTH_ARRAY_PITCH_SHR 6u /* a6xx.xml:2486 */
#define A840_STENCIL_ENABLE (UINT32_C(1) << 0) /* a6xx.xml:2494 */
#define A840_STENCIL_ENABLE_BACK (UINT32_C(1) << 1) /* a6xx.xml:2495 */
#define A840_STENCIL_READ (UINT32_C(1) << 2) /* a6xx.xml:2502 */
#define A840_STENCIL_FUNC_SHIFT 8u /* a6xx.xml:2503 */
#define A840_STENCIL_FUNC_MAX UINT32_C(7) /* a6xx.xml:2503 */
#define A840_STENCIL_FAIL_SHIFT 11u /* a6xx.xml:2504 */
#define A840_STENCIL_FAIL_MAX UINT32_C(7) /* a6xx.xml:2504 */
#define A840_STENCIL_PASS_SHIFT 14u /* a6xx.xml:2505 */
#define A840_STENCIL_PASS_MAX UINT32_C(7) /* a6xx.xml:2505 */
#define A840_STENCIL_ZFAIL_SHIFT 17u /* a6xx.xml:2506 */
#define A840_STENCIL_ZFAIL_MAX UINT32_C(7) /* a6xx.xml:2506 */
#define A840_STENCIL_FUNC_BACK_SHIFT 20u /* a6xx.xml:2507 */
#define A840_STENCIL_FUNC_BACK_MAX UINT32_C(7) /* a6xx.xml:2507 */
#define A840_STENCIL_FAIL_BACK_SHIFT 23u /* a6xx.xml:2508 */
#define A840_STENCIL_FAIL_BACK_MAX UINT32_C(7) /* a6xx.xml:2508 */
#define A840_STENCIL_PASS_BACK_SHIFT 26u /* a6xx.xml:2509 */
#define A840_STENCIL_PASS_BACK_MAX UINT32_C(7) /* a6xx.xml:2509 */
#define A840_STENCIL_ZFAIL_BACK_SHIFT 29u /* a6xx.xml:2510 */
#define A840_STENCIL_ZFAIL_BACK_MAX UINT32_C(7) /* a6xx.xml:2510 */
#define A840_STENCIL_SEPARATE (UINT32_C(1) << 0) /* a6xx.xml:2518 */
#define A840_STENCIL_TILE_SHIFT 2u /* a6xx.xml:2520 */
#define A840_STENCIL_TILE_MAX UINT32_C(3) /* a6xx.xml:2520 */
#define A840_STENCIL_PITCH_SHIFT 0u /* a6xx.xml:2522 */
#define A840_STENCIL_PITCH_MAX UINT32_C(4095) /* a6xx.xml:2522 */
#define A840_STENCIL_PITCH_SHR 6u /* a6xx.xml:2522 */
#define A840_STENCIL_ARRAY_PITCH_SHIFT 0u /* a6xx.xml:2523 */
#define A840_STENCIL_ARRAY_PITCH_MAX UINT32_C(16777215) /* a6xx.xml:2523 */
#define A840_STENCIL_ARRAY_PITCH_SHR 6u /* a6xx.xml:2523 */
#define A840_STENCIL_BACK_BYTE_SHIFT 8u /* a6xx.xml:2528 */
#define A840_STENCIL_BACK_BYTE_MAX UINT32_C(255) /* a6xx.xml:2528 */
#define A840_COLOR_FLAG_PITCH_SHIFT 0u /* a6xx.xml:2601 */
#define A840_COLOR_FLAG_PITCH_MAX UINT32_C(2047) /* a6xx.xml:2601 */
#define A840_COLOR_FLAG_PITCH_SHR 6u /* a6xx.xml:2601 */
#define A840_COLOR_FLAG_ARRAY_SHIFT 11u /* a6xx.xml:2602 */
#define A840_COLOR_FLAG_ARRAY_MAX UINT32_C(262143) /* a6xx.xml:2602 */
#define A840_COLOR_FLAG_ARRAY_SHR 7u /* a6xx.xml:2602 */
#define A840_DEPTH_FLAG_PITCH_SHIFT 0u /* a6xx.xml:2721 */
#define A840_DEPTH_FLAG_PITCH_MAX UINT32_C(127) /* a6xx.xml:2721 */
#define A840_DEPTH_FLAG_PITCH_SHR 6u /* a6xx.xml:2721 */
#define A840_DEPTH_FLAG_ARRAY_SHIFT 11u /* a6xx.xml:2724 */
#define A840_DEPTH_FLAG_ARRAY_MAX UINT32_C(131071) /* a6xx.xml:2724 */
#define A840_DEPTH_FLAG_ARRAY_SHR 7u /* a6xx.xml:2724 */
#define A840_MODE_THREAD64 0u /* a6xx.xml:3629 */
#define A840_MODE_THREAD128 1u /* a6xx.xml:3630 */
#define A840_MODE_CONSTLEN_128 0u /* a6xx.xml:4122 */
#define A840_MODE_CONSTLEN_192 1u /* a6xx.xml:4123 */
#define A840_MODE_CONSTLEN_256 2u /* a6xx.xml:4124 */
#define A840_MODE_CONSTLEN_512 3u /* a6xx.xml:4125 */
#define A840_MODE_BINDLESS_64B 3u /* a6xx.xml:4219 */
#define A840_MODE_ISAM_GL 2u /* a6xx.xml:4293 */

#define A840_MODE_WORKITEM_LINEAR 0u /* a6xx.xml:4184 */
#define A840_MODE_WORKITEM_TILED 1u /* a6xx.xml:4191 */
#define A840_INTERP_FLAT 1u /* a6xx.xml:3038 */
#define A840_PC_POINT_SIZE (UINT32_C(1) << 8) /* a6xx.xml:3417 */
#define A840_DEPTH_OUTPUT_CLAMP (UINT32_C(1) << 8) /* a6xx.xml:2473 */
#define A840_GRAS_DEPTH_TEST UINT32_C(1) /* a6xx.xml:1616 */
#define A840_GRAS_STENCIL_TEST UINT32_C(1) /* a6xx.xml:1623 */
#define A840_RASTER_LINE_RADIX 2u /* a6xx.xml:1574 */

/* adreno_pm4.xml */
#define A840_EVENT_LABEL 63u /* adreno_pm4.xml:136 */
#define A840_MODE_DIRECT_RENDER 1u /* adreno_pm4.xml:1812 */
#define A840_MODE_COMPUTE 8u /* adreno_pm4.xml:1819 */
#define A840_LOAD_CS_SHADER 13u /* adreno_pm4.xml:815 */
#define A840_LOAD_CONSTANTS 1u /* adreno_pm4.xml:821 */
#define A840_LOAD_DIRECT 0u /* adreno_pm4.xml:826 */
#define A840_LOAD_INDIRECT 2u /* adreno_pm4.xml:828 */
#define A840_LOAD_DST_SHIFT 0u /* adreno_pm4.xml:839 */
#define A840_LOAD_DST_MAX UINT32_C(16383) /* adreno_pm4.xml:839 */
#define A840_LOAD_TYPE_SHIFT 14u /* adreno_pm4.xml:840 */
#define A840_LOAD_TYPE_MAX UINT32_C(3) /* adreno_pm4.xml:840 */
#define A840_LOAD_SOURCE_SHIFT 16u /* adreno_pm4.xml:841 */
#define A840_LOAD_SOURCE_MAX UINT32_C(3) /* adreno_pm4.xml:841 */
#define A840_LOAD_BLOCK_SHIFT 18u /* adreno_pm4.xml:842 */
#define A840_LOAD_BLOCK_MAX UINT32_C(15) /* adreno_pm4.xml:842 */
#define A840_LOAD_UNITS_SHIFT 22u /* adreno_pm4.xml:843 */
#define A840_LOAD_UNITS_MAX UINT32_C(1023) /* adreno_pm4.xml:843 */
#define A840_DRAW_PRIMITIVE_SHIFT 0u /* adreno_pm4.xml:880 */
#define A840_DRAW_PRIMITIVE_MAX UINT32_C(63) /* adreno_pm4.xml:880 */
#define A840_DRAW_SOURCE_SHIFT 6u /* adreno_pm4.xml:881 */
#define A840_DRAW_SOURCE_MAX UINT32_C(3) /* adreno_pm4.xml:881 */
#define A840_DRAW_VISIBILITY_SHIFT 8u /* adreno_pm4.xml:882 */
#define A840_DRAW_VISIBILITY_MAX UINT32_C(3) /* adreno_pm4.xml:882 */
#define A840_DRAW_INDEX_SHIFT 10u /* adreno_pm4.xml:883 */
#define A840_DRAW_INDEX_MAX UINT32_C(3) /* adreno_pm4.xml:883 */
#define A840_PRIM_LINES 2u /* adreno_pm4.xml:165 */
#define A840_PRIM_LINE_STRIP 3u /* adreno_pm4.xml:166 */
#define A840_PRIM_TRIANGLES 4u /* adreno_pm4.xml:167 */
#define A840_PRIM_TRIANGLE_FAN 5u /* adreno_pm4.xml:168 */
#define A840_PRIM_TRIANGLE_STRIP 6u /* adreno_pm4.xml:169 */
#define A840_PRIM_POINTS 9u /* adreno_pm4.xml:172 */
#define A840_DRAW_DMA 0u /* adreno_pm4.xml:213 */
#define A840_DRAW_AUTO 2u /* adreno_pm4.xml:215 */
#define A840_DRAW_IGNORE_VIS 0u /* adreno_pm4.xml:235 */
#define A840_DRAW_USE_VIS 1u /* adreno_pm4.xml:236 */
#define A840_DRAW_INDEX_16 1u /* adreno_pm4.xml:868 */
#define A840_DRAW_INDEX_32 2u /* adreno_pm4.xml:869 */

/* a6xx_enums.xml */
#define A840_DEPTH_NONE 0u /* a6xx_enums.xml:185 */
#define A840_DEPTH_16 1u /* a6xx_enums.xml:186 */
#define A840_DEPTH_24_8 2u /* a6xx_enums.xml:187 */
#define A840_DEPTH_32 4u /* a6xx_enums.xml:188 */

/* adreno_common.xml */
#define A840_FACTOR_ZERO 0u /* adreno_common.xml:67 */
#define A840_FACTOR_ONE 1u /* adreno_common.xml:68 */
#define A840_FACTOR_SRC_COLOR 4u /* adreno_common.xml:69 */
#define A840_FACTOR_ONE_MINUS_SRC_COLOR 5u /* adreno_common.xml:70 */
#define A840_FACTOR_SRC_ALPHA 6u /* adreno_common.xml:71 */
#define A840_FACTOR_ONE_MINUS_SRC_ALPHA 7u /* adreno_common.xml:72 */
#define A840_FACTOR_DST_COLOR 8u /* adreno_common.xml:73 */
#define A840_FACTOR_ONE_MINUS_DST_COLOR 9u /* adreno_common.xml:74 */
#define A840_FACTOR_DST_ALPHA 10u /* adreno_common.xml:75 */
#define A840_FACTOR_ONE_MINUS_DST_ALPHA 11u /* adreno_common.xml:76 */
#define A840_FACTOR_CONSTANT_COLOR 12u /* adreno_common.xml:77 */
#define A840_FACTOR_ONE_MINUS_CONSTANT_COLOR 13u /* adreno_common.xml:78 */
#define A840_FACTOR_CONSTANT_ALPHA 14u /* adreno_common.xml:79 */
#define A840_FACTOR_ONE_MINUS_CONSTANT_ALPHA 15u /* adreno_common.xml:80 */
#define A840_FACTOR_SRC_ALPHA_SATURATE 16u /* adreno_common.xml:81 */
#define A840_BLEND_ADD 0u /* adreno_common.xml:374 */
#define A840_BLEND_SUB 1u /* adreno_common.xml:375 */
#define A840_BLEND_REVERSE_SUB 2u /* adreno_common.xml:376 */
#define A840_BLEND_MIN 3u /* adreno_common.xml:377 */
#define A840_BLEND_MAX 4u /* adreno_common.xml:378 */

#endif /* A840_BITFIELDS_H */
