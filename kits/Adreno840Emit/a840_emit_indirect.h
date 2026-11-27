/* SPDX-License-Identifier: MIT */
#ifndef A840_EMIT_INDIRECT_H
#define A840_EMIT_INDIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_emit_compute.h"
#include "a840_emit_3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum packet sizes (in dwords, including headers) for A8XX indirect ops. */
#define A840_EMIT_DRAW_INDIRECT_MAX_DWORDS 10u
#define A840_EMIT_DRAW_INDEXED_INDIRECT_MAX_DWORDS 12u
#define A840_EMIT_DISPATCH_INDIRECT_MAX_DWORDS 20u
#define A840_EMIT_ICB_SLOT_MAX_DWORDS 4u
#define A840_ICB_MAX_SLOTS 1024u

/* Direct indirect draw via A8XX PC_DRAW_INITIATOR + CP_DRAW_INDIRECT (0x28).
 * Indirect buffer layout in GPU memory: { vertex_count, instance_count,
 * first_vertex, first_instance } (16 bytes, 4-byte aligned). */
typedef struct A840DrawIndirectDesc {
    uint32_t primitive;          /* A840_PRIM_* from gen/a840_bitfields.h */
    bool use_visibility;
    uint32_t first_vertex_offset;/* VFD_INDEX_OFFSET initial value (0 if unused) */
    uint32_t first_instance_offset;
    A840GpuAddress indirect;     /* 4B aligned, non-zero, >= 16 bytes */
} A840DrawIndirectDesc;

/* Indexed indirect draw via A8XX PC_DRAW_INITIATOR + CP_DRAW_INDX_INDIRECT (0x29).
 * Indirect buffer layout in GPU memory: { index_count, instance_count,
 * first_index, base_vertex, first_instance } (20 bytes, 4-byte aligned). */
typedef struct A840DrawIndexedIndirectDesc {
    uint32_t primitive;          /* A840_PRIM_* from gen/a840_bitfields.h */
    A840IndexType index_type;    /* A840_INDEX_UINT16 or A840_INDEX_UINT32 */
    bool use_visibility;
    uint32_t max_indices;        /* > 0, element capacity of index buffer */
    A840GpuAddress index_buffer; /* 2B/4B aligned per index_type */
    A840GpuAddress indirect;     /* 4B aligned, non-zero, >= 20 bytes */
} A840DrawIndexedIndirectDesc;

/* Indirect compute dispatch via A8XX SP_CS_NDRANGE_0..7 + CP_EXEC_CS_INDIRECT (0x41).
 * Indirect buffer layout in GPU memory: { groups_x, groups_y, groups_z } (12 bytes). */
typedef struct A840DispatchIndirectDesc {
    uint32_t local_size[3];      /* Each 1..1024, product <= 1024 */
    uint32_t work_dim;           /* 0 defaults to 3; valid 1..3 */
    A840GpuAddress indirect;     /* 4B aligned, non-zero, >= 12 bytes */
} A840DispatchIndirectDesc;

/* Descriptor of one compiled secondary IB slot inside an ICB execution range. */
typedef struct A840IcbSubIb {
    A840GpuAddress ib;           /* 4B aligned, non-zero GPU VA */
    uint32_t dwords;             /* 1..(2^20 - 1) */
} A840IcbSubIb;

/* Range of secondary IBs to execute via A8XX CP_INDIRECT_BUFFER (0x3f). */
typedef struct A840IcbExecuteRangeDesc {
    const A840IcbSubIb *slots;   /* Array of total_slots entries */
    uint32_t total_slots;        /* 1..A840_ICB_MAX_SLOTS */
    uint32_t start;              /* Start slot index */
    uint32_t count;              /* Number of slots to execute (>= 1) */
    bool insert_wait_for_me;     /* Emit CP_WAIT_FOR_ME before secondary IB replay */
} A840IcbExecuteRangeDesc;

/* All functions enforce strict transactional rollback: on any error status,
 * cs->used_dwords and cs->reloc_count remain completely unchanged. */
A840EmitStatus a840_emit_draw_indirect(A840CmdStream *cs,
                                       const A840DrawIndirectDesc *desc);

A840EmitStatus a840_emit_draw_indexed_indirect(A840CmdStream *cs,
                                               const A840DrawIndexedIndirectDesc *desc);

A840EmitStatus a840_emit_dispatch_indirect(A840CmdStream *cs,
                                           const A840DispatchIndirectDesc *desc);

A840EmitStatus a840_emit_icb_execute_range(A840CmdStream *cs,
                                           const A840IcbExecuteRangeDesc *icb_range);

#ifdef __cplusplus
}
#endif

#endif /* A840_EMIT_INDIRECT_H */
