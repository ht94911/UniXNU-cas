/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#define A840_EMIT_PIPELINE_INTERNAL
#include "a840_emit_compute.h"
#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

typedef struct A840ShaderRegisters {
    uint32_t control, length, pc, code, private_param, private_base, private_size,
        private_stack, vgs, config, constants;
} A840ShaderRegisters;

#define SHADER_REGISTERS(stage) { \
    A840_REG_SP_##stage##_CNTL_0, A840_REG_SP_##stage##_INSTR_SIZE, \
    A840_REG_SP_##stage##_PROGRAM_COUNTER_OFFSET, A840_REG_SP_##stage##_BASE, \
    A840_REG_SP_##stage##_PVT_MEM_PARAM, A840_REG_SP_##stage##_PVT_MEM_BASE, \
    A840_REG_SP_##stage##_PVT_MEM_SIZE, A840_REG_SP_##stage##_PVT_MEM_STACK_OFFSET, \
    A840_REG_SP_##stage##_VGS_CNTL, A840_REG_SP_##stage##_CONFIG, \
    A840_REG_SP_##stage##_CONST_CONFIG \
}

static const A840ShaderRegisters shader_regs[] = {
    [A840_STAGE_VERTEX] = SHADER_REGISTERS(VS),
    [A840_STAGE_FRAGMENT] = SHADER_REGISTERS(PS),
    [A840_STAGE_COMPUTE] = SHADER_REGISTERS(CS),
};

/* AdrenoCompileABI counts 16-byte groups. IR3 A6XX+ instrlen counts 16
 * instructions of 8 bytes each (ir3_compiler.c:367, ir3.c:374). */
enum { TEXT_BYTES_PER_INSTRLEN = 128, GROUPS_PER_INSTRLEN = TEXT_BYTES_PER_INSTRLEN / 16 };

static A840EmitStatus shader_instrlen(const A840CompiledStage *stage, uint32_t kind,
    uint32_t *instrlen)
{
    if (stage->abi_version != ADRENO_COMPILE_ABI_VERSION || stage->stage != kind)
        return A840_EMIT_INVALID_ARG;
    if (!stage->instruction_groups || !stage->isa_dwords ||
        (uint64_t)stage->instruction_groups * 4u > stage->isa_dwords)
        return A840_EMIT_RANGE_ERROR;
    uint32_t units = (stage->instruction_groups - 1) / GROUPS_PER_INSTRLEN + 1;
    if (units > A840_INSTR_SIZE_MAX) return A840_EMIT_RANGE_ERROR;
    *instrlen = units;
    return A840_EMIT_OK;
}

static A840EmitStatus private_layout(const A840ShaderCode *shader,
    uint32_t *item_bytes, uint32_t *sp_bytes)
{
    const uint32_t private_bytes = shader->stage->private_mem_bytes;
    *item_bytes = 0;
    *sp_bytes = 0;
    if (!private_bytes) return A840_EMIT_OK;
    if (private_bytes > (A840_PRIVATE_ITEM_MAX << A840_PRIVATE_ITEM_SHR))
        return A840_EMIT_RANGE_ERROR;
    /* Match tu_shader.cc:2875-2882, with aligned per-SP allocations. */
    uint32_t item = 1u << A840_PRIVATE_ITEM_SHR;
    while (item < private_bytes)
        item *= 2u;
    uint64_t per_sp = item * A840_LIMIT_FIBERS_PER_SP;
    if ((item >> A840_PRIVATE_ITEM_SHR) > A840_PRIVATE_ITEM_MAX ||
        (per_sp >> A840_PRIVATE_SIZE_SHR) > A840_PRIVATE_SIZE_MAX ||
        (per_sp >> A840_PRIVATE_STACK_SHR) > A840_PRIVATE_STACK_MAX)
        return A840_EMIT_RANGE_ERROR;
    A840_OPS_TRY(a840_ops_va(shader->private_memory.gpu_va,
        per_sp * A840_LIMIT_NUM_SP_CORES, 32));
    *item_bytes = item;
    *sp_bytes = (uint32_t)per_sp;
    return A840_EMIT_OK;
}

