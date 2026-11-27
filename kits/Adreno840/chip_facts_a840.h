/* SPDX-License-Identifier: MIT */
#ifndef CHIP_FACTS_A840_H
#define CHIP_FACTS_A840_H

#include <stddef.h>
#include <stdint.h>

/*
 * W00 facts for the three-slice Adreno 840 v2 on Kaanapali.
 * NezhaGpu.c:3135-3143 and NezhaAdrenoGpu.cpp:424 program/report HBB 16.
 * Pinned Mesa 85fda3b36405 __info29 uses HBB 15, inherited from the catalog
 * constructor, not a Kaanapali runtime query. Do not silently substitute 15.
 * The Mesa 0xffff44050a31 catalog key has wildcard fuse bits; it is not the
 * 32-bit chip_id returned by NativeAdreno's GetDeviceInfo.
 */
#define A840_KAANAPALI_CHIP_ID UINT64_C(0x44050a31)
#define A840_MESA_CATALOG_CHIP_ID UINT64_C(0xffff44050a31)
#define A840_GMEM_RANGE_MIN UINT64_C(0x04000000)
#define A840_UCHE_TRAP_BASE UINT64_C(0x1fffffffff000)

enum {
    A840_KAANAPALI_HIGHEST_BANK_BIT = 16,
    A840_MESA_HIGHEST_BANK_BIT = 15,
    A840_KAANAPALI_GMEM_BYTES = 18u * 1024u * 1024u,
    /* W00 API granularity, not a GMEM_PROTECT register encoding. */
    A840_GMEM_BYTES_ALIGNMENT = 4096,
    A840_CHIP_FACTS_BYTES = 168
};

/*
 * Logical UBWC configuration, not a hardware register image.
 * NezhaGpu.c:3135-3136: YUVNOTCOMPTOFC, RGB565_PREDICATOR, AMSBC,
 * UAVFLAGPRD_INV=2, UBWC_MODE=0. Encoder version 6 != version tag 5.
 */
enum {
    A840_UBWC_YUV_NOT_COMP_TO_FC = 1u << 0,
    A840_UBWC_RGB565_PREDICATOR = 1u << 1,
    A840_UBWC_AMSBC = 1u << 2,
    A840_UBWC_UAVFLAGPRD_INV_SHIFT = 3,
    A840_UBWC_UAVFLAGPRD_INV_MASK = 3u << 3,
    A840_UBWC_MODE_SHIFT = 5,
    A840_UBWC_MODE_MASK = 3u << 5,
    A840_UBWC_KAANAPALI_FLAGS =
        A840_UBWC_YUV_NOT_COMP_TO_FC |
        A840_UBWC_RGB565_PREDICATOR |
        A840_UBWC_AMSBC | (2u << A840_UBWC_UAVFLAGPRD_INV_SHIFT)
};

/*
 * Fixed-width masks avoid implementation-defined C/C++ bit-field layout.
 * The first nine bits are pinned Mesa source declarations; the last is the
 * kernel's GMEM save/restore enable. These are not Metal or ABI capability
 * flags and do not certify runtime support or preemption on the device.
 */
enum {
    A840_FEATURE_HAS_EARLY_PREAMBLE = 1u << 0,
    A840_FEATURE_HAS_GMEM_FAST_CLEAR = 1u << 1,
    A840_FEATURE_HAS_COHERENT_UBWC_FLAG_CACHES = 1u << 2,
    A840_FEATURE_SUPPORTS_UAV_UBWC = 1u << 3,
    A840_FEATURE_UBWC_COHERENCY_QUIRK = 1u << 4,
    A840_FEATURE_R8G8_FAULTY_FAST_CLEAR_QUIRK = 1u << 5,
    A840_FEATURE_PREDE_NOP_QUIRK = 1u << 6,
    A840_FEATURE_PREDTF_NOP_QUIRK = 1u << 7,
    A840_FEATURE_HAS_SALU_INT_NARROWING_QUIRK = 1u << 8,
    A840_FEATURE_HAS_GMEM_SAVE_RESTORE = 1u << 9,
    A840_KAANAPALI_FEATURES = (1u << 10) - 1u
};

