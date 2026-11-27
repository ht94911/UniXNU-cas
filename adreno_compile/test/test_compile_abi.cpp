/* SPDX-License-Identifier: MIT */
#include "AdrenoCompileABI.h"

#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>

static unsigned int stage_checks;
static unsigned int layout_checks;

static void expect_compile(const A840CompileTarget *target, const A840StageInput *input,
                           A840CompiledStage *out, A840CompileStatus status,
                           const char *message)
{
    unsigned char before[sizeof(*out)];
    if (out != nullptr)
        std::memcpy(before, out, sizeof(before));
    char error[96] = {};
    assert(a840_compile_stage(target, input, out, error, sizeof(error)) == status);
    assert(std::strcmp(error, message) == 0);
    if (out != nullptr)
        assert(std::memcmp(before, out, sizeof(before)) == 0);
    ++stage_checks;
}

static void expect_layout(const A840CompileTarget *target, const A840TextureLayoutQuery *query,
                          A840TextureLayoutResult *out, A840CompileStatus status)
{
    unsigned char before[sizeof(*out)];
    if (out != nullptr)
        std::memcpy(before, out, sizeof(before));
    assert(a840_compute_texture_layout(target, query, out) == status);
    if (out != nullptr)
        assert(std::memcmp(before, out, sizeof(before)) == 0);
    ++layout_checks;
}

static void test_stage(const A840CompileTarget &target)
{
    const uint32_t spirv[] = {0x07230203, 0x00010000, 0, 1, 0};
    A840StageInput input{};
    input.abi_version = ADRENO_COMPILE_ABI_VERSION;
    input.spirv_words = spirv;
    input.spirv_bytes = sizeof(spirv);
    input.entry_point = "main";
    input.sample_count = 1;

    A840CompiledStage out{};
    std::memset(&out, 0xa5, sizeof(out));
    const char pending[] = "ir3 backend lands in Wave 1";
    expect_compile(&target, &input, &out, A840_COMPILE_UNSUPPORTED_FEATURE, pending);
    input.stage = A840_STAGE_FRAGMENT;
    input.sample_count = 2;
    expect_compile(&target, &input, &out, A840_COMPILE_UNSUPPORTED_FEATURE, pending);
    input.stage = A840_STAGE_COMPUTE;
    input.sample_count = 4;
    expect_compile(&target, &input, &out, A840_COMPILE_UNSUPPORTED_FEATURE, pending);

    expect_compile(nullptr, &input, &out, A840_COMPILE_INVALID_ARG, "target is required");
    expect_compile(&target, nullptr, &out, A840_COMPILE_INVALID_ARG,
                   "input and out_stage are required");
    expect_compile(&target, &input, nullptr, A840_COMPILE_INVALID_ARG,
                   "input and out_stage are required");
    A840CompileTarget bad_target = target;
    ++bad_target.abi_version;
    expect_compile(&bad_target, &input, &out, A840_COMPILE_ABI_MISMATCH,
                   "target ABI version mismatch");
    bad_target = target;
    bad_target.chip_id = 0;
    expect_compile(&bad_target, &input, &out, A840_COMPILE_INVALID_ARG,
                   "chip_id must identify the Adreno 840 target");
    bad_target.chip_id = target.chip_id + 1;
    expect_compile(&bad_target, &input, &out, A840_COMPILE_INVALID_ARG,
                   "chip_id must identify the Adreno 840 target");

    A840StageInput bad_input = input;
    ++bad_input.abi_version;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_ABI_MISMATCH,
                   "stage input ABI version mismatch");
    bad_input = input;
    bad_input.stage = A840_STAGE_COMPUTE + 1;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG, "invalid shader stage");
    bad_input = input;
    bad_input.entry_point = nullptr;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG,
                   "entry_point is required");
    bad_input.entry_point = "";
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG,
                   "entry_point is required");
    bad_input = input;
    bad_input.sample_count = 8;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG,
                   "sample_count must be 1, 2, or 4");

    const char alignment[] = "SPIR-V requires a 4-byte-aligned pointer and a whole number of words";
    bad_input = input;
    bad_input.spirv_words = nullptr;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG, alignment);
    bad_input = input;
    bad_input.spirv_bytes = 0;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG, alignment);
    bad_input.spirv_bytes = sizeof(spirv) - 1;
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG, alignment);
    alignas(uint32_t) unsigned char unaligned[sizeof(spirv) + 1]{};
    bad_input = input;
    bad_input.spirv_words = reinterpret_cast<const uint32_t *>(unaligned + 1);
    expect_compile(&target, &bad_input, &out, A840_COMPILE_INVALID_ARG, alignment);
    const uint32_t bad_spirv[] = {0x03022307};
    bad_input.spirv_words = bad_spirv;
    bad_input.spirv_bytes = sizeof(bad_spirv);
    expect_compile(&target, &bad_input, &out, A840_COMPILE_SPIRV_ERROR, "invalid SPIR-V magic");

    unsigned char before[sizeof(out)];
    std::memcpy(before, &out, sizeof(before));
    assert(a840_compile_stage(&target, &input, &out, nullptr, 0) ==
           A840_COMPILE_UNSUPPORTED_FEATURE);
    assert(a840_compile_stage(&target, &input, &out, nullptr, 1) == A840_COMPILE_INVALID_ARG);
    char error = 'X';
    assert(a840_compile_stage(&target, &input, &out, &error, 0) ==
           A840_COMPILE_UNSUPPORTED_FEATURE);
    assert(error == 'X');
    assert(a840_compile_stage(&target, &input, &out, &error, 1) ==
           A840_COMPILE_UNSUPPORTED_FEATURE);
    assert(error == '\0');
    assert(std::memcmp(before, &out, sizeof(before)) == 0);
    stage_checks += 4;
}

