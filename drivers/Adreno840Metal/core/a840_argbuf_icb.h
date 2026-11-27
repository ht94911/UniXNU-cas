/* SPDX-License-Identifier: MIT */
#ifndef A840_ARGBUF_ICB_H
#define A840_ARGBUF_ICB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "a840_dualpath_router.h"
#include "a840_emit_indirect.h"
#include "a840_emit_ops.h"
#include "a840_heap_allocator.h"
#include "a840_runtime_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 4 capability flags extending A840RouteCategory (bits 10..13).
 * Keeping bits 0..5 (Phase 2) and bits 6..9 (Phase 3) untouched preserves
 * 100% backward compatibility with all Phase 2 & Phase 3 routing decisions. */
enum {
    A840_ROUTE_P4_HEAP_ALIASING = 1u << 10,
    A840_ROUTE_P4_INDIRECT_COMPUTE = 1u << 11,
    A840_ROUTE_P4_INDIRECT_RENDER_ICB = 1u << 12,
    A840_ROUTE_P4_SPARSE = 1u << 13,
    A840_ROUTE_PHASE4_ALL = A840_ROUTE_P4_HEAP_ALIASING |
                            A840_ROUTE_P4_INDIRECT_COMPUTE |
                            A840_ROUTE_P4_INDIRECT_RENDER_ICB |
                            A840_ROUTE_P4_SPARSE
};

enum {
    A840_ARGBUF_MAX_SLOTS = 64u,
    A840_ARGBUF_MAX_DEPS = 64u,
    A840_ARGBUF_BUFFER_ENTRY_BYTES = 16u,   /* uint64_t gpu_va + uint64_t size */
    A840_ARGBUF_TEXTURE_ENTRY_BYTES = 64u,  /* 16 dwords A8XX_TEX_MEMOBJ */
    A840_ARGBUF_SAMPLER_ENTRY_BYTES = 16u,  /* 4 dwords A8XX_TEX_SAMP */
    A840_ICB_MAX_BIND_BUFFERS = 8u
};

/* ---------- Native Argument Buffer Encoder ---------- */

typedef enum A840ArgSlotKind {
    A840_ARG_SLOT_BUFFER = 0,
    A840_ARG_SLOT_TEXTURE = 1,
    A840_ARG_SLOT_SAMPLER = 2,
    A840_ARG_SLOT_INLINE_DATA = 3
} A840ArgSlotKind;

typedef struct A840ArgSlotDesc {
    uint32_t index;             /* Logical argument index */
    A840ArgSlotKind kind;
    uint32_t byte_offset;       /* Offset inside argument buffer (8B/16B/64B aligned) */
    uint32_t byte_size;         /* Required for INLINE_DATA; validated for others */
} A840ArgSlotDesc;

typedef enum A840ArgDepKind {
    A840_ARG_DEP_BUFFER = 0,
    A840_ARG_DEP_TEXTURE = 1,
    A840_ARG_DEP_HEAP = 2
} A840ArgDepKind;

typedef struct A840ArgDep {
    A840ArgDepKind kind;
    uint64_t bo_handle;
    uint64_t gpu_va;
    uint64_t size_bytes;
    const A840Heap *heap;       /* Non-NULL when backed by an A840Heap */
} A840ArgDep;

typedef struct A840ArgBufEncoder {
    A840RtDevice *device;
    uint64_t device_id;
    A840RtBuffer *target_buffer;
    uint64_t base_offset;
    uint64_t encoded_length;

    uint32_t slot_count;
    A840ArgSlotDesc slots[A840_ARGBUF_MAX_SLOTS];

    uint32_t dep_count;
    A840ArgDep deps[A840_ARGBUF_MAX_DEPS];
} A840ArgBufEncoder;

A840RtStatus a840_argbuf_encoder_init(A840ArgBufEncoder *enc,
                                      A840RtDevice *device,
                                      const A840ArgSlotDesc *slots,
                                      uint32_t slot_count);

A840RtStatus a840_argbuf_encoder_set_target(A840ArgBufEncoder *enc,
                                            A840RtBuffer *buffer,
                                            uint64_t offset);

A840RtStatus a840_argbuf_set_buffer(A840ArgBufEncoder *enc,
                                    uint32_t slot_index,
                                    const A840RtBuffer *buf,
                                    uint64_t offset);

A840RtStatus a840_argbuf_set_heap_suballoc(A840ArgBufEncoder *enc,
                                           uint32_t slot_index,
                                           const A840HeapSubAlloc *sub);

A840RtStatus a840_argbuf_set_texture(A840ArgBufEncoder *enc,
                                     uint32_t slot_index,
                                     const A840RtTexture *tex);

A840RtStatus a840_argbuf_set_sampler(A840ArgBufEncoder *enc,
                                     uint32_t slot_index,
                                     const A840SampDescInput *samp);

A840RtStatus a840_argbuf_set_inline_bytes(A840ArgBufEncoder *enc,
                                          uint32_t slot_index,
                                          const void *data,
                                          size_t bytes);

/* Validate all recorded dependencies against a caller's resident BO/heap sets
 * (models Metal useResource: / useHeap: residency verification). */
A840RtStatus a840_argbuf_validate_residency(const A840ArgBufEncoder *enc,
                                            const uint64_t *resident_bos,
                                            size_t resident_bo_count,
                                            const A840Heap *const *resident_heaps,
                                            size_t resident_heap_count);

/* ---------- Native Indirect Command Buffer (ICB) ---------- */

