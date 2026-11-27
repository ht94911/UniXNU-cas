/* SPDX-License-Identifier: MIT */
#include "a840_runtime_cmd.h"

#include <stdlib.h>
#include <string.h>

#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

typedef struct CmdCallbackEntry {
    A840RtCmdCallback fn;
    void *user_data;
} CmdCallbackEntry;

struct A840RtQueue {
    A840RtDevice *device;
    uint64_t device_id;
    uint64_t queue_handle;
    uint32_t priority;

    A840RtBuffer *ts_buffer;
    uint64_t next_ts_slot;
    uint64_t next_completion_marker;

    A840RtCmdBuffer *pending_head;
    A840RtCmdBuffer *pending_tail;
    size_t pending_count;
};

struct A840RtCmdBuffer {
    A840RtQueue *queue;
    A840RtDevice *device;
    uint64_t device_id;
    uint32_t refcount;

    A840CmdBufferState state;
    A840EncoderKind active_encoder;
    void *active_encoder_ptr;

    A840RtBuffer *ib_buffer;
    A840Reloc *relocs;
    size_t ib_capacity_dwords;
    size_t reloc_capacity;
    A840CmdStream cs;

    uint64_t submit_fence;
    uint64_t completion_marker;
    uint64_t ts_slot_va;
    A840RouteDecision last_route;

    A840RtBuffer **retained_buffers;
    size_t retained_buffer_count;
    size_t retained_buffer_cap;

    A840RtTexture **retained_textures;
    size_t retained_texture_count;
    size_t retained_texture_cap;

    CmdCallbackEntry *scheduled_handlers;
    size_t scheduled_count;
    size_t scheduled_cap;

    CmdCallbackEntry *completed_handlers;
    size_t completed_count;
    size_t completed_cap;

    A840RtCmdBuffer *next_pending;
    bool in_pending_list;
};

struct A840RtBlitEncoder {
    A840RtCmdBuffer *cb;
    bool ended;
};

struct A840RtComputeEncoder {
    A840RtCmdBuffer *cb;
    bool ended;
    bool has_pipeline;
    A840CompiledStage bound_stage;
};

struct A840RtRenderEncoder {
    A840RtCmdBuffer *cb;
    bool ended;
    bool has_program;
    bool has_state;
    uint32_t color_count;
    A840RtTexture *color_targets[A840_MAX_COLOR_TARGETS];
    A840RtTexture *depth_target;
    A840RtTexture *stencil_target;
};

/* ---------- Internal Helpers ---------- */

static A840RtStatus ensure_ptr_cap(void **arr, size_t *cap, size_t elem_size, size_t needed)
{
    size_t new_cap;
    void *grown;

    if (needed <= *cap) {
        return A840_RT_OK;
    }
    new_cap = *cap ? *cap : 8u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / elem_size) {
        return A840_RT_OOM;
    }
    grown = realloc(*arr, new_cap * elem_size);
    if (!grown) {
        return A840_RT_OOM;
    }
    *arr = grown;
    *cap = new_cap;
    return A840_RT_OK;
}

static A840RtStatus pre_reserve_buffers(A840RtCmdBuffer *cb, size_t extra)
{
    void *ptr = cb->retained_buffers;
    A840RtStatus st;
    if (extra > SIZE_MAX - cb->retained_buffer_count) {
        return A840_RT_OOM;
    }
    st = ensure_ptr_cap(&ptr, &cb->retained_buffer_cap,
                        sizeof(A840RtBuffer *), cb->retained_buffer_count + extra);
    cb->retained_buffers = (A840RtBuffer **)ptr;
    return st;
}

static A840RtStatus pre_reserve_textures(A840RtCmdBuffer *cb, size_t extra)
{
    void *ptr = cb->retained_textures;
    A840RtStatus st;
    if (extra > SIZE_MAX - cb->retained_texture_count) {
        return A840_RT_OOM;
    }
    st = ensure_ptr_cap(&ptr, &cb->retained_texture_cap,
                        sizeof(A840RtTexture *), cb->retained_texture_count + extra);
    cb->retained_textures = (A840RtTexture **)ptr;
    return st;
}

static void retain_buffer_reserved(A840RtCmdBuffer *cb, A840RtBuffer *buf)
{
    size_t i;
    if (!cb || !buf) {
        return;
    }
    for (i = 0; i < cb->retained_buffer_count; ++i) {
        if (cb->retained_buffers[i] == buf) {
            return;
        }
    }
    a840_rt_buffer_retain(buf);
    cb->retained_buffers[cb->retained_buffer_count++] = buf;
}

static void retain_texture_reserved(A840RtCmdBuffer *cb, A840RtTexture *tex)
{
    size_t i;
    if (!cb || !tex) {
        return;
    }
    for (i = 0; i < cb->retained_texture_count; ++i) {
        if (cb->retained_textures[i] == tex) {
            return;
        }
    }
    a840_rt_texture_retain(tex);
    cb->retained_textures[cb->retained_texture_count++] = tex;
}

static void release_all_retained(A840RtCmdBuffer *cb)
{
    size_t i;
    for (i = 0; i < cb->retained_buffer_count; ++i) {
        a840_rt_buffer_release(cb->retained_buffers[i]);
    }
    cb->retained_buffer_count = 0u;
    for (i = 0; i < cb->retained_texture_count; ++i) {
        a840_rt_texture_release(cb->retained_textures[i]);
    }
    cb->retained_texture_count = 0u;
}

static A840RtStatus check_buffer_range(const A840RtDevice *dev,
                                       const A840RtBuffer *buf,
                                       uint64_t offset,
                                       uint64_t bytes)
{
    if (!buf) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_buffer(dev, buf)) {
        return A840_RT_WRONG_DEVICE;
    }
    if (offset > buf->size_bytes || bytes > buf->size_bytes - offset) {
        return A840_RT_RANGE_ERROR;
    }
    return A840_RT_OK;
}

static A840RtStatus check_private_buffer(const A840RtDevice *dev,
                                         uint32_t private_mem_bytes,
                                         const A840RtBuffer *buf,
                                         uint64_t offset)
{
    uint32_t item;
    uint64_t need;

    if (private_mem_bytes == 0u) {
        if (buf != NULL && !a840_rt_device_owns_buffer(dev, buf)) {
            return A840_RT_WRONG_DEVICE;
        }
        return A840_RT_OK;
    }
    if (private_mem_bytes > (A840_PRIVATE_ITEM_MAX << A840_PRIVATE_ITEM_SHR)) {
        return A840_RT_RANGE_ERROR;
    }
    item = UINT32_C(1) << A840_PRIVATE_ITEM_SHR;
    while (item < private_mem_bytes) {
        item *= 2u;
    }
    need = (uint64_t)item * A840_LIMIT_FIBERS_PER_SP * A840_LIMIT_NUM_SP_CORES;
    return check_buffer_range(dev, buf, offset, need);
}

static A840RtStatus check_texture_owner(const A840RtDevice *dev, const A840RtTexture *tex)
{
    if (!tex) {
        return A840_RT_INVALID_ARG;
    }
    if (!a840_rt_device_owns_texture(dev, tex)) {
        return A840_RT_WRONG_DEVICE;
    }
    return A840_RT_OK;
}

static A840RtStatus emit_barrier_tx(A840CmdStream *cs,
                                    uint32_t cache_mask,
                                    bool wait_mem_writes,
                                    bool wfi)
{
    size_t saved_words = cs->used_dwords;
    size_t saved_relocs = cs->reloc_count;
    A840EmitStatus est;

    if (cache_mask != 0u) {
        est = a840_emit_cache_flush_inv(cs, cache_mask);
        if (est != A840_EMIT_OK) {
            cs->used_dwords = saved_words;
            cs->reloc_count = saved_relocs;
            return a840_rt_status_from_emit(est);
        }
    }
    if (wait_mem_writes) {
        est = a840_emit_wait_mem_writes(cs);
        if (est != A840_EMIT_OK) {
            cs->used_dwords = saved_words;
            cs->reloc_count = saved_relocs;
            return a840_rt_status_from_emit(est);
        }
    }
    if (wfi) {
        est = a840_emit_wfi(cs);
        if (est != A840_EMIT_OK) {
            cs->used_dwords = saved_words;
            cs->reloc_count = saved_relocs;
            return a840_rt_status_from_emit(est);
        }
    }
    return A840_RT_OK;
}

static void fire_callbacks(A840RtCmdBuffer *cb,
                           const CmdCallbackEntry *entries,
                           size_t count)
{
    size_t i;
    a840_rt_cb_retain(cb);
    for (i = 0; i < count; ++i) {
        if (entries[i].fn) {
            entries[i].fn(cb, entries[i].user_data);
        }
    }
    a840_rt_cb_release(cb);
}

