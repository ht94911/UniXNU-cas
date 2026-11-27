/* SPDX-License-Identifier: MIT */
#include "chip_facts_a840.h"
#include "AdrenoCompileABI.h"
#include "NativeAdrenoABI_v3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

static_assert(std::is_standard_layout<A840ChipFacts>::value,
              "facts have a standard layout");
static_assert(std::is_trivially_copyable<A840ChipFacts>::value,
              "facts can be copied without ownership");
static_assert(sizeof(A840ChipFacts) == 3 * sizeof(std::uint64_t) +
                                           36 * sizeof(std::uint32_t),
              "facts have no padding on this host");
static_assert(ADRENO_COMPILE_MAX_VARYINGS == 32, "frozen varying limit");
static_assert(ADRENO_COMPILE_MAX_BIND_SETS == 8, "frozen descriptor-set limit");
static_assert(NativeAdrenoAbiVersionV2 == 2, "frozen native v2 version");
static_assert(NativeAdrenoAbiVersionV3 == 3, "frozen native v3 version");
static_assert(NativeAdrenoAbiEnvelopeBytes == 3104, "frozen method envelope");
static_assert(sizeof(NativeAdrenoAbiSurfaceMetaV3) == 48,
              "frozen UBWC metadata size");

namespace {

std::size_t checks = 0;
std::size_t runtime_rejects = 0;
std::size_t fixed_rejects = 0;
std::size_t legal_updates = 0;

void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL at line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

struct FieldCase {
    const char *name;
    std::uint32_t A840ChipFacts::*member;
    std::uint32_t expected;
    bool runtime;
};

/* Independent expectations transcribed from the task and pinned source table. */
const std::array<FieldCase, 36> fields = {{
    {"chip_gen", &A840ChipFacts::chip_gen, 8, false},
    {"num_slices", &A840ChipFacts::num_slices, 3, false},
    {"num_ccu", &A840ChipFacts::num_ccu, 6, false},
    {"num_sp_cores", &A840ChipFacts::num_sp_cores, 6, false},
    {"gmem_bytes", &A840ChipFacts::gmem_bytes, 18874368, true},
    {"gmem_align_w", &A840ChipFacts::gmem_align_w, 16, false},
    {"gmem_align_h", &A840ChipFacts::gmem_align_h, 4, false},
    {"tile_align_w", &A840ChipFacts::tile_align_w, 96, false},
    {"tile_align_h", &A840ChipFacts::tile_align_h, 32, false},
    {"tile_max_w", &A840ChipFacts::tile_max_w, 16416, false},
    {"tile_max_h", &A840ChipFacts::tile_max_h, 16384, false},
    {"num_vsc_pipes", &A840ChipFacts::num_vsc_pipes, 32, false},
    {"ubwc_version", &A840ChipFacts::ubwc_version, 6, false},
    {"highest_bank_bit", &A840ChipFacts::highest_bank_bit, 16, true},
    {"ubwc_swizzle", &A840ChipFacts::ubwc_swizzle, 6, false},
    {"macrotile_mode", &A840ChipFacts::macrotile_mode, 1, false},
    {"ubwc_version_tag", &A840ChipFacts::ubwc_version_tag, 5, false},
    {"ubwc_flags", &A840ChipFacts::ubwc_flags, 23, false},
    {"cs_shared_mem_size", &A840ChipFacts::cs_shared_mem_size, 32768, false},
    {"fibers_per_sp", &A840ChipFacts::fibers_per_sp, 4096, false},
    {"threadsize_base", &A840ChipFacts::threadsize_base, 64, false},
    {"max_waves", &A840ChipFacts::max_waves, 16, false},
    {"wave_granularity", &A840ChipFacts::wave_granularity, 2, false},
    {"reg_size_vec4", &A840ChipFacts::reg_size_vec4, 128, false},
    {"instr_cache_size", &A840ChipFacts::instr_cache_size, 127, false},
    {"max_draw_states", &A840ChipFacts::max_draw_states, 64, false},
    {"vs_max_inputs_count", &A840ChipFacts::vs_max_inputs_count, 32, false},
    {"max_sets", &A840ChipFacts::max_sets, 8, false},
    {"sysmem_per_ccu_color_cache_size",
     &A840ChipFacts::sysmem_per_ccu_color_cache_size, 131072, false},
    {"sysmem_per_ccu_depth_cache_size",
     &A840ChipFacts::sysmem_per_ccu_depth_cache_size, 196608, false},
    {"gmem_per_ccu_color_cache_size",
     &A840ChipFacts::gmem_per_ccu_color_cache_size, 16384, false},
    {"gmem_per_ccu_depth_cache_size",
     &A840ChipFacts::gmem_per_ccu_depth_cache_size, 262144, false},
    {"sysmem_vpc_attr_buf_size",
     &A840ChipFacts::sysmem_vpc_attr_buf_size, 131072, false},
    {"gmem_vpc_attr_buf_size",
     &A840ChipFacts::gmem_vpc_attr_buf_size, 49152, false},
    {"features", &A840ChipFacts::features, 1023, false},
    {"smmu_page_shift", &A840ChipFacts::smmu_page_shift, 12, false}
}};

const std::array<std::uint32_t, 10> feature_bits = {{
    A840_FEATURE_HAS_EARLY_PREAMBLE,
    A840_FEATURE_HAS_GMEM_FAST_CLEAR,
    A840_FEATURE_HAS_COHERENT_UBWC_FLAG_CACHES,
    A840_FEATURE_SUPPORTS_UAV_UBWC,
    A840_FEATURE_UBWC_COHERENCY_QUIRK,
    A840_FEATURE_R8G8_FAULTY_FAST_CLEAR_QUIRK,
    A840_FEATURE_PREDE_NOP_QUIRK,
    A840_FEATURE_PREDTF_NOP_QUIRK,
    A840_FEATURE_HAS_SALU_INT_NARROWING_QUIRK,
    A840_FEATURE_HAS_GMEM_SAVE_RESTORE
}};

using Bytes = std::array<unsigned char, sizeof(A840ChipFacts)>;

Bytes bytes_of(const A840ChipFacts &facts)
{
    Bytes bytes{};
    std::memcpy(bytes.data(), &facts, bytes.size());
    return bytes;
}

void expect_invalid(const A840ChipFacts &facts, const char *field_name)
{
    const Bytes before = bytes_of(facts);
    std::array<char, 128> error{};
    CHECK(a840_chip_facts_validate(&facts, error.data(), error.size()) != 0);
    CHECK(std::strstr(error.data(), field_name) != nullptr);
    CHECK(bytes_of(facts) == before);
}

void expect_runtime_reject(A840ChipFacts facts, std::uint64_t chip,
                           std::uint32_t gmem, std::uint32_t hbb)
{
    const Bytes before = bytes_of(facts);
    CHECK(a840_chip_facts_apply_runtime(&facts, chip, gmem, hbb) != 0);
    CHECK(bytes_of(facts) == before);
    ++runtime_rejects;
}

void expect_fixed_reject(A840ChipFacts facts, const char *field_name)
{
    expect_invalid(facts, field_name);
    expect_runtime_reject(facts, UINT64_C(0x44050a31), 16777216, 15);
    ++fixed_rejects;
}

void test_defaults(const A840ChipFacts &facts)
{
    CHECK(facts.chip_id == UINT64_C(0x44050a31));
    CHECK(facts.gmem_range_min == UINT64_C(0x04000000));
    CHECK(facts.uche_trap_base == UINT64_C(0x1fffffffff000));
    CHECK(A840_KAANAPALI_GMEM_BYTES == 18874368);
    CHECK(A840_GMEM_BYTES_ALIGNMENT == 4096);
    CHECK(A840_KAANAPALI_HIGHEST_BANK_BIT == 16);
    CHECK(A840_MESA_HIGHEST_BANK_BIT == 15);
    CHECK(A840_MESA_CATALOG_CHIP_ID == UINT64_C(0xffff44050a31));
    CHECK(sizeof(facts) == 168);
    CHECK(facts.gmem_range_min + facts.gmem_bytes == UINT64_C(0x05200000));
    CHECK(facts.tile_max_w % facts.tile_align_w == 0);
    CHECK(facts.tile_max_h % facts.tile_align_h == 0);
    CHECK(facts.fibers_per_sp == facts.reg_size_vec4 * 2 * facts.max_waves);
    for (const FieldCase &field : fields) {
        CHECK(facts.*(field.member) == field.expected);
    }
    for (std::size_t i = 0; i < feature_bits.size(); ++i) {
        CHECK(feature_bits[i] == (1u << i));
        CHECK((facts.features & feature_bits[i]) != 0);
    }
    CHECK((facts.ubwc_flags & A840_UBWC_YUV_NOT_COMP_TO_FC) != 0);
    CHECK((facts.ubwc_flags & A840_UBWC_RGB565_PREDICATOR) != 0);
    CHECK((facts.ubwc_flags & A840_UBWC_AMSBC) != 0);
    CHECK(((facts.ubwc_flags & A840_UBWC_UAVFLAGPRD_INV_MASK) >>
           A840_UBWC_UAVFLAGPRD_INV_SHIFT) == 2);
    CHECK(((facts.ubwc_flags & A840_UBWC_MODE_MASK) >>
           A840_UBWC_MODE_SHIFT) == 0);
    CHECK(a840_chip_facts_validate(&facts, nullptr, 0) == 0);
}

void test_runtime(const A840ChipFacts &defaults)
{
    const std::array<std::array<std::uint32_t, 2>, 4> valid = {{
        {{18874368, 16}},
        {{16777216, 15}},
        {{18870272, 16}},
        {{4096, 15}}
    }};
    for (const auto &input : valid) {
        A840ChipFacts facts = defaults;
        A840ChipFacts expected = defaults;
        expected.gmem_bytes = input[0];
        expected.highest_bank_bit = input[1];
        CHECK(a840_chip_facts_apply_runtime(
                  &facts, UINT64_C(0x44050a31), input[0], input[1]) == 0);
        CHECK(bytes_of(facts) == bytes_of(expected));
        CHECK(a840_chip_facts_validate(&facts, nullptr, 0) == 0);
        ++legal_updates;
    }

    A840ChipFacts previous = defaults;
    CHECK(a840_chip_facts_apply_runtime(
              &previous, UINT64_C(0x44050a31), 16777216, 15) == 0);
    const std::array<A840ChipFacts, 2> starting = {{defaults, previous}};
    const std::array<std::uint64_t, 8> bad_chips = {{
        0, UINT64_C(0x44050a01), UINT64_C(0x44050a21),
        UINT64_C(0x44050a32), UINT64_C(0x43050a01),
        UINT64_C(0xffff44050a31), UINT64_C(0x144050a31),
        std::numeric_limits<std::uint64_t>::max()
    }};
    const std::array<std::uint32_t, 9> bad_hbb = {{
        0, 1, 13, 14, 17, 18, 31, 32,
        std::numeric_limits<std::uint32_t>::max()
    }};
    const std::array<std::uint32_t, 9> bad_gmem = {{
        0, 1, 4095, 4097, 18874367, 18874369, 18878464, 0x12000000u,
        std::numeric_limits<std::uint32_t>::max()
    }};
    for (const A840ChipFacts &initial : starting) {
        for (const std::uint64_t chip : bad_chips) {
            expect_runtime_reject(initial, chip, 18874368, 16);
            A840ChipFacts bad = initial;
            bad.chip_id = chip;
            expect_invalid(bad, "chip_id");
        }
        for (const std::uint32_t hbb : bad_hbb) {
            expect_runtime_reject(initial, UINT64_C(0x44050a31), 18874368, hbb);
            A840ChipFacts bad = initial;
            bad.highest_bank_bit = hbb;
            expect_invalid(bad, "highest_bank_bit");
        }
        for (const std::uint32_t gmem : bad_gmem) {
            expect_runtime_reject(initial, UINT64_C(0x44050a31), gmem, 16);
            A840ChipFacts bad = initial;
            bad.gmem_bytes = gmem;
            expect_invalid(bad, "gmem_bytes");
        }
    }
    CHECK(a840_chip_facts_apply_runtime(
              nullptr, UINT64_C(0x44050a31), 18874368, 16) != 0);
}

void test_fixed_fields(const A840ChipFacts &defaults)
{
    for (const FieldCase &field : fields) {
        if (field.runtime) {
            continue;
        }
        A840ChipFacts bad = defaults;
        bad.*(field.member) = field.expected ^ 1u;
        expect_fixed_reject(bad, field.name);
    }
    A840ChipFacts bad = defaults;
    bad.gmem_range_min += 4096;
    expect_fixed_reject(bad, "gmem_range_min");
    bad = defaults;
    bad.uche_trap_base += 4096;
    expect_fixed_reject(bad, "uche_trap_base");
    for (const std::uint32_t bit : feature_bits) {
        bad = defaults;
        bad.features &= ~bit;
        expect_fixed_reject(bad, "features");
    }
    bad = defaults;
    bad.features |= 1u << 31;
    expect_fixed_reject(bad, "features");
    bad = defaults;
    bad.ubwc_flags |= 1u << 31;
    expect_fixed_reject(bad, "ubwc_flags");
    bad = {};
    expect_runtime_reject(bad, UINT64_C(0x44050a31), 18874368, 16);
}

void test_error_buffers(const A840ChipFacts &defaults)
{
    std::array<char, 128> error{};
    CHECK(a840_chip_facts_validate(nullptr, error.data(), error.size()) != 0);
    CHECK(std::strcmp(error.data(), "facts pointer is null") == 0);
    CHECK(a840_chip_facts_validate(nullptr, nullptr, 0) != 0);
    CHECK(a840_chip_facts_validate(nullptr, nullptr, 128) != 0);
    CHECK(a840_chip_facts_validate(&defaults, nullptr, 128) == 0);

    std::array<char, 6> tiny = {{'#', '#', '#', '#', '#', '#'}};
    const auto original = tiny;
    CHECK(a840_chip_facts_validate(nullptr, tiny.data() + 2, 0) != 0);
    CHECK(tiny == original);
    CHECK(a840_chip_facts_validate(nullptr, tiny.data() + 2, 1) != 0);
    CHECK(tiny[0] == '#' && tiny[1] == '#' && tiny[2] == '\0' &&
          tiny[3] == '#' && tiny[4] == '#' && tiny[5] == '#');
    tiny = original;
    CHECK(a840_chip_facts_validate(nullptr, tiny.data() + 2, 2) != 0);
    CHECK(tiny[0] == '#' && tiny[1] == '#' && tiny[2] == 'f' &&
          tiny[3] == '\0' && tiny[4] == '#' && tiny[5] == '#');
    tiny = original;
    CHECK(a840_chip_facts_validate(&defaults, tiny.data() + 2, 2) == 0);
    CHECK(tiny[0] == '#' && tiny[1] == '#' && tiny[2] == '\0' &&
          tiny[3] == '#' && tiny[4] == '#' && tiny[5] == '#');
    tiny = original;
    CHECK(a840_chip_facts_validate(&defaults, tiny.data() + 2, 0) == 0);
    CHECK(tiny == original);
}

void test_contracts(const A840ChipFacts &facts)
{
    CHECK(facts.vs_max_inputs_count == ADRENO_COMPILE_MAX_VARYINGS);
    CHECK(facts.max_sets == ADRENO_COMPILE_MAX_BIND_SETS);

    A840CompileTarget target{};
    target.abi_version = ADRENO_COMPILE_ABI_VERSION;
    target.chip_id = facts.chip_id;
    target.num_slices = facts.num_slices;
    target.num_ccu = facts.num_ccu;
    target.highest_bank_bit = facts.highest_bank_bit;
    target.ubwc_swizzle = facts.ubwc_swizzle;
    target.macrotile_mode = facts.macrotile_mode;
    if ((facts.features & A840_FEATURE_HAS_EARLY_PREAMBLE) != 0) {
        target.flags |= A840_COMPILE_FLAG_EARLY_PREAMBLE;
    }
    CHECK(sizeof(target) == 40);
    CHECK(target.abi_version == 1);
    CHECK(target.reserved0 == 0);
    CHECK(target.chip_id == UINT64_C(0x44050a31));
    CHECK(target.num_slices == 3 && target.num_ccu == 6);
    CHECK(target.highest_bank_bit == 16);
    CHECK(target.ubwc_swizzle == 6 && target.macrotile_mode == 1);
    CHECK(target.flags == (1u << 1));

    /* Numeric hardware defaults are comments in the frozen v3 contract.
     * This fixture checks those defaults and field/layout compatibility;
     * it does not implement or advertise a v3 external method. */
    NativeAdrenoAbiSurfaceMetaV3 meta{};
    meta.ubwc_enabled = 1;
    meta.ubwc_version = facts.ubwc_version;
    meta.highest_bank_bit = facts.highest_bank_bit;
    meta.swizzle_levels = facts.ubwc_swizzle;
    meta.macrotile_mode = facts.macrotile_mode;
    CHECK(meta.ubwc_version == 6);
    CHECK(meta.highest_bank_bit == 16);
    CHECK(meta.swizzle_levels == 6);
    CHECK(meta.macrotile_mode == 1);
    CHECK(meta.flags == 0);
    CHECK(NativeAdrenoParamsV3UbwcSurfaceMeta == (1u << 13));
    CHECK(NativeAdrenoMethodSurfaceMetaV3 == 25);
    CHECK(NativeAdrenoMethodQueueConfigV3 == 26);
    CHECK(sizeof(meta) <= NativeAdrenoAbiEnvelopeBytes);
}

} /* namespace */

int main()
{
    const A840ChipFacts *defaults = a840_chip_facts_kaanapali_default();
    CHECK(defaults != nullptr);
    CHECK(defaults == a840_chip_facts_kaanapali_default());
    const Bytes original = bytes_of(*defaults);
    test_defaults(*defaults);
    test_runtime(*defaults);
    test_fixed_fields(*defaults);
    test_error_buffers(*defaults);
    test_contracts(*defaults);
    CHECK(bytes_of(*defaults) == original);

    std::printf("PASS: default facts: 39 fields, 10 feature bits, size=%zu\n",
                sizeof(A840ChipFacts));
    std::printf("PASS: runtime updates=%zu, byte-preserving rejections=%zu\n",
                legal_updates, runtime_rejects);
    std::printf("PASS: fixed-field/flag rejections=%zu; bounded error buffers\n",
                fixed_rejects);
    std::printf("PASS: frozen compiler/native contracts; SurfaceMeta=%zu bytes\n",
                sizeof(NativeAdrenoAbiSurfaceMetaV3));
    std::printf("PASS: %zu checks; immutable default retained\n", checks);
    return EXIT_SUCCESS;
}
