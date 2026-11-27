/* SPDX-License-Identifier: MIT */
#include "a840_argbuf_icb.h"

#include <stdlib.h>
#include <string.h>

#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

/* ---------- Argument Buffer Encoder ---------- */

static uint32_t arg_slot_required_align(A840ArgSlotKind kind)
{
    switch (kind) {
    case A840_ARG_SLOT_BUFFER:
        return 8u;
    case A840_ARG_SLOT_TEXTURE:
        return 64u;
    case A840_ARG_SLOT_SAMPLER:
        return 16u;
    case A840_ARG_SLOT_INLINE_DATA:
        return 4u;
    default:
        return 0u;
    }
}

static uint32_t arg_slot_fixed_size(A840ArgSlotKind kind, uint32_t inline_bytes)
{
    switch (kind) {
    case A840_ARG_SLOT_BUFFER:
        return A840_ARGBUF_BUFFER_ENTRY_BYTES;
    case A840_ARG_SLOT_TEXTURE:
        return A840_ARGBUF_TEXTURE_ENTRY_BYTES;
    case A840_ARG_SLOT_SAMPLER:
        return A840_ARGBUF_SAMPLER_ENTRY_BYTES;
    case A840_ARG_SLOT_INLINE_DATA:
        return inline_bytes;
    default:
        return 0u;
    }
}

static bool ranges_overlap_u64(uint64_t a0, uint64_t alen, uint64_t b0, uint64_t blen)
{
    if (!alen || !blen) {
        return false;
    }
    return a0 < b0 + blen && b0 < a0 + alen;
}

static const A840ArgSlotDesc *find_arg_slot(const A840ArgBufEncoder *enc, uint32_t slot_index)
{
    uint32_t i;
    for (i = 0u; i < enc->slot_count; ++i) {
        if (enc->slots[i].index == slot_index) {
            return &enc->slots[i];
        }
    }
    return NULL;
}

static A840RtStatus argbuf_record_dep(A840ArgBufEncoder *enc,
                                      A840ArgDepKind kind,
                                      uint64_t bo_handle,
                                      uint64_t gpu_va,
                                      uint64_t size_bytes,
                                      const A840Heap *heap)
{
    uint32_t i;
    for (i = 0u; i < enc->dep_count; ++i) {
        if (enc->deps[i].bo_handle == bo_handle &&
            enc->deps[i].gpu_va == gpu_va &&
            enc->deps[i].heap == heap) {
            return A840_RT_OK;
        }
    }
    if (enc->dep_count >= A840_ARGBUF_MAX_DEPS) {
        return A840_RT_CAPACITY;
    }
    enc->deps[enc->dep_count].kind = kind;
    enc->deps[enc->dep_count].bo_handle = bo_handle;
    enc->deps[enc->dep_count].gpu_va = gpu_va;
    enc->deps[enc->dep_count].size_bytes = size_bytes;
    enc->deps[enc->dep_count].heap = heap;
    enc->dep_count++;
    return A840_RT_OK;
}

static A840RtStatus argbuf_target_ptr(const A840ArgBufEncoder *enc,
                                      uint32_t rel_offset,
                                      uint32_t bytes,
                                      uint8_t **out_ptr)
{
    uint64_t total_off;
    if (!enc->target_buffer || !enc->target_buffer->cpu_ptr) {
        return A840_RT_STATE_ERROR;
    }
    if (enc->base_offset > UINT64_MAX - (uint64_t)rel_offset) {
        return A840_RT_RANGE_ERROR;
    }
    total_off = enc->base_offset + (uint64_t)rel_offset;
    if (total_off > enc->target_buffer->size_bytes ||
        (uint64_t)bytes > enc->target_buffer->size_bytes - total_off) {
        return A840_RT_RANGE_ERROR;
    }
    *out_ptr = (uint8_t *)enc->target_buffer->cpu_ptr + total_off;
    return A840_RT_OK;
}