static void complete_pending_head(A840RtQueue *queue)
{
    A840RtCmdBuffer *cb = queue->pending_head;
    if (!cb) {
        return;
    }
    queue->pending_head = cb->next_pending;
    if (!queue->pending_head) {
        queue->pending_tail = NULL;
    }
    cb->next_pending = NULL;
    cb->in_pending_list = false;
    queue->pending_count--;

    cb->state = A840_CB_COMPLETED;
    fire_callbacks(cb, cb->completed_handlers, cb->completed_count);
    release_all_retained(cb);
    a840_rt_cb_release(cb);
}

static void drain_queue_up_to_fence(A840RtQueue *queue, uint64_t up_to_fence)
{
    while (queue->pending_head && queue->pending_head->submit_fence <= up_to_fence) {
        complete_pending_head(queue);
    }
}

/* ---------- Queue API ---------- */

A840RtStatus a840_rt_queue_create(A840RtDevice *device,
                                  uint32_t priority,
                                  A840RtQueue **out_queue)
{
    A840RtQueue *q;
    A840TransportStatus tst;
    A840RtBufferDesc ts_desc;
    A840RtStatus st;

    if (!out_queue) {
        return A840_RT_INVALID_ARG;
    }
    *out_queue = NULL;
    if (!device) {
        return A840_RT_INVALID_ARG;
    }

    q = (A840RtQueue *)calloc(1u, sizeof(*q));
    if (!q) {
        return A840_RT_OOM;
    }

    tst = device->transport.ops->queue_create(device->transport.ctx, priority, &q->queue_handle);
    if (tst != A840_TRANSPORT_OK) {
        free(q);
        return (tst == A840_TRANSPORT_INVALID_ARG) ? A840_RT_INVALID_ARG : A840_RT_TRANSPORT_ERROR;
    }

    memset(&ts_desc, 0, sizeof(ts_desc));
    ts_desc.size_bytes = A840_RT_PAGE_BYTES;
    ts_desc.alignment = A840_RT_PAGE_BYTES;
    ts_desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE;
    st = a840_rt_buffer_create(device, &ts_desc, &q->ts_buffer);
    if (st != A840_RT_OK) {
        device->transport.ops->queue_destroy(device->transport.ctx, q->queue_handle);
        free(q);
        return st;
    }

    q->device = device;
    q->device_id = device->device_id;
    q->priority = priority;
    device->live_queues++;

    *out_queue = q;
    return A840_RT_OK;
}

void a840_rt_queue_destroy(A840RtQueue *queue)
{
    if (!queue) {
        return;
    }
    a840_rt_queue_poll_completions(queue);
    while (queue->pending_head) {
        A840RtCmdBuffer *cb = queue->pending_head;
        queue->pending_head = cb->next_pending;
        cb->next_pending = NULL;
        cb->in_pending_list = false;
        queue->pending_count--;
        cb->state = A840_CB_ERROR;
        release_all_retained(cb);
        a840_rt_cb_release(cb);
    }
    queue->pending_tail = NULL;

    a840_rt_buffer_release(queue->ts_buffer);
    queue->device->transport.ops->queue_destroy(queue->device->transport.ctx, queue->queue_handle);
    if (queue->device->live_queues > 0u) {
        queue->device->live_queues--;
    }
    free(queue);
}

uint64_t a840_rt_queue_handle(const A840RtQueue *queue)
{
    return queue ? queue->queue_handle : 0u;
}

size_t a840_rt_queue_pending_count(const A840RtQueue *queue)
{
    return queue ? queue->pending_count : 0u;
}