A840EmitStatus a840_emit_shader_bind(A840CmdStream *batch, const A840ShaderCode *shader,
    uint32_t kind, uint32_t control)
{
    const A840CompiledStage *stage = shader->stage;
    const A840ShaderRegisters *regs = &shader_regs[kind];
    uint32_t length, item_bytes, sp_bytes;
    A840_OPS_TRY(shader_instrlen(stage, kind, &length));
    if (stage->max_full_reg < -1 || stage->max_full_reg >= (int32_t)A840_XS_FULL_MAX ||
        stage->max_half_reg < -1 || stage->max_half_reg >= (int32_t)A840_XS_HALF_MAX ||
        stage->constlen_vec4 > A840_COMPUTE_MAX_CONST_VEC4 ||
        shader->textures > A840_XS_NTEX_MAX || shader->samplers > A840_XS_NSAMP_MAX ||
        shader->uavs > A840_XS_NUAV_MAX)
        return A840_EMIT_RANGE_ERROR;
    if (stage->constlen_vec4 % (1u << A840_XS_CONSTLEN_SHR))
        return A840_EMIT_ALIGNMENT;
    uint64_t code_bytes = (uint64_t)length * TEXT_BYTES_PER_INSTRLEN;
    if ((uint64_t)stage->isa_dwords * 4u > code_bytes)
        code_bytes = (uint64_t)stage->isa_dwords * 4u;
    A840_OPS_TRY(a840_ops_va(shader->code.gpu_va, code_bytes, TEXT_BYTES_PER_INSTRLEN));
    A840_OPS_TRY(private_layout(shader, &item_bytes, &sp_bytes));

    const unsigned branchstack = stage->branchstack > 64u ? 64u : (stage->branchstack + 1u) & ~1u;
    control |= (uint32_t)(stage->max_full_reg + 1) << A840_XS_FULL_SHIFT |
        (uint32_t)(stage->max_half_reg + 1) << A840_XS_HALF_SHIFT |
        branchstack << A840_XS_BRANCH_SHIFT;
    const uint32_t config = A840_XS_ENABLE |
        (shader->bindless_tex ? A840_XS_BINDLESS_TEX : 0u) |
        (shader->bindless_samp ? A840_XS_BINDLESS_SAMP : 0u) |
        (shader->bindless_uav ? A840_XS_BINDLESS_UAV : 0u) |
        (shader->bindless_ubo ? A840_XS_BINDLESS_UBO : 0u) |
        shader->textures << A840_XS_NTEX_SHIFT |
        shader->samplers << A840_XS_NSAMP_SHIFT |
        shader->uavs << A840_XS_NUAV_SHIFT;

    A840_OPS_TRY(a840_emit_reg(batch, regs->constants,
        A840_XS_CONST_ENABLE | (stage->constlen_vec4 >> A840_XS_CONSTLEN_SHR)));
    A840_OPS_TRY(a840_emit_reg(batch, regs->config, config));
    A840_OPS_TRY(a840_emit_reg(batch, regs->control, control));
    A840_OPS_TRY(a840_emit_reg(batch, regs->length, length));
    A840_OPS_TRY(a840_emit_reg(batch, regs->pc, 0));
    A840_OPS_TRY(a840_cs_emit_reg64_va(batch, regs->code, shader->code.bo_handle,
        shader->code.gpu_va, A840_RELOC_READ | A840_RELOC_SHADER));
    A840_OPS_TRY(a840_emit_reg(batch, regs->private_param, item_bytes >> A840_PRIVATE_ITEM_SHR));
    if (item_bytes) {
        A840_OPS_TRY(a840_cs_emit_reg64_va(batch, regs->private_base,
            shader->private_memory.bo_handle, shader->private_memory.gpu_va,
            A840_RELOC_READ | A840_RELOC_WRITE));
    } else {
        const uint32_t no_address[2] = {0, 0};
        A840_OPS_TRY(a840_cs_emit_pkt4(batch, regs->private_base, no_address, 2));
    }
    uint32_t private_size = sp_bytes >> A840_PRIVATE_SIZE_SHR;
    if (item_bytes && shader->per_wave_private_memory) private_size |= A840_PRIVATE_PER_WAVE;
    A840_OPS_TRY(a840_emit_reg(batch, regs->private_size, private_size));
    A840_OPS_TRY(a840_emit_reg(batch, regs->private_stack, sp_bytes >> A840_PRIVATE_STACK_SHR));
    return a840_emit_reg(batch, regs->vgs, 0);
}