A840RtStatus a840_argbuf_encoder_init(A840ArgBufEncoder *enc,
                                      A840RtDevice *device,
                                      const A840ArgSlotDesc *slots,
                                      uint32_t slot_count)
{
    uint32_t i;
    uint32_t j;
    uint64_t max_end = 0u;

    if (!enc || !device || !slots || !slot_count || slot_count > A840_ARGBUF_MAX_SLOTS) {
        return A840_RT_INVALID_ARG;
    }
    memset(enc, 0, sizeof(*enc));

    for (i = 0u; i < slot_count; ++i) {
        uint32_t req_align = arg_slot_required_align(slots[i].kind);
        uint32_t sz = arg_slot_fixed_size(slots[i].kind, slots[i].byte_size);
        uint64_t end;

        if (!req_align || !sz) {
            return A840_RT_INVALID_ARG;
        }
        if ((slots[i].byte_offset % req_align) != 0u) {
            return A840_RT_ALIGNMENT;
        }
        if (slots[i].kind != A840_ARG_SLOT_INLINE_DATA &&
            slots[i].byte_size != 0u && slots[i].byte_size != sz) {
            return A840_RT_INVALID_ARG;
        }
        end = (uint64_t)slots[i].byte_offset + (uint64_t)sz;
        if (end > max_end) {
            max_end = end;
        }

        for (j = 0u; j < i; ++j) {
            uint32_t prev_sz = arg_slot_fixed_size(slots[j].kind, slots[j].byte_size);
            if (slots[j].index == slots[i].index) {
                return A840_RT_INVALID_ARG;
            }
            if (ranges_overlap_u64(slots[i].byte_offset, sz, slots[j].byte_offset, prev_sz)) {
                return A840_RT_RANGE_ERROR;
            }
        }
        enc->slots[i] = slots[i];
        enc->slots[i].byte_size = sz;
    }

    enc->device = device;
    enc->device_id = device->device_id;
    enc->slot_count = slot_count;
    enc->encoded_length = max_end;
    return A840_RT_OK;
}

A840RtStatus a840_argbuf_encoder_set_target(A840ArgBufEncoder *enc,
                                            A840RtBuffer *buffer,
                                            uint64_t offset)
{
    if (!enc || !enc->device || !buffer) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_buffer(enc->device, buffer)) {
        return A840_RT_WRONG_DEVICE;
    }
    if (!buffer->cpu_ptr || (buffer->usage_flags & A840_BUF_USAGE_CPU_VISIBLE) == 0u) {
        return A840_RT_INVALID_ARG;
    }
    if ((offset % 64u) != 0u) {
        return A840_RT_ALIGNMENT;
    }
    if (offset > buffer->size_bytes || enc->encoded_length > buffer->size_bytes - offset) {
        return A840_RT_RANGE_ERROR;
    }
    enc->target_buffer = buffer;
    enc->base_offset = offset;
    enc->dep_count = 0u;
    memset((uint8_t *)buffer->cpu_ptr + offset, 0, (size_t)enc->encoded_length);
    return A840_RT_OK;
}

A840RtStatus a840_argbuf_set_buffer(A840ArgBufEncoder *enc,
                                    uint32_t slot_index,
                                    const A840RtBuffer *buf,
                                    uint64_t offset)
{
    const A840ArgSlotDesc *slot;
    uint8_t *dst = NULL;
    uint64_t entry[2];
    A840RtStatus st;

    if (!enc || !buf) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_buffer(enc->device, buf)) {
        return A840_RT_WRONG_DEVICE;
    }
    slot = find_arg_slot(enc, slot_index);
    if (!slot || slot->kind != A840_ARG_SLOT_BUFFER) {
        return A840_RT_INVALID_ARG;
    }
    if (offset >= buf->size_bytes) {
        return A840_RT_RANGE_ERROR;
    }
    st = argbuf_target_ptr(enc, slot->byte_offset, A840_ARGBUF_BUFFER_ENTRY_BYTES, &dst);
    if (st != A840_RT_OK) {
        return st;
    }

    entry[0] = buf->gpu_va + offset;
    entry[1] = buf->size_bytes - offset;
    memcpy(dst, entry, sizeof(entry));

    return argbuf_record_dep(enc, A840_ARG_DEP_BUFFER, buf->bo_handle,
                             entry[0], entry[1], NULL);
}

