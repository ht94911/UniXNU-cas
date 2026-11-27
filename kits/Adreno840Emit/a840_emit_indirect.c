/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#define A840_EMIT_PIPELINE_INTERNAL
#include "a840_emit_indirect.h"

#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

enum {
    A840_INDIRECT_DRAW_ARGS_BYTES = 16u,
    A840_INDIRECT_DRAW_INDEXED_ARGS_BYTES = 20u,
    A840_INDIRECT_DISPATCH_ARGS_BYTES = 12u,
    A840_INDIRECT_IB_MAX_DWORDS = (1u << 20) - 1u
};

static bool indirect_prim_valid(uint32_t primitive)
{
    switch (primitive) {
    case A840_PRIM_POINTS:
    case A840_PRIM_LINES:
    case A840_PRIM_LINE_STRIP:
    case A840_PRIM_TRIANGLES:
    case A840_PRIM_TRIANGLE_STRIP:
    case A840_PRIM_TRIANGLE_FAN:
        return true;
    default:
        return false;
    }
}

static A840EmitStatus validate_local_size(const uint32_t size[3])
{
    const uint64_t max_threads = (uint64_t)A840_LIMIT_THREADSIZE_BASE * (uint64_t)A840_LIMIT_MAX_WAVES;
    uint64_t threads = 1u;
    unsigned axis;
    for (axis = 0u; axis < 3u; ++axis) {
        if (!size[axis] || size[axis] > A840_NDRANGE_LOCAL_X_MAX + 1u) {
            return A840_EMIT_RANGE_ERROR;
        }
        threads *= (uint64_t)size[axis];
    }
    return threads <= max_threads ? A840_EMIT_OK : A840_EMIT_RANGE_ERROR;
}

static uint32_t pack_cs_local_size(const uint32_t size[3])
{
    return ((size[0] - 1u) << A840_NDRANGE_LOCAL_X_SHIFT) |
           ((size[1] - 1u) << A840_NDRANGE_LOCAL_Y_SHIFT) |
           ((size[2] - 1u) << A840_NDRANGE_LOCAL_Z_SHIFT);
}

