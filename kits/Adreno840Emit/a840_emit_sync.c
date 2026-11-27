/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_emit_ops.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

A840EmitStatus a840_emit_wfi(A840CmdStream *cs)
{
    return a840_cs_emit_pkt7(cs, A840_CP_WAIT_FOR_IDLE, NULL, 0);
}

A840EmitStatus a840_emit_wait_mem_writes(A840CmdStream *cs)
{
    return a840_cs_emit_pkt7(cs, A840_CP_WAIT_MEM_WRITES, NULL, 0);
}

A840EmitStatus a840_emit_cache_flush_inv(A840CmdStream *cs, uint32_t mask)
{
    A840OpsBatch batch;
    const uint32_t valid = A840_CACHE_UCHE_INV | A840_CACHE_CCU_FLUSH_COLOR |
        A840_CACHE_CCU_FLUSH_DEPTH | A840_CACHE_CCU_INV_COLOR;
    const uint32_t flags[] = {A840_CACHE_CCU_FLUSH_COLOR, A840_CACHE_CCU_FLUSH_DEPTH,
        A840_CACHE_CCU_INV_COLOR, A840_CACHE_UCHE_INV};
    const uint32_t events[] = {A840_EVENT_CCU_FLUSH_COLOR, A840_EVENT_CCU_FLUSH_DEPTH,
        A840_EVENT_CCU_INVALIDATE_COLOR, A840_EVENT_CACHE_INVALIDATE7};
    size_t i;
    if (mask & ~valid) return A840_EMIT_RANGE_ERROR;
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    /* A8XX uses the A7XX+ event IDs; a6xx CACHE_INVALIDATE=0x31 is CLEAN here.
     * Non-writing events have WRITE_ENABLED clear and one payload dword. */
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
        if (mask & flags[i])
            A840_OPS_TRY(a840_cs_emit_pkt7(&batch.cs, A840_CP_EVENT_WRITE7, &events[i], 1));
    return a840_ops_commit(cs, &batch);
}

A840EmitStatus a840_emit_mem_write64(A840CmdStream *cs, uint64_t bo,
    uint64_t dst_va, uint64_t value)
{
    A840OpsBatch batch;
    uint32_t payload[4];
    A840_OPS_TRY(a840_ops_va(dst_va, sizeof(value), 8));
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    payload[0] = (uint32_t)dst_va;
    payload[1] = (uint32_t)(dst_va >> 32);
    payload[2] = (uint32_t)value;
    payload[3] = (uint32_t)(value >> 32);
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch.cs, A840_CP_MEM_WRITE, payload, 4));
    A840_OPS_TRY(a840_ops_add_reloc(&batch, 1, bo, dst_va, A840_RELOC_WRITE));
    return a840_ops_commit(cs, &batch);
}

A840EmitStatus a840_emit_timestamp_event(A840CmdStream *cs, uint32_t event_id,
    uint64_t bo, uint64_t dst_va, uint64_t payload)
{
    A840OpsBatch batch;
    uint32_t words[5];
    uint32_t event = event_id & ~A840_TIMESTAMP_GPU_CLOCK;
    int clock = (event_id & A840_TIMESTAMP_GPU_CLOCK) != 0;
    if (event > UINT8_MAX) return A840_EMIT_RANGE_ERROR;
    if (event != A840_EVENT_RB_DONE_TS && event != A840_EVENT_CACHE_FLUSH_TS)
        return A840_EMIT_UNSUPPORTED;
    if (clock && payload) return A840_EMIT_INVALID_ARG;
    A840_OPS_TRY(a840_ops_va(dst_va, sizeof(payload), 8));
    A840_OPS_TRY(a840_ops_batch_init(&batch));
    /* adreno_pm4.xml:1672-1720; user64 marker or always-on GPU timestamp.
     * RB_DONE_TS provides 3D-bottom ordering without inserting a WFI.
     * tu_pass_stamp<A8XX> uses the same always-on source and 3-word payload. */
    words[0] = event | A840_EVENT_WRITE_ENABLED |
        ((clock ? A840_EV_WRITE_ALWAYSON : A840_EV_WRITE_USER_64B) << A840_EVENT_WRITE_SRC_SHIFT);
    words[1] = (uint32_t)dst_va;
    words[2] = (uint32_t)(dst_va >> 32);
    words[3] = (uint32_t)payload;
    words[4] = (uint32_t)(payload >> 32);
    A840_OPS_TRY(a840_cs_emit_pkt7(&batch.cs, A840_CP_EVENT_WRITE7, words, clock ? 3 : 5));
    A840_OPS_TRY(a840_ops_add_reloc(&batch, 2, bo, dst_va, A840_RELOC_WRITE));
    return a840_ops_commit(cs, &batch);
}