A840RtStatus a840_argbuf_set_heap_suballoc(A840ArgBufEncoder *enc,
                                           uint32_t slot_index,
                                           const A840HeapSubAlloc *sub)
{
    const A840ArgSlotDesc *slot;
    uint8_t *dst = NULL;
    A840RtStatus st;

    if (!enc || !sub) {
        return A840_RT_INVALID_ARG;
    }
    st = a840_heap_suballoc_validate(enc->device, sub);
    if (st != A840_RT_OK) {
        return st;
    }
    slot = find_arg_slot(enc, slot_index);
    if (!slot) {
        return A840_RT_INVALID_ARG;
    }

    if (sub->kind == A840_HEAP_RES_BUFFER) {
        uint64_t entry[2];
        if (slot->kind != A840_ARG_SLOT_BUFFER) {
            return A840_RT_INVALID_ARG;
        }
        st = argbuf_target_ptr(enc, slot->byte_offset, A840_ARGBUF_BUFFER_ENTRY_BYTES, &dst);
        if (st != A840_RT_OK) {
            return st;
        }
        entry[0] = sub->gpu_va;
        entry[1] = sub->size_bytes;
        memcpy(dst, entry, sizeof(entry));
    } else {
        if (slot->kind != A840_ARG_SLOT_TEXTURE) {
            return A840_RT_INVALID_ARG;
        }
        st = argbuf_target_ptr(enc, slot->byte_offset, A840_ARGBUF_TEXTURE_ENTRY_BYTES, &dst);
        if (st != A840_RT_OK) {
            return st;
        }
        memcpy(dst, sub->tex_const, A840_ARGBUF_TEXTURE_ENTRY_BYTES);
    }

    return argbuf_record_dep(enc, A840_ARG_DEP_HEAP, sub->bo_handle,
                             sub->gpu_va, sub->size_bytes, sub->heap);
}

A840RtStatus a840_argbuf_set_texture(A840ArgBufEncoder *enc,
                                     uint32_t slot_index,
                                     const A840RtTexture *tex)
{
    const A840ArgSlotDesc *slot;
    uint8_t *dst = NULL;
    A840RtStatus st;

    if (!enc || !tex) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_texture(enc->device, tex)) {
        return A840_RT_WRONG_DEVICE;
    }
    slot = find_arg_slot(enc, slot_index);
    if (!slot || slot->kind != A840_ARG_SLOT_TEXTURE) {
        return A840_RT_INVALID_ARG;
    }
    st = argbuf_target_ptr(enc, slot->byte_offset, A840_ARGBUF_TEXTURE_ENTRY_BYTES, &dst);
    if (st != A840_RT_OK) {
        return st;
    }
    memcpy(dst, tex->tex_const, A840_ARGBUF_TEXTURE_ENTRY_BYTES);

    return argbuf_record_dep(enc, A840_ARG_DEP_TEXTURE, tex->bo_handle,
                             tex->gpu_va, tex->alloc_bytes, NULL);
}

A840RtStatus a840_argbuf_set_sampler(A840ArgBufEncoder *enc,
                                     uint32_t slot_index,
                                     const A840SampDescInput *samp)
{
    const A840ArgSlotDesc *slot;
    uint8_t *dst = NULL;
    uint32_t words[A840_SAMP_CONST_DWORDS];
    A840EmitStatus est;
    A840RtStatus st;

    if (!enc || !samp) {
        return A840_RT_INVALID_ARG;
    }
    slot = find_arg_slot(enc, slot_index);
    if (!slot || slot->kind != A840_ARG_SLOT_SAMPLER) {
        return A840_RT_INVALID_ARG;
    }
    est = a840_encode_samp_const(samp, words);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    st = argbuf_target_ptr(enc, slot->byte_offset, A840_ARGBUF_SAMPLER_ENTRY_BYTES, &dst);
    if (st != A840_RT_OK) {
        return st;
    }
    memcpy(dst, words, sizeof(words));
    return A840_RT_OK;
}