A840EmitStatus a840_emit_draw_indirect(A840CmdStream *cs,
                                       const A840DrawIndirectDesc *desc)
{
    uint32_t words[A840_EMIT_DRAW_INDIRECT_MAX_DWORDS];
    A840Reloc relocs[1];
    A840CmdStream batch;
    uint32_t initiator;
    uint32_t offsets[2];
    uint32_t payload[3];
    size_t addr_index;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840DrawIndirectDesc)));
    if (!indirect_prim_valid(desc->primitive)) {
        return A840_EMIT_UNSUPPORTED;
    }
    A840_OPS_TRY(a840_ops_va(desc->indirect.gpu_va, A840_INDIRECT_DRAW_ARGS_BYTES, 4u));

    initiator = (desc->primitive << A840_DRAW_PRIMITIVE_SHIFT) |
                (A840_DRAW_AUTO << A840_DRAW_SOURCE_SHIFT) |
                ((desc->use_visibility ? A840_DRAW_USE_VIS : A840_DRAW_IGNORE_VIS) << A840_DRAW_VISIBILITY_SHIFT);

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_DRAW_INDIRECT_MAX_DWORDS, relocs, 1u));

    offsets[0] = desc->first_vertex_offset;
    offsets[1] = desc->first_instance_offset;
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_VFD_INDEX_OFFSET, offsets, 2u));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_DRAW_INITIATOR, initiator));
    /* Gen8 requires explicit CP_WAIT_FOR_ME before CP reads indirect draw args. */
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_WAIT_FOR_ME, NULL, 0u));

    /* A8XX CP_DRAW_INDIRECT (adreno_pm4.xml:946-961):
     * dword 0 = vgt_draw_initiator_a4xx, dword 1 = INDIRECT_LO, dword 2 = INDIRECT_HI */
    payload[0] = initiator;
    payload[1] = (uint32_t)desc->indirect.gpu_va;
    payload[2] = (uint32_t)(desc->indirect.gpu_va >> 32);
    addr_index = batch.used_dwords + 2u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_DRAW_INDIRECT, payload, 3u));
    A840_OPS_TRY(a840_emit_record_address(&batch, addr_index, desc->indirect, A840_RELOC_READ));

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_draw_indexed_indirect(A840CmdStream *cs,
                                               const A840DrawIndexedIndirectDesc *desc)
{
    uint32_t words[A840_EMIT_DRAW_INDEXED_INDIRECT_MAX_DWORDS];
    A840Reloc relocs[2];
    A840CmdStream batch;
    uint32_t index_bytes;
    uint32_t initiator;
    uint32_t payload[6];
    size_t ibuf_addr_index;
    size_t ind_addr_index;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840DrawIndexedIndirectDesc)));
    if (!indirect_prim_valid(desc->primitive)) {
        return A840_EMIT_UNSUPPORTED;
    }
    if (desc->index_type != A840_INDEX_UINT16 && desc->index_type != A840_INDEX_UINT32) {
        return A840_EMIT_INVALID_ARG;
    }
    if (!desc->max_indices) {
        return A840_EMIT_RANGE_ERROR;
    }
    index_bytes = (uint32_t)desc->index_type;
    A840_OPS_TRY(a840_ops_va(desc->index_buffer.gpu_va,
                             (uint64_t)desc->max_indices * (uint64_t)index_bytes,
                             index_bytes));
    A840_OPS_TRY(a840_ops_va(desc->indirect.gpu_va, A840_INDIRECT_DRAW_INDEXED_ARGS_BYTES, 4u));

    initiator = (desc->primitive << A840_DRAW_PRIMITIVE_SHIFT) |
                (A840_DRAW_DMA << A840_DRAW_SOURCE_SHIFT) |
                ((desc->use_visibility ? A840_DRAW_USE_VIS : A840_DRAW_IGNORE_VIS) << A840_DRAW_VISIBILITY_SHIFT) |
                ((desc->index_type == A840_INDEX_UINT16 ? A840_DRAW_INDEX_16 : A840_DRAW_INDEX_32)
                 << A840_DRAW_INDEX_SHIFT);

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_DRAW_INDEXED_INDIRECT_MAX_DWORDS, relocs, 2u));

    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_PC_DRAW_INITIATOR, initiator));
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_WAIT_FOR_ME, NULL, 0u));

    /* A8XX CP_DRAW_INDX_INDIRECT (adreno_pm4.xml:963-996):
     * 0: initiator, 1..2: INDX_BASE (64-bit), 3: MAX_INDICES, 4..5: INDIRECT (64-bit) */
    payload[0] = initiator;
    payload[1] = (uint32_t)desc->index_buffer.gpu_va;
    payload[2] = (uint32_t)(desc->index_buffer.gpu_va >> 32);
    payload[3] = desc->max_indices;
    payload[4] = (uint32_t)desc->indirect.gpu_va;
    payload[5] = (uint32_t)(desc->indirect.gpu_va >> 32);

    ibuf_addr_index = batch.used_dwords + 2u;
    ind_addr_index = batch.used_dwords + 5u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_DRAW_INDX_INDIRECT, payload, 6u));
    A840_OPS_TRY(a840_emit_record_address(&batch, ibuf_addr_index, desc->index_buffer, A840_RELOC_READ));
    A840_OPS_TRY(a840_emit_record_address(&batch, ind_addr_index, desc->indirect, A840_RELOC_READ));

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_dispatch_indirect(A840CmdStream *cs,
                                           const A840DispatchIndirectDesc *desc)
{
    uint32_t words[A840_EMIT_DISPATCH_INDIRECT_MAX_DWORDS];
    A840Reloc relocs[1];
    A840CmdStream batch;
    uint32_t dim;
    uint32_t packed_local;
    uint32_t ndrange[7];
    uint32_t mode;
    uint32_t payload[4];
    size_t addr_index;

    A840_OPS_TRY(a840_ops_input_storage(cs, desc, sizeof(*desc), _Alignof(A840DispatchIndirectDesc)));
    A840_OPS_TRY(validate_local_size(desc->local_size));
    dim = desc->work_dim ? desc->work_dim : 3u;
    if (dim > A840_NDRANGE_DIM_MAX) {
        return A840_EMIT_RANGE_ERROR;
    }
    A840_OPS_TRY(a840_ops_va(desc->indirect.gpu_va, A840_INDIRECT_DISPATCH_ARGS_BYTES, 4u));

    packed_local = pack_cs_local_size(desc->local_size);
    memset(ndrange, 0, sizeof(ndrange));
    ndrange[0] = (dim << A840_NDRANGE_DIM_SHIFT) | packed_local;

    A840_OPS_TRY(a840_cs_init(&batch, words, A840_EMIT_DISPATCH_INDIRECT_MAX_DWORDS, relocs, 1u));

    mode = A840_MODE_COMPUTE;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_SET_MARKER, &mode, 1u));
    /* On A8XX (A7XX+), HLSQ_CS_NDRANGE_0..7 were unified into SP_CS_NDRANGE_0..7. */
    A840_OPS_TRY(a840_cs_emit_pkt4(&batch, A840_REG_SP_CS_NDRANGE_0, ndrange, 7u));
    A840_OPS_TRY(a840_emit_reg(&batch, A840_REG_SP_CS_NDRANGE_7, packed_local));
    /* Gen8 requires explicit CP_WAIT_FOR_ME before indirect dispatch args fetch. */
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_WAIT_FOR_ME, NULL, 0u));

    /* A8XX CP_EXEC_CS_INDIRECT (adreno_pm4.xml:1545-1565):
     * 0: reserved (0), 1..2: ADDR (64-bit), 3: packed local size */
    payload[0] = 0u;
    payload[1] = (uint32_t)desc->indirect.gpu_va;
    payload[2] = (uint32_t)(desc->indirect.gpu_va >> 32);
    payload[3] = packed_local;
    addr_index = batch.used_dwords + 2u;
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch, A840_CP_EXEC_CS_INDIRECT, payload, 4u));
    A840_OPS_TRY(a840_emit_record_address(&batch, addr_index, desc->indirect, A840_RELOC_READ));

    return a840_emit_commit(cs, &batch);
}