static void test_layout(const A840CompileTarget &target)
{
    A840TextureLayoutQuery query{};
    query.abi_version = ADRENO_COMPILE_ABI_VERSION;
    query.mtl_pixel_format = 70;
    query.width = 128;
    query.height = 64;
    query.depth = 1;
    query.array_layers = 1;
    query.mip_levels = 1;
    query.sample_count = 1;

    A840TextureLayoutResult out{};
    std::memset(&out, 0x5a, sizeof(out));
    for (uint32_t mode = 0; mode <= 2; ++mode) {
        query.tile_mode = mode;
        expect_layout(&target, &query, &out, A840_COMPILE_UNSUPPORTED_FEATURE);
    }
    expect_layout(nullptr, &query, &out, A840_COMPILE_INVALID_ARG);
    expect_layout(&target, nullptr, &out, A840_COMPILE_INVALID_ARG);
    expect_layout(&target, &query, nullptr, A840_COMPILE_INVALID_ARG);
    A840CompileTarget bad_target = target;
    ++bad_target.abi_version;
    expect_layout(&bad_target, &query, &out, A840_COMPILE_ABI_MISMATCH);
    bad_target = target;
    bad_target.chip_id = 0;
    expect_layout(&bad_target, &query, &out, A840_COMPILE_INVALID_ARG);
    A840TextureLayoutQuery bad_query = query;
    ++bad_query.abi_version;
    expect_layout(&target, &bad_query, &out, A840_COMPILE_ABI_MISMATCH);

    uint32_t A840TextureLayoutQuery::*required_fields[] = {
        &A840TextureLayoutQuery::mtl_pixel_format,
        &A840TextureLayoutQuery::width,
        &A840TextureLayoutQuery::height,
        &A840TextureLayoutQuery::depth,
        &A840TextureLayoutQuery::array_layers,
        &A840TextureLayoutQuery::mip_levels,
        &A840TextureLayoutQuery::sample_count,
    };
    for (auto field : required_fields) {
        bad_query = query;
        bad_query.*field = 0;
        expect_layout(&target, &bad_query, &out, A840_COMPILE_INVALID_ARG);
    }
    bad_query = query;
    bad_query.sample_count = 8;
    expect_layout(&target, &bad_query, &out, A840_COMPILE_INVALID_ARG);
    bad_query = query;
    bad_query.tile_mode = 3;
    expect_layout(&target, &bad_query, &out, A840_COMPILE_INVALID_ARG);
}

int main()
{
    A840CompileTarget target{};
    target.abi_version = ADRENO_COMPILE_ABI_VERSION;
    target.chip_id = UINT64_C(0x44050a31);
    target.num_slices = 3;
    target.num_ccu = 6;
    target.highest_bank_bit = 16;
    target.ubwc_swizzle = 6;
    target.macrotile_mode = 1;

    test_stage(target);
    test_layout(target);
    a840_compile_stage_free(nullptr);
    const uint64_t fingerprint = a840_compile_abi_fingerprint();
    assert(fingerprint != 0 && fingerprint == a840_compile_abi_fingerprint());
    std::printf("adreno_compile ABI: %u stage, %u layout checks passed, "
                "free(NULL) passed, fingerprint=0x%016" PRIx64 "\n",
                stage_checks, layout_checks, fingerprint);
}
