/* SPDX-License-Identifier: MIT */
#include "NativeMetalTranslate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(NmbSpirvBinding) == 32, "legacy reflection ABI");
_Static_assert(sizeof(NmbVertexAttribute) == 16, "C/Rust vertex attribute ABI");
_Static_assert(sizeof(NmbVertexLayout) == 16, "C/Rust vertex layout ABI");
_Static_assert(offsetof(NmbRenderPipelineDesc, vertex_attributes) == 52, "C/Rust attribute array");
_Static_assert(offsetof(NmbRenderPipelineDesc, vertex_layouts) == 552, "C/Rust layout array");
_Static_assert(sizeof(NmbRenderPipelineDesc) == 1048, "C/Rust render descriptor ABI");
_Static_assert(sizeof(NmbComputePipelineDesc) == 16, "C/Rust compute descriptor ABI");

static const char samples[] =
    "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/";
static const char video[] =
    "/home/hua/nezha-work/private/ios27/camera27-c0/registration093-research/VideoToolbox-default.metallib";

enum {
    SPV_EXECUTION_MODE = 16, SPV_CONSTANT = 43,
    SPV_CONSTANT_COMPOSITE = 44, SPV_SPEC_CONSTANT = 50, SPV_SPEC_CONSTANT_COMPOSITE = 51,
    SPV_VARIABLE = 59, SPV_STORE = 62, SPV_DECORATE = 71, SPV_CONTROL_BARRIER = 224,
    SPV_WORKGROUP = 4, SPV_BUILTIN = 11, SPV_LOCAL_SIZE = 17,
    SPV_WORKGROUP_SIZE = 25, SPV_BINDING = 33, SPV_DESCRIPTOR_SET = 34,
};

static unsigned assertions;

static void check(bool ok, const char *what)
{
    ++assertions;
    if (!ok) {
        fprintf(stderr, "%s: %s\n", what, nmb_last_error());
        exit(EXIT_FAILURE);
    }
}
#define CHECK(expr) check((expr), #expr)

static NmbLibrary *load(const NmbDevice *dev, const char *path)
{
    FILE *file = fopen(path, "rb");
    check(file != NULL, path);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    CHECK(bytes != NULL);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    NmbLibrary *lib = nmb_library_parse(dev, bytes, (size_t)length);
    free(bytes);
    CHECK(lib != NULL);
    return lib;
}

static NmbLibrary *sample(const NmbDevice *dev, const char *name)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s%s", samples, name);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    return load(dev, path);
}

static size_t next_word(const uint32_t *words, size_t count, size_t pos)
{
    uint32_t length = words[pos] >> 16;
    check(length && length <= count - pos, "valid SPIR-V instruction length");
    return pos + length;
}

static uint32_t decoration(const uint32_t *words, size_t count, uint32_t id, uint32_t kind)
{
    for (size_t pos = 5; pos < count; pos = next_word(words, count, pos)) {
        const uint32_t *op = &words[pos];
        if (op[0] == (4u << 16 | SPV_DECORATE) && op[1] == id && op[2] == kind)
            return op[3];
    }
    return UINT32_MAX;
}

static const uint32_t *stage_words(const NmbSpirvResult *result, uint32_t stage, size_t *count)
{
    size_t bytes = 0;
    const uint32_t *words = nmb_spirv_words(result, stage, &bytes);
    CHECK(words && bytes >= 20 && bytes % 4 == 0 && words[0] == 0x07230203);
    CHECK(nmb_spirv_entry_point(result, stage) != NULL);
    *count = bytes / 4;
    return words;
}