A840EmitStatus a840_emit_icb_execute_range(A840CmdStream *cs,
                                           const A840IcbExecuteRangeDesc *icb_range)
{
    A840OpsSpan words_span, relocs_span, meta_span;
    A840CmdStream cursor;
    size_t needed_dwords;
    size_t needed_relocs;
    uint32_t i;

    A840_OPS_TRY(a840_ops_input_storage(cs, icb_range, sizeof(*icb_range), _Alignof(A840IcbExecuteRangeDesc)));
    if (!icb_range->total_slots || icb_range->total_slots > A840_ICB_MAX_SLOTS) {
        return A840_EMIT_RANGE_ERROR;
    }
    if (!icb_range->count ||
        icb_range->start >= icb_range->total_slots ||
        icb_range->count > icb_range->total_slots - icb_range->start) {
        return A840_EMIT_RANGE_ERROR;
    }
    A840_OPS_TRY(a840_ops_input_storage(cs,
                                        icb_range->slots,
                                        (size_t)icb_range->total_slots * sizeof(A840IcbSubIb),
                                        _Alignof(A840IcbSubIb)));

    /* Pre-validate every slot in [start, start + count) before touching cs. */
    for (i = 0u; i < icb_range->count; ++i) {
        const A840IcbSubIb *slot = &icb_range->slots[icb_range->start + i];
        if (!slot->dwords || slot->dwords > A840_INDIRECT_IB_MAX_DWORDS) {
            return A840_EMIT_RANGE_ERROR;
        }
        A840_OPS_TRY(a840_ops_va(slot->ib.gpu_va, (uint64_t)slot->dwords * 4u, 4u));
    }

    A840_OPS_TRY(a840_ops_cs_storage(cs, &words_span, &relocs_span, &meta_span));
    needed_dwords = (size_t)icb_range->count * A840_EMIT_ICB_SLOT_MAX_DWORDS +
                    (icb_range->insert_wait_for_me ? 1u : 0u);
    needed_relocs = cs->relocs ? (size_t)icb_range->count : 0u;
    if (needed_dwords > cs->capacity_dwords - cs->used_dwords ||
        needed_relocs > cs->reloc_capacity - cs->reloc_count) {
        return A840_EMIT_CAPACITY;
    }
    if (cs->relocs && cs->used_dwords > (size_t)UINT32_MAX - needed_dwords) {
        return A840_EMIT_RANGE_ERROR;
    }

    /* Emit into a local copy of cursor; commit cursors only on full success. */
    cursor = *cs;
    if (icb_range->insert_wait_for_me) {
        A840_OPS_TRY(a840_cs_emit_pkt7(&cursor, A840_CP_WAIT_FOR_ME, NULL, 0u));
    }
    for (i = 0u; i < icb_range->count; ++i) {
        const A840IcbSubIb *slot = &icb_range->slots[icb_range->start + i];
        uint32_t payload[3];
        size_t addr_index;
        payload[0] = (uint32_t)slot->ib.gpu_va;
        payload[1] = (uint32_t)(slot->ib.gpu_va >> 32);
        payload[2] = slot->dwords;
        addr_index = cursor.used_dwords + 1u;
        A840_OPS_TRY(a840_cs_emit_pkt7(&cursor, A840_CP_INDIRECT_BUFFER, payload, 3u));
        if (cursor.relocs) {
            A840_OPS_TRY(a840_emit_record_address(&cursor, addr_index, slot->ib, A840_RELOC_READ));
        }
    }

    cs->used_dwords = cursor.used_dwords;
    cs->reloc_count = cursor.reloc_count;
    return A840_EMIT_OK;
}
