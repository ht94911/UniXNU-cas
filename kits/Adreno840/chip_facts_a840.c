/* SPDX-License-Identifier: MIT */
#include "chip_facts_a840.h"

/* Values and provenance are itemized in A840_HARDWARE_AUDIT.md. */
static const A840ChipFacts kaanapali_facts = {
    .chip_id = A840_KAANAPALI_CHIP_ID,
    .gmem_range_min = A840_GMEM_RANGE_MIN,
    .uche_trap_base = A840_UCHE_TRAP_BASE,
    .chip_gen = 8,
    .num_slices = 3,
    .num_ccu = 6,
    .num_sp_cores = 6,
    .gmem_bytes = A840_KAANAPALI_GMEM_BYTES,
    .gmem_align_w = 16,
    .gmem_align_h = 4,
    .tile_align_w = 96,
    .tile_align_h = 32,
    .tile_max_w = 16416,
    .tile_max_h = 16384,
    .num_vsc_pipes = 32,
    .ubwc_version = 6,
    .highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT,
    .ubwc_swizzle = 6,
    .macrotile_mode = 1,
    .ubwc_version_tag = 5,
    .ubwc_flags = A840_UBWC_KAANAPALI_FLAGS,
    .cs_shared_mem_size = 32768,
    .fibers_per_sp = 4096,
    .threadsize_base = 64,
    .max_waves = 16,
    .wave_granularity = 2,
    .reg_size_vec4 = 128,
    .instr_cache_size = 127,
    .max_draw_states = 64,
    .vs_max_inputs_count = 32,
    .max_sets = 8,
    .sysmem_per_ccu_color_cache_size = 131072,
    .sysmem_per_ccu_depth_cache_size = 196608,
    .gmem_per_ccu_color_cache_size = 16384,
    .gmem_per_ccu_depth_cache_size = 262144,
    .sysmem_vpc_attr_buf_size = 131072,
    .gmem_vpc_attr_buf_size = 49152,
    .features = A840_KAANAPALI_FEATURES,
    .smmu_page_shift = 12
};

const A840ChipFacts *
a840_chip_facts_kaanapali_default(void)
{
    return &kaanapali_facts;
}

static void
write_reason(char *err_buf, size_t err_bytes, const char *reason)
{
    size_t i = 0;
    if (err_buf == NULL || err_bytes == 0) {
        return;
    }
    while (i < err_bytes - 1 && reason[i] != '\0') {
        err_buf[i] = reason[i];
        ++i;
    }
    err_buf[i] = '\0';
}

int
a840_chip_facts_validate(const A840ChipFacts *facts,
                         char *err_buf, size_t err_bytes)
{
    if (facts == NULL) {
        write_reason(err_buf, err_bytes, "facts pointer is null");
        return 1;
    }
    if (facts->chip_id != A840_KAANAPALI_CHIP_ID) {
        write_reason(err_buf, err_bytes,
                     "chip_id must be the three-slice A840 v2 0x44050a31");
        return 1;
    }
    if (facts->highest_bank_bit != A840_KAANAPALI_HIGHEST_BANK_BIT &&
        facts->highest_bank_bit != A840_MESA_HIGHEST_BANK_BIT) {
        write_reason(err_buf, err_bytes,
                     "highest_bank_bit must be 15 or 16 (W00 profiles)");
        return 1;
    }
    if (facts->gmem_bytes == 0 ||
        facts->gmem_bytes > A840_KAANAPALI_GMEM_BYTES ||
        facts->gmem_bytes % A840_GMEM_BYTES_ALIGNMENT != 0) {
        write_reason(err_buf, err_bytes,
                     "gmem_bytes must be positive, 4 KiB aligned, <= 18 MiB");
        return 1;
    }

#define A840_CHECK_FIXED(member)                                             \
    do {                                                                     \
        if (facts->member != kaanapali_facts.member) {                         \
            write_reason(err_buf, err_bytes,                                  \
                         #member " differs from the fixed W00 A840 profile"); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

    A840_CHECK_FIXED(gmem_range_min);
    A840_CHECK_FIXED(uche_trap_base);
    A840_CHECK_FIXED(chip_gen);
    A840_CHECK_FIXED(num_slices);
    A840_CHECK_FIXED(num_ccu);
    A840_CHECK_FIXED(num_sp_cores);
    A840_CHECK_FIXED(gmem_align_w);
    A840_CHECK_FIXED(gmem_align_h);
    A840_CHECK_FIXED(tile_align_w);
    A840_CHECK_FIXED(tile_align_h);
    A840_CHECK_FIXED(tile_max_w);
    A840_CHECK_FIXED(tile_max_h);
    A840_CHECK_FIXED(num_vsc_pipes);
    A840_CHECK_FIXED(ubwc_version);
    A840_CHECK_FIXED(ubwc_swizzle);
    A840_CHECK_FIXED(macrotile_mode);
    A840_CHECK_FIXED(ubwc_version_tag);
    A840_CHECK_FIXED(ubwc_flags);
    A840_CHECK_FIXED(cs_shared_mem_size);
    A840_CHECK_FIXED(fibers_per_sp);
    A840_CHECK_FIXED(threadsize_base);
    A840_CHECK_FIXED(max_waves);
    A840_CHECK_FIXED(wave_granularity);
    A840_CHECK_FIXED(reg_size_vec4);
    A840_CHECK_FIXED(instr_cache_size);
    A840_CHECK_FIXED(max_draw_states);
    A840_CHECK_FIXED(vs_max_inputs_count);
    A840_CHECK_FIXED(max_sets);
    A840_CHECK_FIXED(sysmem_per_ccu_color_cache_size);
    A840_CHECK_FIXED(sysmem_per_ccu_depth_cache_size);
    A840_CHECK_FIXED(gmem_per_ccu_color_cache_size);
    A840_CHECK_FIXED(gmem_per_ccu_depth_cache_size);
    A840_CHECK_FIXED(sysmem_vpc_attr_buf_size);
    A840_CHECK_FIXED(gmem_vpc_attr_buf_size);
    A840_CHECK_FIXED(features);
    A840_CHECK_FIXED(smmu_page_shift);
#undef A840_CHECK_FIXED

    write_reason(err_buf, err_bytes, "");
    return 0;
}

int
a840_chip_facts_apply_runtime(A840ChipFacts *facts,
                              uint64_t runtime_chip_id,
                              uint32_t runtime_gmem_bytes,
                              uint32_t runtime_hbb)
{
    A840ChipFacts candidate;
    if (facts == NULL || runtime_chip_id != A840_KAANAPALI_CHIP_ID) {
        return 1;
    }

    candidate = *facts;
    candidate.chip_id = runtime_chip_id;
    candidate.gmem_bytes = runtime_gmem_bytes;
    candidate.highest_bank_bit = runtime_hbb;
    if (a840_chip_facts_validate(&candidate, NULL, 0) != 0) {
        return 1;
    }

    *facts = candidate;
    return 0;
}