static void check_bindless(const NmbSpirvResult *result, uint32_t index, uint32_t stage_mask)
{
    size_t count, bindings = nmb_spirv_binding_count(result);
    const uint32_t *words = stage_words(result, index, &count);
    uint32_t ends[8] = {0};
    for (size_t i = 0; i < bindings; ++i) {
        NmbSpirvBinding b;
        CHECK(nmb_spirv_binding(result, i, &b));
        CHECK(b.descriptor_set < 8 && b.binding >= ends[b.descriptor_set] && b.descriptor_count);
        ends[b.descriptor_set] = b.binding + b.descriptor_count * (b.descriptor_type == 1 ? 2u : 1u);
        CHECK(ends[b.descriptor_set] <= 65536);
    }
    for (size_t pos = 5; pos < count; pos = next_word(words, count, pos)) {
        const uint32_t *op = &words[pos];
        if (op[0] != (4u << 16 | SPV_DECORATE) || op[2] != SPV_BINDING) continue;
        uint32_t set = decoration(words, count, op[1], SPV_DESCRIPTOR_SET);
        bool found = false;
        for (size_t i = 0; i < bindings; ++i) {
            NmbSpirvBinding b;
            CHECK(nmb_spirv_binding(result, i, &b));
            if (b.descriptor_set == set && b.binding == op[3] && (b.stage_mask & stage_mask)) {
                found = true;
                break;
            }
        }
        check(found, "SPIR-V binding matches packed reflection");
    }
}

static void test_vertex(const NmbDevice *dev)
{
    const char *vert = "colorCubeVertex";
    const char *frag = "colorCubeFragment2D";
    NmbLibrary *lib = sample(dev, "MetalFilter.metallib");
    bool found = false;
    for (size_t i = 0; i < nmb_library_function_count(lib); ++i) {
        char name[64];
        uint32_t kind;
        CHECK(nmb_library_function_name(lib, i, name, sizeof(name)));
        CHECK(nmb_library_function_stage_kind(lib, i, &kind));
        CHECK(kind == (!strcmp(name, vert) ? NMB_FUNCTION_VERTEX : NMB_FUNCTION_FRAGMENT));
        found |= kind == NMB_FUNCTION_VERTEX;
    }
    CHECK(found);
    uint32_t unchanged = 77;
    CHECK(!nmb_library_function_stage_kind(lib, 500, &unchanged) && unchanged == 77);
    CHECK(!nmb_library_function_stage_kind(lib, 0, NULL));

    NmbRenderPipelineDesc desc;
    CHECK(nmb_library_default_render_desc(dev, lib, vert, frag, &desc));
    CHECK(desc.sample_count == 1 && desc.color_attachment_count == 1);
    CHECK(desc.vertex_attribute_count == 1 && desc.vertex_layout_count == 1);
    CHECK(desc.vertex_attributes[0].attribute_index == 0);
    CHECK(desc.vertex_attributes[0].mtl_vertex_format != 0);
    CHECK(desc.vertex_layouts[0].stride > 0);
    NmbSpirvResult *result = nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc);
    CHECK(result && nmb_spirv_stage_count(result) == 2);
    check_bindless(result, 0, NMB_STAGE_VERTEX_BIT);
    check_bindless(result, 1, NMB_STAGE_FRAGMENT_BIT);
    nmb_spirv_result_free(result);

    NmbRenderPipelineDesc layout = desc;
    layout.vertex_layouts[0].buffer_index = 3;
    layout.vertex_layouts[0].stride += 16;
    layout.vertex_layouts[0].step_function = 2; /* PerInstance */
    layout.vertex_layouts[0].step_rate = 2;
    layout.vertex_attributes[0].buffer_index = 3;
    layout.vertex_attributes[0].offset = 8;
    result = nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout);
    CHECK(result != NULL);
    nmb_spirv_result_free(result);

    layout.vertex_layouts[0].stride = UINT32_MAX;
    result = nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout);
    CHECK(result != NULL);
    nmb_spirv_result_free(result);

    layout.vertex_layouts[0].step_rate = 0;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout = desc;
    layout.vertex_attribute_count = 32;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout = desc;
    layout.vertex_layout_count = 32;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout.vertex_layout_count = 2;
    layout.vertex_layouts[1] = layout.vertex_layouts[0];
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout = desc;
    layout.vertex_attribute_count = 2;
    layout.vertex_attributes[1] = layout.vertex_attributes[0];
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout = desc;
    layout.vertex_layout_count = 0;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    layout = desc;
    layout.vertex_attributes[0].mtl_vertex_format = UINT32_MAX;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &layout));
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, NULL));
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, frag, vert, &desc));

    NmbRenderPipelineDesc unchanged_desc = desc;
    CHECK(!nmb_library_default_render_desc(dev, lib, frag, vert, &unchanged_desc));
    CHECK(!memcmp(&desc, &unchanged_desc, sizeof(desc)));
    nmb_library_destroy(lib);
    puts("PASS: MetalFilter vertex reflection, explicit layout/step/rate and rejection cases");
}