A840RtStatus a840_argbuf_set_inline_bytes(A840ArgBufEncoder *enc,
                                          uint32_t slot_index,
                                          const void *data,
                                          size_t bytes)
{
    const A840ArgSlotDesc *slot;
    uint8_t *dst = NULL;
    A840RtStatus st;

    if (!enc || !data || !bytes) {
        return A840_RT_INVALID_ARG;
    }
    slot = find_arg_slot(enc, slot_index);
    if (!slot || slot->kind != A840_ARG_SLOT_INLINE_DATA) {
        return A840_RT_INVALID_ARG;
    }
    if (bytes > slot->byte_size) {
        return A840_RT_RANGE_ERROR;
    }
    st = argbuf_target_ptr(enc, slot->byte_offset, (uint32_t)bytes, &dst);
    if (st != A840_RT_OK) {
        return st;
    }
    memcpy(dst, data, bytes);
    return A840_RT_OK;
}

A840RtStatus a840_argbuf_validate_residency(const A840ArgBufEncoder *enc,
                                            const uint64_t *resident_bos,
                                            size_t resident_bo_count,
                                            const A840Heap *const *resident_heaps,
                                            size_t resident_heap_count)
{
    uint32_t i;
    size_t j;

    if (!enc) {
        return A840_RT_INVALID_ARG;
    }
    for (i = 0u; i < enc->dep_count; ++i) {
        const A840ArgDep *dep = &enc->deps[i];
        bool found = false;

        if (dep->heap != NULL) {
            A840RtStatus hst = a840_heap_validate_for_submission(dep->heap);
            if (hst != A840_RT_OK) {
                return hst;
            }
            for (j = 0u; j < resident_heap_count; ++j) {
                if (resident_heaps && resident_heaps[j] == dep->heap) {
                    found = true;
                    break;
                }
            }
        }
        if (!found && resident_bos) {
            for (j = 0u; j < resident_bo_count; ++j) {
                if (resident_bos[j] == dep->bo_handle) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return A840_RT_STATE_ERROR;
        }
    }
    return A840_RT_OK;
}

/* ---------- Native Indirect Command Buffer (ICB) ---------- */

static uint64_t fnv1a_u32_words(const uint32_t *words, size_t count)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const uint64_t prime = UINT64_C(1099511628211);
    size_t i;
    for (i = 0u; i < count; ++i) {
        uint32_t w = words[i];
        unsigned b;
        for (b = 0u; b < 4u; ++b) {
            hash ^= (uint64_t)((w >> (b * 8u)) & 0xffu);
            hash *= prime;
        }
    }
    return hash;
}

static bool icb_local_size_ok(const uint32_t size[3])
{
    uint64_t max_threads = (uint64_t)A840_LIMIT_THREADSIZE_BASE * (uint64_t)A840_LIMIT_MAX_WAVES;
    uint64_t prod = 1u;
    unsigned i;
    for (i = 0u; i < 3u; ++i) {
        if (!size[i] || size[i] > A840_NDRANGE_LOCAL_X_MAX + 1u) {
            return false;
        }
        prod *= (uint64_t)size[i];
    }
    return prod <= max_threads;
}