A840RtStatus a840_rt_queue_poll_completions(A840RtQueue *queue)
{
    if (!queue) {
        return A840_RT_INVALID_ARG;
    }
    while (queue->pending_head) {
        uint64_t ticks = 0u;
        A840TransportStatus tst = queue->device->transport.ops->fence_wait(
            queue->device->transport.ctx, queue->queue_handle,
            queue->pending_head->submit_fence, 0u, &ticks);
        if (tst == A840_TRANSPORT_OK) {
            complete_pending_head(queue);
        } else if (tst == A840_TRANSPORT_TIMEOUT) {
            break;
        } else {
            return A840_RT_TRANSPORT_ERROR;
        }
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_queue_wait_idle(A840RtQueue *queue, uint64_t timeout_ns)
{
    if (!queue) {
        return A840_RT_INVALID_ARG;
    }
    if (!queue->pending_tail) {
        return A840_RT_OK;
    }
    return a840_rt_cb_wait_until_completed(queue->pending_tail, timeout_ns);
}

/* ---------- Command Buffer API ---------- */

A840RtStatus a840_rt_cb_create(A840RtQueue *queue,
                               const A840RtCmdBufferDesc *desc,
                               A840RtCmdBuffer **out_cb)
{
    size_t ib_dwords;
    size_t reloc_cap;
    A840RtCmdBuffer *cb;
    A840RtBufferDesc ib_desc;
    A840RtStatus st;
    A840EmitStatus est;

    if (!out_cb) {
        return A840_RT_INVALID_ARG;
    }
    *out_cb = NULL;
    if (!queue) {
        return A840_RT_INVALID_ARG;
    }

    ib_dwords = (desc && desc->ib_capacity_dwords) ? desc->ib_capacity_dwords : A840_RT_DEFAULT_IB_DWORDS;
    reloc_cap = (desc && desc->reloc_capacity) ? desc->reloc_capacity : A840_RT_DEFAULT_RELOC_CAPACITY;
    if (ib_dwords > SIZE_MAX / sizeof(uint32_t) || reloc_cap > SIZE_MAX / sizeof(A840Reloc)) {
        return A840_RT_RANGE_ERROR;
    }

    cb = (A840RtCmdBuffer *)calloc(1u, sizeof(*cb));
    if (!cb) {
        return A840_RT_OOM;
    }

    cb->relocs = (A840Reloc *)calloc(reloc_cap, sizeof(A840Reloc));
    if (!cb->relocs) {
        free(cb);
        return A840_RT_OOM;
    }

    memset(&ib_desc, 0, sizeof(ib_desc));
    ib_desc.size_bytes = (uint64_t)ib_dwords * sizeof(uint32_t);
    ib_desc.alignment = A840_RT_PAGE_BYTES;
    ib_desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    st = a840_rt_buffer_create(queue->device, &ib_desc, &cb->ib_buffer);
    if (st != A840_RT_OK) {
        free(cb->relocs);
        free(cb);
        return st;
    }

    est = a840_cs_init(&cb->cs, (uint32_t *)cb->ib_buffer->cpu_ptr, ib_dwords,
                       cb->relocs, reloc_cap);
    if (est != A840_EMIT_OK) {
        a840_rt_buffer_release(cb->ib_buffer);
        free(cb->relocs);
        free(cb);
        return a840_rt_status_from_emit(est);
    }

    cb->queue = queue;
    cb->device = queue->device;
    cb->device_id = queue->device_id;
    cb->refcount = 1u;
    cb->state = A840_CB_RECORDING;
    cb->active_encoder = A840_ENCODER_NONE;
    cb->ib_capacity_dwords = ib_dwords;
    cb->reloc_capacity = reloc_cap;

    *out_cb = cb;
    return A840_RT_OK;
}

void a840_rt_cb_retain(A840RtCmdBuffer *cb)
{
    if (cb && cb->refcount > 0u) {
        cb->refcount++;
    }
}

void a840_rt_cb_release(A840RtCmdBuffer *cb)
{
    if (!cb || cb->refcount == 0u) {
        return;
    }
    cb->refcount--;
    if (cb->refcount > 0u) {
        return;
    }
    release_all_retained(cb);
    free(cb->retained_buffers);
    free(cb->retained_textures);
    free(cb->scheduled_handlers);
    free(cb->completed_handlers);
    a840_rt_buffer_release(cb->ib_buffer);
    free(cb->relocs);
    free(cb);
}

A840CmdBufferState a840_rt_cb_state(const A840RtCmdBuffer *cb)
{
    return cb ? cb->state : A840_CB_ERROR;
}

size_t a840_rt_cb_used_dwords(const A840RtCmdBuffer *cb)
{
    return cb ? cb->cs.used_dwords : 0u;
}

size_t a840_rt_cb_reloc_count(const A840RtCmdBuffer *cb)
{
    return cb ? cb->cs.reloc_count : 0u;
}

uint64_t a840_rt_cb_submit_fence(const A840RtCmdBuffer *cb)
{
    return cb ? cb->submit_fence : 0u;
}

uint64_t a840_rt_cb_completion_marker(const A840RtCmdBuffer *cb)
{
    return cb ? cb->completion_marker : 0u;
}

A840RouteDecision a840_rt_cb_last_route(const A840RtCmdBuffer *cb)
{
    A840RouteDecision empty;
    memset(&empty, 0, sizeof(empty));
    return cb ? cb->last_route : empty;
}

size_t a840_rt_cb_retained_buffer_count(const A840RtCmdBuffer *cb)
{
    return cb ? cb->retained_buffer_count : 0u;
}

size_t a840_rt_cb_retained_texture_count(const A840RtCmdBuffer *cb)
{
    return cb ? cb->retained_texture_count : 0u;
}

A840RtStatus a840_rt_cb_add_scheduled_handler(A840RtCmdBuffer *cb,
                                              A840RtCmdCallback fn,
                                              void *user_data)
{
    void *ptr;
    A840RtStatus st;
    if (!cb || !fn) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING) {
        return A840_RT_STATE_ERROR;
    }
    ptr = cb->scheduled_handlers;
    st = ensure_ptr_cap(&ptr, &cb->scheduled_cap,
                        sizeof(CmdCallbackEntry), cb->scheduled_count + 1u);
    cb->scheduled_handlers = (CmdCallbackEntry *)ptr;
    if (st != A840_RT_OK) {
        return st;
    }
    cb->scheduled_handlers[cb->scheduled_count].fn = fn;
    cb->scheduled_handlers[cb->scheduled_count].user_data = user_data;
    cb->scheduled_count++;
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_add_completed_handler(A840RtCmdBuffer *cb,
                                              A840RtCmdCallback fn,
                                              void *user_data)
{
    void *ptr;
    A840RtStatus st;
    if (!cb || !fn) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING) {
        return A840_RT_STATE_ERROR;
    }
    ptr = cb->completed_handlers;
    st = ensure_ptr_cap(&ptr, &cb->completed_cap,
                        sizeof(CmdCallbackEntry), cb->completed_count + 1u);
    cb->completed_handlers = (CmdCallbackEntry *)ptr;
    if (st != A840_RT_OK) {
        return st;
    }
    cb->completed_handlers[cb->completed_count].fn = fn;
    cb->completed_handlers[cb->completed_count].user_data = user_data;
    cb->completed_count++;
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_encode_barrier(A840RtCmdBuffer *cb,
                                       uint32_t cache_mask,
                                       bool wait_mem_writes,
                                       bool wfi)
{
    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }
    return emit_barrier_tx(&cb->cs, cache_mask, wait_mem_writes, wfi);
}

A840RtStatus a840_rt_cb_encode_mem_write64(A840RtCmdBuffer *cb,
                                           A840RtBuffer *dst_buffer,
                                           uint64_t dst_offset,
                                           uint64_t value)
{
    A840RtStatus st;
    A840EmitStatus est;

    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }
    st = check_buffer_range(cb->device, dst_buffer, dst_offset, sizeof(uint64_t));
    if (st != A840_RT_OK) {
        return st;
    }
    st = pre_reserve_buffers(cb, 1u);
    if (st != A840_RT_OK) {
        return st;
    }
    est = a840_emit_mem_write64(&cb->cs, dst_buffer->bo_handle,
                                dst_buffer->gpu_va + dst_offset, value);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_buffer_reserved(cb, dst_buffer);
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_encode_timestamp(A840RtCmdBuffer *cb,
                                         A840RtBuffer *dst_buffer,
                                         uint64_t dst_offset,
                                         uint64_t payload,
                                         bool sample_gpu_clock)
{
    A840RtStatus st;
    A840EmitStatus est;
    uint32_t event_id;

    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }
    st = check_buffer_range(cb->device, dst_buffer, dst_offset, sizeof(uint64_t));
    if (st != A840_RT_OK) {
        return st;
    }
    st = pre_reserve_buffers(cb, 1u);
    if (st != A840_RT_OK) {
        return st;
    }
    event_id = A840_EVENT_RB_DONE_TS | (sample_gpu_clock ? A840_TIMESTAMP_GPU_CLOCK : 0u);
    est = a840_emit_timestamp_event(&cb->cs, event_id, dst_buffer->bo_handle,
                                    dst_buffer->gpu_va + dst_offset, payload);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_buffer_reserved(cb, dst_buffer);
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_commit(A840RtCmdBuffer *cb)
{
    A840RtQueue *q;
    uint64_t slot_count;
    uint64_t slot_idx;
    uint64_t slot_va;
    uint64_t marker;
    size_t saved_words;
    size_t saved_relocs;
    A840EmitStatus est;
    A840RtStatus st;
    A840SubmitIb ib;
    A840SubmitDesc submit;
    A840SubmitReply reply;
    A840TransportStatus tst;

    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING) {
        return A840_RT_STATE_ERROR;
    }
    if (cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }
    if (cb->cs.used_dwords == 0u) {
        return A840_RT_STATE_ERROR;
    }

    q = cb->queue;
    slot_count = q->ts_buffer->alloc_bytes / sizeof(uint64_t);
    slot_idx = q->next_ts_slot % slot_count;
    slot_va = q->ts_buffer->gpu_va + slot_idx * sizeof(uint64_t);
    marker = q->next_completion_marker + 1u;

    st = pre_reserve_buffers(cb, 1u);
    if (st != A840_RT_OK) {
        return st;
    }

    saved_words = cb->cs.used_dwords;
    saved_relocs = cb->cs.reloc_count;
    est = a840_emit_timestamp_event(&cb->cs, A840_EVENT_RB_DONE_TS,
                                    q->ts_buffer->bo_handle, slot_va, marker);
    if (est != A840_EMIT_OK) {
        /* Transactional rollback: preserve CS & RECORDING state. */
        cb->cs.used_dwords = saved_words;
        cb->cs.reloc_count = saved_relocs;
        return a840_rt_status_from_emit(est);
    }

    retain_buffer_reserved(cb, q->ts_buffer);

    memset(&ib, 0, sizeof(ib));
    ib.bo_handle = cb->ib_buffer->bo_handle;
    ib.gpu_va = cb->ib_buffer->gpu_va;
    ib.bytes = (uint64_t)cb->cs.used_dwords * sizeof(uint32_t);

    memset(&submit, 0, sizeof(submit));
    submit.queue = q->queue_handle;
    submit.ibs = &ib;
    submit.ib_count = 1u;
    submit.relocs = cb->cs.relocs;
    submit.reloc_count = (uint32_t)cb->cs.reloc_count;

    memset(&reply, 0, sizeof(reply));
    cb->state = A840_CB_COMMITTED;
    tst = cb->device->transport.ops->queue_submit(cb->device->transport.ctx, &submit, &reply);
    if (tst != A840_TRANSPORT_OK) {
        cb->cs.used_dwords = saved_words;
        cb->cs.reloc_count = saved_relocs;
        cb->state = A840_CB_ERROR;
        return A840_RT_TRANSPORT_ERROR;
    }

    q->next_ts_slot++;
    q->next_completion_marker = marker;
    cb->submit_fence = reply.fence;
    cb->completion_marker = marker;
    cb->ts_slot_va = slot_va;

    /* Enqueue on queue's FIFO pending list and retain a queue-owned reference. */
    a840_rt_cb_retain(cb);
    cb->next_pending = NULL;
    cb->in_pending_list = true;
    if (q->pending_tail) {
        q->pending_tail->next_pending = cb;
    } else {
        q->pending_head = cb;
    }
    q->pending_tail = cb;
    q->pending_count++;

    cb->state = A840_CB_SCHEDULED;
    fire_callbacks(cb, cb->scheduled_handlers, cb->scheduled_count);

    /* Immediately advance FIFO completions if backend already finished the fence. */
    a840_rt_queue_poll_completions(q);
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_wait_until_completed(A840RtCmdBuffer *cb, uint64_t timeout_ns)
{
    uint64_t ticks = 0u;
    A840TransportStatus tst;

    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state == A840_CB_COMPLETED) {
        return A840_RT_OK;
    }
    if (cb->state != A840_CB_SCHEDULED) {
        return A840_RT_STATE_ERROR;
    }

    tst = cb->device->transport.ops->fence_wait(cb->device->transport.ctx,
                                                cb->queue->queue_handle,
                                                cb->submit_fence, timeout_ns, &ticks);
    if (tst == A840_TRANSPORT_TIMEOUT) {
        return A840_RT_TIMEOUT;
    }
    if (tst != A840_TRANSPORT_OK) {
        return A840_RT_TRANSPORT_ERROR;
    }

    drain_queue_up_to_fence(cb->queue, cb->submit_fence);
    return A840_RT_OK;
}

/* ---------- Blit Encoder ---------- */