static void test_attachments(const NmbDevice *dev)
{
    const char *vert = "VTMTSVertexFunction";
    const char *frag = "VTMTSFragmentFunctionA";
    NmbLibrary *lib = load(dev, video);
    NmbRenderPipelineDesc desc;
    CHECK(nmb_library_default_render_desc(dev, lib, vert, frag, &desc));
    CHECK(desc.color_attachment_count == 2);
    CHECK(desc.color_mtl_formats[0] == 65 && desc.color_mtl_formats[1] == 25);

    desc.depth_mtl_format = 252; /* Depth32Float */
    NmbSpirvResult *result = nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc);
    CHECK(result && nmb_spirv_stage_count(result) == 2);
    NmbSpirvPushConstantRange push = {0};
    CHECK(nmb_spirv_push_constant_range(result, 0, &push) && push.size_bytes > 0);
    CHECK(!nmb_spirv_push_constant_range(result, 2, &push) && push.size_bytes > 0);
    nmb_spirv_result_free(result);

    desc.depth_mtl_format = desc.stencil_mtl_format = 260; /* Depth32Float_Stencil8 */
    desc.color_attachment_count = 4;
    desc.color_mtl_formats[3] = 70;
    for (uint32_t count = 2; count <= 4; count *= 2) {
        desc.sample_count = count;
        result = nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc);
        CHECK(result != NULL);
        nmb_spirv_result_free(result);
    }

    desc.sample_count = 3;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc));
    desc.sample_count = 1;
    desc.depth_mtl_format = 253; /* stencil-only format in the depth field */
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc));
    desc.depth_mtl_format = 0;
    desc.stencil_mtl_format = 252;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc));
    desc.stencil_mtl_format = 0;
    desc.color_attachment_count = 9;
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc));
    desc.color_attachment_count = 2;
    desc.color_mtl_formats[0] = 70; /* RGBA is wider than the actual RG output */
    CHECK(!nmb_compile_render_spirv_ex(dev, lib, vert, frag, &desc));
    nmb_library_destroy(lib);
    puts("PASS: VideoToolbox MRT, sparse colors, depth/stencil aspects and MSAA");
}

static void check_local_size(const uint32_t *words, size_t count, const uint32_t expected[3])
{
    uint32_t builtin = 0, literal[3] = {0}, components[3] = {0}, values[3] = {0};
    for (size_t pos = 5; pos < count; pos = next_word(words, count, pos)) {
        const uint32_t *op = &words[pos];
        if (op[0] == (4u << 16 | SPV_DECORATE) && op[2] == SPV_BUILTIN && op[3] == SPV_WORKGROUP_SIZE)
            builtin = op[1];
        if (op[0] == (6u << 16 | SPV_EXECUTION_MODE) && op[2] == SPV_LOCAL_SIZE)
            memcpy(literal, op + 3, sizeof(literal));
        if ((op[0] == (6u << 16 | SPV_SPEC_CONSTANT_COMPOSITE) ||
             op[0] == (6u << 16 | SPV_CONSTANT_COMPOSITE)) && op[2] == builtin)
            memcpy(components, op + 3, sizeof(components));
    }
    if (!builtin) {
        CHECK(!memcmp(literal, expected, sizeof(literal)));
        return;
    }
    CHECK(components[0] && components[1] && components[2]);
    for (size_t pos = 5; pos < count; pos = next_word(words, count, pos)) {
        const uint32_t *op = &words[pos];
        if (op[0] != (4u << 16 | SPV_SPEC_CONSTANT) && op[0] != (4u << 16 | SPV_CONSTANT)) continue;
        for (unsigned i = 0; i < 3; ++i)
            if (components[i] == op[2]) values[i] = op[3];
    }
    CHECK(!memcmp(values, expected, sizeof(values)));
}

