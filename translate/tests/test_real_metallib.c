/* SPDX-License-Identifier: MIT */
#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char compute_sample[] =
    "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib";
static const char render_sample[] =
    "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib";

static void require(bool success, const char *what)
{
    if (!success) {
        fprintf(stderr, "%s: %s\n", what, nmb_last_error());
        exit(EXIT_FAILURE);
    }
}

static void file_error(const char *path, const char *operation)
{
    fprintf(stderr, "%s %s: %s\n", operation, path, strerror(errno));
    exit(EXIT_FAILURE);
}

static NmbLibrary *load_library(const NmbDevice *dev, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        file_error(path, "cannot open metallib");
    if (fseek(file, 0, SEEK_END) != 0)
        file_error(path, "cannot seek metallib");
    long size = ftell(file);
    if (size <= 0 || (unsigned long)size > SIZE_MAX) {
        fprintf(stderr, "invalid metallib length: %s\n", path);
        exit(EXIT_FAILURE);
    }
    if (fseek(file, 0, SEEK_SET) != 0)
        file_error(path, "cannot rewind metallib");
    uint8_t *bytes = malloc((size_t)size);
    if (!bytes)
        file_error(path, "cannot allocate metallib buffer for");
    size_t actual = fread(bytes, 1, (size_t)size, file);
    if (fclose(file) != 0)
        file_error(path, "cannot close metallib");
    if (actual != (size_t)size) {
        fprintf(stderr, "short metallib read: %s\n", path);
        free(bytes);
        exit(EXIT_FAILURE);
    }

    NmbLibrary *lib = nmb_library_parse(dev, bytes, actual);
    free(bytes);
    require(lib != NULL, path);
    size_t count = nmb_library_function_count(lib);
    require(count > 0, "metallib function count");
    printf("%s: %zu functions\n", path, count);
    for (size_t i = 0; i < count; ++i) {
        char name[512];
        require(nmb_library_function_name(lib, i, name, sizeof(name)), "function enumeration");
        printf("  %s\n", name);
    }
    char sentinel = 'x';
    require(!nmb_library_function_name(lib, 0, &sentinel, sizeof(sentinel)) &&
                sentinel == 'x' && strstr(nmb_last_error(), "buffer") != NULL,
            "short name buffer error");
    require(!nmb_library_function_name(lib, count, &sentinel, sizeof(sentinel)) &&
                sentinel == 'x' && strstr(nmb_last_error(), "index") != NULL,
            "function index error");
    return lib;
}