static A840RtStatus check_blit_active(const A840RtBlitEncoder *enc)
{
    if (!enc || !enc->cb) {
        return A840_RT_INVALID_ARG;
    }
    if (enc->ended || enc->cb->state != A840_CB_RECORDING ||
        enc->cb->active_encoder != A840_ENCODER_BLIT ||
        enc->cb->active_encoder_ptr != enc) {
        return A840_RT_STATE_ERROR;
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_begin_blit(A840RtCmdBuffer *cb, A840RtBlitEncoder **out_enc)
{
    A840RtBlitEncoder *enc;

    if (!out_enc) {
        return A840_RT_INVALID_ARG;
    }
    *out_enc = NULL;
    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }

    enc = (A840RtBlitEncoder *)calloc(1u, sizeof(*enc));
    if (!enc) {
        return A840_RT_OOM;
    }
    enc->cb = cb;
    cb->active_encoder = A840_ENCODER_BLIT;
    cb->active_encoder_ptr = enc;

    *out_enc = enc;
    return A840_RT_OK;
}

A840RtStatus a840_rt_blit_copy_buffer(A840RtBlitEncoder *enc,
                                      A840RtBuffer *src_buffer,
                                      uint64_t src_offset,
                                      A840RtBuffer *dst_buffer,
                                      uint64_t dst_offset,
                                      uint32_t bytes)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840BlitRouteQuery rq;
    A840RouteDecision dec;
    A840EmitStatus est;

    st = check_blit_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    cb = enc->cb;

    st = check_buffer_range(cb->device, src_buffer, src_offset, (uint64_t)bytes);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_buffer_range(cb->device, dst_buffer, dst_offset, (uint64_t)bytes);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&rq, 0, sizeof(rq));
    rq.is_buffer_copy = true;
    dec = a840_router_eval_blit(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    st = pre_reserve_buffers(cb, 2u);
    if (st != A840_RT_OK) {
        return st;
    }

    est = a840_emit_buffer_copy(&cb->cs,
                                src_buffer->bo_handle, src_buffer->gpu_va + src_offset,
                                dst_buffer->bo_handle, dst_buffer->gpu_va + dst_offset,
                                bytes);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_buffer_reserved(cb, src_buffer);
    retain_buffer_reserved(cb, dst_buffer);
    return A840_RT_OK;
}

A840RtStatus a840_rt_blit_clear_2d(A840RtBlitEncoder *enc,
                                   A840RtTexture *dst_texture,
                                   const A840Rect2D *rect,
                                   const uint32_t color[4])
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840BlitRouteQuery rq;
    A840RouteDecision dec;
    A840Clear2DDesc clear_desc;
    A840EmitStatus est;

    st = check_blit_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!rect || !color) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    st = check_texture_owner(cb->device, dst_texture);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&rq, 0, sizeof(rq));
    rq.is_clear = true;
    rq.dst_mtl_format = dst_texture->mtl_pixel_format;
    rq.tex_type = dst_texture->tex_type;
    rq.dst_tile_mode = dst_texture->tile_mode;
    dec = a840_router_eval_blit(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    st = pre_reserve_textures(cb, 1u);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&clear_desc, 0, sizeof(clear_desc));
    a840_rt_texture_as_surface2d(dst_texture, &clear_desc.dst);
    clear_desc.rect = *rect;
    memcpy(clear_desc.color, color, sizeof(clear_desc.color));

    est = a840_emit_2d_clear(&cb->cs, &clear_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_texture_reserved(cb, dst_texture);
    return A840_RT_OK;
}

A840RtStatus a840_rt_blit_2d(A840RtBlitEncoder *enc,
                             A840RtTexture *src_texture,
                             const A840Rect2D *src_rect,
                             A840RtTexture *dst_texture,
                             const A840Rect2D *dst_rect,
                             uint32_t filter)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840BlitRouteQuery rq;
    A840RouteDecision dec;
    A840Blit2DDesc blit_desc;
    A840EmitStatus est;

    st = check_blit_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!src_rect || !dst_rect) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    st = check_texture_owner(cb->device, src_texture);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_texture_owner(cb->device, dst_texture);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&rq, 0, sizeof(rq));
    rq.src_mtl_format = src_texture->mtl_pixel_format;
    rq.dst_mtl_format = dst_texture->mtl_pixel_format;
    rq.tex_type = (src_texture->tex_type == A840_TEX_3D || dst_texture->tex_type == A840_TEX_3D)
                      ? A840_TEX_3D
                      : dst_texture->tex_type;
    rq.src_tile_mode = src_texture->tile_mode;
    rq.dst_tile_mode = dst_texture->tile_mode;
    rq.filter = filter;
    dec = a840_router_eval_blit(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    /* Check whether src and dst share overlapping VA storage (same texture or
     * aliased views). Metal allows same-texture blits (e.g. QuartzCore warmup_shaders
     * copying (0,0,8x8) -> (8,8,8x8) within one 776x24 texture), whereas the raw
     * hardware 2D emitter strictly rejects overlapping storage (A840_EMIT_ALIAS). */
    {
        const uint64_t src_end = src_texture->gpu_va + src_texture->alloc_bytes;
        const uint64_t dst_end = dst_texture->gpu_va + dst_texture->alloc_bytes;
        const bool aliased = (src_texture == dst_texture) ||
                             (src_texture->alloc_bytes > 0u && dst_texture->alloc_bytes > 0u &&
                              src_texture->gpu_va < dst_end && dst_texture->gpu_va < src_end);
        if (aliased) {
            A840RtTextureDesc scratch_desc;
            A840RtTexture *scratch = NULL;
            A840Rect2D scratch_rect;
            A840Blit2DDesc step1, step2;
            size_t saved_words, saved_relocs;

            if (src_rect->width == 0u || src_rect->height == 0u ||
                dst_rect->width == 0u || dst_rect->height == 0u ||
                src_rect->x > src_texture->width || src_rect->width > src_texture->width - src_rect->x ||
                src_rect->y > src_texture->height || src_rect->height > src_texture->height - src_rect->y ||
                dst_rect->x > dst_texture->width || dst_rect->width > dst_texture->width - dst_rect->x ||
                dst_rect->y > dst_texture->height || dst_rect->height > dst_texture->height - dst_rect->y) {
                return A840_RT_RANGE_ERROR;
            }
            if (src_texture == dst_texture &&
                src_rect->x == dst_rect->x && src_rect->y == dst_rect->y &&
                src_rect->width == dst_rect->width && src_rect->height == dst_rect->height) {
                st = pre_reserve_textures(cb, 1u);
                if (st != A840_RT_OK) {
                    return st;
                }
                retain_texture_reserved(cb, src_texture);
                return A840_RT_OK;
            }

            memset(&scratch_desc, 0, sizeof(scratch_desc));
            scratch_desc.mtl_pixel_format = dst_texture->mtl_pixel_format;
            scratch_desc.tex_type = A840_TEX_2D;
            scratch_desc.width = dst_rect->width;
            scratch_desc.height = dst_rect->height;
            scratch_desc.depth = 1u;
            scratch_desc.array_layers = 1u;
            scratch_desc.mip_levels = 1u;
            scratch_desc.samples = 1u;
            scratch_desc.tile_mode = A840_TILE_LINEAR;
            scratch_desc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET;

            st = a840_rt_texture_create(cb->device, &scratch_desc, &scratch);
            if (st != A840_RT_OK) {
                return st;
            }
            st = pre_reserve_textures(cb, 3u);
            if (st != A840_RT_OK) {
                a840_rt_texture_release(scratch);
                return st;
            }

            scratch_rect.x = 0u;
            scratch_rect.y = 0u;
            scratch_rect.width = dst_rect->width;
            scratch_rect.height = dst_rect->height;

            memset(&step1, 0, sizeof(step1));
            a840_rt_texture_as_surface2d(src_texture, &step1.src);
            a840_rt_texture_as_surface2d(scratch, &step1.dst);
            step1.src_rect = *src_rect;
            step1.dst_rect = scratch_rect;
            step1.filter = filter;

            memset(&step2, 0, sizeof(step2));
            a840_rt_texture_as_surface2d(scratch, &step2.src);
            a840_rt_texture_as_surface2d(dst_texture, &step2.dst);
            step2.src_rect = scratch_rect;
            step2.dst_rect = *dst_rect;
            step2.filter = A840_FILTER_NEAREST;

            saved_words = cb->cs.used_dwords;
            saved_relocs = cb->cs.reloc_count;
            est = a840_emit_2d_blit(&cb->cs, &step1);
            if (est != A840_EMIT_OK) {
                cb->cs.used_dwords = saved_words;
                cb->cs.reloc_count = saved_relocs;
                a840_rt_texture_release(scratch);
                return a840_rt_status_from_emit(est);
            }
            st = emit_barrier_tx(&cb->cs,
                                 A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_CCU_INV_COLOR | A840_CACHE_UCHE_INV,
                                 true, true);
            if (st != A840_RT_OK) {
                cb->cs.used_dwords = saved_words;
                cb->cs.reloc_count = saved_relocs;
                a840_rt_texture_release(scratch);
                return st;
            }
            est = a840_emit_2d_blit(&cb->cs, &step2);
            if (est != A840_EMIT_OK) {
                cb->cs.used_dwords = saved_words;
                cb->cs.reloc_count = saved_relocs;
                a840_rt_texture_release(scratch);
                return a840_rt_status_from_emit(est);
            }
            retain_texture_reserved(cb, src_texture);
            retain_texture_reserved(cb, dst_texture);
            retain_texture_reserved(cb, scratch);
            a840_rt_texture_release(scratch);
            return A840_RT_OK;
        }
    }

    st = pre_reserve_textures(cb, 2u);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&blit_desc, 0, sizeof(blit_desc));
    a840_rt_texture_as_surface2d(src_texture, &blit_desc.src);
    a840_rt_texture_as_surface2d(dst_texture, &blit_desc.dst);
    blit_desc.src_rect = *src_rect;
    blit_desc.dst_rect = *dst_rect;
    blit_desc.filter = filter;

    est = a840_emit_2d_blit(&cb->cs, &blit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_texture_reserved(cb, src_texture);
    retain_texture_reserved(cb, dst_texture);
    return A840_RT_OK;
}