static void test_compute(const NmbDevice *dev)
{
    const char *kernel = "fpr_slm_calc";
    NmbLibrary *lib = sample(dev, "MattingV2.metallib");
    NmbComputePipelineDesc desc = {{4, 2, 1}, 0};
    CHECK(!nmb_compile_compute_spirv(dev, lib, kernel));
    CHECK(strstr(nmb_last_error(), "ZeroedThreadgroupMemory") != NULL);
    CHECK(!nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc));
    CHECK(strstr(nmb_last_error(), "ZeroedThreadgroupMemory") != NULL);
    desc.zeroed_threadgroup_memory = 1;
    NmbSpirvResult *result = nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc);
    CHECK(result && nmb_spirv_stage_count(result) == 1);
    size_t count;
    const uint32_t *words = stage_words(result, 0, &count);
    check_local_size(words, count, desc.local_size);
    uint32_t shared[32];
    size_t shared_count = 0;
    bool stored = false, barrier_after_store = false;
    for (size_t pos = 5; pos < count; pos = next_word(words, count, pos)) {
        const uint32_t *op = &words[pos];
        uint32_t opcode = op[0] & 0xffff;
        if (opcode == SPV_VARIABLE && op[3] == SPV_WORKGROUP) {
            CHECK(op[0] >> 16 == 4);
            CHECK(shared_count < sizeof(shared) / sizeof(*shared));
            shared[shared_count++] = op[2];
        }
        if (opcode == SPV_STORE)
            for (size_t i = 0; i < shared_count; ++i) stored |= shared[i] == op[1];
        if (opcode == SPV_CONTROL_BARRIER && stored) barrier_after_store = true;
    }
    CHECK(shared_count && stored && barrier_after_store);
    check_bindless(result, 0, NMB_STAGE_COMPUTE_BIT);
    nmb_spirv_result_free(result);

    memset(desc.local_size, 0, sizeof(desc.local_size));
    result = nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc);
    CHECK(result != NULL);
    nmb_spirv_result_free(result);
    desc.local_size[0] = 1;
    CHECK(!nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc));
    desc.local_size[0] = UINT32_MAX;
    desc.local_size[1] = desc.local_size[2] = 32;
    CHECK(!nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc));
    memset(desc.local_size, 0, sizeof(desc.local_size));
    desc.zeroed_threadgroup_memory = 2;
    CHECK(!nmb_compile_compute_spirv_ex(dev, lib, kernel, &desc));
    CHECK(!nmb_compile_compute_spirv_ex(dev, lib, kernel, NULL));
    nmb_library_destroy(lib);
    puts("PASS: MattingV2 explicit workgroup size, zero stores/barrier, and legacy behavior");
}

int main(void)
{
    NmbDevice *dev = nmb_device_create_a840(0);
    CHECK(dev != NULL);
    CHECK(nmb_translate_abi_version() == 2);
    test_vertex(dev);
    test_attachments(dev);
    test_compute(dev);
    nmb_device_destroy(dev);
    printf("PASS: extended translate C ABI (%u assertions)\n", assertions);
    return EXIT_SUCCESS;
}