A840RtStatus a840_icb_create(A840RtDevice *device,
                             const A840IcbDesc *desc,
                             A840IndirectCmdBuffer **out_icb)
{
    A840IndirectCmdBuffer *icb;

    if (!device || !desc || !out_icb) {
        return A840_RT_INVALID_ARG;
    }
    *out_icb = NULL;
    if (desc->type != A840_ICB_TYPE_RENDER && desc->type != A840_ICB_TYPE_COMPUTE) {
        return A840_RT_INVALID_ARG;
    }
    if (!desc->max_commands || desc->max_commands > A840_ICB_MAX_SLOTS) {
        return A840_RT_RANGE_ERROR;
    }

    icb = (A840IndirectCmdBuffer *)calloc(1u, sizeof(*icb));
    if (!icb) {
        return A840_RT_OOM;
    }
    icb->slots = (A840IcbSlot *)calloc(desc->max_commands, sizeof(A840IcbSlot));
    if (!icb->slots) {
        free(icb);
        return A840_RT_OOM;
    }

    icb->device = device;
    icb->device_id = device->device_id;
    icb->type = desc->type;
    icb->max_commands = desc->max_commands;
    icb->inherit_pipeline = desc->inherit_pipeline;
    icb->inherit_buffers = desc->inherit_buffers;

    *out_icb = icb;
    return A840_RT_OK;
}

void a840_icb_destroy(A840IndirectCmdBuffer *icb)
{
    if (!icb) {
        return;
    }
    free(icb->slots);
    free(icb);
}

A840RtStatus a840_icb_reset_range(A840IndirectCmdBuffer *icb,
                                  uint32_t start,
                                  uint32_t count)
{
    if (!icb || !count || start >= icb->max_commands || count > icb->max_commands - start) {
        return A840_RT_RANGE_ERROR;
    }
    memset(&icb->slots[start], 0, (size_t)count * sizeof(A840IcbSlot));
    return A840_RT_OK;
}

A840RtStatus a840_icb_copy_range(A840IndirectCmdBuffer *dst_icb,
                                 uint32_t dst_start,
                                 const A840IndirectCmdBuffer *src_icb,
                                 uint32_t src_start,
                                 uint32_t count)
{
    if (!dst_icb || !src_icb) {
        return A840_RT_INVALID_ARG;
    }
    if (dst_icb->device_id != src_icb->device_id) {
        return A840_RT_WRONG_DEVICE;
    }
    if (dst_icb->type != src_icb->type) {
        return A840_RT_INVALID_ARG;
    }
    if (!count ||
        src_start >= src_icb->max_commands || count > src_icb->max_commands - src_start ||
        dst_start >= dst_icb->max_commands || count > dst_icb->max_commands - dst_start) {
        return A840_RT_RANGE_ERROR;
    }
    memmove(&dst_icb->slots[dst_start],
            &src_icb->slots[src_start],
            (size_t)count * sizeof(A840IcbSlot));
    return A840_RT_OK;
}

A840RtStatus a840_icb_set_pipeline(A840IndirectCmdBuffer *icb,
                                   uint32_t index,
                                   uint64_t pipeline_id,
                                   const A840SdsGroupDesc *opt_sds)
{
    A840IcbSlot *slot;
    if (!icb || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->inherit_pipeline) {
        return A840_RT_STATE_ERROR;
    }
    slot = &icb->slots[index];
    slot->has_pipeline = true;
    slot->pipeline_id = pipeline_id;
    if (opt_sds) {
        if (opt_sds->group_id >= ADRENO840_MAX_SDS_SLOTS) {
            return A840_RT_RANGE_ERROR;
        }
        slot->has_sds = true;
        slot->sds = *opt_sds;
    } else {
        slot->has_sds = false;
    }
    return A840_RT_OK;
}

A840RtStatus a840_icb_set_bindless_buffer(A840IndirectCmdBuffer *icb,
                                          uint32_t index,
                                          uint32_t slot_id,
                                          A840GpuAddress addr)
{
    A840IcbSlot *slot;
    if (!icb || index >= icb->max_commands || slot_id >= A840_ICB_MAX_BIND_BUFFERS) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->inherit_buffers) {
        return A840_RT_STATE_ERROR;
    }
    if (addr.gpu_va != 0u && (addr.gpu_va % 64u) != 0u) {
        return A840_RT_ALIGNMENT;
    }
    slot = &icb->slots[index];
    slot->bindless_mask |= (1u << slot_id);
    slot->bindless_tables[slot_id] = addr;
    return A840_RT_OK;
}

