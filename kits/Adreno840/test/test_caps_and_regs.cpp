/* SPDX-License-Identifier: MIT */
#include "gen/Adreno840Caps.h"
#include "chip_facts_a840.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

static_assert(std::is_standard_layout<A840FormatCaps>::value, "C-compatible format record");
static_assert(A840_CP_SET_DRAW_STATE == 0x43u, "PM4 XML draw state opcode");
static_assert(A840_CP_SET_DRAW_STATE_A8XX_AVAILABLE == 1u, "draw state available on A8XX");
static_assert(A840_REG_RB_GC_GMEM_PROTECT == 0x8e09u, "A8XX GMEM protect register");
static_assert(A840_REG_CP_MISC_CNTL == 0x084cu, "A8XX CP_MISC, not the old 0x0840");
static_assert(A840_REG_CP_RB_BASE == 0x0800u, "A8XX main domain takes priority");
static_assert(A840_REG_AXXX_CP_RB_BASE == 0x01c0u, "legacy common value remains qualified");
static_assert(A840_REG_UCHE_TRAP_BASE_HI == A840_REG_UCHE_TRAP_BASE + 1u,
              "reg64 high half is the next dword");
static_assert(A840_REG_RB_MRT_1_BUF_INFO == A840_REG_RB_MRT_0_BUF_INFO + 8u,
              "XML array stride is expanded");
static_assert(A840_WZYX == 0 && A840_WXYZ == 1 && A840_ZYXW == 2 && A840_XYZW == 3,
              "pinned swap enum");
static_assert(A840_REG_COUNT > 2500, "all A8XX main registers emitted");
static_assert(A840_CP_OPCODE_COUNT == 141, "full pinned opcode catalog");