typedef enum A840IcbType {
    A840_ICB_TYPE_RENDER = 0,
    A840_ICB_TYPE_COMPUTE = 1
} A840IcbType;

typedef enum A840IcbCmdKind {
    A840_ICB_CMD_EMPTY = 0,
    A840_ICB_CMD_DRAW = 1,
    A840_ICB_CMD_DRAW_INDEXED = 2,
    A840_ICB_CMD_DRAW_INDIRECT = 3,
    A840_ICB_CMD_DRAW_INDEXED_INDIRECT = 4,
    A840_ICB_CMD_DISPATCH = 5,
    A840_ICB_CMD_DISPATCH_INDIRECT = 6
} A840IcbCmdKind;

typedef struct A840IcbSlot {
    A840IcbCmdKind kind;
    bool has_pipeline;
    uint64_t pipeline_id;
    bool has_sds;
    A840SdsGroupDesc sds;

    uint32_t bindless_mask;
    A840GpuAddress bindless_tables[A840_ICB_MAX_BIND_BUFFERS];

    A840DrawDesc draw;
    A840DrawIndirectDesc draw_indirect;
    A840DrawIndexedIndirectDesc draw_indexed_indirect;

    uint32_t cs_local_size[3];
    uint32_t cs_grid_groups[3];
    A840DispatchIndirectDesc dispatch_indirect;
} A840IcbSlot;

typedef struct A840IcbDesc {
    A840IcbType type;
    uint32_t max_commands;      /* 1..A840_ICB_MAX_SLOTS */
    bool inherit_pipeline;
    bool inherit_buffers;
} A840IcbDesc;

typedef struct A840IndirectCmdBuffer {
    A840RtDevice *device;
    uint64_t device_id;
    A840IcbType type;
    uint32_t max_commands;
    bool inherit_pipeline;
    bool inherit_buffers;
    A840IcbSlot *slots;
} A840IndirectCmdBuffer;

typedef struct A840IcbCompiledRange {
    uint32_t non_empty_count;
    uint32_t total_ib_dwords;
    uint32_t total_relocs;
    uint64_t content_hash;      /* Deterministic FNV-1a 64-bit hash of compiled IB words */
    A840IcbSubIb sub_ibs[A840_ICB_MAX_SLOTS];
} A840IcbCompiledRange;

A840RtStatus a840_icb_create(A840RtDevice *device,
                             const A840IcbDesc *desc,
                             A840IndirectCmdBuffer **out_icb);
void a840_icb_destroy(A840IndirectCmdBuffer *icb);

A840RtStatus a840_icb_reset_range(A840IndirectCmdBuffer *icb,
                                  uint32_t start,
                                  uint32_t count);

A840RtStatus a840_icb_copy_range(A840IndirectCmdBuffer *dst_icb,
                                 uint32_t dst_start,
                                 const A840IndirectCmdBuffer *src_icb,
                                 uint32_t src_start,
                                 uint32_t count);

A840RtStatus a840_icb_set_pipeline(A840IndirectCmdBuffer *icb,
                                   uint32_t index,
                                   uint64_t pipeline_id,
                                   const A840SdsGroupDesc *opt_sds);

A840RtStatus a840_icb_set_bindless_buffer(A840IndirectCmdBuffer *icb,
                                          uint32_t index,
                                          uint32_t slot,
                                          A840GpuAddress addr);

A840RtStatus a840_icb_encode_draw(A840IndirectCmdBuffer *icb,
                                  uint32_t index,
                                  const A840DrawDesc *draw);

A840RtStatus a840_icb_encode_draw_indirect(A840IndirectCmdBuffer *icb,
                                           uint32_t index,
                                           const A840DrawIndirectDesc *draw);

A840RtStatus a840_icb_encode_draw_indexed_indirect(A840IndirectCmdBuffer *icb,
                                                   uint32_t index,
                                                   const A840DrawIndexedIndirectDesc *draw);

A840RtStatus a840_icb_encode_dispatch(A840IndirectCmdBuffer *icb,
                                      uint32_t index,
                                      const uint32_t local_size[3],
                                      const uint32_t group_count[3]);

A840RtStatus a840_icb_encode_dispatch_indirect(A840IndirectCmdBuffer *icb,
                                               uint32_t index,
                                               const A840DispatchIndirectDesc *dispatch);

/* Compile non-empty ICB slots in [start, start + count) into secondary A8XX PM4
 * commands stored directly inside dst_ib_buffer (CPU-mapped GPU buffer), and
 * populate out_compiled with per-slot A840IcbSubIb descriptors + content hash. */
A840RtStatus a840_icb_compile_to_gpu_ib(const A840IndirectCmdBuffer *icb,
                                        uint32_t start,
                                        uint32_t count,
                                        A840RtBuffer *dst_ib_buffer,
                                        A840Reloc *relocs,
                                        size_t reloc_capacity,
                                        A840IcbCompiledRange *out_compiled);

/* Emit primary IB playback calls for a previously compiled ICB range. */
A840EmitStatus a840_icb_emit_execute(A840CmdStream *primary_cs,
                                     const A840IcbCompiledRange *compiled,
                                     bool insert_wait_for_me);

/* Convenience helper: enable Phase 4 router bits (10..13) on a DualPathRouter. */
void a840_argbuf_icb_enable_phase4_router(A840DualPathRouter *router);

#ifdef __cplusplus
}
#endif

#endif /* A840_ARGBUF_ICB_H */