A840RtStatus a840_icb_encode_draw(A840IndirectCmdBuffer *icb,
                                  uint32_t index,
                                  const A840DrawDesc *draw)
{
    if (!icb || !draw || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->type != A840_ICB_TYPE_RENDER) {
        return A840_RT_INVALID_ARG;
    }
    icb->slots[index].kind = (draw->index_type == A840_INDEX_NONE)
                                 ? A840_ICB_CMD_DRAW
                                 : A840_ICB_CMD_DRAW_INDEXED;
    icb->slots[index].draw = *draw;
    return A840_RT_OK;
}

A840RtStatus a840_icb_encode_draw_indirect(A840IndirectCmdBuffer *icb,
                                           uint32_t index,
                                           const A840DrawIndirectDesc *draw)
{
    if (!icb || !draw || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->type != A840_ICB_TYPE_RENDER) {
        return A840_RT_INVALID_ARG;
    }
    icb->slots[index].kind = A840_ICB_CMD_DRAW_INDIRECT;
    icb->slots[index].draw_indirect = *draw;
    return A840_RT_OK;
}

A840RtStatus a840_icb_encode_draw_indexed_indirect(A840IndirectCmdBuffer *icb,
                                                   uint32_t index,
                                                   const A840DrawIndexedIndirectDesc *draw)
{
    if (!icb || !draw || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->type != A840_ICB_TYPE_RENDER) {
        return A840_RT_INVALID_ARG;
    }
    icb->slots[index].kind = A840_ICB_CMD_DRAW_INDEXED_INDIRECT;
    icb->slots[index].draw_indexed_indirect = *draw;
    return A840_RT_OK;
}

A840RtStatus a840_icb_encode_dispatch(A840IndirectCmdBuffer *icb,
                                      uint32_t index,
                                      const uint32_t local_size[3],
                                      const uint32_t group_count[3])
{
    unsigned i;
    if (!icb || !local_size || !group_count || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->type != A840_ICB_TYPE_COMPUTE) {
        return A840_RT_INVALID_ARG;
    }
    if (!icb_local_size_ok(local_size)) {
        return A840_RT_RANGE_ERROR;
    }
    for (i = 0u; i < 3u; ++i) {
        if (!group_count[i]) {
            return A840_RT_RANGE_ERROR;
        }
        icb->slots[index].cs_local_size[i] = local_size[i];
        icb->slots[index].cs_grid_groups[i] = group_count[i];
    }
    icb->slots[index].kind = A840_ICB_CMD_DISPATCH;
    return A840_RT_OK;
}

A840RtStatus a840_icb_encode_dispatch_indirect(A840IndirectCmdBuffer *icb,
                                               uint32_t index,
                                               const A840DispatchIndirectDesc *dispatch)
{
    if (!icb || !dispatch || index >= icb->max_commands) {
        return A840_RT_RANGE_ERROR;
    }
    if (icb->type != A840_ICB_TYPE_COMPUTE) {
        return A840_RT_INVALID_ARG;
    }
    if (!icb_local_size_ok(dispatch->local_size)) {
        return A840_RT_RANGE_ERROR;
    }
    icb->slots[index].kind = A840_ICB_CMD_DISPATCH_INDIRECT;
    icb->slots[index].dispatch_indirect = *dispatch;
    return A840_RT_OK;
}