namespace {
std::size_t checks = 0;
void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

struct RequiredFormat {
    std::uint32_t id;
    const char *name;
};
/* Independent public Metal IDs/names transcribed from the W01 task card. */
const std::array<RequiredFormat, 65> required = {{
    {1, "A8Unorm"}, {10, "R8Unorm"}, {11, "R8Unorm_sRGB"}, {12, "R8Snorm"},
    {13, "R8Uint"}, {14, "R8Sint"}, {20, "R16Unorm"}, {22, "R16Snorm"},
    {23, "R16Uint"}, {24, "R16Sint"}, {25, "R16Float"},
    {30, "RG8Unorm"}, {31, "RG8Unorm_sRGB"}, {32, "RG8Snorm"}, {33, "RG8Uint"},
    {34, "RG8Sint"}, {40, "B5G6R5Unorm"}, {41, "A1BGR5Unorm"}, {42, "ABGR4Unorm"},
    {43, "BGR5A1Unorm"}, {53, "R32Uint"}, {54, "R32Sint"}, {55, "R32Float"},
    {60, "RG16Unorm"}, {62, "RG16Snorm"}, {63, "RG16Uint"}, {64, "RG16Sint"},
    {65, "RG16Float"}, {70, "RGBA8Unorm"}, {71, "RGBA8Unorm_sRGB"},
    {72, "RGBA8Snorm"}, {73, "RGBA8Uint"}, {74, "RGBA8Sint"},
    {80, "BGRA8Unorm"}, {81, "BGRA8Unorm_sRGB"}, {90, "RGB10A2Unorm"},
    {91, "RGB10A2Uint"}, {92, "RG11B10Float"}, {93, "RGB9E5Float"},
    {94, "BGR10A2Unorm"}, {103, "RG32Uint"}, {104, "RG32Sint"}, {105, "RG32Float"},
    {110, "RGBA16Unorm"}, {112, "RGBA16Snorm"}, {113, "RGBA16Uint"},
    {114, "RGBA16Sint"}, {115, "RGBA16Float"}, {123, "RGBA32Uint"},
    {124, "RGBA32Sint"}, {125, "RGBA32Float"},
    {186, "ASTC_4x4_sRGB"}, {190, "ASTC_6x6_sRGB"}, {194, "ASTC_8x8_sRGB"},
    {204, "ASTC_4x4_LDR"}, {208, "ASTC_6x6_LDR"}, {212, "ASTC_8x8_LDR"},
    {250, "Depth16Unorm"}, {252, "Depth32Float"}, {253, "Stencil8"},
    {260, "Depth32Float_Stencil8"}, {552, "BGRA10_XR"}, {553, "BGRA10_XR_sRGB"},
    {554, "BGR10_XR"}, {555, "BGR10_XR_sRGB"}
}};

const A840FormatCaps &format(std::uint32_t id)
{
    const A840FormatCaps *result = a840_format_caps_lookup(id);
    CHECK(result != nullptr);
    return *result;
}

void test_formats()
{
    CHECK(a840_format_caps_count() == required.size());
    CHECK(a840_format_caps_at(required.size()) == nullptr);
    CHECK(a840_format_caps_at(std::numeric_limits<std::size_t>::max()) == nullptr);
    std::size_t xr_count = 0;
    std::size_t quirk_count = 0;
    for (std::size_t i = 0; i < required.size(); ++i) {
        const A840FormatCaps &caps = format(required[i].id);
        CHECK(&caps == a840_format_caps_at(i));
        CHECK(&caps == a840_format_info(required[i].id));
        CHECK(caps.mtl_pixel_format == required[i].id);
        CHECK(std::strcmp(caps.mtl_name, required[i].name) == 0);
        CHECK(caps.grade == A840_CAPS_GRADE_SOURCE);
        CHECK(std::strcmp(caps.hwtest_id, "-") == 0);
        CHECK(std::strncmp(caps.pipe_format, "PIPE_FORMAT_", 12) == 0);
        CHECK(caps.citation != nullptr && std::strchr(caps.citation, ':') != nullptr);
        CHECK(caps.notes != nullptr && caps.notes[0] != '\0');
        CHECK(caps.a840_fmt_id != A840_FMT6_NONE);
        CHECK(caps.swap <= A840_XYZW);
        CHECK(caps.tiled_swap == A840_WZYX);
        CHECK(caps.block_width != 0 && caps.block_height != 0 && caps.block_bits != 0);
        CHECK(caps.bpp_numerator != 0 && caps.bpp_denominator != 0);
        CHECK(static_cast<std::uint64_t>(caps.bpp_numerator) * caps.block_width * caps.block_height ==
              static_cast<std::uint64_t>(caps.block_bits) * caps.bpp_denominator);
        CHECK(caps.ubwc_supported <= 1 && caps.renderable <= 1 &&
              caps.filterable <= 1 && caps.blendable <= 1);
        CHECK(caps.blendable == 0 || caps.renderable != 0);
        if (caps.flags & A840_FORMAT_REQUIRES_XR_CODEC) {
            ++xr_count;
            CHECK(caps.ubwc_supported == 0 && caps.renderable == 0 &&
                  caps.filterable == 0 && caps.blendable == 0 && caps.msaa == 0);
            CHECK(std::strstr(caps.notes, "NOT implemented/qualified") != nullptr);
        } else {
            CHECK(caps.msaa == (caps.renderable ? 7u : 1u));
        }
        if (caps.flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) {
            ++quirk_count;
            CHECK(caps.mtl_pixel_format >= 30 && caps.mtl_pixel_format <= 34);
            CHECK(std::strstr(caps.notes, "r8g8_faulty_fast_clear_quirk") != nullptr);
        }
        if (caps.flags & A840_FORMAT_COMPRESSED)
            CHECK(caps.renderable == 0 && caps.ubwc_supported == 0 && caps.msaa == 1);
    }
    CHECK(xr_count == 4 && quirk_count == 5);
    std::size_t found = 0;
    for (std::uint32_t id = 0; id < 600; ++id) {
        bool expected = false;
        for (const auto &item : required)
            expected |= item.id == id;
        CHECK((a840_format_caps_lookup(id) != nullptr) == expected);
        found += a840_format_caps_lookup(id) != nullptr;
    }
    CHECK(found == required.size());
    CHECK(a840_format_caps_lookup(std::numeric_limits<std::uint32_t>::max()) == nullptr);

    CHECK(format(10).a840_fmt_id == A840_FMT6_8_UNORM);
    CHECK(format(10).ubwc_supported == 0); /* pinned profile has no 8-bpp UBWC */
    CHECK(format(13).filterable == 0 && format(13).blendable == 0);
    CHECK(format(30).flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK);
    CHECK(format(32).flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK);
    CHECK(format(30).ubwc_supported == 1 && format(32).ubwc_supported == 1);
    CHECK(format(80).swap == A840_WXYZ && format(80).tiled_swap == A840_WZYX);
    CHECK(format(41).a840_fmt_id == A840_FMT6_1_5_5_5_UNORM);
    CHECK(format(41).swap == A840_WXYZ);
    CHECK(format(41).tiled_fmt_id == A840_FMT6_5_5_5_1_UNORM);
    CHECK(format(90).a840_fmt_id == A840_FMT6_10_10_10_2_UNORM);
    CHECK(format(90).color_fmt_id == A840_FMT6_10_10_10_2_UNORM_DEST);
    CHECK(format(93).renderable == 1 && format(93).ubwc_supported == 0);
    CHECK(format(115).block_bits == 64 && format(125).block_bits == 128);
    CHECK(format(204).block_width == 4 && format(204).block_height == 4);
    CHECK(format(208).bpp_numerator == 32 && format(208).bpp_denominator == 9);
    CHECK(format(212).bpp_numerator == 2 && format(212).bpp_denominator == 1);
    CHECK(format(186).flags & A840_FORMAT_SRGB);
    CHECK(format(250).flags & A840_FORMAT_DEPTH);
    CHECK(format(253).flags & A840_FORMAT_STENCIL);
    CHECK(format(253).ubwc_supported == 0 && format(253).filterable == 0);
    CHECK(format(260).flags & A840_FORMAT_SEPARATE_DEPTH_STENCIL_PLANES);
    CHECK(format(552).block_bits == 64 && format(554).block_bits == 32);
}

#define FACT_FIELDS(X) \
    X(chip_id, CHIP_ID) \
    X(gmem_range_min, GMEM_RANGE_MIN) \
    X(uche_trap_base, UCHE_TRAP_BASE) \
    X(chip_gen, CHIP_GEN) \
    X(num_slices, NUM_SLICES) \
    X(num_ccu, NUM_CCU) \
    X(num_sp_cores, NUM_SP_CORES) \
    X(gmem_bytes, GMEM_BYTES) \
    X(gmem_align_w, GMEM_ALIGN_W) \
    X(gmem_align_h, GMEM_ALIGN_H) \
    X(tile_align_w, TILE_ALIGN_W) \
    X(tile_align_h, TILE_ALIGN_H) \
    X(tile_max_w, TILE_MAX_W) \
    X(tile_max_h, TILE_MAX_H) \
    X(num_vsc_pipes, NUM_VSC_PIPES) \
    X(ubwc_version, UBWC_VERSION) \
    X(highest_bank_bit, HIGHEST_BANK_BIT) \
    X(ubwc_swizzle, UBWC_SWIZZLE) \
    X(macrotile_mode, MACROTILE_MODE) \
    X(ubwc_version_tag, UBWC_VERSION_TAG) \
    X(ubwc_flags, UBWC_FLAGS) \
    X(cs_shared_mem_size, CS_SHARED_MEM_SIZE) \
    X(fibers_per_sp, FIBERS_PER_SP) \
    X(threadsize_base, THREADSIZE_BASE) \
    X(max_waves, MAX_WAVES) \
    X(wave_granularity, WAVE_GRANULARITY) \
    X(reg_size_vec4, REG_SIZE_VEC4) \
    X(instr_cache_size, INSTR_CACHE_SIZE) \
    X(max_draw_states, MAX_DRAW_STATES) \
    X(vs_max_inputs_count, VS_MAX_INPUTS_COUNT) \
    X(max_sets, MAX_SETS) \
    X(sysmem_per_ccu_color_cache_size, SYSMEM_PER_CCU_COLOR_CACHE_SIZE) \
    X(sysmem_per_ccu_depth_cache_size, SYSMEM_PER_CCU_DEPTH_CACHE_SIZE) \
    X(gmem_per_ccu_color_cache_size, GMEM_PER_CCU_COLOR_CACHE_SIZE) \
    X(gmem_per_ccu_depth_cache_size, GMEM_PER_CCU_DEPTH_CACHE_SIZE) \
    X(sysmem_vpc_attr_buf_size, SYSMEM_VPC_ATTR_BUF_SIZE) \
    X(gmem_vpc_attr_buf_size, GMEM_VPC_ATTR_BUF_SIZE) \
    X(features, FEATURES) \
    X(smmu_page_shift, SMMU_PAGE_SHIFT)

void test_limits()
{
    const A840ChipFacts *facts = a840_chip_facts_kaanapali_default();
    CHECK(facts != nullptr);
#define CHECK_FACT(field, upper) CHECK(A840_LIMIT_##upper == facts->field);
    FACT_FIELDS(CHECK_FACT)
#undef CHECK_FACT
    struct Expected { const char *name; std::uint64_t value; };
#define EXPECT_FACT(field, upper) {#field, facts->field},
    const std::array<Expected, 39> expected = {{FACT_FIELDS(EXPECT_FACT)}};
#undef EXPECT_FACT
    CHECK(a840_limit_caps_count() == 50);
    CHECK(a840_limit_caps_at(50) == nullptr);
    for (const auto &entry : expected) {
        std::size_t occurrences = 0;
        for (std::size_t i = 0; i < a840_limit_caps_count(); ++i) {
            const auto *limit = a840_limit_caps_at(i);
            CHECK(limit != nullptr);
            if (std::strcmp(limit->limit_id, entry.name) == 0) {
                CHECK(limit->value == entry.value);
                ++occurrences;
            }
        }
        CHECK(occurrences == 1);
    }
    for (std::size_t i = 0; i < a840_limit_caps_count(); ++i) {
        const auto *limit = a840_limit_caps_at(i);
        CHECK(limit->citation != nullptr && std::strchr(limit->citation, ':') != nullptr);
        CHECK(std::strcmp(limit->bound_kind, "exact") == 0 ||
              std::strcmp(limit->bound_kind, "max") == 0 ||
              std::strcmp(limit->bound_kind, "min") == 0);
        if (i) CHECK(std::strcmp(a840_limit_caps_at(i - 1)->limit_id, limit->limit_id) < 0);
    }
    CHECK(A840_LIMIT_MAX_TEXTURE_SIZE == 16384);
    CHECK(A840_LIMIT_MAX_TEXTURE_SIZE_1D == 16384);
    CHECK(A840_LIMIT_MAX_TEXTURE_SIZE_3D == 2048);
    CHECK(A840_LIMIT_MAX_TEXTURE_SIZE_CUBE == 16384);
    CHECK(A840_LIMIT_MAX_ARRAY_LAYERS == 2048);
    CHECK(A840_LIMIT_MAX_MIP_LEVELS_2D == 15 && A840_LIMIT_MAX_MIP_LEVELS_3D == 12);
    CHECK(A840_LIMIT_MAX_SAMPLES == 4);
    CHECK(A840_LIMIT_SDS_SLOTS == facts->max_draw_states);
    CHECK(A840_LIMIT_GMEM_BYTES_ALIGNMENT == A840_GMEM_BYTES_ALIGNMENT);
    CHECK(A840_LIMIT_MESA_HIGHEST_BANK_BIT == A840_MESA_HIGHEST_BANK_BIT);
    CHECK(A840_LIMIT_HIGHEST_BANK_BIT == 16);
}

void test_features()
{
    CHECK(a840_feature_caps_count() == 22);
    CHECK(a840_feature_caps_at(22) == nullptr);
    for (std::size_t i = 0; i < a840_feature_caps_count(); ++i) {
        const auto *feature = a840_feature_caps_at(i);
        CHECK(feature != nullptr && feature->grade == A840_CAPS_GRADE_SOURCE);
        CHECK(feature->supported == 1);
        CHECK(std::strcmp(feature->hwtest_id, "-") == 0);
        CHECK(feature->citation != nullptr && std::strchr(feature->citation, ':') != nullptr);
        if (i) CHECK(std::strcmp(a840_feature_caps_at(i - 1)->feature_id, feature->feature_id) < 0);
    }
    CHECK(A840_CAP_HAS_EARLY_PREAMBLE == 1);
    CHECK(A840_CAP_HAS_GMEM_FAST_CLEAR == 1);
    CHECK(A840_CAP_HAS_COHERENT_UBWC_FLAG_CACHES == 1);
    CHECK(A840_CAP_SUPPORTS_UAV_UBWC == 1);
    CHECK(A840_CAP_UBWC_COHERENCY_QUIRK == 1);
    CHECK(A840_CAP_R8G8_FAULTY_FAST_CLEAR_QUIRK == 1);
    CHECK(A840_CAP_PREDE_NOP_QUIRK == 1);
    CHECK(A840_CAP_PREDTF_NOP_QUIRK == 1);
    CHECK(A840_CAP_HAS_SALU_INT_NARROWING_QUIRK == 1);
    CHECK(A840_CAP_HAS_GMEM_SAVE_RESTORE == 1);
    CHECK(A840_CAP_UBWC_6_0 == 1 && A840_CAP_TBDR_GMEM == 1 && A840_CAP_SDS_64_SLOTS == 1);
    CHECK(A840_CAP_DUAL_WAVE_DISPATCH == 1 && A840_CAP_ASTC_HDR == 1);
    CHECK(A840_CAP_LRZ_FAST_CLEAR == 1 && A840_CAP_LRZ_FEEDBACK == 1);
    CHECK(A840_CAP_CUBIC_FILTER == 1);
    CHECK(A840_CAP_SSBO_64B_ATOMICS == 1 && A840_CAP_IMAGE_64B_ATOMICS == 1);
}
} /* namespace */

int main()
{
    test_formats();
    test_limits();
    test_features();
    std::printf("PASS: all %zu Metal formats; unknown lookups, rational ASTC, XR/source grades and R8G8 quirk\n",
                a840_format_caps_count());
    std::printf("PASS: %zu limits including all 39 frozen chip fields; %zu source features\n",
                a840_limit_caps_count(), a840_feature_caps_count());
    std::printf("PASS: %u A8XX registers, %u opcodes; variants/arrays/reg64/XML constants\n",
                static_cast<unsigned>(A840_REG_COUNT), static_cast<unsigned>(A840_CP_OPCODE_COUNT));
    std::printf("PASS: %zu checks\n", checks);
    return EXIT_SUCCESS;
}