static void save_spirv(const char *stage, const uint32_t *words, size_t len)
{
    const char *dir = getenv("NMB_TEST_SPIRV_DIR");
    if (dir == NULL)
        return;

    char path[4096];
    int count = snprintf(path, sizeof(path), "%s/%s.spv", dir, stage);
    require(count > 0 && (size_t)count < sizeof(path), "SPIR-V output path");
    FILE *file = fopen(path, "wb");
    if (!file)
        file_error(path, "cannot create");
    for (size_t i = 0; i < len / sizeof(*words); ++i) {
        uint32_t word = words[i];
        uint8_t bytes[4] = {(uint8_t)word, (uint8_t)(word >> 8),
                            (uint8_t)(word >> 16), (uint8_t)(word >> 24)};
        if (fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
            file_error(path, "cannot write");
    }
    if (fclose(file) != 0)
        file_error(path, "cannot close");
}

static void check_stage(const NmbSpirvResult *result, uint32_t index,
                        A840ShaderStage stage, const char *label)
{
    const A840CompileTarget target = {
        .abi_version = ADRENO_COMPILE_ABI_VERSION,
        .chip_id = UINT64_C(0x44050a31),
        .num_slices = 3,
        .num_ccu = 6,
        .highest_bank_bit = 16,
        .ubwc_swizzle = 6,
        .macrotile_mode = 1,
    };
    size_t len = 0;
    const uint32_t *words = nmb_spirv_words(result, index, &len);
    require(words != NULL && (uintptr_t)words % 4 == 0 && len > 20 && len % 4 == 0,
            "SPIR-V length and alignment");
    require(words[0] == UINT32_C(0x07230203), "SPIR-V magic");
    const char *entry = nmb_spirv_entry_point(result, index);
    require(entry != NULL && entry[0] != '\0', "SPIR-V entry point");
    const A840StageInput input = {
        .abi_version = ADRENO_COMPILE_ABI_VERSION,
        .stage = stage,
        .spirv_words = words,
        .spirv_bytes = len,
        .entry_point = entry,
        .sample_count = 1,
    };
    A840CompiledStage output;
    memset(&output, 0x5a, sizeof(output));
    unsigned char original[sizeof(output)];
    memcpy(original, &output, sizeof(output));
    char error[128] = {0};
    A840CompileStatus status = a840_compile_stage(&target, &input, &output, error, sizeof(error));
    if (status != A840_COMPILE_UNSUPPORTED_FEATURE ||
        strcmp(error, "ir3 backend lands in Wave 1") != 0 ||
        memcmp(original, &output, sizeof(output)) != 0) {
        fprintf(stderr, "%s: Adreno ABI returned %d (%s)\n", label, status, error);
        exit(EXIT_FAILURE);
    }
    save_spirv(label, words, len);
    printf("%s: entry=%s, %zu bytes, magic=0x%08x, Adreno ABI status=%d (%s)\n",
           label, entry, len, (unsigned)words[0], status, error);
}

static void check_bindings(const NmbSpirvResult *result, uint32_t allowed_stages,
                           bool require_descriptors, const char *pipeline)
{
    size_t count = nmb_spirv_binding_count(result);
    for (size_t i = 0; i < count; ++i) {
        NmbSpirvBinding binding;
        require(nmb_spirv_binding(result, i, &binding), "SPIR-V descriptor reflection");
        require(binding.descriptor_count != 0 && binding.stage_mask != 0 &&
                    (binding.stage_mask & ~allowed_stages) == 0,
                "SPIR-V descriptor count and stages");
    }
    require(!require_descriptors || count > 0, "expected reflected descriptors");
    NmbSpirvBinding sentinel = {.binding = UINT32_MAX};
    require(!nmb_spirv_binding(result, count, &sentinel) &&
                sentinel.binding == UINT32_MAX && strstr(nmb_last_error(), "index") != NULL,
            "binding index error");
    printf("%s: %zu reflected descriptor bindings\n", pipeline, count);
}

int main(int argc, char **argv)
{
    if (argc != 1 && argc != 3 && argc != 6) {
        fprintf(stderr, "usage: %s [compute.metallib render.metallib [compute_fn vertex_fn fragment_fn]]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    const char *compute_path = argc >= 3 ? argv[1] : compute_sample;
    const char *render_path = argc >= 3 ? argv[2] : render_sample;
    const char *compute_fn = argc == 6 ? argv[3] : "copyTexture";
    const char *vertex_fn = argc == 6 ? argv[4] : "kal_fullscreen_vert";
    const char *fragment_fn = argc == 6 ? argv[5] : "kal_rosette_frag";
    enum { MTL_RGBA8_UNORM = 70 };

    NmbDevice *dev = nmb_device_create_a840(0);
    require(dev != NULL, "create A840 translation device");
    NmbLibrary *compute_lib = load_library(dev, compute_path);
    require(!nmb_compile_compute_spirv(dev, compute_lib, "nonexistent_w01_l2_function") &&
                strstr(nmb_last_error(), "function") != NULL,
            "missing kernel error");
    NmbSpirvResult *compute = nmb_compile_compute_spirv(dev, compute_lib, compute_fn);
    require(compute != NULL, compute_fn);
    nmb_library_destroy(compute_lib);
    require(nmb_spirv_stage_count(compute) == 1, "compute stage count");
    check_stage(compute, 0, A840_STAGE_COMPUTE, "compute");
    size_t untouched = 42;
    require(!nmb_spirv_words(compute, 1, &untouched) && untouched == 42 &&
                strstr(nmb_last_error(), "stage index") != NULL,
            "compute stage index error");
    check_bindings(compute, NMB_STAGE_COMPUTE_BIT, strcmp(compute_fn, "copyTexture") == 0, "compute");
    nmb_spirv_result_free(compute);

    NmbLibrary *render_lib = load_library(dev, render_path);
    require(!nmb_compile_render_spirv(dev, render_lib, vertex_fn, fragment_fn, 0) &&
                strstr(nmb_last_error(), "pixel format") != NULL,
            "invalid color attachment format error");
    NmbSpirvResult *render =
        nmb_compile_render_spirv(dev, render_lib, vertex_fn, fragment_fn, MTL_RGBA8_UNORM);
    require(render != NULL, fragment_fn);
    nmb_library_destroy(render_lib);
    nmb_device_destroy(dev);
    require(nmb_spirv_stage_count(render) == 2, "render stage count");
    check_stage(render, 0, A840_STAGE_VERTEX, "vertex");
    check_stage(render, 1, A840_STAGE_FRAGMENT, "fragment");
    check_bindings(render, NMB_STAGE_VERTEX_BIT | NMB_STAGE_FRAGMENT_BIT, false, "render");
    nmb_spirv_result_free(render);
    puts("real metallib: compute + render VS/FS passed");
    return EXIT_SUCCESS;
}