static A840EmitStatus emit_icb_bindless(A840CmdStream *cs,
                                        A840IcbType type,
                                        uint32_t mask,
                                        const A840GpuAddress tables[A840_ICB_MAX_BIND_BUFFERS])
{
    uint32_t base_reg = (type == A840_ICB_TYPE_COMPUTE)
                            ? A840_REG_SP_CS_BINDLESS_BASE_0_DESCRIPTOR
                            : A840_REG_SP_GFX_BINDLESS_BASE_0_DESCRIPTOR;
    uint32_t slot;
    for (slot = 0u; slot < A840_ICB_MAX_BIND_BUFFERS; ++slot) {
        if ((mask & (1u << slot)) == 0u) {
            continue;
        }
        A840EmitStatus est = a840_cs_emit_reg64_va(cs,
                                                   base_reg + slot * 2u,
                                                   tables[slot].bo_handle,
                                                   tables[slot].gpu_va,
                                                   A840_RELOC_READ);
        if (est != A840_EMIT_OK) {
            return est;
        }
    }
    return A840_EMIT_OK;
}

static A840EmitStatus emit_icb_direct_dispatch(A840CmdStream *cs,
                                               const uint32_t local_size[3],
                                               const uint32_t groups[3])
{
    uint32_t packed;
    uint32_t ndrange[7];
    uint32_t mode = A840_MODE_COMPUTE;
    uint32_t exec_words[4];
    unsigned axis;
    A840EmitStatus est;

    packed = ((local_size[0] - 1u) << A840_NDRANGE_LOCAL_X_SHIFT) |
             ((local_size[1] - 1u) << A840_NDRANGE_LOCAL_Y_SHIFT) |
             ((local_size[2] - 1u) << A840_NDRANGE_LOCAL_Z_SHIFT);
    memset(ndrange, 0, sizeof(ndrange));
    ndrange[0] = (3u << A840_NDRANGE_DIM_SHIFT) | packed;
    for (axis = 0u; axis < 3u; ++axis) {
        if (groups[axis] > UINT32_MAX / local_size[axis]) {
            return A840_EMIT_RANGE_ERROR;
        }
        ndrange[1u + 2u * axis] = groups[axis] * local_size[axis];
    }

    est = a840_cs_emit_pkt7(cs, A840_CP_SET_MARKER, &mode, 1u);
    if (est != A840_EMIT_OK) {
        return est;
    }
    est = a840_cs_emit_pkt4(cs, A840_REG_SP_CS_NDRANGE_0, ndrange, 7u);
    if (est != A840_EMIT_OK) {
        return est;
    }
    est = a840_cs_emit_pkt4(cs, A840_REG_SP_CS_NDRANGE_7, &packed, 1u);
    if (est != A840_EMIT_OK) {
        return est;
    }
    exec_words[0] = 0u;
    exec_words[1] = groups[0];
    exec_words[2] = groups[1];
    exec_words[3] = groups[2];
    return a840_cs_emit_pkt7(cs, A840_CP_EXEC_CS, exec_words, 4u);
}

