/* SPDX-License-Identifier: MIT */
#include "a840_context_init.h"
#include "gen/Adreno840Regs.h"
#include <string.h>

/* Derived from caps/context_init_a840.tsv, never a second editable table.
 * Mesa 85fda3b36405; source refs/order/roles are verified against the TSV.
 * Reg/opcode identifiers exclusively use the frozen generated header.
 */
static const A840ContextInitReg context_regs[] = {
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_0_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_0_OFFSET", "src/freedreno/common/freedreno_devices.py:1258", "a8xx_base_raw_magic_regs entry 0; preserve source order" },
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_1_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_1_OFFSET", "src/freedreno/common/freedreno_devices.py:1259", "a8xx_base_raw_magic_regs entry 1; preserve source order" },
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_2_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_2_OFFSET", "src/freedreno/common/freedreno_devices.py:1260", "a8xx_base_raw_magic_regs entry 2; preserve source order" },
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_3_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_3_OFFSET", "src/freedreno/common/freedreno_devices.py:1261", "a8xx_base_raw_magic_regs entry 3; preserve source order" },
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_4_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_4_OFFSET", "src/freedreno/common/freedreno_devices.py:1262", "a8xx_base_raw_magic_regs entry 4; preserve source order" },
    { A840_REG_GRAS_BIN_FOVEAT_XY_FDM_5_OFFSET, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_BIN_FOVEAT_XY_FDM_5_OFFSET", "src/freedreno/common/freedreno_devices.py:1263", "a8xx_base_raw_magic_regs entry 5; preserve source order" },
    { A840_REG_RB_UNKNOWN_8818, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_8818", "src/freedreno/common/freedreno_devices.py:1265", "a8xx_base_raw_magic_regs entry 6; preserve source order" },
    { A840_REG_RB_UNKNOWN_8819, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_8819", "src/freedreno/common/freedreno_devices.py:1266", "a8xx_base_raw_magic_regs entry 7; preserve source order" },
    { A840_REG_RB_UNKNOWN_881A, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_881A", "src/freedreno/common/freedreno_devices.py:1267", "a8xx_base_raw_magic_regs entry 8; preserve source order" },
    { A840_REG_RB_UNKNOWN_881B, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_881B", "src/freedreno/common/freedreno_devices.py:1268", "a8xx_base_raw_magic_regs entry 9; preserve source order" },
    { A840_REG_RB_UNKNOWN_881C, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_881C", "src/freedreno/common/freedreno_devices.py:1269", "a8xx_base_raw_magic_regs entry 10; preserve source order" },
    { A840_REG_RB_UNKNOWN_881D, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_881D", "src/freedreno/common/freedreno_devices.py:1270", "a8xx_base_raw_magic_regs entry 11; preserve source order" },
    { A840_REG_RB_UNKNOWN_881E, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_881E", "src/freedreno/common/freedreno_devices.py:1271", "a8xx_base_raw_magic_regs entry 12; preserve source order" },
    { A840_REG_RB_LRZ_CNTL2, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_LRZ_CNTL2", "src/freedreno/common/freedreno_devices.py:1272", "a8xx_base_raw_magic_regs entry 13; preserve source order" },
    { A840_REG_RB_RESOLVE_CNTL_5, UINT32_C(1), UINT32_C(1), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_RESOLVE_CNTL_5", "src/freedreno/common/freedreno_devices.py:1273", "a8xx_base_raw_magic_regs entry 14; preserve source order" },
    { A840_REG_SP_UNKNOWN_AB01, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_UNKNOWN_AB01", "src/freedreno/common/freedreno_devices.py:1275", "a8xx_base_raw_magic_regs entry 15; preserve source order" },
    { A840_REG_SP_HLSQ_MODE_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_HLSQ_MODE_CNTL", "src/freedreno/common/freedreno_devices.py:1276", "a8xx_base_raw_magic_regs entry 16; preserve source order" },
    { A840_REG_SP_UNKNOWN_AB23, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_UNKNOWN_AB23", "src/freedreno/common/freedreno_devices.py:1277", "a8xx_base_raw_magic_regs entry 17; preserve source order" },
    { A840_REG_TPL1_PS_ROTATION_CNTL, UINT32_C(4), UINT32_C(4), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_TPL1_PS_ROTATION_CNTL", "src/freedreno/common/freedreno_devices.py:1279", "a8xx_base_raw_magic_regs entry 18; preserve source order" },
    { A840_REG_TPL1_PS_SWIZZLE_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_TPL1_PS_SWIZZLE_CNTL", "src/freedreno/common/freedreno_devices.py:1280", "a8xx_base_raw_magic_regs entry 19; preserve source order" },
    { A840_REG_PC_UNKNOWN_980B, UINT32_C(8389248), UINT32_C(8389248), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_PC_UNKNOWN_980B", "src/freedreno/common/freedreno_devices.py:1282", "a8xx_base_raw_magic_regs entry 20; preserve source order" },
    { A840_REG_PC_MODE_CNTL, UINT32_C(16128), UINT32_C(16128), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_PC_MODE_CNTL", "src/freedreno/common/freedreno_devices.py:1283", "a8xx_base_raw_magic_regs entry 21; preserve source order" },
    { A840_REG_GRAS_LRZ_QUALITY_LOOKUP_TABLE_0, UINT32_C(156305472), UINT32_C(156305472), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_LRZ_QUALITY_LOOKUP_TABLE_0", "src/freedreno/vulkan/tu_cmd_buffer.cc:2190", "fd_gras_shading_rate_lut(0); src/freedreno/common/freedreno_vrs.h:35-104" },
    { A840_REG_GRAS_LRZ_QUALITY_LOOKUP_TABLE_1, UINT32_C(2658), UINT32_C(2658), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_LRZ_QUALITY_LOOKUP_TABLE_1", "src/freedreno/vulkan/tu_cmd_buffer.cc:2192", "fd_gras_shading_rate_lut(1); src/freedreno/common/freedreno_vrs.h:35-104" },
    { A840_REG_SP_PERFCTR_SHADER_MASK, UINT32_C(63), UINT32_C(63), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_PERFCTR_SHADER_MASK", "src/freedreno/vulkan/tu_cmd_buffer.cc:2199", "tu6_init_static_regs<A8XX>" },
    { A840_REG_SP_GFX_USIZE, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_GFX_USIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2207", "tu6_init_static_regs<A8XX>" },
    { A840_REG_TPL1_PS_ROTATION_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_TPL1_PS_ROTATION_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2208", "source reset after raw-magic value 4" },
    { A840_REG_SP_UNKNOWN_A9A8, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_UNKNOWN_A9A8", "src/freedreno/vulkan/tu_cmd_buffer.cc:2211", "tu6_init_static_regs<A8XX>" },
    { A840_REG_SP_MODE_CNTL, UINT32_C(5), UINT32_C(5), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2212", "tu6_init_static_regs<A8XX>" },
    { A840_REG_VFD_MODE_CNTL, UINT32_C(3), UINT32_C(3), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_VFD_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2216", "tu6_init_static_regs<A8XX>" },
    { A840_REG_RB_MODE_CNTL, UINT32_C(16), UINT32_C(16), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2217", "tu6_init_static_regs<A8XX>" },
    { A840_REG_GRAS_MODE_CNTL, UINT32_C(2), UINT32_C(2), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2219", "tu6_init_static_regs<A8XX>" },
    { A840_REG_RB_UNKNOWN_8818, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_UNKNOWN_8818", "src/freedreno/vulkan/tu_cmd_buffer.cc:2221", "tu6_init_static_regs<A8XX>" },
    { A840_REG_VPC_REPLACE_MODE_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_VPC_REPLACE_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2234", "tu6_init_static_regs<A8XX>" },
    { A840_REG_VPC_ROTATION_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_VPC_ROTATION_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2235", "tu6_init_static_regs<A8XX>" },
    { A840_REG_VPC_SO_OVERRIDE, UINT32_C(1), UINT32_C(1), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_VPC_SO_OVERRIDE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2237", "tu6_init_static_regs<A8XX>" },
    { A840_REG_TPL1_PS_SWIZZLE_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_TPL1_PS_SWIZZLE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2239", "tu6_init_static_regs<A8XX>" },
    { A840_REG_GRAS_SC_SCREEN_SCISSOR_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_GRAS_SC_SCREEN_SCISSOR_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2241", "tu6_init_static_regs<A8XX>" },
    { A840_REG_TPL1_MODE_CNTL, UINT32_C(4258), UINT32_C(4258), A840_CTX_STATIC, A840_CTX_NEAREST, 1u, "A840_REG_TPL1_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2255", "base truncate; params.use_tex_coord_nearest_even changes XML TEXCOORDROUNDMODE bit2" },
    { A840_REG_SP_REG_PROG_ID_3, UINT32_C(252), UINT32_C(252), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_REG_PROG_ID_3", "src/freedreno/vulkan/tu_cmd_buffer.cc:2262", "tu6_init_static_regs<A8XX>" },
    { A840_REG_VFD_RENDER_MODE, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_VFD_RENDER_MODE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2264", "tu6_init_static_regs<A8XX>" },
    { A840_REG_RB_ALPHA_TEST_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_RB_ALPHA_TEST_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2266", "tu6_init_static_regs<A8XX>" },
    { A840_REG_SP_ALPHA_TEST_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_ALPHA_TEST_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2268", "tu6_init_static_regs<A8XX>" },
    { A840_REG_TPL1_GFX_BORDER_COLOR_BASE, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_BORDER_VA, 2u, "A840_REG_TPL1_GFX_BORDER_COLOR_BASE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2270", "runtime border_color_gpu_va, 128-byte aligned; value column is placeholder, emits two dwords" },
    { A840_REG_TPL1_CS_BORDER_COLOR_BASE, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_BORDER_VA, 2u, "A840_REG_TPL1_CS_BORDER_COLOR_BASE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2271", "runtime border_color_gpu_va, 128-byte aligned; value column is placeholder, emits two dwords" },
    { A840_REG_PC_HS_BUFFER_SIZE, UINT32_C(262144), UINT32_C(262144), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_PC_HS_BUFFER_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2299", "TU_TESS<A8XX>::PARAM_SIZE, src/freedreno/vulkan/tu_device.h:507-510" },
    { A840_REG_PC_TF_BUFFER_SIZE, UINT32_C(16448), UINT32_C(16448), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_PC_TF_BUFFER_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2304", "TU_TESS<A8XX>::FACTOR_SIZE, src/freedreno/vulkan/tu_device.h:507-510" },
    { A840_REG_SP_PS_CNTL_0, UINT32_C(0), UINT32_C(0), A840_CTX_STATIC, A840_CTX_EARLY_RESET, 1u, "A840_REG_SP_PS_CNTL_0", "src/freedreno/vulkan/tu_cmd_buffer.cc:2323", "conditionally enabled reset: params.enable_early_preamble_reset" },
    { A840_REG_SP_VS_CONST_CONFIG, UINT32_C(258), UINT32_C(258), A840_CTX_STATIC, A840_CTX_NORMAL, 1u, "A840_REG_SP_VS_CONST_CONFIG", "src/freedreno/vulkan/tu_cmd_buffer.cc:2330", "CONSTLEN 8 uses XML shr=2; ENABLED=1" },
    { A840_REG_VPC_UNKNOWN_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_RP, A840_CTX_NORMAL, 1u, "A840_REG_VPC_UNKNOWN_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2417", "tu_init_hw_rp<A8XX>" },
    { A840_REG_RB_A2D_DEST_BUFFER_ARRAY_PITCH, UINT32_C(0), UINT32_C(0), A840_CTX_RP, A840_CTX_NORMAL, 1u, "A840_REG_RB_A2D_DEST_BUFFER_ARRAY_PITCH", "src/freedreno/vulkan/tu_cmd_buffer.cc:2418", "tu_init_hw_rp<A8XX>" },
    { A840_REG_RB_CCU_CACHE_CNTL, UINT32_C(3469742064), UINT32_C(265081164), A840_CTX_RP, A840_CTX_CACHE_BR, 1u, "A840_REG_RB_CCU_CACHE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:660", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0xfccd14c; src/freedreno/common/fd6_gmem_cache.h:39-98" },
    { A840_REG_VPC_ATTR_BUF_GMEM_SIZE, UINT32_C(131072), UINT32_C(49152), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_ATTR_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:606", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0xc000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_VPC_ATTR_BUF_GMEM_BASE, UINT32_C(17891328), UINT32_C(18382848), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_ATTR_BUF_GMEM_BASE", "src/freedreno/vulkan/tu_cmd_buffer.cc:607", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x1188000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_PC_ATTR_BUF_GMEM_SIZE, UINT32_C(131072), UINT32_C(49152), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_PC_ATTR_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:608", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0xc000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_VPC_POS_BUF_GMEM_SIZE, UINT32_C(65536), UINT32_C(24576), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_POS_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:611", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x6000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_VPC_POS_BUF_GMEM_BASE, UINT32_C(17498112), UINT32_C(18235392), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_POS_BUF_GMEM_BASE", "src/freedreno/vulkan/tu_cmd_buffer.cc:612", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x1164000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_PC_POS_BUF_GMEM_SIZE, UINT32_C(65536), UINT32_C(24576), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_PC_POS_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:613", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x6000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_VPC_BV_POS_BUF_GMEM_BASE, UINT32_C(18677760), UINT32_C(18677760), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_BV_POS_BUF_GMEM_BASE", "src/freedreno/vulkan/tu_cmd_buffer.cc:614", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x11d0000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_VPC_BV_POS_BUF_GMEM_SIZE, UINT32_C(32768), UINT32_C(32768), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_VPC_BV_POS_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:615", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x8000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_PC_BV_POS_BUF_GMEM_SIZE, UINT32_C(32768), UINT32_C(32768), A840_CTX_RP, A840_CTX_CACHE_BOTH, 1u, "A840_REG_PC_BV_POS_BUF_GMEM_SIZE", "src/freedreno/vulkan/tu_cmd_buffer.cc:616", "fd6_calc_gmem_cache_offsets A8XX uses num_ccu=6 times each cache/VPC size; sysmem value here, bin_gmem_value=0x8000; per-device default profile from frozen chip and pinned a8xx_gen2" },
    { A840_REG_RB_BUFFER_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_RP, A840_CTX_BIN_BR, 1u, "A840_REG_RB_BUFFER_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:1531", "bin BR-only tile begin, skipped by context_init" },
    { A840_REG_RB_CLEAR_TARGET, UINT32_C(1), UINT32_C(1), A840_CTX_RP, A840_CTX_BIN_BR, 1u, "A840_REG_RB_CLEAR_TARGET", "src/freedreno/vulkan/tu_cmd_buffer.cc:1532", "bin BR-only GMEM clear mode, skipped by context_init" },
    { A840_REG_RB_DBG_ECO_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_RB_DBG_ECO_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2279", "Gen8 KMD-only RB_DBG_ECO_CNTL (CHIP < A8XX branch only); value is not emitted" },
    { A840_REG_RB_RBP_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_RB_RBP_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2281", "Gen8 KMD-only RB_RBP_CNTL (CHIP < A8XX branch only); value is not emitted" },
    { A840_REG_RB_GC_GMEM_PROTECT, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_RB_GC_GMEM_PROTECT", "src/freedreno/vulkan/tu_cmd_buffer.cc:2284", "legacy RB_UNKNOWN_8E09 shares this address; value7 only Gen7, never UMD Gen8; value is not emitted" },
    { A840_REG_SP_NC_MODE_CNTL_2, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_SP_NC_MODE_CNTL_2", "src/freedreno/vulkan/tu_cmd_buffer.cc:2197", "CHIP < A8XX only; no Gen8 UMD programming; value is not emitted" },
    { A840_REG_VPC_LB_MODE_CNTL, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_VPC_LB_MODE_CNTL", "src/freedreno/vulkan/tu_cmd_buffer.cc:2251", "CHIP < A8XX only; no Gen8 UMD programming; value is not emitted" },
    { A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE, UINT32_C(0), UINT32_C(0), A840_CTX_UMD_BLOCKED, A840_CTX_BLOCKED, 1u, "A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE", "src/freedreno/vulkan/tu_cmd_buffer.cc:2252", "CHIP < A8XX only; no Gen8 UMD programming; value is not emitted" },
};
static const char context_tsv_sha[] = "9266ed2506b225338150fe2ae4a9cddd9dd9b6eb7aaa1d9573b1c3fb5d2ac657";
_Static_assert(sizeof(context_regs) / sizeof(context_regs[0]) ==
    A840_CONTEXT_INIT_TABLE_ROWS, "context table row count");

const A840ContextInitReg *a840_context_init_table(size_t *count)
{
    if (count != NULL)
        *count = sizeof(context_regs) / sizeof(context_regs[0]);
    return context_regs;
}

const char *a840_context_init_tsv_sha256(void)
{
    return context_tsv_sha;
}

int a840_reg_is_umd_blocked_on_a8xx(uint32_t reg_offset)
{
    size_t i;
    for (i = 0; i < A840_CONTEXT_INIT_TABLE_ROWS; ++i)
        if (context_regs[i].scope == A840_CTX_UMD_BLOCKED &&
            context_regs[i].reg == reg_offset)
            return 1;
    return 0;
}

typedef struct {
    uintptr_t address;
    size_t bytes;
} CtxSpan;

static A840EmitStatus span(const void *pointer, size_t count, size_t size,
    size_t alignment, CtxSpan *out)
{
    out->address = (uintptr_t)pointer;
    out->bytes = 0;
    if (count == 0)
        return A840_EMIT_OK;
    if (pointer == NULL)
        return A840_EMIT_INVALID_ARG;
    if (count > SIZE_MAX / size)
        return A840_EMIT_RANGE_ERROR;
    if (out->address % alignment)
        return A840_EMIT_ALIGNMENT;
    out->bytes = count * size;
    if (out->bytes > UINTPTR_MAX - out->address)
        return A840_EMIT_RANGE_ERROR;
    return A840_EMIT_OK;
}

static int overlap(CtxSpan a, CtxSpan b)
{
    if (a.bytes == 0 || b.bytes == 0)
        return 0;
    if (a.address <= b.address)
        return b.address - a.address < a.bytes;
    return a.address - b.address < b.bytes;
}

static A840EmitStatus storage(const A840CmdStream *cs,
    const A840ContextInitParams *params)
{
    CtxSpan meta, words, relocs, input;
    A840EmitStatus status;
    status = span(cs, 1, sizeof(*cs), _Alignof(A840CmdStream), &meta);
    if (status != A840_EMIT_OK)
        return status;
    if (cs->used_dwords > cs->capacity_dwords ||
        cs->reloc_count > cs->reloc_capacity)
        return A840_EMIT_INVALID_ARG;
    status = span(cs->words, cs->capacity_dwords, sizeof(*cs->words),
        _Alignof(uint32_t), &words);
    if (status != A840_EMIT_OK)
        return status;
    status = span(cs->relocs, cs->reloc_capacity, sizeof(*cs->relocs),
        _Alignof(A840Reloc), &relocs);
    if (status != A840_EMIT_OK)
        return status;
    status = span(params, 1, sizeof(*params), _Alignof(A840ContextInitParams), &input);
    if (status != A840_EMIT_OK)
        return status;
    if (overlap(meta, words) || overlap(meta, relocs) || overlap(words, relocs) ||
        overlap(input, meta) || overlap(input, words) || overlap(input, relocs))
        return A840_EMIT_ALIAS;
    return A840_EMIT_OK;
}

static A840EmitStatus gpu_range(uint64_t va, uint64_t bytes, uint32_t alignment)
{
    const uint64_t limit = UINT64_C(1) << 48;
    if (va == 0)
        return A840_EMIT_INVALID_ARG;
    if (va >= limit || bytes > limit - va)
        return A840_EMIT_RANGE_ERROR;
    if (va % alignment)
        return A840_EMIT_ALIGNMENT;
    return A840_EMIT_OK;
}

static A840EmitStatus common_params(const A840ContextInitParams *p)
{
    if (p->use_tex_coord_nearest_even > 1 ||
        p->enable_early_preamble_reset > 1)
        return A840_EMIT_INVALID_ARG;
    return gpu_range(p->border_color_gpu_va, 0, A840_CONTEXT_BORDER_ALIGNMENT);
}

static A840EmitStatus amble_range(uint64_t va, uint32_t dwords, int required)
{
    if (dwords > A840_CONTEXT_AMBLE_MAX_DWORDS)
        return A840_EMIT_RANGE_ERROR;
    if (dwords == 0)
        return (va == 0 && !required) ? A840_EMIT_OK : A840_EMIT_INVALID_ARG;
    return gpu_range(va, (uint64_t)dwords * 4, 4);
}

typedef struct {
    uint32_t words[A840_CTX_INIT_MAX_DWORDS];
    A840Reloc relocs[A840_CTX_INIT_MAX_RELOCS];
    size_t used;
    size_t reloc_count;
} CtxStage;

static uint32_t odd_bit(uint32_t value)
{
    uint32_t parity = 1;
    while (value != 0) {
        parity ^= 1;
        value &= value - 1;
    }
    return parity;
}

/* Same odd-parity/header/count profile as the read-only a840_cs.c.
 * Only caller-owned staging is touched here. No external hard symbols.
 */
static A840EmitStatus packet(CtxStage *st, uint32_t type, uint32_t id,
    const uint32_t *payload, uint32_t count)
{
    uint32_t header, i;
    if (type == 4) {
        const uint32_t limit = UINT32_C(1) << ADRENO840_PM4_MAX_REG_BITS;
        if (count == 0 || count > ADRENO840_PM4_MAX_TYPE4_COUNT ||
            id >= limit || count > limit - id)
            return A840_EMIT_RANGE_ERROR;
        for (i = 0; i < count; ++i)
            if (a840_reg_is_umd_blocked_on_a8xx(id + i))
                return A840_EMIT_UNSUPPORTED;
        header = (UINT32_C(4) << 28) | count | (odd_bit(count) << 7) |
            (id << 8) | (odd_bit(id) << 27);
    } else {
        if (type != 7 || id >= (UINT32_C(1) << ADRENO840_PM4_MAX_OPCODE_BITS) ||
            count > ADRENO840_PM4_MAX_TYPE7_COUNT)
            return A840_EMIT_RANGE_ERROR;
        header = (UINT32_C(7) << 28) | count | (odd_bit(count) << 15) |
            (id << 16) | (odd_bit(id) << 23);
    }
    if ((size_t)count + 1 > A840_CTX_INIT_MAX_DWORDS - st->used)
        return A840_EMIT_CAPACITY;
    st->words[st->used++] = header;
    if (count != 0) {
        memcpy(st->words + st->used, payload, (size_t)count * sizeof(*payload));
        st->used += count;
    }
    return A840_EMIT_OK;
}

static A840EmitStatus reg32(CtxStage *st, uint32_t reg, uint32_t value)
{
    return packet(st, 4, reg, &value, 1);
}

static A840EmitStatus relocation(CtxStage *st, size_t word,
    uint64_t bo, uint64_t va)
{
    A840Reloc *r;
    if (st->reloc_count >= A840_CTX_INIT_MAX_RELOCS)
        return A840_EMIT_CAPACITY;
    r = &st->relocs[st->reloc_count++];
    r->dword_offset = (uint32_t)word;
    r->flags = A840_RELOC_READ;
    r->bo_handle = bo;
    r->gpu_va = va;
    return A840_EMIT_OK;
}

static A840EmitStatus reg64(CtxStage *st, uint32_t reg, uint64_t bo, uint64_t va)
{
    uint32_t values[2] = { (uint32_t)va, (uint32_t)(va >> 32) };
    size_t start = st->used;
    A840EmitStatus status = packet(st, 4, reg, values, 2);
    if (status != A840_EMIT_OK)
        return status;
    return relocation(st, start + 1, bo, va);
}

static A840EmitStatus amble(CtxStage *st, uint64_t va, uint32_t dwords,
    uint32_t type)
{
    uint32_t payload[3] = { (uint32_t)va, (uint32_t)(va >> 32), dwords | (type << 20) };
    size_t start = st->used;
    A840EmitStatus status = packet(st, 7, A840_CP_SET_AMBLE, payload, 3);
    if (status != A840_EMIT_OK || va == 0)
        return status;
    return relocation(st, start + 1, 0, va);
}

static A840EmitStatus thread(CtxStage *st, uint32_t value)
{
    return packet(st, 7, A840_CP_THREAD_CONTROL, &value, 1);
}

#define CTX_TRY(call) do { A840EmitStatus rc_ = (call); if (rc_ != A840_EMIT_OK) return rc_; } while (0)

static A840EmitStatus static_regs(CtxStage *st, const A840ContextInitParams *p)
{
    size_t i;
    for (i = 0; i < A840_CONTEXT_INIT_TABLE_ROWS; ++i) {
        const A840ContextInitReg *r = &context_regs[i];
        uint32_t value = r->value;
        if (r->scope != A840_CTX_STATIC)
            continue;
        if (r->role == A840_CTX_EARLY_RESET && !p->enable_early_preamble_reset)
            continue;
        if (r->role == A840_CTX_BORDER_VA) {
            CTX_TRY(reg64(st, r->reg, p->border_color_bo, p->border_color_gpu_va));
        } else {
            if (r->role == A840_CTX_NEAREST && p->use_tex_coord_nearest_even)
                value |= UINT32_C(1) << 2;
            CTX_TRY(reg32(st, r->reg, value));
        }
    }
    return A840_EMIT_OK;
}

static A840EmitStatus commit(A840CmdStream *cs, const CtxStage *st)
{
    size_t i, start = cs->used_dwords;
    if (st->used > cs->capacity_dwords - start)
        return A840_EMIT_CAPACITY;
    if (cs->relocs != NULL) {
        if (st->reloc_count > cs->reloc_capacity - cs->reloc_count)
            return A840_EMIT_CAPACITY;
        for (i = 0; i < st->reloc_count; ++i)
            if (start > UINT32_MAX - (size_t)st->relocs[i].dword_offset)
                return A840_EMIT_RANGE_ERROR;
    }
    /* No fallible operation beyond this point: words and relocs commit once. */
    memcpy(cs->words + start, st->words, st->used * sizeof(*cs->words));
    if (cs->relocs != NULL) {
        for (i = 0; i < st->reloc_count; ++i) {
            A840Reloc value = st->relocs[i];
            value.dword_offset += (uint32_t)start;
            cs->relocs[cs->reloc_count + i] = value;
        }
        cs->reloc_count += st->reloc_count;
    }
    cs->used_dwords += st->used;
    return A840_EMIT_OK;
}

A840EmitStatus a840_emit_context_init(A840CmdStream *cs,
    const A840ContextInitParams *p)
{
    CtxStage st;
    size_t i;
    uint32_t value;
    uint32_t counts[2];
    uint32_t predicates[3];
    CTX_TRY(storage(cs, p));
    CTX_TRY(common_params(p));
    if (p->vis_stream_count == 0 || p->vis_stream_count > A840_CONTEXT_VIS_STREAM_MAX)
        return A840_EMIT_RANGE_ERROR;
    CTX_TRY(amble_range(p->bin_preamble_br_va, p->bin_preamble_br_dwords, 1));
    CTX_TRY(amble_range(p->bin_preamble_bv_va, p->bin_preamble_bv_dwords, 1));
    CTX_TRY(amble_range(p->preempt_preamble_va, p->preempt_preamble_dwords, 0));
    CTX_TRY(amble_range(p->preempt_postamble_va, p->preempt_postamble_dwords, 0));
    st.used = st.reloc_count = 0;
    CTX_TRY(thread(&st, 3)); /* CP_SET_THREAD_BOTH, source cp_thread enum */
    value = A840_CTX_EVENT_CCU_INVALIDATE_COLOR;
    CTX_TRY(packet(&st, 7, A840_CP_EVENT_WRITE7, &value, 1));
    value = A840_CTX_EVENT_CCU_INVALIDATE_DEPTH;
    CTX_TRY(packet(&st, 7, A840_CP_EVENT_WRITE7, &value, 1));
    value = A840_CTX_EVENT_LRZ_INVALIDATE;
    CTX_TRY(packet(&st, 7, A840_CP_EVENT_WRITE7, &value, 1));
    value = A840_CTX_EVENT_CACHE_INVALIDATE;
    CTX_TRY(packet(&st, 7, A840_CP_EVENT_WRITE7, &value, 1));
    CTX_TRY(packet(&st, 7, A840_CP_WAIT_FOR_IDLE, NULL, 0));
    /* A8XX XML exposes only six stage bits; do not copy the A7XX mask. */
    CTX_TRY(reg32(&st, A840_REG_SP_UPDATE_CNTL, (UINT32_C(1) << 6) - 1));
    CTX_TRY(packet(&st, 7, A840_CP_WAIT_FOR_IDLE, NULL, 0));
    CTX_TRY(static_regs(&st, p));
    for (i = 0; i < A840_CONTEXT_INIT_TABLE_ROWS; ++i) {
        const A840ContextInitReg *r = &context_regs[i];
        if (r->scope == A840_CTX_RP && r->role != A840_CTX_BIN_BR)
            CTX_TRY(reg32(&st, r->reg, r->value));
    }
    value = UINT32_C(1) << 18; /* DISABLE_ALL_GROUPS */
    {
        uint32_t disable[3] = { value, 0, 0 };
        CTX_TRY(packet(&st, 7, A840_CP_SET_DRAW_STATE, disable, 3));
    }
    counts[0] = 2; /* PIPE_SET_BR_OFFSET */
    counts[1] = p->vis_stream_count;
    CTX_TRY(packet(&st, 7, A840_CP_BV_BR_COUNT_OPS, counts, 2));
    CTX_TRY(thread(&st, 1u | (UINT32_C(1) << 27)));
    CTX_TRY(amble(&st, p->bin_preamble_br_va, p->bin_preamble_br_dwords, 1));
    CTX_TRY(thread(&st, 2));
    CTX_TRY(amble(&st, p->bin_preamble_bv_va, p->bin_preamble_bv_dwords, 1));
    CTX_TRY(thread(&st, 3));
    predicates[0] = (UINT32_C(1) << 31) | (UINT32_C(1) << 25);
    predicates[1] = (UINT32_C(1) << 3) | (UINT32_C(1) << 4);
    predicates[2] = UINT32_C(1) << 4;
    CTX_TRY(packet(&st, 7, A840_CP_REG_TEST, predicates, 3));
    CTX_TRY(amble(&st, p->preempt_preamble_va, p->preempt_preamble_dwords, 0));
    CTX_TRY(amble(&st, p->preempt_postamble_va, p->preempt_postamble_dwords, 2));
    CTX_TRY(thread(&st, 1u | (UINT32_C(1) << 27)));
    return commit(cs, &st);
}

A840EmitStatus a840_emit_bin_preamble(A840CmdStream *cs,
    const A840ContextInitParams *p, int is_bv_thread)
{
    CtxStage st;
    size_t i;
    CTX_TRY(storage(cs, p));
    CTX_TRY(common_params(p));
    if (is_bv_thread != 0 && is_bv_thread != 1)
        return A840_EMIT_INVALID_ARG;
    st.used = st.reloc_count = 0;
    CTX_TRY(static_regs(&st, p));
    for (i = 0; i < A840_CONTEXT_INIT_TABLE_ROWS; ++i) {
        const A840ContextInitReg *r = &context_regs[i];
        if (r->scope != A840_CTX_RP)
            continue;
        if (r->role == A840_CTX_CACHE_BOTH ||
            ((r->role == A840_CTX_CACHE_BR || r->role == A840_CTX_BIN_BR) && !is_bv_thread))
            CTX_TRY(reg32(&st, r->reg, r->bin_value));
    }
    return commit(cs, &st);
}
#undef CTX_TRY
