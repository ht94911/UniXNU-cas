/* SPDX-License-Identifier: MIT */
#ifndef A840_BINDING_PLAN_H
#define A840_BINDING_PLAN_H

#include "AdrenoCompileABI.h"
#include "../../translate/include/NativeMetalTranslate.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_BINDING_PLAN_MAX_BINDINGS = 128,
    A840_BINDING_PLAN_MAX_LOOKUPS = 4 * A840_BINDING_PLAN_MAX_BINDINGS,
    A840_BINDING_PLAN_MAX_SLOTS_PER_SET = 65536,
    A840_BINDING_PLAN_SLOT_BYTES = 64,
    A840_BINDING_PLAN_SAMPLER_BYTES = 16,
    A840_BINDING_PLAN_MAX_CONST_VEC4 = 512,
    A840_BINDING_PLAN_METAL_PUSH_BYTES = 128
};
#define A840_BINDING_PLAN_NO_SLOT UINT32_MAX

typedef enum A840BindingPlanStatus {
    A840_BINDING_PLAN_OK = 0,
    A840_BINDING_PLAN_INVALID_ARG,
    A840_BINDING_PLAN_CAPACITY,
    A840_BINDING_PLAN_STAGE,
    A840_BINDING_PLAN_DESCRIPTOR_TYPE,
    A840_BINDING_PLAN_DESCRIPTOR_RANGE,
    A840_BINDING_PLAN_OVERLAP,
    A840_BINDING_PLAN_CONST_RANGE,
    A840_BINDING_PLAN_UBO_SOURCE
} A840BindingPlanStatus;

typedef enum A840MetalSlotKind {
    A840_METAL_SLOT_BUFFER = 0,
    A840_METAL_SLOT_TEXTURE,
    A840_METAL_SLOT_SAMPLER
} A840MetalSlotKind;

typedef struct A840BindingSetPlan {
    uint32_t base_offset_bytes, table_bytes;
    uint32_t first_binding, binding_count; /* Index into A840BindingPlan.bindings. */
} A840BindingSetPlan;

typedef struct A840BindingLocation {
    uint32_t set, binding, descriptor_type, descriptor_count, stage_mask;
    /* First hardware slot within the set; UINT32_MAX = absent. */
    uint32_t tex_slot, samp_slot, ubo_slot, ssbo_slot, storage_img_slot;
    /* From the start of the entire allocation, including preceding sets. */
    uint32_t table_offset_bytes, element_stride_bytes;
} A840BindingLocation;

typedef struct A840MetalBindingLocation {
    uint32_t stage; /* A840ShaderStage */
    uint32_t kind;  /* A840MetalSlotKind */
    uint32_t metal_index;
    uint32_t set, binding;
    /* Byte offset of the first element, including the sampler half if combined. */
    uint32_t table_offset_bytes, element_stride_bytes, descriptor_count;
} A840MetalBindingLocation;

typedef struct A840ConstantCopy {
    uint32_t src_offset_bytes, dst_offset_bytes, size_bytes;
} A840ConstantCopy;

typedef enum A840InlineUboSource {
    A840_INLINE_UBO_BINDLESS = 0,
    A840_INLINE_UBO_IR3_BINDFUL = 1
} A840InlineUboSource;

typedef struct A840InlineUbo {
    uint32_t source; /* A840InlineUboSource */
    uint32_t set, binding; /* Exact hardware slot, also for an element of an array. */
    /* IR3 bindful blocks use the stage-local UBO binding path, outside the
     * bindless table. They have no Metal slot and table_offset_bytes is UINT32_MAX. */
    uint32_t table_offset_bytes;
    A840ConstantCopy copy;
} A840InlineUbo;

typedef struct A840StageBindingPlan {
    uint32_t stage, const_bytes;
    A840ConstantCopy push_constants;
    uint32_t inactive_push_constants; /* Reflected, then eliminated by IR3. */
    uint32_t inline_ubo_count;
    uint32_t inactive_ubo_count; /* Compiler ranges entirely outside emitted constlen. */
    A840InlineUbo inline_ubos[ADRENO_COMPILE_MAX_UBO_PUSH];
} A840StageBindingPlan;

typedef struct A840BindingPlan {
    uint32_t total_table_bytes, binding_count, metal_lookup_count, stage_count;
    A840BindingSetPlan sets[ADRENO_COMPILE_MAX_BIND_SETS];
    A840BindingLocation bindings[A840_BINDING_PLAN_MAX_BINDINGS];
    A840MetalBindingLocation metal_lookup[A840_BINDING_PLAN_MAX_LOOKUPS];
    A840StageBindingPlan stages[2]; /* Compute or ordered VS+FS. */
} A840BindingPlan;

const char *a840_binding_plan_status_string(A840BindingPlanStatus status);

/* No allocation. Failure preserves *out. Output storage must be distinct from
 * the input arrays. The binding numbers must already match the shaders' IR3
 * slots, as they do from the translator's _ex entry points. The _ex numbering
 * leaves set 0 slots 1..4 available to identify the compiler's bindful UBOs.
 * A hardware slot is 64 bytes even for a 16-byte sampler. A combined descriptor occupies two
 * consecutive slots per element, texture then sampler. No resource data is
 * owned or copied into the plan.
 *
 * push_constants, when supplied, is the reflected source range per stage.
 * The pinned A840 lowering allocates the first push window at const vec4 0,
 * with a 64-byte source base and upload granularity. The plan pads the ranges
 * to that granularity and checks all active copies against the emitted file. */
A840BindingPlanStatus a840_binding_plan_build(const NmbSpirvBinding *bindings,
    size_t binding_count, const A840CompiledStage *stages, size_t stage_count,
    const NmbSpirvPushConstantRange *push_constants, A840BindingPlan *out);

/* O(log n) lookup by stage, kind and reflected Metal base index. Returns a
 * borrowed contiguous span sorted by (set,binding), including when a Metal
 * slot supplies several per-use descriptors. Array elements are addressed by
 * element_stride_bytes. On miss, returns NULL and writes zero to *count. */
const A840MetalBindingLocation *a840_binding_plan_lookup(const A840BindingPlan *plan,
    A840ShaderStage stage, A840MetalSlotKind kind, uint32_t metal_index, size_t *count);

#ifdef __cplusplus
}
#endif
#endif /* A840_BINDING_PLAN_H */
