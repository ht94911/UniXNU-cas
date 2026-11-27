/* SPDX-License-Identifier: MIT */
#include "AdrenoCompileABI.h"

#include <stdio.h>

static const uint64_t a840_chip_id = UINT64_C(0x44050a31);
static const uint32_t spirv_magic = UINT32_C(0x07230203);

static A840CompileStatus report(A840CompileStatus status, char *err_buf,
                                size_t err_buf_bytes, const char *message)
{
    if (err_buf != NULL && err_buf_bytes != 0)
        (void)snprintf(err_buf, err_buf_bytes, "%s", message);
    return status;
}

static A840CompileStatus validate_target(const A840CompileTarget *target,
                                         char *err_buf, size_t err_buf_bytes)
{
    if (target == NULL)
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "target is required");
    if (target->abi_version != ADRENO_COMPILE_ABI_VERSION)
        return report(A840_COMPILE_ABI_MISMATCH, err_buf, err_buf_bytes,
                      "target ABI version mismatch");
    if (target->chip_id != a840_chip_id)
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "chip_id must identify the Adreno 840 target");
    return A840_COMPILE_OK;
}

static bool valid_sample_count(uint32_t sample_count)
{
    return sample_count == 1 || sample_count == 2 || sample_count == 4;
}

A840CompileStatus a840_compile_stage(
    const A840CompileTarget *target,
    const A840StageInput *input,
    A840CompiledStage *out_stage,
    char *err_buf,
    size_t err_buf_bytes)
{
    if (err_buf == NULL && err_buf_bytes != 0)
        return A840_COMPILE_INVALID_ARG;
    if (input == NULL || out_stage == NULL)
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "input and out_stage are required");

    A840CompileStatus status = validate_target(target, err_buf, err_buf_bytes);
    if (status != A840_COMPILE_OK)
        return status;
    if (input->abi_version != ADRENO_COMPILE_ABI_VERSION)
        return report(A840_COMPILE_ABI_MISMATCH, err_buf, err_buf_bytes,
                      "stage input ABI version mismatch");
    if (input->stage > A840_STAGE_COMPUTE)
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "invalid shader stage");
    if (input->entry_point == NULL || input->entry_point[0] == '\0')
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "entry_point is required");
    if (!valid_sample_count(input->sample_count))
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "sample_count must be 1, 2, or 4");
    if (input->spirv_words == NULL || input->spirv_bytes < 4 ||
        input->spirv_bytes % 4 != 0 || (uintptr_t)input->spirv_words % 4 != 0)
        return report(A840_COMPILE_INVALID_ARG, err_buf, err_buf_bytes,
                      "SPIR-V requires a 4-byte-aligned pointer and a whole number of words");
    if (input->spirv_words[0] != spirv_magic)
        return report(A840_COMPILE_SPIRV_ERROR, err_buf, err_buf_bytes,
                      "invalid SPIR-V magic");

    return report(A840_COMPILE_UNSUPPORTED_FEATURE, err_buf, err_buf_bytes,
                  "ir3 backend lands in Wave 1");
}

void a840_compile_stage_free(A840CompiledStage *stage)
{
    /* Wave 0 creates no compiled stage or backing allocation. */
    (void)stage;
}

A840CompileStatus a840_compute_texture_layout(
    const A840CompileTarget *target,
    const A840TextureLayoutQuery *query,
    A840TextureLayoutResult *out_layout)
{
    if (query == NULL || out_layout == NULL)
        return A840_COMPILE_INVALID_ARG;

    A840CompileStatus status = validate_target(target, NULL, 0);
    if (status != A840_COMPILE_OK)
        return status;
    if (query->abi_version != ADRENO_COMPILE_ABI_VERSION)
        return A840_COMPILE_ABI_MISMATCH;
    if (query->mtl_pixel_format == 0 || query->width == 0 ||
        query->height == 0 || query->depth == 0 ||
        query->array_layers == 0 || query->mip_levels == 0 ||
        !valid_sample_count(query->sample_count) || query->tile_mode > 2)
        return A840_COMPILE_INVALID_ARG;

    return A840_COMPILE_UNSUPPORTED_FEATURE;
}

uint64_t a840_compile_abi_fingerprint(void)
{
    /* Hash the version followed by every ABI struct size as little-endian u64. */
    const uint64_t values[] = {
        ADRENO_COMPILE_ABI_VERSION,
        sizeof(A840CompileTarget),
        sizeof(A840UboPushRange),
        sizeof(A840StageInput),
        sizeof(A840CompiledStage),
        sizeof(A840TextureLayoutQuery),
        sizeof(A840TextureLayoutResult),
    };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint64_t value = values[i];
        for (unsigned int byte = 0; byte < 8; ++byte) {
            hash ^= (uint8_t)value;
            hash *= UINT64_C(1099511628211);
            value >>= 8;
        }
    }
    return hash;
}
