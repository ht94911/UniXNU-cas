/* SPDX-License-Identifier: MIT */
#include "a840_binding_plan.h"

#include <string.h>

enum {
    /* Numeric VkDescriptorType values from NmbSpirvBinding. */
    DESC_SAMPLER = 0,
    DESC_COMBINED_IMAGE_SAMPLER = 1,
    DESC_SAMPLED_IMAGE = 2,
    DESC_STORAGE_IMAGE = 3,
    DESC_UNIFORM_TEXEL_BUFFER = 4,
    DESC_STORAGE_TEXEL_BUFFER = 5,
    DESC_UNIFORM_BUFFER = 6,
    DESC_STORAGE_BUFFER = 7,
    DESC_UNIFORM_BUFFER_DYNAMIC = 8,
    DESC_STORAGE_BUFFER_DYNAMIC = 9,
    DESC_INPUT_ATTACHMENT = 10,
    VEC4_BYTES = 16,
    MAX_UBO_BYTES = 65536,
    /* a840_spirv_lower.c starts num_ubos at zero; Mesa ir3_get_driver_ubo
     * skips cb0 and numbers consts, params, primitive map and primitive params. */
    IR3_FIRST_DRIVER_UBO = 1,
    IR3_LAST_DRIVER_UBO = 4
};

const char *a840_binding_plan_status_string(A840BindingPlanStatus status)
{
    switch (status) {
    case A840_BINDING_PLAN_OK: return "OK";
    case A840_BINDING_PLAN_INVALID_ARG: return "invalid binding plan argument";
    case A840_BINDING_PLAN_CAPACITY: return "too many reflected bindings";
    case A840_BINDING_PLAN_STAGE: return "invalid stage order, ABI or visibility";
    case A840_BINDING_PLAN_DESCRIPTOR_TYPE: return "unsupported descriptor type";
    case A840_BINDING_PLAN_DESCRIPTOR_RANGE: return "invalid descriptor slot range or Metal slot kind";
    case A840_BINDING_PLAN_OVERLAP: return "overlapping descriptor or constant slots";
    case A840_BINDING_PLAN_CONST_RANGE: return "invalid push constant or inline UBO range";
    case A840_BINDING_PLAN_UBO_SOURCE: return "inline UBO source absent from stage bindings";
    }
    return "unknown binding plan status";
}

static uint32_t stage_bit(uint32_t stage)
{
    const uint32_t bits[] = {NMB_STAGE_VERTEX_BIT, NMB_STAGE_FRAGMENT_BIT, NMB_STAGE_COMPUTE_BIT};
    return bits[stage];
}

static bool before_binding(const NmbSpirvBinding *a, const NmbSpirvBinding *b)
{
    return a->descriptor_set < b->descriptor_set ||
        (a->descriptor_set == b->descriptor_set && a->binding < b->binding);
}

static A840BindingPlanStatus describe_binding(const NmbSpirvBinding *ref, A840BindingLocation *out)
{
    const uint32_t slot = ref->binding;
    const uint32_t absent = A840_BINDING_PLAN_NO_SLOT;
    *out = (A840BindingLocation){
        .set = ref->descriptor_set, .binding = slot, .descriptor_type = ref->descriptor_type,
        .descriptor_count = ref->descriptor_count, .stage_mask = ref->stage_mask,
        .tex_slot = absent, .samp_slot = absent, .ubo_slot = absent,
        .ssbo_slot = absent, .storage_img_slot = absent,
        .element_stride_bytes = A840_BINDING_PLAN_SLOT_BYTES,
    };
    switch (ref->descriptor_type) {
    case DESC_SAMPLER:
        out->samp_slot = slot;
        break;
    case DESC_COMBINED_IMAGE_SAMPLER:
        out->tex_slot = slot;
        out->samp_slot = slot + 1;
        out->element_stride_bytes *= 2;
        break;
    case DESC_SAMPLED_IMAGE:
    case DESC_UNIFORM_TEXEL_BUFFER:
    case DESC_INPUT_ATTACHMENT:
        out->tex_slot = slot;
        break;
    case DESC_STORAGE_IMAGE:
    case DESC_STORAGE_TEXEL_BUFFER:
        out->storage_img_slot = slot;
        break;
    case DESC_UNIFORM_BUFFER:
    case DESC_UNIFORM_BUFFER_DYNAMIC:
        out->ubo_slot = slot;
        break;
    case DESC_STORAGE_BUFFER:
    case DESC_STORAGE_BUFFER_DYNAMIC:
        out->ssbo_slot = slot;
        break;
    default:
        return A840_BINDING_PLAN_DESCRIPTOR_TYPE;
    }
    if ((ref->buffer_index != absent && out->ubo_slot == absent && out->ssbo_slot == absent) ||
        (ref->texture_index != absent && out->tex_slot == absent && out->storage_img_slot == absent) ||
        (ref->sampler_index != absent && out->samp_slot == absent))
        return A840_BINDING_PLAN_DESCRIPTOR_RANGE;
    return A840_BINDING_PLAN_OK;
}