A840RtStatus a840_icb_compile_to_gpu_ib(const A840IndirectCmdBuffer *icb,
                                        uint32_t start,
                                        uint32_t count,
                                        A840RtBuffer *dst_ib_buffer,
                                        A840Reloc *relocs,
                                        size_t reloc_capacity,
                                        A840IcbCompiledRange *out_compiled)
{
    A840CmdStream cs;
    A840EmitStatus est;
    uint32_t i;

    if (!icb || !dst_ib_buffer || !out_compiled) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_buffer(icb->device, dst_ib_buffer)) {
        return A840_RT_WRONG_DEVICE;
    }
    if (!dst_ib_buffer->cpu_ptr || dst_ib_buffer->size_bytes < 4u) {
        return A840_RT_INVALID_ARG;
    }
    if (!count || start >= icb->max_commands || count > icb->max_commands - start) {
        return A840_RT_RANGE_ERROR;
    }

    memset(out_compiled, 0, sizeof(*out_compiled));
    est = a840_cs_init(&cs,
                       (uint32_t *)dst_ib_buffer->cpu_ptr,
                       (size_t)(dst_ib_buffer->size_bytes / 4u),
                       relocs,
                       reloc_capacity);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    for (i = 0u; i < count; ++i) {
        const A840IcbSlot *slot = &icb->slots[start + i];
        size_t slot_start_dw;
        size_t slot_dwords;

        if (slot->kind == A840_ICB_CMD_EMPTY) {
            continue;
        }
        slot_start_dw = cs.used_dwords;

        if (slot->has_sds) {
            est = a840_emit_sds_group(&cs, &slot->sds, 1u);
            if (est != A840_EMIT_OK) {
                return a840_rt_status_from_emit(est);
            }
        }
        if (slot->bindless_mask != 0u) {
            est = emit_icb_bindless(&cs, icb->type, slot->bindless_mask, slot->bindless_tables);
            if (est != A840_EMIT_OK) {
                return a840_rt_status_from_emit(est);
            }
        }

        switch (slot->kind) {
        case A840_ICB_CMD_DRAW:
        case A840_ICB_CMD_DRAW_INDEXED:
            est = a840_emit_3d_draw(&cs, &slot->draw);
            break;
        case A840_ICB_CMD_DRAW_INDIRECT:
            est = a840_emit_draw_indirect(&cs, &slot->draw_indirect);
            break;
        case A840_ICB_CMD_DRAW_INDEXED_INDIRECT:
            est = a840_emit_draw_indexed_indirect(&cs, &slot->draw_indexed_indirect);
            break;
        case A840_ICB_CMD_DISPATCH:
            est = emit_icb_direct_dispatch(&cs, slot->cs_local_size, slot->cs_grid_groups);
            break;
        case A840_ICB_CMD_DISPATCH_INDIRECT:
            est = a840_emit_dispatch_indirect(&cs, &slot->dispatch_indirect);
            break;
        default:
            est = A840_EMIT_INVALID_ARG;
            break;
        }
        if (est != A840_EMIT_OK) {
            return a840_rt_status_from_emit(est);
        }

        slot_dwords = cs.used_dwords - slot_start_dw;
        if (slot_dwords == 0u || slot_dwords > UINT32_MAX) {
            return A840_RT_RANGE_ERROR;
        }

        out_compiled->sub_ibs[out_compiled->non_empty_count].ib.bo_handle = dst_ib_buffer->bo_handle;
        out_compiled->sub_ibs[out_compiled->non_empty_count].ib.gpu_va =
            dst_ib_buffer->gpu_va + (uint64_t)slot_start_dw * 4u;
        out_compiled->sub_ibs[out_compiled->non_empty_count].dwords = (uint32_t)slot_dwords;
        out_compiled->non_empty_count++;
    }

    out_compiled->total_ib_dwords = (uint32_t)cs.used_dwords;
    out_compiled->total_relocs = (uint32_t)cs.reloc_count;
    out_compiled->content_hash = fnv1a_u32_words((const uint32_t *)dst_ib_buffer->cpu_ptr,
                                                 cs.used_dwords);
    return A840_RT_OK;
}

A840EmitStatus a840_icb_emit_execute(A840CmdStream *primary_cs,
                                     const A840IcbCompiledRange *compiled,
                                     bool insert_wait_for_me)
{
    A840IcbExecuteRangeDesc desc;
    if (!primary_cs || !compiled) {
        return A840_EMIT_INVALID_ARG;
    }
    if (compiled->non_empty_count == 0u) {
        return A840_EMIT_RANGE_ERROR;
    }
    desc.slots = compiled->sub_ibs;
    desc.total_slots = compiled->non_empty_count;
    desc.start = 0u;
    desc.count = compiled->non_empty_count;
    desc.insert_wait_for_me = insert_wait_for_me;
    return a840_emit_icb_execute_range(primary_cs, &desc);
}

void a840_argbuf_icb_enable_phase4_router(A840DualPathRouter *router)
{
    if (!router) {
        return;
    }
    a840_router_enable_category(router, A840_ROUTE_PHASE4_ALL);
}