static A840EmitStatus local_size(const uint32_t size[3])
{
    const uint64_t max_threads = A840_LIMIT_THREADSIZE_BASE * A840_LIMIT_MAX_WAVES;
    uint64_t threads = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!size[axis] || size[axis] > A840_NDRANGE_LOCAL_X_MAX + 1u)
            return A840_EMIT_RANGE_ERROR;
        threads *= size[axis];
    }
    return threads <= max_threads ? A840_EMIT_OK : A840_EMIT_RANGE_ERROR;
}

static uint32_t cs_const_mode(uint32_t len)
{
    if (len > 256u) return A840_MODE_CONSTLEN_512;
    if (len > 192u) return A840_MODE_CONSTLEN_256;
    if (len > 128u) return A840_MODE_CONSTLEN_192;
    return A840_MODE_CONSTLEN_128;
}

A840EmitStatus a840_emit_compute_pipeline(A840CmdStream *cs, const A840ComputePipelineDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840ComputePipelineDesc)));
    const A840CompiledStage *stage = desc->shader.stage;
    A840_OPS_TRY(a840_ops_input_storage(cs, stage, sizeof(*stage), _Alignof(A840CompiledStage)));
    A840_OPS_TRY(local_size(stage->local_size));
    if (stage->shared_mem_bytes > A840_LIMIT_CS_SHARED_MEM_SIZE ||
        desc->local_id_regid > A840_REGID_UNUSED || desc->wg_id_regid > A840_REGID_UNUSED)
        return A840_EMIT_RANGE_ERROR;

    uint32_t words[A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS];
    A840Reloc relocs[2];
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS, relocs, 2));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_MODE_CNTL,
        A840_SP_CONST_DEMOTION | A840_MODE_ISAM_GL << A840_SP_ISAM_SHIFT));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_UPDATE_CNTL, A840_UPDATE_CS));

    /* A840 lacks supports_double_threadsize (freedreno_devices.py:1243-1252).
     * Mesa programs THREAD128 in CS control, WGE and WIE for this branch. */
    uint32_t control = A840_MODE_THREAD128 << A840_CS_THREADSIZE_SHIFT |
        (desc->round_robin ? A840_CS_ROUND_ROBIN : 0u) |
        ((stage->flags_out & (1u << 3)) ? A840_CS_MERGED : 0u) |
        ((stage->flags_out & (1u << 4)) ? A840_CS_PREAMBLE : 0u);
    A840_OPS_TRY(a840_emit_shader_bind(&batch, &desc->shader, A840_STAGE_COMPUTE, control));

    uint32_t shared = stage->shared_mem_bytes ? (stage->shared_mem_bytes - 1u) / 1024u : 0u;
    if (shared < 1u) shared = 1u;
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_CS_CNTL_1,
        shared | cs_const_mode(stage->constlen_vec4) << A840_CS_CONST_MODE_SHIFT));
    const uint32_t y = stage->local_size[1];
    const uint32_t tile_height = y % 8u == 0 ? 3u : y % 4u == 0 ? 5u : y % 2u == 0 ? 9u : 17u;
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_CS_WGE_CNTL,
        A840_REGID_UNUSED | A840_MODE_THREAD128 << A840_CS_WGE_THREAD_SHIFT |
        A840_CS_WGE_Z_FIRST | 4u << A840_CS_WGE_TILE_W_SHIFT |
        tile_height << A840_CS_WGE_TILE_H_SHIFT));
    /* Gen8 SP_PS_WAVE_CNTL has only VARYINGS, disabled in compute mode. */
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_PS_WAVE_CNTL, 0));
    const uint32_t wie[] = {
        (uint32_t)desc->wg_id_regid << A840_CS_WGID_SHIFT |
        A840_REGID_UNUSED << A840_CS_WGSIZE_SHIFT | A840_REGID_UNUSED << A840_CS_WGOFFSET_SHIFT |
        (uint32_t)desc->local_id_regid << A840_CS_LOCALID_SHIFT,
        A840_REGID_UNUSED | A840_MODE_THREAD128 << A840_CS_WIE_THREAD_SHIFT |
        (desc->linear_dispatch ? A840_MODE_WORKITEM_LINEAR : A840_MODE_WORKITEM_TILED) << A840_CS_WIE_ORDER_SHIFT,
    };
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_SP_CS_WIE_CNTL_0, wie, 2));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_CS_HYSTERESIS, 0));
    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_compute_bindless(A840CmdStream *cs, const A840BindlessTableDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840BindlessTableDesc)));
    if (desc->count > ADRENO_COMPILE_MAX_BIND_SETS) return A840_EMIT_RANGE_ERROR;
    if (!desc->count) return A840_EMIT_OK;

    uint32_t words[A840_EMIT_COMPUTE_BINDLESS_MAX_DWORDS];
    uint32_t values[2u * ADRENO_COMPILE_MAX_BIND_SETS] = {0};
    A840Reloc relocs[ADRENO_COMPILE_MAX_BIND_SETS];
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_COMPUTE_BINDLESS_MAX_DWORDS,
        relocs, ADRENO_COMPILE_MAX_BIND_SETS));
    for (uint32_t i = 0; i < desc->count; ++i) {
        const A840GpuAddress address = desc->tables[i];
        if (!address.gpu_va) {
            if (address.bo_handle) return A840_EMIT_INVALID_ARG;
            continue;
        }
        A840_OPS_TRY(a840_ops_va(address.gpu_va, 64u, 64u));
        values[2u * i] = (uint32_t)address.gpu_va | A840_MODE_BINDLESS_64B;
        values[2u * i + 1u] = (uint32_t)(address.gpu_va >> 32);
    }
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_SP_CS_BINDLESS_BASE_0_DESCRIPTOR,
        values, 2u * desc->count));
    for (uint32_t i = 0; i < desc->count; ++i) {
        if (desc->tables[i].gpu_va)
            A840_OPS_TRY(a840_emit_record_address(&batch, 1u + 2u * i, desc->tables[i], A840_RELOC_READ));
    }
    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_compute_consts(A840CmdStream *cs, const A840ConstUploadDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840ConstUploadDesc)));
    if (desc->source != A840_CONST_DIRECT && desc->source != A840_CONST_INDIRECT)
        return A840_EMIT_INVALID_ARG;
    if (!desc->count_vec4 || desc->constlen_vec4 > A840_COMPUTE_MAX_CONST_VEC4 ||
        desc->dst_vec4 > desc->constlen_vec4 || desc->count_vec4 > desc->constlen_vec4 - desc->dst_vec4)
        return A840_EMIT_RANGE_ERROR;
    const uint32_t units = desc->count_vec4;
    const uint32_t control = desc->dst_vec4 << A840_LOAD_DST_SHIFT |
        A840_LOAD_CONSTANTS << A840_LOAD_TYPE_SHIFT | A840_LOAD_CS_SHADER << A840_LOAD_BLOCK_SHIFT |
        units << A840_LOAD_UNITS_SHIFT;
    if (desc->source == A840_CONST_DIRECT) {
        const size_t bytes = (size_t)units * 16u;
        A840_OPS_TRY(a840_ops_input_storage(cs, desc->words, bytes, _Alignof(uint32_t)));
        uint32_t payload[A840_EMIT_COMPUTE_CONSTS_MAX_DWORDS - 1u] = {
            control | A840_LOAD_DIRECT << A840_LOAD_SOURCE_SHIFT, 0u, 0u,
        };
        memcpy(payload + 3, desc->words, bytes);
        return a840_cs_emit_pkt7(cs, A840_CP_LOAD_STATE6_FRAG, payload, 3u + 4u * units);
    }

    A840_OPS_TRY(a840_ops_va(desc->address.gpu_va, units * 16u, 16u));
    uint32_t words[4];
    A840Reloc reloc;
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, 4, &reloc, 1));
    const uint32_t payload[] = {
        control | A840_LOAD_INDIRECT << A840_LOAD_SOURCE_SHIFT,
        (uint32_t)desc->address.gpu_va, (uint32_t)(desc->address.gpu_va >> 32),
    };
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_LOAD_STATE6_FRAG, payload, 3));
    A840_OPS_TRY(a840_emit_record_address(&batch, 2, desc->address, A840_RELOC_READ));
    return a840_emit_commit(cs, &batch);
}