static int compare_metal_key(const A840MetalBindingLocation *a, uint32_t stage,
    uint32_t kind, uint32_t index)
{
    if (a->stage != stage) return a->stage < stage ? -1 : 1;
    if (a->kind != kind) return a->kind < kind ? -1 : 1;
    if (a->metal_index != index) return a->metal_index < index ? -1 : 1;
    return 0;
}

static void add_metal_lookup(A840BindingPlan *plan, const NmbSpirvBinding *ref,
    const A840BindingLocation *binding)
{
    const uint32_t indices[] = {ref->buffer_index, ref->texture_index, ref->sampler_index};
    const uint32_t sampler_delta = binding->samp_slot == binding->binding + 1 ?
        A840_BINDING_PLAN_SLOT_BYTES : 0u;
    for (uint32_t stage = A840_STAGE_VERTEX; stage <= A840_STAGE_COMPUTE; ++stage) {
        if (!(ref->stage_mask & stage_bit(stage))) continue;
        for (uint32_t kind = A840_METAL_SLOT_BUFFER; kind <= A840_METAL_SLOT_SAMPLER; ++kind) {
            uint32_t index = indices[kind];
            if (index == A840_BINDING_PLAN_NO_SLOT) continue;
            uint32_t insert = plan->metal_lookup_count++;
            while (insert && compare_metal_key(&plan->metal_lookup[insert - 1], stage, kind, index) > 0) {
                plan->metal_lookup[insert] = plan->metal_lookup[insert - 1];
                --insert;
            }
            plan->metal_lookup[insert] = (A840MetalBindingLocation){
                .stage = stage, .kind = kind, .metal_index = index,
                .set = binding->set, .binding = binding->binding,
                .table_offset_bytes = binding->table_offset_bytes +
                    (kind == A840_METAL_SLOT_SAMPLER ? sampler_delta : 0),
                .element_stride_bytes = binding->element_stride_bytes,
                .descriptor_count = binding->descriptor_count,
            };
        }
    }
}

static A840BindingPlanStatus build_descriptor_tables(A840BindingPlan *plan,
    const NmbSpirvBinding *bindings, size_t count, uint32_t active_stages)
{
    const NmbSpirvBinding *ordered[A840_BINDING_PLAN_MAX_BINDINGS];
    for (size_t i = 0; i < count; ++i) {
        size_t insert = i;
        while (insert && before_binding(&bindings[i], ordered[insert - 1])) {
            ordered[insert] = ordered[insert - 1];
            --insert;
        }
        ordered[insert] = &bindings[i];
    }
    for (size_t i = 0; i < count; ++i) {
        const NmbSpirvBinding *ref = ordered[i];
        if (ref->descriptor_set >= ADRENO_COMPILE_MAX_BIND_SETS ||
            ref->binding >= A840_BINDING_PLAN_MAX_SLOTS_PER_SET || !ref->descriptor_count)
            return A840_BINDING_PLAN_DESCRIPTOR_RANGE;
        if (!ref->stage_mask || (ref->stage_mask & ~active_stages)) return A840_BINDING_PLAN_STAGE;

        A840BindingLocation *binding = &plan->bindings[i];
        A840BindingPlanStatus result = describe_binding(ref, binding);
        if (result != A840_BINDING_PLAN_OK) return result;
        uint32_t slots_per_element = binding->element_stride_bytes / A840_BINDING_PLAN_SLOT_BYTES;
        if (ref->descriptor_count > (A840_BINDING_PLAN_MAX_SLOTS_PER_SET - ref->binding) / slots_per_element)
            return A840_BINDING_PLAN_DESCRIPTOR_RANGE;

        A840BindingSetPlan *set = &plan->sets[ref->descriptor_set];
        uint32_t offset = ref->binding * A840_BINDING_PLAN_SLOT_BYTES;
        if (offset < set->table_bytes) return A840_BINDING_PLAN_OVERLAP;
        if (!set->binding_count) set->first_binding = (uint32_t)i;
        ++set->binding_count;
        set->table_bytes = offset + ref->descriptor_count * binding->element_stride_bytes;
    }
    for (uint32_t i = 0; i < ADRENO_COMPILE_MAX_BIND_SETS; ++i) {
        A840BindingSetPlan *set = &plan->sets[i];
        set->base_offset_bytes = plan->total_table_bytes;
        plan->total_table_bytes += set->table_bytes;
    }
    for (size_t i = 0; i < count; ++i) {
        A840BindingLocation *binding = &plan->bindings[i];
        binding->table_offset_bytes = plan->sets[binding->set].base_offset_bytes +
            binding->binding * A840_BINDING_PLAN_SLOT_BYTES;
        add_metal_lookup(plan, ordered[i], binding);
    }
    plan->binding_count = (uint32_t)count;
    return A840_BINDING_PLAN_OK;
}