typedef struct A840ChipFacts {
    uint64_t chip_id;
    uint64_t gmem_range_min;
    uint64_t uche_trap_base;

    uint32_t chip_gen;
    uint32_t num_slices;
    uint32_t num_ccu;
    uint32_t num_sp_cores;

    uint32_t gmem_bytes;
    uint32_t gmem_align_w;
    uint32_t gmem_align_h;
    uint32_t tile_align_w;
    uint32_t tile_align_h;
    uint32_t tile_max_w;
    uint32_t tile_max_h;
    uint32_t num_vsc_pipes;

    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    uint32_t ubwc_version_tag;
    uint32_t ubwc_flags;

    uint32_t cs_shared_mem_size;
    uint32_t fibers_per_sp;
    uint32_t threadsize_base;
    uint32_t max_waves;
    uint32_t wave_granularity;
    uint32_t reg_size_vec4;
    uint32_t instr_cache_size;
    uint32_t max_draw_states;
    uint32_t vs_max_inputs_count;
    uint32_t max_sets; /* Mesa __info29; ADRENO_COMPILE_MAX_BIND_SETS. */

    uint32_t sysmem_per_ccu_color_cache_size;
    uint32_t sysmem_per_ccu_depth_cache_size;
    uint32_t gmem_per_ccu_color_cache_size;
    uint32_t gmem_per_ccu_depth_cache_size;
    uint32_t sysmem_vpc_attr_buf_size;
    uint32_t gmem_vpc_attr_buf_size;

    uint32_t features;
    uint32_t smmu_page_shift;
} A840ChipFacts;

#if defined(__cplusplus)
static_assert(sizeof(A840ChipFacts) == A840_CHIP_FACTS_BYTES,
              "A840ChipFacts size locked");
static_assert(offsetof(A840ChipFacts, chip_gen) == 24,
              "A840ChipFacts scalar offset locked");
static_assert(offsetof(A840ChipFacts, smmu_page_shift) == 164,
              "A840ChipFacts tail offset locked");
#else
_Static_assert(sizeof(A840ChipFacts) == A840_CHIP_FACTS_BYTES,
               "A840ChipFacts size locked");
_Static_assert(offsetof(A840ChipFacts, chip_gen) == 24,
               "A840ChipFacts scalar offset locked");
_Static_assert(offsetof(A840ChipFacts, smmu_page_shift) == 164,
               "A840ChipFacts tail offset locked");
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Immutable process-lifetime default; copy before applying runtime facts. */
const A840ChipFacts *a840_chip_facts_kaanapali_default(void);

/*
 * 0 is valid; nonzero is invalid. A writable, nonempty err_buf receives a
 * NUL-terminated reason (possibly truncated), or an empty string on success.
 * NULL/zero-sized error buffers are supported.
 * Only chip_id, gmem_bytes and highest_bank_bit are runtime inputs. Other
 * fields must retain this W00 profile. GMEM must be positive, 4 KiB aligned
 * and <= the reported 18 MiB capacity. W00 accepts the two documented HBB
 * profiles (15/16); it makes no claim about other platforms' legal HBB values.
 */
int a840_chip_facts_validate(const A840ChipFacts *facts,
                             char *err_buf, size_t err_bytes);

/*
 * Apply a GetDeviceInfo triple to a copy of the default/profile.
 * Requires the exact three-slice chip_id, not a wildcard/fuse catalog key.
 * Validate the complete candidate before assigning; failure leaves every
 * byte of *facts unchanged. NULL facts is rejected.
 */
int a840_chip_facts_apply_runtime(A840ChipFacts *facts,
                                  uint64_t runtime_chip_id,
                                  uint32_t runtime_gmem_bytes,
                                  uint32_t runtime_hbb);

#ifdef __cplusplus
}
#endif

#endif /* CHIP_FACTS_A840_H */
