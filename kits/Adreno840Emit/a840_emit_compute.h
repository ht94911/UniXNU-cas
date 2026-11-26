/* SPDX-License-Identifier: MIT */
#ifndef A840_EMIT_COMPUTE_H
#define A840_EMIT_COMPUTE_H

#include "Adreno840EmitABI.h"
#include "AdrenoCompileABI.h"

/* Maximum packet length in dwords, including headers. */
#define A840_EMIT_SHADER_BIND_DWORDS 24u
#define A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS (A840_EMIT_SHADER_BIND_DWORDS + 15u)
#define A840_EMIT_COMPUTE_BINDLESS_MAX_DWORDS (1u + 2u * ADRENO_COMPILE_MAX_BIND_SETS)
#define A840_COMPUTE_MAX_CONST_VEC4 512u
#define A840_EMIT_COMPUTE_CONSTS_MAX_DWORDS (4u + 4u * A840_COMPUTE_MAX_CONST_VEC4)
#define A840_EMIT_COMPUTE_DISPATCH_MAX_DWORDS 24u

/* The IR3 register number for a missing input or output (r63.x). */
#define A840_REGID_UNUSED (63u * 4u)

typedef struct A840GpuAddress {
    uint64_t bo_handle;
    uint64_t gpu_va;
} A840GpuAddress;

/* The stage metadata is borrowed for the duration of the call. code must
 * reference a 128-byte aligned GPU upload of its ISA. Private storage is
 * caller-owned, sized for the chip's SPs and the requested per-fiber size.
 * Its layout must match the compiler's per-fiber/per-wave selection. */
typedef struct A840ShaderCode {
    const A840CompiledStage *stage;
    A840GpuAddress code, private_memory;
    uint32_t textures, samplers, uavs;
    bool per_wave_private_memory;
    bool bindless_tex, bindless_samp, bindless_uav, bindless_ubo;
} A840ShaderCode;

typedef struct A840ComputePipelineDesc {
    A840ShaderCode shader;
    uint8_t local_id_regid, wg_id_regid;
    bool round_robin, linear_dispatch;
} A840ComputePipelineDesc;

typedef struct A840BindlessTableDesc {
    /* Update slots [0,count). A zero address explicitly clears that slot.
     * Populated slots contain 64-byte A8XX descriptors. */
    uint32_t count;
    A840GpuAddress tables[ADRENO_COMPILE_MAX_BIND_SETS];
} A840BindlessTableDesc;

typedef enum A840ConstSource {
    A840_CONST_DIRECT = 0,
    A840_CONST_INDIRECT = 1
} A840ConstSource;

typedef struct A840ConstUploadDesc {
    A840ConstSource source;
    uint32_t dst_vec4, count_vec4, constlen_vec4;
    const uint32_t *words;   /* DIRECT: exactly 4 * count_vec4 readable words */
    A840GpuAddress address;  /* INDIRECT: the 16-byte aligned source */
} A840ConstUploadDesc;

typedef enum A840DispatchMode {
    A840_DISPATCH_UNIFORM = 0,
    A840_DISPATCH_NON_UNIFORM = 1,
    A840_DISPATCH_INDIRECT = 2
} A840DispatchMode;

typedef struct A840DispatchDesc {
    const A840CompiledStage *stage; /* same compiled stage as the bound program */
    A840DispatchMode mode;
    /* Threadgroup counts (uniform), or grid thread counts (non-uniform). */
    uint32_t size[3];
    A840GpuAddress indirect; /* three uint32_t group counts */
} A840DispatchDesc;

#ifdef __cplusplus
extern "C" {
#endif

/* All errors leave word and relocation storage and cursors untouched.
 * The caller handles resource visibility and inter-pass synchronization. */
A840EmitStatus a840_emit_compute_pipeline(A840CmdStream *cs, const A840ComputePipelineDesc *desc);
A840EmitStatus a840_emit_compute_bindless(A840CmdStream *cs, const A840BindlessTableDesc *desc);
A840EmitStatus a840_emit_compute_consts(A840CmdStream *cs, const A840ConstUploadDesc *desc);
A840EmitStatus a840_emit_compute_dispatch(A840CmdStream *cs, const A840DispatchDesc *desc);

#ifdef __cplusplus
}
#endif

#ifdef A840_EMIT_PIPELINE_INTERNAL
#include "a840_emit_ops.h"

static inline A840EmitStatus a840_emit_reg(A840CmdStream *cs, uint32_t reg, uint32_t value)
{
    return a840_cs_emit_pkt4(cs, reg, &value, 1);
}

/* Packet address fields can share a packet with non-address words. */
static inline A840EmitStatus a840_emit_record_address(A840CmdStream *batch, size_t index,
    A840GpuAddress address, uint32_t flags)
{
    if (batch->reloc_count == batch->reloc_capacity) return A840_EMIT_CAPACITY;
    if (index > UINT32_MAX || index >= batch->used_dwords ||
        batch->used_dwords - index < 2) return A840_EMIT_RANGE_ERROR;
    batch->relocs[batch->reloc_count++] = (A840Reloc){
        (uint32_t)index, flags, address.bo_handle, address.gpu_va,
    };
    return A840_EMIT_OK;
}

/* Keep the W01 transaction discipline for the larger variable-size packets. */
static inline A840EmitStatus a840_emit_commit(A840CmdStream *cs, const A840CmdStream *batch)
{
    A840OpsSpan words, relocs, metadata;
    A840_OPS_TRY(a840_ops_cs_storage(cs, &words, &relocs, &metadata));
    size_t reloc_count = cs->relocs ? batch->reloc_count : 0;
    if (batch->used_dwords > cs->capacity_dwords - cs->used_dwords ||
        reloc_count > cs->reloc_capacity - cs->reloc_count) return A840_EMIT_CAPACITY;
    size_t start = cs->used_dwords;
    for (size_t i = 0; i < reloc_count; ++i) {
        if (start > (size_t)UINT32_MAX - batch->relocs[i].dword_offset)
            return A840_EMIT_RANGE_ERROR;
    }
    if (batch->used_dwords)
        memcpy(cs->words + start, batch->words, batch->used_dwords * sizeof(*cs->words));
    for (size_t i = 0; i < reloc_count; ++i) {
        A840Reloc reloc = batch->relocs[i];
        reloc.dword_offset += (uint32_t)start;
        cs->relocs[cs->reloc_count + i] = reloc;
    }
    cs->used_dwords += batch->used_dwords;
    cs->reloc_count += reloc_count;
    return A840_EMIT_OK;
}

A840EmitStatus a840_emit_shader_bind(A840CmdStream *batch, const A840ShaderCode *shader,
    uint32_t stage, uint32_t control);
#endif /* A840_EMIT_PIPELINE_INTERNAL */

#endif /* A840_EMIT_COMPUTE_H */