static bool ubo_table_offset(const A840BindingPlan *plan, const A840UboPushRange *range,
    uint32_t stage, uint32_t *offset)
{
    const A840BindingSetPlan *set = &plan->sets[range->set_index];
    for (uint32_t i = 0; i < set->binding_count; ++i) {
        const A840BindingLocation *binding = &plan->bindings[set->first_binding + i];
        if (binding->ubo_slot == A840_BINDING_PLAN_NO_SLOT ||
            !(binding->stage_mask & stage_bit(stage)) || range->binding_index < binding->binding)
            continue;
        uint32_t element = range->binding_index - binding->binding;
        if (element < binding->descriptor_count) {
            *offset = binding->table_offset_bytes + element * binding->element_stride_bytes;
            return true;
        }
    }
    return false;
}

static bool claim_constants(bool *occupied, uint32_t first, uint32_t count)
{
    for (uint32_t i = first; i < first + count; ++i) {
        if (occupied[i]) return false;
        occupied[i] = true;
    }
    return true;
}

static A840BindingPlanStatus build_stage_plan(const A840BindingPlan *plan,
    const A840CompiledStage *stage, const NmbSpirvPushConstantRange *push, A840StageBindingPlan *out)
{
    if (stage->constlen_vec4 > A840_BINDING_PLAN_MAX_CONST_VEC4 ||
        stage->ubo_push_count > ADRENO_COMPILE_MAX_UBO_PUSH)
        return A840_BINDING_PLAN_CONST_RANGE;
    out->stage = stage->stage;
    out->const_bytes = stage->constlen_vec4 * VEC4_BYTES;
    bool occupied[A840_BINDING_PLAN_MAX_CONST_VEC4] = {false};
    if (push) {
        uint32_t offset = push->offset_bytes, size = push->size_bytes;
        if ((offset % 4) || (size % 4) || offset > A840_BINDING_PLAN_METAL_PUSH_BYTES ||
            size > A840_BINDING_PLAN_METAL_PUSH_BYTES - offset || (!size && offset))
            return A840_BINDING_PLAN_CONST_RANGE;
        if (size && !out->const_bytes) {
            out->inactive_push_constants = 1;
        } else if (size) {
            const uint32_t mask = A840_BINDING_PLAN_SLOT_BYTES - 1u;
            uint32_t source = offset & ~mask, end = (offset + size + mask) & ~mask;
            if (end - source > out->const_bytes) return A840_BINDING_PLAN_CONST_RANGE;
            out->push_constants = (A840ConstantCopy){source, 0, end - source};
            (void)claim_constants(occupied, 0, (end - source) / VEC4_BYTES);
        }
    }
    for (uint32_t i = 0; i < stage->ubo_push_count; ++i) {
        const A840UboPushRange *range = &stage->ubo_push[i];
        uint32_t first = range->dst_const_vec4, size = range->size_vec4;
        if (!size || range->reserved || first >= A840_BINDING_PLAN_MAX_CONST_VEC4 ||
            size > A840_BINDING_PLAN_MAX_CONST_VEC4 - first ||
            range->src_offset_vec4 >= MAX_UBO_BYTES / VEC4_BYTES ||
            size > MAX_UBO_BYTES / VEC4_BYTES - range->src_offset_vec4)
            return A840_BINDING_PLAN_CONST_RANGE;
        if (range->set_index >= ADRENO_COMPILE_MAX_BIND_SETS ||
            range->binding_index >= A840_BINDING_PLAN_MAX_SLOTS_PER_SET)
            return A840_BINDING_PLAN_DESCRIPTOR_RANGE;
        // Mesa records the UBO analysis ranges before preamble lowering. A
        // range entirely outside the final constlen has no upload destination.
        if (first >= stage->constlen_vec4) {
            ++out->inactive_ubo_count;
            continue;
        }
        if (size > stage->constlen_vec4 - first) return A840_BINDING_PLAN_CONST_RANGE;
        if (!claim_constants(occupied, first, size)) return A840_BINDING_PLAN_OVERLAP;

        uint32_t offset = A840_BINDING_PLAN_NO_SLOT;
        A840InlineUboSource source = A840_INLINE_UBO_BINDLESS;
        if (!ubo_table_offset(plan, range, stage->stage, &offset)) {
            if (range->set_index || range->binding_index < IR3_FIRST_DRIVER_UBO ||
                range->binding_index > IR3_LAST_DRIVER_UBO)
                return A840_BINDING_PLAN_UBO_SOURCE;
            source = A840_INLINE_UBO_IR3_BINDFUL;
        }
        out->inline_ubos[out->inline_ubo_count++] = (A840InlineUbo){
            .source = source, .set = range->set_index, .binding = range->binding_index,
            .table_offset_bytes = offset,
            .copy = {range->src_offset_vec4 * VEC4_BYTES, first * VEC4_BYTES, size * VEC4_BYTES},
        };
    }
    return A840_BINDING_PLAN_OK;
}