A840RtStatus a840_rt_blit_barrier(A840RtBlitEncoder *enc,
                                  uint32_t cache_mask,
                                  bool wait_mem_writes,
                                  bool wfi)
{
    A840RtStatus st = check_blit_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    return emit_barrier_tx(&enc->cb->cs, cache_mask, wait_mem_writes, wfi);
}

A840RtStatus a840_rt_blit_end(A840RtBlitEncoder *enc)
{
    A840RtStatus st = check_blit_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    enc->ended = true;
    enc->cb->active_encoder = A840_ENCODER_NONE;
    enc->cb->active_encoder_ptr = NULL;
    return A840_RT_OK;
}

void a840_rt_blit_destroy(A840RtBlitEncoder *enc)
{
    if (!enc) {
        return;
    }
    if (!enc->ended && enc->cb && enc->cb->active_encoder_ptr == enc) {
        enc->cb->active_encoder = A840_ENCODER_NONE;
        enc->cb->active_encoder_ptr = NULL;
    }
    free(enc);
}

/* ---------- Compute Encoder ---------- */

static A840RtStatus check_compute_active(const A840RtComputeEncoder *enc)
{
    if (!enc || !enc->cb) {
        return A840_RT_INVALID_ARG;
    }
    if (enc->ended || enc->cb->state != A840_CB_RECORDING ||
        enc->cb->active_encoder != A840_ENCODER_COMPUTE ||
        enc->cb->active_encoder_ptr != enc) {
        return A840_RT_STATE_ERROR;
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_cb_begin_compute(A840RtCmdBuffer *cb, A840RtComputeEncoder **out_enc)
{
    A840RtComputeEncoder *enc;

    if (!out_enc) {
        return A840_RT_INVALID_ARG;
    }
    *out_enc = NULL;
    if (!cb) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }

    enc = (A840RtComputeEncoder *)calloc(1u, sizeof(*enc));
    if (!enc) {
        return A840_RT_OOM;
    }
    enc->cb = cb;
    cb->active_encoder = A840_ENCODER_COMPUTE;
    cb->active_encoder_ptr = enc;

    *out_enc = enc;
    return A840_RT_OK;
}

A840RtStatus a840_rt_compute_set_pipeline(A840RtComputeEncoder *enc,
                                          const A840RtComputePipelineDesc *desc)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840ComputeRouteQuery rq;
    A840RouteDecision dec;
    A840ComputePipelineDesc emit_desc;
    A840EmitStatus est;
    uint64_t code_bytes;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!desc) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    memset(&rq, 0, sizeof(rq));
    rq.has_compiled_kernel = (desc->stage != NULL);
    rq.has_valid_local_size = desc->stage != NULL &&
                              desc->stage->local_size[0] != 0u &&
                              desc->stage->local_size[1] != 0u &&
                              desc->stage->local_size[2] != 0u;
    dec = a840_router_eval_compute(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    code_bytes = (uint64_t)desc->stage->isa_dwords * sizeof(uint32_t);
    if (code_bytes == 0u) {
        return A840_RT_RANGE_ERROR;
    }
    st = check_buffer_range(cb->device, desc->code_buffer, desc->code_offset, code_bytes);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_private_buffer(cb->device, desc->stage->private_mem_bytes,
                              desc->private_buffer, desc->private_offset);
    if (st != A840_RT_OK) {
        return st;
    }

    st = pre_reserve_buffers(cb, 2u);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.shader.stage = desc->stage;
    emit_desc.shader.code.bo_handle = desc->code_buffer->bo_handle;
    emit_desc.shader.code.gpu_va = desc->code_buffer->gpu_va + desc->code_offset;
    if (desc->private_buffer != NULL) {
        emit_desc.shader.private_memory.bo_handle = desc->private_buffer->bo_handle;
        emit_desc.shader.private_memory.gpu_va = desc->private_buffer->gpu_va + desc->private_offset;
    }
    emit_desc.shader.textures = desc->textures;
    emit_desc.shader.samplers = desc->samplers;
    emit_desc.shader.uavs = desc->uavs;
    emit_desc.shader.per_wave_private_memory = desc->per_wave_private_memory;
    emit_desc.shader.bindless_tex = desc->bindless_tex;
    emit_desc.shader.bindless_samp = desc->bindless_samp;
    emit_desc.shader.bindless_uav = desc->bindless_uav;
    emit_desc.shader.bindless_ubo = desc->bindless_ubo;
    emit_desc.local_id_regid = desc->local_id_regid;
    emit_desc.wg_id_regid = desc->wg_id_regid;
    emit_desc.round_robin = desc->round_robin;
    emit_desc.linear_dispatch = desc->linear_dispatch;

    est = a840_emit_compute_pipeline(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    retain_buffer_reserved(cb, desc->code_buffer);
    if (desc->private_buffer != NULL) {
        retain_buffer_reserved(cb, desc->private_buffer);
    }
    enc->bound_stage = *desc->stage;
    enc->has_pipeline = true;
    return A840_RT_OK;
}

A840RtStatus a840_rt_compute_set_bindless(A840RtComputeEncoder *enc,
                                          uint32_t count,
                                          A840RtBuffer *const *tables,
                                          const uint64_t *offsets)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840BindlessTableDesc emit_desc;
    A840EmitStatus est;
    uint32_t i;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (count > ADRENO_COMPILE_MAX_BIND_SETS || (count > 0u && !tables)) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.count = count;
    for (i = 0; i < count; ++i) {
        if (tables[i] != NULL) {
            uint64_t off = offsets ? offsets[i] : 0u;
            st = check_buffer_range(cb->device, tables[i], off, 64u);
            if (st != A840_RT_OK) {
                return st;
            }
            emit_desc.tables[i].bo_handle = tables[i]->bo_handle;
            emit_desc.tables[i].gpu_va = tables[i]->gpu_va + off;
        }
    }

    st = pre_reserve_buffers(cb, count);
    if (st != A840_RT_OK) {
        return st;
    }

    est = a840_emit_compute_bindless(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    for (i = 0; i < count; ++i) {
        if (tables[i] != NULL) {
            retain_buffer_reserved(cb, tables[i]);
        }
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_compute_set_consts_direct(A840RtComputeEncoder *enc,
                                               uint32_t dst_vec4,
                                               uint32_t count_vec4,
                                               uint32_t constlen_vec4,
                                               const uint32_t *words)
{
    A840RtStatus st;
    A840ConstUploadDesc emit_desc;
    A840EmitStatus est;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.source = A840_CONST_DIRECT;
    emit_desc.dst_vec4 = dst_vec4;
    emit_desc.count_vec4 = count_vec4;
    emit_desc.constlen_vec4 = constlen_vec4;
    emit_desc.words = words;

    est = a840_emit_compute_consts(&enc->cb->cs, &emit_desc);
    return a840_rt_status_from_emit(est);
}

A840RtStatus a840_rt_compute_set_consts_indirect(A840RtComputeEncoder *enc,
                                                 uint32_t dst_vec4,
                                                 uint32_t count_vec4,
                                                 uint32_t constlen_vec4,
                                                 A840RtBuffer *src_buffer,
                                                 uint64_t src_offset)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840ConstUploadDesc emit_desc;
    A840EmitStatus est;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    cb = enc->cb;
    st = check_buffer_range(cb->device, src_buffer, src_offset, (uint64_t)count_vec4 * 16u);
    if (st != A840_RT_OK) {
        return st;
    }
    st = pre_reserve_buffers(cb, 1u);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.source = A840_CONST_INDIRECT;
    emit_desc.dst_vec4 = dst_vec4;
    emit_desc.count_vec4 = count_vec4;
    emit_desc.constlen_vec4 = constlen_vec4;
    emit_desc.address.bo_handle = src_buffer->bo_handle;
    emit_desc.address.gpu_va = src_buffer->gpu_va + src_offset;

    est = a840_emit_compute_consts(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    retain_buffer_reserved(cb, src_buffer);
    return A840_RT_OK;
}

static A840RtStatus compute_dispatch_common(A840RtComputeEncoder *enc,
                                            A840DispatchMode mode,
                                            uint32_t x,
                                            uint32_t y,
                                            uint32_t z)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840ComputeRouteQuery rq;
    A840RouteDecision dec;
    A840DispatchDesc emit_desc;
    A840EmitStatus est;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!enc->has_pipeline) {
        return A840_RT_STATE_ERROR;
    }
    cb = enc->cb;

    memset(&rq, 0, sizeof(rq));
    rq.has_compiled_kernel = true;
    rq.has_valid_local_size = true;
    dec = a840_router_eval_compute(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.stage = &enc->bound_stage;
    emit_desc.mode = mode;
    emit_desc.size[0] = x;
    emit_desc.size[1] = y;
    emit_desc.size[2] = z;

    {
        size_t saved_words = cb->cs.used_dwords;
        size_t saved_relocs = cb->cs.reloc_count;
        est = a840_emit_compute_dispatch(&cb->cs, &emit_desc);
        if (est != A840_EMIT_OK) {
            return a840_rt_status_from_emit(est);
        }
        /* Serial Metal compute encoders order back-to-back dispatches and
         * subsequent passes on the same command buffer. */
        est = a840_emit_wfi(&cb->cs);
        if (est == A840_EMIT_OK) {
            est = a840_emit_cache_flush_inv(&cb->cs, A840_CACHE_UCHE_INV);
        }
        if (est != A840_EMIT_OK) {
            cb->cs.used_dwords = saved_words;
            cb->cs.reloc_count = saved_relocs;
            return a840_rt_status_from_emit(est);
        }
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_compute_dispatch_threadgroups(A840RtComputeEncoder *enc,
                                                   uint32_t groups_x,
                                                   uint32_t groups_y,
                                                   uint32_t groups_z)
{
    return compute_dispatch_common(enc, A840_DISPATCH_UNIFORM, groups_x, groups_y, groups_z);
}

A840RtStatus a840_rt_compute_dispatch_threads(A840RtComputeEncoder *enc,
                                              uint32_t threads_x,
                                              uint32_t threads_y,
                                              uint32_t threads_z)
{
    return compute_dispatch_common(enc, A840_DISPATCH_NON_UNIFORM, threads_x, threads_y, threads_z);
}

A840RtStatus a840_rt_compute_dispatch_indirect(A840RtComputeEncoder *enc,
                                               A840RtBuffer *indirect_buffer,
                                               uint64_t offset)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840ComputeRouteQuery rq;
    A840RouteDecision dec;

    st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    cb = enc->cb;
    st = check_buffer_range(cb->device, indirect_buffer, offset, 3u * sizeof(uint32_t));
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&rq, 0, sizeof(rq));
    rq.has_compiled_kernel = enc->has_pipeline;
    rq.has_valid_local_size = enc->has_pipeline;
    rq.is_indirect_dispatch = true;
    dec = a840_router_eval_compute(&cb->device->router, &rq);
    cb->last_route = dec;
    /* Phase 2 strictly routes indirect compute dispatch to fallback predicate. */
    return A840_RT_FALLBACK_REQUIRED;
}

A840RtStatus a840_rt_compute_barrier(A840RtComputeEncoder *enc,
                                     uint32_t cache_mask,
                                     bool wait_mem_writes,
                                     bool wfi)
{
    A840RtStatus st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    return emit_barrier_tx(&enc->cb->cs, cache_mask, wait_mem_writes, wfi);
}

A840RtStatus a840_rt_compute_end(A840RtComputeEncoder *enc)
{
    A840RtStatus st = check_compute_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    enc->ended = true;
    enc->cb->active_encoder = A840_ENCODER_NONE;
    enc->cb->active_encoder_ptr = NULL;
    return A840_RT_OK;
}

void a840_rt_compute_destroy(A840RtComputeEncoder *enc)
{
    if (!enc) {
        return;
    }
    if (!enc->ended && enc->cb && enc->cb->active_encoder_ptr == enc) {
        enc->cb->active_encoder = A840_ENCODER_NONE;
        enc->cb->active_encoder_ptr = NULL;
    }
    free(enc);
}

/* ---------- Basic Sysmem 3D Render Encoder ---------- */

static A840RtStatus emit_sysmem_render_begin(A840CmdStream *cs,
                                             uint32_t fb_w,
                                             uint32_t fb_h)
{
    /* Match Mesa tu6_sysmem_render_begin<A8XX> (tu_cmd_buffer.cc:3281-3320):
     * Direct-to-sysmem rendering requires window scissor, zero window offsets,
     * A8XX bin size = 0 / FORCE_LRZ_WRITE_DIS, RB_BUFFER_CNTL = all sysmem,
     * RB_CLEAR_TARGET = SYSMEM, CP_SET_MARKER(DIRECT_RENDER),
     * CP_SKIP_IB2_ENABLE_GLOBAL(0), CP_SET_VISIBILITY_OVERRIDE(1), CP_SET_MODE(0).
     */
    const uint32_t w = fb_w ? fb_w : 1u;
    const uint32_t h = fb_h ? fb_h : 1u;
    const uint32_t br = ((w - 1u) & 0x3fffu) | (((h - 1u) & 0x3fffu) << 16);
    const uint32_t scissor[2] = { 0u, br };
    const uint32_t zero = 0u;
    const uint32_t zero8[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    const uint32_t bin_cntl = UINT32_C(1) << 31; /* RENDERING_PASS | FORCE_LRZ_WRITE_DIS */
    const uint32_t sysmem_mask = UINT32_C(0x3ff); /* Z|S|RT0..RT7 in sysmem */
    const uint32_t direct_mode = A840_MODE_DIRECT_RENDER;
    const uint32_t vis_override = 1u;
    A840EmitStatus est;

    est = a840_cs_emit_pkt4(cs, A840_REG_GRAS_SC_WINDOW_SCISSOR_TL, scissor, 2u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_GRAS_A2D_SCISSOR_TL, scissor, 2u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);

    est = a840_cs_emit_pkt4(cs, A840_REG_RB_WINDOW_OFFSET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_RESOLVE_WINDOW_OFFSET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_SP_WINDOW_OFFSET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_TPL1_WINDOW_OFFSET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_TPL1_A2D_WINDOW_OFFSET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);

    est = a840_cs_emit_pkt4(cs, A840_REG_GRAS_SC_BIN_CNTL, &bin_cntl, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_CNTL, &bin_cntl, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_TPL1_BIN_SIZE, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_TPL1_A2D_BIN_SIZE, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_SP_BIN_SIZE, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_MRT_GMEM_DIMENSION_0_REG, zero8, 8u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_DEPTH_GMEM_DIMENSION, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);

    est = a840_cs_emit_pkt4(cs, A840_REG_RB_BUFFER_CNTL, &sysmem_mask, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_CLEAR_TARGET, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);

    est = a840_cs_emit_pkt7(cs, A840_CP_SET_MARKER, &direct_mode, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt7(cs, A840_CP_SKIP_IB2_ENABLE_GLOBAL, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt7(cs, A840_CP_SET_VISIBILITY_OVERRIDE, &vis_override, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt7(cs, A840_CP_SET_MODE, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_GRAS_BIN_FOVEAT, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    est = a840_cs_emit_pkt4(cs, A840_REG_RB_BIN_FOVEAT, &zero, 1u);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    return A840_RT_OK;
}

static A840RtStatus check_render_active(const A840RtRenderEncoder *enc)
{
    if (!enc || !enc->cb) {
        return A840_RT_INVALID_ARG;
    }
    if (enc->ended || enc->cb->state != A840_CB_RECORDING ||
        enc->cb->active_encoder != A840_ENCODER_RENDER_3D ||
        enc->cb->active_encoder_ptr != enc) {
        return A840_RT_STATE_ERROR;
    }
    return A840_RT_OK;
}

static uint32_t depth_format_from_mtl(uint32_t mtl_pixel_format)
{
    switch (mtl_pixel_format) {
    case 250u: /* Depth16Unorm */
        return A840_DEPTH_16;
    case 252u: /* Depth32Float */
    case 260u: /* Depth32Float_Stencil8 (separate depth plane) */
        return A840_DEPTH_32;
    default:
        return A840_DEPTH_NONE;
    }
}

A840RtStatus a840_rt_cb_begin_render_pass(A840RtCmdBuffer *cb,
                                          const A840RtRenderPassDesc *pass,
                                          A840RtRenderEncoder **out_enc)
{
    A840RenderRouteQuery rq;
    A840RouteDecision dec;
    A840RtRenderEncoder *enc;
    A840RtStatus st;
    size_t saved_words;
    size_t saved_relocs;
    bool did_clear = false;
    uint32_t i;

    if (!out_enc) {
        return A840_RT_INVALID_ARG;
    }
    *out_enc = NULL;
    if (!cb || !pass) {
        return A840_RT_INVALID_ARG;
    }
    if (cb->state != A840_CB_RECORDING || cb->active_encoder != A840_ENCODER_NONE) {
        return A840_RT_STATE_ERROR;
    }
    if (pass->color_count > A840_MAX_COLOR_TARGETS) {
        return A840_RT_RANGE_ERROR;
    }

    memset(&rq, 0, sizeof(rq));
    rq.color_attachment_count = pass->color_count;
    rq.sample_count = pass->sample_count ? pass->sample_count : 1u;
    rq.has_memoryless_attachment = pass->has_memoryless_attachment;
    rq.has_msaa_resolve = pass->has_msaa_resolve;
    rq.uses_fbfetch = pass->uses_fbfetch;
    rq.uses_programmable_blend = pass->uses_programmable_blend;
    rq.uses_tessellation = pass->uses_tessellation;
    rq.uses_vertex_amplification = pass->uses_vertex_amplification;
    rq.uses_indirect_draw = pass->uses_indirect_draw;
    rq.uses_icb = pass->uses_icb;
    rq.has_ycbcr_attachment = pass->has_ycbcr_attachment;

    for (i = 0; i < pass->color_count; ++i) {
        A840RtTexture *ct = pass->color_targets[i];
        if (ct != NULL) {
            st = check_texture_owner(cb->device, ct);
            if (st != A840_RT_OK) {
                return st;
            }
            rq.color_mtl_formats[i] = ct->mtl_pixel_format;
            rq.color_tile_modes[i] = ct->tile_mode;
            if (ct->samples > rq.sample_count) {
                rq.sample_count = ct->samples;
            }
        }
    }
    if (pass->depth_target != NULL) {
        st = check_texture_owner(cb->device, pass->depth_target);
        if (st != A840_RT_OK) {
            return st;
        }
        rq.depth_mtl_format = pass->depth_target->mtl_pixel_format;
        rq.depth_tile_mode = pass->depth_target->tile_mode;
        if (pass->depth_target->samples > rq.sample_count) {
            rq.sample_count = pass->depth_target->samples;
        }
    }
    if (pass->stencil_target != NULL) {
        st = check_texture_owner(cb->device, pass->stencil_target);
        if (st != A840_RT_OK) {
            return st;
        }
        if ((pass->stencil_target->format_caps->flags & A840_FORMAT_STENCIL) == 0u ||
            pass->stencil_target->tile_mode != A840_TILE_TILED) {
            return A840_RT_UNSUPPORTED;
        }
    }

    dec = a840_router_eval_render(&cb->device->router, &rq);
    cb->last_route = dec;
    if (dec.path != A840_ROUTE_PATH_RAW) {
        return A840_RT_FALLBACK_REQUIRED;
    }

    st = pre_reserve_textures(cb, (size_t)pass->color_count + 2u);
    if (st != A840_RT_OK) {
        return st;
    }

    enc = (A840RtRenderEncoder *)calloc(1u, sizeof(*enc));
    if (!enc) {
        return A840_RT_OOM;
    }

    /* Transactionally emit optional sysmem 2D color clears on pass entry. */
    saved_words = cb->cs.used_dwords;
    saved_relocs = cb->cs.reloc_count;
    for (i = 0; i < pass->color_count; ++i) {
        A840RtTexture *ct = pass->color_targets[i];
        if (ct != NULL && pass->color_clear[i]) {
            A840Clear2DDesc cdesc;
            A840EmitStatus est;
            memset(&cdesc, 0, sizeof(cdesc));
            a840_rt_texture_as_surface2d(ct, &cdesc.dst);
            cdesc.rect.x = 0u;
            cdesc.rect.y = 0u;
            cdesc.rect.width = ct->width;
            cdesc.rect.height = ct->height;
            memcpy(cdesc.color, pass->color_clear_values[i], sizeof(cdesc.color));
            est = a840_emit_2d_clear(&cb->cs, &cdesc);
            if (est != A840_EMIT_OK) {
                cb->cs.used_dwords = saved_words;
                cb->cs.reloc_count = saved_relocs;
                free(enc);
                return a840_rt_status_from_emit(est);
            }
            did_clear = true;
        }
    }
    if (did_clear) {
        st = emit_barrier_tx(&cb->cs,
                             A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_CCU_INV_COLOR | A840_CACHE_UCHE_INV,
                             true, true);
        if (st != A840_RT_OK) {
            cb->cs.used_dwords = saved_words;
            cb->cs.reloc_count = saved_relocs;
            free(enc);
            return st;
        }
    }

    {
        uint32_t fb_w = 0u, fb_h = 0u;
        for (i = 0; i < pass->color_count; ++i) {
            if (pass->color_targets[i] != NULL) {
                fb_w = pass->color_targets[i]->width;
                fb_h = pass->color_targets[i]->height;
                break;
            }
        }
        if (fb_w == 0u && pass->depth_target != NULL) {
            fb_w = pass->depth_target->width;
            fb_h = pass->depth_target->height;
        }
        if (fb_w == 0u && pass->stencil_target != NULL) {
            fb_w = pass->stencil_target->width;
            fb_h = pass->stencil_target->height;
        }
        st = emit_sysmem_render_begin(&cb->cs, fb_w, fb_h);
        if (st != A840_RT_OK) {
            cb->cs.used_dwords = saved_words;
            cb->cs.reloc_count = saved_relocs;
            free(enc);
            return st;
        }
    }

    enc->cb = cb;
    enc->color_count = pass->color_count;
    for (i = 0; i < pass->color_count; ++i) {
        enc->color_targets[i] = pass->color_targets[i];
        if (pass->color_targets[i] != NULL) {
            retain_texture_reserved(cb, pass->color_targets[i]);
        }
    }
    enc->depth_target = pass->depth_target;
    if (pass->depth_target != NULL) {
        retain_texture_reserved(cb, pass->depth_target);
    }
    enc->stencil_target = pass->stencil_target;
    if (pass->stencil_target != NULL) {
        retain_texture_reserved(cb, pass->stencil_target);
    }

    cb->active_encoder = A840_ENCODER_RENDER_3D;
    cb->active_encoder_ptr = enc;

    *out_enc = enc;
    return A840_RT_OK;
}

A840RtStatus a840_rt_render_set_program(A840RtRenderEncoder *enc,
                                        const A840RtGraphicsProgramDesc *desc)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840GraphicsProgramDesc emit_desc;
    A840EmitStatus est;
    uint64_t vs_bytes;
    uint64_t fs_bytes;

    st = check_render_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!desc || !desc->emit_desc.vs.stage || !desc->emit_desc.fs.stage) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    vs_bytes = (uint64_t)desc->emit_desc.vs.stage->isa_dwords * sizeof(uint32_t);
    fs_bytes = (uint64_t)desc->emit_desc.fs.stage->isa_dwords * sizeof(uint32_t);
    if (!vs_bytes || !fs_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    st = check_buffer_range(cb->device, desc->vs_code_buffer, desc->vs_code_offset, vs_bytes);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_buffer_range(cb->device, desc->fs_code_buffer, desc->fs_code_offset, fs_bytes);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_private_buffer(cb->device, desc->emit_desc.vs.stage->private_mem_bytes,
                              desc->vs_private_buffer, desc->vs_private_offset);
    if (st != A840_RT_OK) {
        return st;
    }
    st = check_private_buffer(cb->device, desc->emit_desc.fs.stage->private_mem_bytes,
                              desc->fs_private_buffer, desc->fs_private_offset);
    if (st != A840_RT_OK) {
        return st;
    }

    st = pre_reserve_buffers(cb, 4u);
    if (st != A840_RT_OK) {
        return st;
    }

    emit_desc = desc->emit_desc;
    emit_desc.vs.code.bo_handle = desc->vs_code_buffer->bo_handle;
    emit_desc.vs.code.gpu_va = desc->vs_code_buffer->gpu_va + desc->vs_code_offset;
    if (desc->vs_private_buffer != NULL) {
        emit_desc.vs.private_memory.bo_handle = desc->vs_private_buffer->bo_handle;
        emit_desc.vs.private_memory.gpu_va = desc->vs_private_buffer->gpu_va + desc->vs_private_offset;
    } else {
        emit_desc.vs.private_memory.bo_handle = 0u;
        emit_desc.vs.private_memory.gpu_va = 0u;
    }

    emit_desc.fs.code.bo_handle = desc->fs_code_buffer->bo_handle;
    emit_desc.fs.code.gpu_va = desc->fs_code_buffer->gpu_va + desc->fs_code_offset;
    if (desc->fs_private_buffer != NULL) {
        emit_desc.fs.private_memory.bo_handle = desc->fs_private_buffer->bo_handle;
        emit_desc.fs.private_memory.gpu_va = desc->fs_private_buffer->gpu_va + desc->fs_private_offset;
    } else {
        emit_desc.fs.private_memory.bo_handle = 0u;
        emit_desc.fs.private_memory.gpu_va = 0u;
    }

    est = a840_emit_3d_program(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    retain_buffer_reserved(cb, desc->vs_code_buffer);
    retain_buffer_reserved(cb, desc->fs_code_buffer);
    if (desc->vs_private_buffer != NULL) {
        retain_buffer_reserved(cb, desc->vs_private_buffer);
    }
    if (desc->fs_private_buffer != NULL) {
        retain_buffer_reserved(cb, desc->fs_private_buffer);
    }
    enc->has_program = true;
    return A840_RT_OK;
}

A840RtStatus a840_rt_render_set_state(A840RtRenderEncoder *enc,
                                      const A840RtRenderStateDesc *desc)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840RenderStateDesc emit_desc;
    A840EmitStatus est;
    uint32_t i;

    st = check_render_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!desc) {
        return A840_RT_INVALID_ARG;
    }
    cb = enc->cb;

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.viewport_count = desc->viewport_count;
    emit_desc.scissor_count = desc->scissor_count;
    memcpy(emit_desc.viewports, desc->viewports, sizeof(emit_desc.viewports));
    memcpy(emit_desc.scissors, desc->scissors, sizeof(emit_desc.scissors));
    emit_desc.line_width = desc->line_width;
    emit_desc.cull_front = desc->cull_front;
    emit_desc.cull_back = desc->cull_back;
    emit_desc.front_clockwise = desc->front_clockwise;
    emit_desc.depth_test = desc->depth_test;
    emit_desc.depth_write = desc->depth_write;
    emit_desc.depth_clamp = desc->depth_clamp;
    emit_desc.stencil_test = desc->stencil_test;
    emit_desc.depth_compare = desc->depth_compare;
    emit_desc.front = desc->front;
    emit_desc.back = desc->back;
    emit_desc.sample_mask = desc->sample_mask;
    memcpy(emit_desc.blend_color, desc->blend_color, sizeof(emit_desc.blend_color));

    for (i = 0; i < enc->color_count; ++i) {
        const A840RtTexture *ct = enc->color_targets[i];
        if (ct != NULL) {
            a840_rt_texture_as_attachment(ct, &emit_desc.colors[i].storage);
            emit_desc.colors[i].format = ct->color_format_id;
            emit_desc.colors[i].swap = ct->color_swap;
            emit_desc.colors[i].srgb = ct->srgb;
            emit_desc.colors[i].blend = desc->color_blends[i];
        }
    }

    if (enc->depth_target != NULL) {
        uint32_t dfmt = depth_format_from_mtl(enc->depth_target->mtl_pixel_format);
        if (dfmt == A840_DEPTH_NONE) {
            return A840_RT_UNSUPPORTED;
        }
        emit_desc.depth_format = dfmt;
        a840_rt_texture_as_attachment(enc->depth_target, &emit_desc.depth);
    } else {
        emit_desc.depth_format = A840_DEPTH_NONE;
    }

    if (enc->stencil_target != NULL) {
        a840_rt_texture_as_attachment(enc->stencil_target, &emit_desc.stencil);
    }

    est = a840_emit_3d_render_state(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    enc->has_state = true;
    return A840_RT_OK;
}

A840RtStatus a840_rt_render_set_sds(A840RtRenderEncoder *enc,
                                    const A840RtSdsGroupDesc *groups,
                                    size_t count)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840SdsGroupDesc emit_groups[ADRENO840_MAX_SDS_SLOTS];
    A840EmitStatus est;
    size_t i;

    st = check_render_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (count > ADRENO840_MAX_SDS_SLOTS || (count > 0u && !groups)) {
        return A840_RT_RANGE_ERROR;
    }
    cb = enc->cb;

    memset(emit_groups, 0, sizeof(emit_groups));
    for (i = 0; i < count; ++i) {
        emit_groups[i].group_id = groups[i].group_id;
        emit_groups[i].enable_mask = groups[i].enable_mask;
        emit_groups[i].state_dwords = groups[i].state_dwords;
        if (groups[i].state_dwords > 0u) {
            st = check_buffer_range(cb->device, groups[i].state_buffer, groups[i].state_offset,
                                    (uint64_t)groups[i].state_dwords * sizeof(uint32_t));
            if (st != A840_RT_OK) {
                return st;
            }
            emit_groups[i].state.bo_handle = groups[i].state_buffer->bo_handle;
            emit_groups[i].state.gpu_va = groups[i].state_buffer->gpu_va + groups[i].state_offset;
        }
    }

    st = pre_reserve_buffers(cb, count);
    if (st != A840_RT_OK) {
        return st;
    }

    est = a840_emit_sds_group(&cb->cs, emit_groups, count);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    for (i = 0; i < count; ++i) {
        if (groups[i].state_dwords > 0u && groups[i].state_buffer != NULL) {
            retain_buffer_reserved(cb, groups[i].state_buffer);
        }
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_render_draw(A840RtRenderEncoder *enc,
                                 const A840RtDrawDesc *desc)
{
    A840RtCmdBuffer *cb;
    A840RtStatus st;
    A840DrawDesc emit_desc;
    A840EmitStatus est;

    st = check_render_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }
    if (!desc) {
        return A840_RT_INVALID_ARG;
    }
    if (!enc->has_program || !enc->has_state) {
        return A840_RT_STATE_ERROR;
    }
    cb = enc->cb;

    memset(&emit_desc, 0, sizeof(emit_desc));
    emit_desc.primitive = desc->primitive;
    emit_desc.index_type = desc->index_type;
    emit_desc.use_visibility = desc->use_visibility;
    emit_desc.count = desc->count;
    emit_desc.instance_count = desc->instance_count;
    emit_desc.first_instance = desc->first_instance;
    emit_desc.first_vertex = desc->first_vertex;
    emit_desc.base_vertex = desc->base_vertex;
    emit_desc.first_index = desc->first_index;
    emit_desc.index_capacity = desc->index_capacity;

    if (desc->index_type != A840_INDEX_NONE) {
        uint64_t idx_bytes = (uint64_t)desc->index_capacity * (uint64_t)desc->index_type;
        st = check_buffer_range(cb->device, desc->index_buffer, desc->index_offset, idx_bytes);
        if (st != A840_RT_OK) {
            return st;
        }
        st = pre_reserve_buffers(cb, 1u);
        if (st != A840_RT_OK) {
            return st;
        }
        emit_desc.index_buffer.bo_handle = desc->index_buffer->bo_handle;
        emit_desc.index_buffer.gpu_va = desc->index_buffer->gpu_va + desc->index_offset;
    }

    est = a840_emit_3d_draw(&cb->cs, &emit_desc);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }
    if (desc->index_type != A840_INDEX_NONE) {
        retain_buffer_reserved(cb, desc->index_buffer);
    }
    return A840_RT_OK;
}

A840RtStatus a840_rt_render_end_pass(A840RtRenderEncoder *enc)
{
    A840RtStatus st;
    uint32_t flush_mask = 0u;
    uint32_t i;

    st = check_render_active(enc);
    if (st != A840_RT_OK) {
        return st;
    }

    for (i = 0; i < enc->color_count; ++i) {
        if (enc->color_targets[i] != NULL) {
            flush_mask |= A840_CACHE_CCU_FLUSH_COLOR;
            break;
        }
    }
    if (enc->depth_target != NULL || enc->stencil_target != NULL) {
        flush_mask |= A840_CACHE_CCU_FLUSH_DEPTH;
    }

    st = emit_barrier_tx(&enc->cb->cs, flush_mask, true, false);
    if (st != A840_RT_OK) {
        return st;
    }

    enc->ended = true;
    enc->cb->active_encoder = A840_ENCODER_NONE;
    enc->cb->active_encoder_ptr = NULL;
    return A840_RT_OK;
}

void a840_rt_render_destroy(A840RtRenderEncoder *enc)
{
    if (!enc) {
        return;
    }
    if (!enc->ended && enc->cb && enc->cb->active_encoder_ptr == enc) {
        enc->cb->active_encoder = A840_ENCODER_NONE;
        enc->cb->active_encoder_ptr = NULL;
    }
    free(enc);
}
