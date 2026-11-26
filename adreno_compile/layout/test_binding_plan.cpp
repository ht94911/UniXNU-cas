/* SPDX-License-Identifier: MIT */
#include "a840_binding_plan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t none = A840_BINDING_PLAN_NO_SLOT;
constexpr uint32_t cs = NMB_STAGE_COMPUTE_BIT;
constexpr uint32_t vs = NMB_STAGE_VERTEX_BIT;
constexpr uint32_t fs = NMB_STAGE_FRAGMENT_BIT;
constexpr uint32_t sampler = 0, combined = 1, texture = 2, storage_image = 3;
constexpr uint32_t uniform_texel = 4, storage_texel = 5, ubo = 6, ssbo = 7;
constexpr uint32_t dynamic_ubo = 8, dynamic_ssbo = 9, input_attachment = 10;

unsigned cases, assertions;
const char *test_name;

void check(bool success, const char *expression)
{
    ++assertions;
    if (!success) {
        std::fprintf(stderr, "FAIL %s: %s\n", test_name, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expr) check((expr), #expr)

template <typename Run>
void test(const char *name, Run run)
{
    ++cases;
    test_name = name;
    run();
}

NmbSpirvBinding ref(uint32_t set, uint32_t binding, uint32_t type, uint32_t count = 1,
                    uint32_t stages = cs)
{
    return {set, binding, type, count, stages, none, none, none};
}

A840CompiledStage stage(uint32_t kind = A840_STAGE_COMPUTE, uint32_t constlen = 32)
{
    A840CompiledStage result{};
    result.abi_version = ADRENO_COMPILE_ABI_VERSION;
    result.stage = kind;
    result.constlen_vec4 = constlen;
    return result;
}

const A840CompiledStage compute = stage();
const std::array<A840CompiledStage, 2> graphics{stage(A840_STAGE_VERTEX), stage(A840_STAGE_FRAGMENT)};

A840BindingPlan build(const NmbSpirvBinding *bindings, size_t count, const A840CompiledStage *stages,
                      size_t stage_count, const NmbSpirvPushConstantRange *push = nullptr)
{
    A840BindingPlan result;
    const auto status = a840_binding_plan_build(bindings, count, stages, stage_count, push, &result);
    check(status == A840_BINDING_PLAN_OK, a840_binding_plan_status_string(status));
    return result;
}

void rejects(A840BindingPlanStatus expected, const NmbSpirvBinding *bindings, size_t count,
             const A840CompiledStage *stages = &compute, size_t stage_count = 1,
             const NmbSpirvPushConstantRange *push = nullptr)
{
    A840BindingPlan result, original;
    std::memset(&result, 0xa5, sizeof(result));
    std::memcpy(&original, &result, sizeof(result));
    const auto status = a840_binding_plan_build(bindings, count, stages, stage_count, push, &result);
    check(status == expected, a840_binding_plan_status_string(status));
    CHECK(std::memcmp(&result, &original, sizeof(result)) == 0);
}

void tables()
{
    test("empty compute", [] {
        const auto plan = build(nullptr, 0, &compute, 1);
        CHECK(plan.stage_count == 1 && plan.total_table_bytes == 0);
        CHECK(plan.stages[0].stage == A840_STAGE_COMPUTE && plan.stages[0].const_bytes == 512);
        CHECK(plan.binding_count == 0 && plan.metal_lookup_count == 0);
    });
    test("empty graphics", [] {
        const auto plan = build(nullptr, 0, graphics.data(), 2);
        CHECK(plan.stages[0].stage == A840_STAGE_VERTEX && plan.stages[1].stage == A840_STAGE_FRAGMENT);
    });
    struct DescriptorCase { uint32_t type; uint32_t slots; uint32_t A840BindingLocation::*member; };
    constexpr DescriptorCase types[] = {
        {sampler, 1, &A840BindingLocation::samp_slot},
        {combined, 2, &A840BindingLocation::tex_slot},
        {texture, 1, &A840BindingLocation::tex_slot},
        {storage_image, 1, &A840BindingLocation::storage_img_slot},
        {uniform_texel, 1, &A840BindingLocation::tex_slot},
        {storage_texel, 1, &A840BindingLocation::storage_img_slot},
        {ubo, 1, &A840BindingLocation::ubo_slot},
        {ssbo, 1, &A840BindingLocation::ssbo_slot},
        {dynamic_ubo, 1, &A840BindingLocation::ubo_slot},
        {dynamic_ssbo, 1, &A840BindingLocation::ssbo_slot},
        {input_attachment, 1, &A840BindingLocation::tex_slot},
    };
    for (const auto &kind : types) {
        char name[48];
        std::snprintf(name, sizeof(name), "descriptor type %u", kind.type);
        test(name, [&] {
            const auto binding = ref(0, 3, kind.type, 4);
            const auto plan = build(&binding, 1, &compute, 1);
            CHECK(plan.bindings[0].*kind.member == 3);
            CHECK(plan.bindings[0].element_stride_bytes == kind.slots * 64);
            CHECK(plan.bindings[0].table_offset_bytes == 3 * 64);
            CHECK(plan.sets[0].table_bytes == (3 + 4 * kind.slots) * 64);
        });
    }
    test("independent hardware sets and sparse offsets", [] {
        const NmbSpirvBinding bindings[] = {ref(7, 1, texture), ref(0, 2, sampler)};
        const auto plan = build(bindings, 2, &compute, 1);
        CHECK(plan.sets[0].base_offset_bytes == 0 && plan.sets[0].table_bytes == 192);
        CHECK(plan.sets[1].base_offset_bytes == 192 && plan.sets[1].table_bytes == 0);
        CHECK(plan.sets[7].base_offset_bytes == 192 && plan.sets[7].table_bytes == 128);
        CHECK(plan.bindings[1].table_offset_bytes == 256 && plan.total_table_bytes == 320);
        CHECK(plan.sets[7].first_binding == 1 && plan.sets[7].binding_count == 1);
    });
    test("all eight full bindless tables", [] {
        std::array<NmbSpirvBinding, 8> bindings{};
        for (uint32_t set = 0; set < bindings.size(); ++set)
            bindings[set] = ref(set, 0, texture, 65536);
        const auto plan = build(bindings.data(), bindings.size(), &compute, 1);
        CHECK(plan.total_table_bytes == 8 * 65536 * 64);
        CHECK(plan.sets[7].table_bytes == 65536 * 64);
        CHECK(plan.sets[7].base_offset_bytes == 7 * 65536 * 64);
    });
    test("last legal sampler slot", [] {
        const auto binding = ref(0, 65535, sampler);
        const auto plan = build(&binding, 1, &compute, 1);
        CHECK(plan.bindings[0].samp_slot == 65535);
        CHECK(plan.bindings[0].table_offset_bytes % A840_BINDING_PLAN_SAMPLER_BYTES == 0);
        CHECK(plan.total_table_bytes == 65536 * 64);
    });
    test("last legal combined pair", [] {
        const auto binding = ref(0, 65534, combined);
        const auto plan = build(&binding, 1, &compute, 1);
        CHECK(plan.bindings[0].tex_slot == 65534 && plan.bindings[0].samp_slot == 65535);
    });
    test("layout and lookup independent of input order", [] {
        std::array<NmbSpirvBinding, 3> bindings{ref(3, 2, combined), ref(0, 4, texture), ref(0, 1, texture)};
        for (auto &b : bindings) b.texture_index = 2;
        const auto forward = build(bindings.data(), bindings.size(), &compute, 1);
        std::reverse(bindings.begin(), bindings.end());
        const auto reverse = build(bindings.data(), bindings.size(), &compute, 1);
        CHECK(std::memcmp(&forward, &reverse, sizeof(forward)) == 0);
    });
    test("combined texture and sampler spans", [] {
        auto binding = ref(1, 1, combined, 5);
        binding.texture_index = 9;
        binding.sampler_index = 2;
        const auto plan = build(&binding, 1, &compute, 1);
        size_t count = 0;
        const auto *t = a840_binding_plan_lookup(&plan, A840_STAGE_COMPUTE, A840_METAL_SLOT_TEXTURE, 9, &count);
        CHECK(t && count == 1 && t->table_offset_bytes == 64 && t->element_stride_bytes == 128);
        CHECK(t->descriptor_count == 5 && t->set == 1 && t->binding == 1);
        const auto *s = a840_binding_plan_lookup(&plan, A840_STAGE_COMPUTE, A840_METAL_SLOT_SAMPLER, 2, &count);
        CHECK(s && count == 1 && s->table_offset_bytes == 128 && s->element_stride_bytes == 128);
        CHECK(s->table_offset_bytes % A840_BINDING_PLAN_SAMPLER_BYTES == 0);
    });
    test("stage visibility and same Metal index in different namespaces", [] {
        NmbSpirvBinding bindings[] = {ref(3, 5, ssbo, 1, vs), ref(2, 2, texture, 1, fs), ref(0, 1, ubo, 1, fs)};
        bindings[0].buffer_index = bindings[1].texture_index = bindings[2].buffer_index = 4;
        const auto plan = build(bindings, 3, graphics.data(), 2);
        size_t count;
        const auto *vert = a840_binding_plan_lookup(&plan, A840_STAGE_VERTEX, A840_METAL_SLOT_BUFFER, 4, &count);
        CHECK(vert && count == 1 && vert->set == 3 && vert->binding == 5);
        const auto *frag = a840_binding_plan_lookup(&plan, A840_STAGE_FRAGMENT, A840_METAL_SLOT_BUFFER, 4, &count);
        CHECK(frag && count == 1 && frag->set == 0 && frag->binding == 1);
        const auto *tex = a840_binding_plan_lookup(&plan, A840_STAGE_FRAGMENT, A840_METAL_SLOT_TEXTURE, 4, &count);
        CHECK(tex && count == 1 && tex->set == 2);
        CHECK(!a840_binding_plan_lookup(&plan, A840_STAGE_VERTEX, A840_METAL_SLOT_TEXTURE, 4, &count) && !count);
    });
    test("one Metal slot feeds several per-use descriptors", [] {
        NmbSpirvBinding bindings[] = {ref(7, 0, storage_image, 1, fs), ref(0, 4, texture, 1, vs | fs)};
        bindings[0].texture_index = bindings[1].texture_index = 6;
        const auto plan = build(bindings, 2, graphics.data(), 2);
        size_t count;
        const auto *frag = a840_binding_plan_lookup(&plan, A840_STAGE_FRAGMENT, A840_METAL_SLOT_TEXTURE, 6, &count);
        CHECK(frag && count == 2 && frag[0].set == 0 && frag[1].set == 7);
        const auto *vert = a840_binding_plan_lookup(&plan, A840_STAGE_VERTEX, A840_METAL_SLOT_TEXTURE, 6, &count);
        CHECK(vert && count == 1 && vert->set == 0);
    });
    test("maximum binding and lookup capacity", [] {
        std::array<NmbSpirvBinding, A840_BINDING_PLAN_MAX_BINDINGS> bindings{};
        for (uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index] = ref(0, 2 * index, combined, 1, vs | fs);
            bindings[index].texture_index = bindings[index].sampler_index = index;
        }
        const auto plan = build(bindings.data(), bindings.size(), graphics.data(), 2);
        CHECK(plan.binding_count == A840_BINDING_PLAN_MAX_BINDINGS);
        CHECK(plan.metal_lookup_count == A840_BINDING_PLAN_MAX_LOOKUPS);
        size_t count;
        const auto *last = a840_binding_plan_lookup(&plan, A840_STAGE_FRAGMENT, A840_METAL_SLOT_SAMPLER, 127, &count);
        CHECK(last && count == 1 && last->table_offset_bytes == 255 * 64);
        CHECK(!a840_binding_plan_lookup(&plan, A840_STAGE_FRAGMENT, A840_METAL_SLOT_SAMPLER, 128, &count) && !count);
    });
}

void invalid_descriptors()
{
    test("null binding array", [] { rejects(A840_BINDING_PLAN_INVALID_ARG, nullptr, 1); });
    test("null stages", [] { rejects(A840_BINDING_PLAN_INVALID_ARG, nullptr, 0, nullptr); });
    test("null output", [] {
        CHECK(a840_binding_plan_build(nullptr, 0, &compute, 1, nullptr, nullptr) == A840_BINDING_PLAN_INVALID_ARG);
    });
    test("null lookup", [] {
        size_t count = 42;
        CHECK(!a840_binding_plan_lookup(nullptr, A840_STAGE_VERTEX, A840_METAL_SLOT_BUFFER, 4, &count) && count == 0);
    });
    test("binding capacity exceeded", [] {
        const auto binding = ref(0, 0, texture);
        rejects(A840_BINDING_PLAN_CAPACITY, &binding, A840_BINDING_PLAN_MAX_BINDINGS + 1);
    });
    test("zero stages", [] { rejects(A840_BINDING_PLAN_STAGE, nullptr, 0, &compute, 0); });
    test("three stages", [] { rejects(A840_BINDING_PLAN_STAGE, nullptr, 0, &compute, 3); });
    test("single vertex", [] { rejects(A840_BINDING_PLAN_STAGE, nullptr, 0, graphics.data(), 1); });
    test("reversed graphics stages", [] {
        const A840CompiledStage reversed[] = {graphics[1], graphics[0]};
        rejects(A840_BINDING_PLAN_STAGE, nullptr, 0, reversed, 2);
    });
    test("compiled ABI mismatch", [] {
        auto mismatch = compute;
        mismatch.abi_version = 0;
        rejects(A840_BINDING_PLAN_STAGE, nullptr, 0, &mismatch);
    });

    struct InvalidCase { const char *name; NmbSpirvBinding binding; A840BindingPlanStatus status; };
    const InvalidCase invalid[] = {
        {"ninth set", ref(8, 0, texture), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"slot 65536", ref(0, 65536, texture), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"uint32 slot overflow", ref(0, UINT32_MAX, combined), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"empty array", ref(0, 0, texture, 0), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"array crosses table end", ref(0, 65535, texture, 2), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"combined array overflow", ref(0, 0, combined, UINT32_MAX), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"combined slot crosses table end", ref(7, 65535, combined), A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"unknown descriptor type", ref(0, 0, UINT32_MAX), A840_BINDING_PLAN_DESCRIPTOR_TYPE},
        {"no stage visibility", ref(0, 0, ubo, 1, 0), A840_BINDING_PLAN_STAGE},
        {"wrong stage visibility", ref(0, 0, ubo, 1, vs), A840_BINDING_PLAN_STAGE},
        {"unknown stage visibility bit", ref(0, 0, ubo, 1, cs | 8), A840_BINDING_PLAN_STAGE},
        {"texture with Metal buffer index", {0, 0, texture, 1, cs, 0, none, none}, A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"sampler with Metal texture index", {0, 0, sampler, 1, cs, none, 0, none}, A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"UBO with Metal sampler index", {0, 0, ubo, 1, cs, none, none, 0}, A840_BINDING_PLAN_DESCRIPTOR_RANGE},
    };
    for (const auto &item : invalid)
        test(item.name, [&] { rejects(item.status, &item.binding, 1); });
    test("duplicate slot with disjoint stages", [] {
        const NmbSpirvBinding overlap[] = {ref(0, 2, ubo, 1, vs), ref(0, 2, ubo, 1, fs)};
        rejects(A840_BINDING_PLAN_OVERLAP, overlap, 2, graphics.data(), 2);
    });
    test("combined sampler conflicts with next descriptor", [] {
        const NmbSpirvBinding overlap[] = {ref(0, 0, combined, 2), ref(0, 3, sampler)};
        rejects(A840_BINDING_PLAN_OVERLAP, overlap, 2);
    });
    test("ordinary array conflict", [] {
        const NmbSpirvBinding overlap[] = {ref(7, 2, texture, 8), ref(7, 9, ssbo)};
        rejects(A840_BINDING_PLAN_OVERLAP, overlap, 2);
    });
}

void constants()
{
    test("maximum constant file", [] {
        const auto large = stage(A840_STAGE_COMPUTE, 512);
        CHECK(build(nullptr, 0, &large, 1).stages[0].const_bytes == 8192);
    });
    test("constant file exceeds hardware", [] {
        const auto large = stage(A840_STAGE_COMPUTE, 513);
        rejects(A840_BINDING_PLAN_CONST_RANGE, nullptr, 0, &large);
    });
    test("nonzero source base for push constants", [] {
        NmbSpirvPushConstantRange push{80, 12};
        const auto plan = build(nullptr, 0, &compute, 1, &push);
        const auto &copy = plan.stages[0].push_constants;
        CHECK(copy.src_offset_bytes == 64 && copy.dst_offset_bytes == 0 && copy.size_bytes == 64);
    });
    test("push constants cross upload alignment", [] {
        NmbSpirvPushConstantRange push{60, 8};
        CHECK(build(nullptr, 0, &compute, 1, &push).stages[0].push_constants.size_bytes == 128);
    });
    test("explicitly unused push constants", [] {
        NmbSpirvPushConstantRange push{};
        CHECK(build(nullptr, 0, &compute, 1, &push).stages[0].push_constants.size_bytes == 0);
    });
    test("optimized stage has no push upload", [] {
        const auto optimized = stage(A840_STAGE_COMPUTE, 0);
        NmbSpirvPushConstantRange push{64, 4};
        const auto plan = build(nullptr, 0, &optimized, 1, &push);
        CHECK(plan.stages[0].inactive_push_constants == 1);
        CHECK(plan.stages[0].push_constants.size_bytes == 0);
    });
    test("VS and FS get distinct constant files", [] {
        NmbSpirvPushConstantRange push[] = {{32, 4}, {64, 4}};
        const auto plan = build(nullptr, 0, graphics.data(), 2, push);
        CHECK(plan.stages[0].push_constants.src_offset_bytes == 0);
        CHECK(plan.stages[1].push_constants.src_offset_bytes == 64);
        CHECK(plan.stages[0].push_constants.dst_offset_bytes == 0 && plan.stages[1].push_constants.dst_offset_bytes == 0);
    });
    test("unaligned push offset", [] {
        NmbSpirvPushConstantRange push{1, 4};
        rejects(A840_BINDING_PLAN_CONST_RANGE, nullptr, 0, &compute, 1, &push);
    });
    test("unaligned push size", [] {
        NmbSpirvPushConstantRange push{0, 15};
        rejects(A840_BINDING_PLAN_CONST_RANGE, nullptr, 0, &compute, 1, &push);
    });
    test("push window outside Metal limit", [] {
        NmbSpirvPushConstantRange push{124, 8};
        rejects(A840_BINDING_PLAN_CONST_RANGE, nullptr, 0, &compute, 1, &push);
    });
    test("push window outside constlen", [] {
        const auto small = stage(A840_STAGE_COMPUTE, 3);
        NmbSpirvPushConstantRange push{0, 4};
        rejects(A840_BINDING_PLAN_CONST_RANGE, nullptr, 0, &small, 1, &push);
    });

    const auto binding = ref(6, 1, ubo, 4);
    const A840UboPushRange range{6, 3, 1, 8, 2, 0};
    auto pushing = compute;
    pushing.ubo_push_count = 1;
    pushing.ubo_push[0] = range;
    test("inline UBO uses exact array element", [&] {
        const auto plan = build(&binding, 1, &pushing, 1);
        CHECK(plan.stages[0].inline_ubo_count == 1);
        const auto &entry = plan.stages[0].inline_ubos[0];
        CHECK(entry.source == A840_INLINE_UBO_BINDLESS);
        CHECK(entry.set == 6 && entry.binding == 3 && entry.table_offset_bytes == 3 * 64);
        CHECK(entry.copy.src_offset_bytes == 16 && entry.copy.dst_offset_bytes == 128 && entry.copy.size_bytes == 32);
    });
    test("all sixteen inline UBO ranges", [&] {
        auto full = pushing;
        full.ubo_push_count = 16;
        for (uint32_t i = 0; i < full.ubo_push_count; ++i)
            full.ubo_push[i] = {6, 1, i, i, 1, 0};
        CHECK(build(&binding, 1, &full, 1).stages[0].inline_ubo_count == 16);
    });
    test("seventeenth UBO range", [&] {
        auto too_many = pushing;
        too_many.ubo_push_count = 17;
        rejects(A840_BINDING_PLAN_CONST_RANGE, &binding, 1, &too_many);
    });
    test("inactive IR3 UBO analysis range", [&] {
        auto inactive = pushing;
        inactive.ubo_push[0].dst_const_vec4 = inactive.constlen_vec4;
        const auto plan = build(nullptr, 0, &inactive, 1);
        CHECK(plan.stages[0].inline_ubo_count == 0 && plan.stages[0].inactive_ubo_count == 1);
    });
    test("partially out-of-range UBO upload", [&] {
        auto partial = pushing;
        partial.ubo_push[0].dst_const_vec4 = partial.constlen_vec4 - 1;
        rejects(A840_BINDING_PLAN_CONST_RANGE, &binding, 1, &partial);
    });
    test("UBO overlaps push constants", [&] {
        auto overlap = pushing;
        overlap.ubo_push[0].dst_const_vec4 = 3;
        NmbSpirvPushConstantRange push{0, 4};
        rejects(A840_BINDING_PLAN_OVERLAP, &binding, 1, &overlap, 1, &push);
    });
    test("two inline UBO ranges overlap", [&] {
        auto overlap = pushing;
        overlap.ubo_push_count = 2;
        overlap.ubo_push[1] = {6, 4, 5, 9, 1, 0};
        rejects(A840_BINDING_PLAN_OVERLAP, &binding, 1, &overlap);
    });
    test("IR3 driver UBO uses the bindful namespace", [&] {
        auto internal = pushing;
        internal.ubo_push[0] = {0, 1, 1, 12, 1, 0};
        const auto plan = build(nullptr, 0, &internal, 1);
        const auto &copy = plan.stages[0].inline_ubos[0];
        CHECK(plan.stages[0].inline_ubo_count == 1 && plan.total_table_bytes == 0);
        CHECK(copy.source == A840_INLINE_UBO_IR3_BINDFUL && copy.binding == 1);
        CHECK(copy.table_offset_bytes == none && copy.copy.dst_offset_bytes == 192);
    });
    test("unused set zero block outside the IR3 driver range", [&] {
        auto unknown = pushing;
        unknown.ubo_push[0] = {0, 5, 1, 12, 1, 0};
        rejects(A840_BINDING_PLAN_UBO_SOURCE, nullptr, 0, &unknown);
    });
    test("UBO binding source absent", [&] {
        rejects(A840_BINDING_PLAN_UBO_SOURCE, nullptr, 0, &pushing);
    });
    test("UBO source slot has another descriptor type", [&] {
        const auto image = ref(6, 3, texture);
        rejects(A840_BINDING_PLAN_UBO_SOURCE, &image, 1, &pushing);
    });
    test("UBO source belongs to another stage", [&] {
        auto stages = graphics;
        stages[1].ubo_push_count = 1;
        stages[1].ubo_push[0] = range;
        const auto other = ref(6, 1, ubo, 4, vs);
        rejects(A840_BINDING_PLAN_UBO_SOURCE, &other, 1, stages.data(), 2);
    });

    struct InvalidUboCase { const char *name; A840UboPushRange range; A840BindingPlanStatus status; };
    const InvalidUboCase invalid[] = {
        {"zero-sized UBO range", {6, 3, 0, 0, 0, 0}, A840_BINDING_PLAN_CONST_RANGE},
        {"reserved UBO field", {6, 3, 0, 0, 1, 1}, A840_BINDING_PLAN_CONST_RANGE},
        {"UBO source size overflow", {6, 3, 4095, 0, 2, 0}, A840_BINDING_PLAN_CONST_RANGE},
        {"UBO destination overflow", {6, 3, 0, 511, 2, 0}, A840_BINDING_PLAN_CONST_RANGE},
        {"UBO destination start overflow", {6, 3, 0, UINT32_MAX, 1, 0}, A840_BINDING_PLAN_CONST_RANGE},
        {"UBO set overflow", {8, 3, 0, 0, 1, 0}, A840_BINDING_PLAN_DESCRIPTOR_RANGE},
        {"UBO slot overflow", {6, UINT32_MAX, 0, 0, 1, 0}, A840_BINDING_PLAN_DESCRIPTOR_RANGE},
    };
    for (const auto &item : invalid) {
        test(item.name, [&] {
            auto bad = pushing;
            bad.ubo_push[0] = item.range;
            rejects(item.status, &binding, 1, &bad);
        });
    }
}
} // namespace

int main()
{
    tables();
    invalid_descriptors();
    constants();
    std::printf("PASS: %u independent binding plan cases, %u assertions\n", cases, assertions);
}