static uint32_t pack_local(const uint32_t size[3])
{
    /* The NDRANGE_0, NDRANGE_7 and EXEC_CS_INDIRECT local-size layouts agree. */
    return (size[0] - 1u) << A840_NDRANGE_LOCAL_X_SHIFT |
           (size[1] - 1u) << A840_NDRANGE_LOCAL_Y_SHIFT |
           (size[2] - 1u) << A840_NDRANGE_LOCAL_Z_SHIFT;
}

A840EmitStatus a840_emit_compute_dispatch(A840CmdStream *cs, const A840DispatchDesc *desc)
{
    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840DispatchDesc)));
    if (desc->mode != A840_DISPATCH_UNIFORM && desc->mode != A840_DISPATCH_NON_UNIFORM &&
        desc->mode != A840_DISPATCH_INDIRECT) return A840_EMIT_INVALID_ARG;
    const A840CompiledStage *stage = desc->stage;
    A840_OPS_TRY(a840_ops_input_storage(cs, stage, sizeof(*stage), _Alignof(A840CompiledStage)));
    uint32_t length;
    A840_OPS_TRY(shader_instrlen(stage, A840_STAGE_COMPUTE, &length));
    A840_OPS_TRY(local_size(stage->local_size));

    const bool indirect = desc->mode == A840_DISPATCH_INDIRECT;
    if (indirect) {
        A840_OPS_TRY(a840_ops_va(desc->indirect.gpu_va, 3u * sizeof(uint32_t), 4u));
    } else if (!desc->size[0] || !desc->size[1] || !desc->size[2]) {
        return A840_EMIT_OK;
    }

    const uint32_t *local = stage->local_size;
    uint32_t ndrange[7] = {3u << A840_NDRANGE_DIM_SHIFT | pack_local(local)};
    uint32_t last[3] = {local[0], local[1], local[2]};
    uint32_t groups[4] = {0};
    if (!indirect) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            const uint32_t total = desc->size[axis];
            if (desc->mode == A840_DISPATCH_NON_UNIFORM) {
                last[axis] = (total - 1u) % local[axis] + 1u;
                ndrange[1u + 2u * axis] = total;
                groups[axis + 1u] = (total - 1u) / local[axis] + 1u;
            } else {
                if (total > UINT32_MAX / local[axis]) return A840_EMIT_RANGE_ERROR;
                ndrange[1u + 2u * axis] = total * local[axis];
                groups[axis + 1u] = total;
            }
        }
    }
    uint32_t words[A840_EMIT_COMPUTE_DISPATCH_MAX_DWORDS];
    A840Reloc reloc;
    A840CmdStream batch;
    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_COMPUTE_DISPATCH_MAX_DWORDS, &reloc, 1));
    const uint32_t label = A840_EVENT_LABEL;
    const bool roll_context = length > A840_LIMIT_INSTR_CACHE_SIZE;
    if (roll_context) {
        /* Mesa #5892: bracket the launch so a branch refill sees the CS
         * instruction limit even when it checks the other PS context. */
        A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_PS_INSTR_SIZE, length));
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, &label, 1));
    }
    const uint32_t mode = A840_MODE_COMPUTE;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_SET_MARKER, &mode, 1));
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_SP_CS_NDRANGE_0, ndrange, 7));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_CS_NDRANGE_7, pack_local(last)));
    if (indirect) {
        const uint32_t payload[] = {
            0, (uint32_t)desc->indirect.gpu_va, (uint32_t)(desc->indirect.gpu_va >> 32), pack_local(local),
        };
        /* Gen8 does not do the implicit CP wait for indirect parameters. */
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_WAIT_FOR_ME, NULL, 0));
        size_t address = batch.used_dwords + 2u;
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EXEC_CS_INDIRECT, payload, 4));
        A840_OPS_TRY(a840_emit_record_address(&batch, address, desc->indirect, A840_RELOC_READ));
    } else {
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EXEC_CS, groups, 4));
    }
    if (roll_context)
        A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EVENT_WRITE7, &label, 1));
    return a840_emit_commit(cs, &batch);
}