A840BindingPlanStatus a840_binding_plan_build(const NmbSpirvBinding *bindings,
    size_t binding_count, const A840CompiledStage *stages, size_t stage_count,
    const NmbSpirvPushConstantRange *push_constants, A840BindingPlan *out)
{
    if (!out || !stages || (!bindings && binding_count)) return A840_BINDING_PLAN_INVALID_ARG;
    if (binding_count > A840_BINDING_PLAN_MAX_BINDINGS) return A840_BINDING_PLAN_CAPACITY;
    if ((stage_count != 1 && stage_count != 2) ||
        (stage_count == 1 && stages[0].stage != A840_STAGE_COMPUTE) ||
        (stage_count == 2 && (stages[0].stage != A840_STAGE_VERTEX || stages[1].stage != A840_STAGE_FRAGMENT)))
        return A840_BINDING_PLAN_STAGE;

    uint32_t active_stages = 0;
    for (size_t i = 0; i < stage_count; ++i) {
        if (stages[i].abi_version != ADRENO_COMPILE_ABI_VERSION) return A840_BINDING_PLAN_STAGE;
        active_stages |= stage_bit(stages[i].stage);
    }
    A840BindingPlan plan;
    memset(&plan, 0, sizeof(plan));
    A840BindingPlanStatus status = build_descriptor_tables(&plan, bindings, binding_count, active_stages);
    if (status != A840_BINDING_PLAN_OK) return status;
    for (size_t i = 0; i < stage_count; ++i) {
        status = build_stage_plan(&plan, &stages[i], push_constants ? &push_constants[i] : NULL, &plan.stages[i]);
        if (status != A840_BINDING_PLAN_OK) return status;
    }
    plan.stage_count = (uint32_t)stage_count;
    *out = plan;
    return A840_BINDING_PLAN_OK;
}

const A840MetalBindingLocation *a840_binding_plan_lookup(const A840BindingPlan *plan,
    A840ShaderStage stage, A840MetalSlotKind kind, uint32_t metal_index, size_t *count)
{
    if (!count) return NULL;
    *count = 0;
    if (!plan || plan->metal_lookup_count > A840_BINDING_PLAN_MAX_LOOKUPS ||
        metal_index == A840_BINDING_PLAN_NO_SLOT)
        return NULL;
    size_t lo = 0, hi = plan->metal_lookup_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_metal_key(&plan->metal_lookup[mid], stage, kind, metal_index) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    const A840MetalBindingLocation *first = &plan->metal_lookup[lo];
    for (; lo < plan->metal_lookup_count; ++lo) {
        if (compare_metal_key(&plan->metal_lookup[lo], stage, kind, metal_index)) break;
        ++*count;
    }
    return *count ? first : NULL;
}
