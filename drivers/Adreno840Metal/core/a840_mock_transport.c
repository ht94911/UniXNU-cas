/* SPDX-License-Identifier: MIT */
#include "a840_backend_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip_facts_a840.h"

enum {
    A840_MOCK_VA_BITS = 48u,
    A840_MOCK_OUTPUT_BITS = 40u,
    A840_MOCK_MIN_ALIGN_LOG2 = 12u,
    A840_MOCK_MAX_ALIGN_LOG2 = 24u,
    A840_MOCK_TICKS_PER_SUBMIT = 1000u,
    A840_MOCK_MAX_HOST_BACKING_BYTES = 256u * 1024u * 1024u
};

typedef struct MockBo {
    uint64_t handle;
    uint64_t bytes;
    uint32_t align_log2;
    uint32_t contiguous;
    bool imported;
    bool kernel_owned;
    bool freed;

    void *backing_mem;

    bool gpu_mapped;
    uint64_t gpu_va;
    uint32_t permissions;
    uint32_t writeback;
    uint32_t shareability;

    bool cpu_mapped;
    uint32_t cpu_token;

    struct NativeAdrenoAbiSurfaceMetaV3 surface_meta;
} MockBo;

typedef struct MockQueue {
    uint64_t id;
    uint32_t priority;
    bool destroyed;
    uint64_t last_submitted_fence;
    uint64_t completed_fence;
} MockQueue;

typedef struct MockSubmission {
    uint64_t queue;
    uint64_t fence;
    uint32_t ib_count;
    uint32_t reloc_count;
    size_t word_offset;
    size_t dword_count;
} MockSubmission;

struct A840MockTransport {
    A840TransportCaps caps;
    bool auto_complete_submits;

    MockBo *bos;
    size_t bo_count;
    size_t bo_cap;
    size_t live_bo_count;
    uint64_t next_bo_handle;
    uint32_t next_cpu_token;

    uint64_t zero_page_handle;
    uint64_t zero_page_va;

    MockQueue *queues;
    size_t queue_count;
    size_t queue_cap;
    size_t live_queue_count;
    uint64_t next_queue_id;

    uint64_t next_fence;
    uint64_t ticks;

    MockSubmission *submissions;
    size_t submission_count;
    size_t submission_cap;

    uint32_t *captured_words;
    size_t captured_count;
    size_t captured_cap;
};

static uint64_t va_limit_48(void)
{
    return UINT64_C(1) << A840_MOCK_VA_BITS;
}

static bool range_overlaps(uint64_t a_start, uint64_t a_len,
                           uint64_t b_start, uint64_t b_len)
{
    if (!a_len || !b_len) {
        return false;
    }
    if (a_start <= b_start) {
        return (b_start - a_start) < a_len;
    }
    return (a_start - b_start) < b_len;
}

static MockBo *find_bo(A840MockTransport *mock, uint64_t handle)
{
    size_t i;
    if (!mock || !handle) {
        return NULL;
    }
    for (i = 0; i < mock->bo_count; ++i) {
        if (mock->bos[i].handle == handle) {
            return &mock->bos[i];
        }
    }
    return NULL;
}

static const MockBo *find_bo_const(const A840MockTransport *mock, uint64_t handle)
{
    size_t i;
    if (!mock || !handle) {
        return NULL;
    }
    for (i = 0; i < mock->bo_count; ++i) {
        if (mock->bos[i].handle == handle) {
            return &mock->bos[i];
        }
    }
    return NULL;
}

static MockQueue *find_queue(A840MockTransport *mock, uint64_t queue_id)
{
    size_t i;
    if (!mock || !queue_id) {
        return NULL;
    }
    for (i = 0; i < mock->queue_count; ++i) {
        if (mock->queues[i].id == queue_id) {
            return &mock->queues[i];
        }
    }
    return NULL;
}

static A840TransportStatus ensure_bo_slot(A840MockTransport *mock, MockBo **out_slot)
{
    if (mock->bo_count == mock->bo_cap) {
        size_t new_cap = mock->bo_cap ? mock->bo_cap * 2u : 32u;
        MockBo *grown;
        if (new_cap < mock->bo_cap || new_cap > SIZE_MAX / sizeof(MockBo)) {
            return A840_TRANSPORT_OOM;
        }
        grown = (MockBo *)realloc(mock->bos, new_cap * sizeof(MockBo));
        if (!grown) {
            return A840_TRANSPORT_OOM;
        }
        mock->bos = grown;
        mock->bo_cap = new_cap;
    }
    *out_slot = &mock->bos[mock->bo_count];
    memset(*out_slot, 0, sizeof(MockBo));
    return A840_TRANSPORT_OK;
}

static A840TransportStatus ensure_queue_slot(A840MockTransport *mock, MockQueue **out_slot)
{
    if (mock->queue_count == mock->queue_cap) {
        size_t new_cap = mock->queue_cap ? mock->queue_cap * 2u : 8u;
        MockQueue *grown;
        if (new_cap < mock->queue_cap || new_cap > SIZE_MAX / sizeof(MockQueue)) {
            return A840_TRANSPORT_OOM;
        }
        grown = (MockQueue *)realloc(mock->queues, new_cap * sizeof(MockQueue));
        if (!grown) {
            return A840_TRANSPORT_OOM;
        }
        mock->queues = grown;
        mock->queue_cap = new_cap;
    }
    *out_slot = &mock->queues[mock->queue_count];
    memset(*out_slot, 0, sizeof(MockQueue));
    return A840_TRANSPORT_OK;
}

static A840TransportStatus ensure_submission_space(A840MockTransport *mock,
                                                   size_t extra_dwords)
{
    if (mock->submission_count == mock->submission_cap) {
        size_t new_cap = mock->submission_cap ? mock->submission_cap * 2u : 16u;
        MockSubmission *grown;
        if (new_cap < mock->submission_cap || new_cap > SIZE_MAX / sizeof(MockSubmission)) {
            return A840_TRANSPORT_OOM;
        }
        grown = (MockSubmission *)realloc(mock->submissions, new_cap * sizeof(MockSubmission));
        if (!grown) {
            return A840_TRANSPORT_OOM;
        }
        mock->submissions = grown;
        mock->submission_cap = new_cap;
    }
    if (extra_dwords > SIZE_MAX - mock->captured_count) {
        return A840_TRANSPORT_OOM;
    }
    if (mock->captured_count + extra_dwords > mock->captured_cap) {
        size_t need = mock->captured_count + extra_dwords;
        size_t new_cap = mock->captured_cap ? mock->captured_cap : 1024u;
        uint32_t *grown;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2u) {
                new_cap = need;
                break;
            }
            new_cap *= 2u;
        }
        if (new_cap > SIZE_MAX / sizeof(uint32_t)) {
            return A840_TRANSPORT_OOM;
        }
        grown = (uint32_t *)realloc(mock->captured_words, new_cap * sizeof(uint32_t));
        if (!grown) {
            return A840_TRANSPORT_OOM;
        }
        mock->captured_words = grown;
        mock->captured_cap = new_cap;
    }
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_query_caps(void *ctx, A840TransportCaps *out_caps)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    if (!mock || !out_caps) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    *out_caps = mock->caps;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_bo_alloc(void *ctx, const A840BoAllocDesc *desc,
                                         uint64_t *out_handle)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;
    void *mem;
    A840TransportStatus st;

    if (!mock || !desc || !out_handle) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (!desc->bytes || (desc->bytes % A840_TRANSPORT_PAGE_BYTES) != 0 ||
        desc->bytes > mock->caps.max_bo_bytes ||
        desc->align_log2 < A840_MOCK_MIN_ALIGN_LOG2 ||
        desc->align_log2 > A840_MOCK_MAX_ALIGN_LOG2 ||
        desc->contiguous > 1u) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (desc->bytes > (uint64_t)A840_MOCK_MAX_HOST_BACKING_BYTES) {
        return A840_TRANSPORT_OOM;
    }

    st = ensure_bo_slot(mock, &bo);
    if (st != A840_TRANSPORT_OK) {
        return st;
    }
    mem = calloc(1u, (size_t)desc->bytes);
    if (!mem) {
        return A840_TRANSPORT_OOM;
    }

    bo->handle = ++mock->next_bo_handle;
    bo->bytes = desc->bytes;
    bo->align_log2 = desc->align_log2;
    bo->contiguous = desc->contiguous;
    bo->backing_mem = mem;
    bo->surface_meta.bo_handle = bo->handle;
    mock->bo_count++;
    mock->live_bo_count++;

    *out_handle = bo->handle;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_bo_import(void *ctx, const A840BoImportDesc *desc,
                                          uint64_t *out_handle)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;
    A840TransportStatus st;

    if (!mock || !desc || !out_handle) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if ((mock->caps.abi_flags & NativeAdrenoParamsV2HostImport) == 0u) {
        return A840_TRANSPORT_UNSUPPORTED;
    }
    if (!desc->host_ptr || ((uintptr_t)desc->host_ptr % A840_TRANSPORT_PAGE_BYTES) != 0 ||
        !desc->bytes || (desc->bytes % A840_TRANSPORT_PAGE_BYTES) != 0 ||
        desc->bytes > mock->caps.max_bo_bytes) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    st = ensure_bo_slot(mock, &bo);
    if (st != A840_TRANSPORT_OK) {
        return st;
    }
    bo->handle = ++mock->next_bo_handle;
    bo->bytes = desc->bytes;
    bo->align_log2 = A840_MOCK_MIN_ALIGN_LOG2;
    bo->imported = true;
    bo->backing_mem = desc->host_ptr;
    bo->surface_meta.bo_handle = bo->handle;
    mock->bo_count++;
    mock->live_bo_count++;

    *out_handle = bo->handle;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_bo_map(void *ctx, const A840BoMapDesc *desc,
                                       A840BoMapReply *out_reply)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;
    bool want_gpu;
    bool want_cpu;
    size_t i;

    if (!mock || !desc || !out_reply) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (!desc->flags || (desc->flags & ~(A840_BO_MAP_GPU | A840_BO_MAP_CPU)) != 0u) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    bo = find_bo(mock, desc->handle);
    if (!bo) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (bo->freed) {
        return A840_TRANSPORT_DANGLING_BO;
    }
    if (bo->kernel_owned) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    want_gpu = (desc->flags & A840_BO_MAP_GPU) != 0u;
    want_cpu = (desc->flags & A840_BO_MAP_CPU) != 0u;

    /* Validate all requested operations before mutating BO state. */
    if (want_gpu) {
        uint64_t aperture_end = mock->caps.va_start + mock->caps.va_size;
        uint64_t align_bytes = UINT64_C(1) << bo->align_log2;
        if (bo->gpu_mapped) {
            return A840_TRANSPORT_STATE_ERROR;
        }
        if ((desc->permissions & A840_BO_PERM_READ) == 0u ||
            (desc->permissions & ~(A840_BO_PERM_READ | A840_BO_PERM_WRITE)) != 0u ||
            desc->writeback > 1u ||
            (desc->shareability != 0u && desc->shareability != A840_BO_SHARE_OUTER &&
             desc->shareability != A840_BO_SHARE_INNER)) {
            return A840_TRANSPORT_INVALID_ARG;
        }
        if (!desc->gpu_va || (desc->gpu_va % A840_TRANSPORT_PAGE_BYTES) != 0u ||
            (desc->gpu_va % align_bytes) != 0u) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if (desc->gpu_va < mock->caps.va_start ||
            desc->gpu_va >= va_limit_48() ||
            bo->bytes > va_limit_48() - desc->gpu_va ||
            desc->gpu_va + bo->bytes > aperture_end) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        for (i = 0; i < mock->bo_count; ++i) {
            const MockBo *other = &mock->bos[i];
            if (!other->freed && other->gpu_mapped &&
                range_overlaps(desc->gpu_va, bo->bytes, other->gpu_va, other->bytes)) {
                return A840_TRANSPORT_STATE_ERROR;
            }
        }
    }

    if (want_cpu) {
        if (bo->imported) {
            return A840_TRANSPORT_UNSUPPORTED;
        }
        if (bo->cpu_mapped) {
            return A840_TRANSPORT_STATE_ERROR;
        }
    }

    memset(out_reply, 0, sizeof(*out_reply));
    if (want_gpu) {
        bo->gpu_mapped = true;
        bo->gpu_va = desc->gpu_va;
        bo->permissions = desc->permissions;
        bo->writeback = desc->writeback;
        bo->shareability = desc->shareability;
    }
    if (want_cpu) {
        bo->cpu_mapped = true;
        bo->cpu_token = ++mock->next_cpu_token;
    }

    out_reply->gpu_va = bo->gpu_mapped ? bo->gpu_va : 0u;
    out_reply->cpu_ptr = bo->cpu_mapped ? bo->backing_mem : NULL;
    out_reply->cpu_token = bo->cpu_mapped ? bo->cpu_token : 0u;
    out_reply->bytes = bo->bytes;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_bo_unmap(void *ctx, const A840BoUnmapDesc *desc)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;
    bool unmap_gpu;
    bool unmap_cpu;

    if (!mock || !desc) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (!desc->flags || (desc->flags & ~(A840_BO_UNMAP_GPU | A840_BO_UNMAP_CPU)) != 0u) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    bo = find_bo(mock, desc->handle);
    if (!bo) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (bo->freed) {
        return A840_TRANSPORT_DANGLING_BO;
    }
    if (bo->kernel_owned) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    unmap_gpu = (desc->flags & A840_BO_UNMAP_GPU) != 0u;
    unmap_cpu = (desc->flags & A840_BO_UNMAP_CPU) != 0u;

    if (unmap_cpu && (!bo->cpu_mapped || desc->cpu_token != bo->cpu_token)) {
        return A840_TRANSPORT_STATE_ERROR;
    }
    if (unmap_gpu && !bo->gpu_mapped) {
        return A840_TRANSPORT_STATE_ERROR;
    }

    if (unmap_cpu) {
        bo->cpu_mapped = false;
        bo->cpu_token = 0u;
    }
    if (unmap_gpu) {
        bo->gpu_mapped = false;
        bo->gpu_va = 0u;
        bo->permissions = 0u;
    }
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_bo_free(void *ctx, uint64_t bo_handle)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;

    if (!mock || !bo_handle) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    bo = find_bo(mock, bo_handle);
    if (!bo) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (bo->freed) {
        return A840_TRANSPORT_DANGLING_BO;
    }
    if (bo->kernel_owned) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (bo->cpu_mapped) {
        return A840_TRANSPORT_BUSY;
    }

    bo->gpu_mapped = false;
    bo->gpu_va = 0u;
    if (!bo->imported && bo->backing_mem) {
        free(bo->backing_mem);
    }
    bo->backing_mem = NULL;
    bo->freed = true;
    mock->live_bo_count--;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_queue_create(void *ctx, uint32_t priority,
                                             uint64_t *out_queue)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockQueue *q;
    A840TransportStatus st;

    if (!mock || !out_queue) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (priority > A840_QUEUE_PRIORITY_REALTIME ||
        priority == A840_QUEUE_PRIORITY_DISABLED) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    st = ensure_queue_slot(mock, &q);
    if (st != A840_TRANSPORT_OK) {
        return st;
    }
    q->id = ++mock->next_queue_id;
    q->priority = priority;
    mock->queue_count++;
    mock->live_queue_count++;

    *out_queue = q->id;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_queue_destroy(void *ctx, uint64_t queue)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockQueue *q;

    if (!mock || !queue) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    q = find_queue(mock, queue);
    if (!q || q->destroyed) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    q->destroyed = true;
    mock->live_queue_count--;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_queue_submit(void *ctx, const A840SubmitDesc *desc,
                                             A840SubmitReply *out_reply)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockQueue *q;
    size_t total_dwords = 0u;
    size_t start_offset;
    uint32_t i;
    A840TransportStatus st;

    if (!mock || !desc || !out_reply) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    q = find_queue(mock, desc->queue);
    if (!q || q->destroyed) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (!desc->ibs || desc->ib_count == 0u ||
        desc->ib_count > A840_TRANSPORT_MAX_SUBMIT_IBS) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (desc->reloc_count > 0u && !desc->relocs) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (desc->wait_fence > mock->next_fence) {
        return A840_TRANSPORT_NOT_FOUND;
    }

    /* Phase 1: Validate every IB BO, mapping, and exact byte range. */
    for (i = 0; i < desc->ib_count; ++i) {
        const A840SubmitIb *ib = &desc->ibs[i];
        const MockBo *bo;
        uint64_t rel_off;
        size_t ib_dwords;

        if (!ib->bytes || (ib->bytes % sizeof(uint32_t)) != 0u) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if (!ib->bo_handle) {
            return A840_TRANSPORT_DANGLING_BO;
        }
        bo = find_bo_const(mock, ib->bo_handle);
        if (!bo) {
            return A840_TRANSPORT_NOT_FOUND;
        }
        if (bo->freed) {
            return A840_TRANSPORT_DANGLING_BO;
        }
        if (!bo->gpu_mapped || !bo->backing_mem) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if (ib->gpu_va < bo->gpu_va || (ib->gpu_va % sizeof(uint32_t)) != 0u) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        rel_off = ib->gpu_va - bo->gpu_va;
        if (rel_off > bo->bytes || ib->bytes > bo->bytes - rel_off) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        ib_dwords = (size_t)(ib->bytes / sizeof(uint32_t));
        if (ib_dwords > SIZE_MAX - total_dwords) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        total_dwords += ib_dwords;
    }

    /* Phase 2: Validate every relocation BO is live, mapped, and VA in bounds. */
    for (i = 0; i < desc->reloc_count; ++i) {
        const A840Reloc *rel = &desc->relocs[i];
        const MockBo *bo;
        uint64_t rel_off;

        if ((size_t)rel->dword_offset + 1u >= total_dwords) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if (!rel->bo_handle) {
            return A840_TRANSPORT_DANGLING_BO;
        }
        bo = find_bo_const(mock, rel->bo_handle);
        if (!bo) {
            return A840_TRANSPORT_NOT_FOUND;
        }
        if (bo->freed) {
            return A840_TRANSPORT_DANGLING_BO;
        }
        if (!bo->gpu_mapped) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if (rel->gpu_va < bo->gpu_va) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        rel_off = rel->gpu_va - bo->gpu_va;
        if (rel_off >= bo->bytes) {
            return A840_TRANSPORT_OUT_OF_BOUNDS;
        }
        if ((rel->flags & A840_RELOC_WRITE) != 0u &&
            (bo->permissions & A840_BO_PERM_WRITE) == 0u) {
            return A840_TRANSPORT_STATE_ERROR;
        }
    }

    /* Phase 3: Transactional capture of submitted PM4 IB stream. */
    st = ensure_submission_space(mock, total_dwords);
    if (st != A840_TRANSPORT_OK) {
        return st;
    }

    start_offset = mock->captured_count;
    for (i = 0; i < desc->ib_count; ++i) {
        const A840SubmitIb *ib = &desc->ibs[i];
        const MockBo *bo = find_bo_const(mock, ib->bo_handle);
        const uint8_t *src = (const uint8_t *)bo->backing_mem + (size_t)(ib->gpu_va - bo->gpu_va);
        size_t ib_dwords = (size_t)(ib->bytes / sizeof(uint32_t));
        memcpy(mock->captured_words + mock->captured_count, src, (size_t)ib->bytes);
        mock->captured_count += ib_dwords;
    }

    mock->next_fence++;
    mock->ticks += A840_MOCK_TICKS_PER_SUBMIT;
    q->last_submitted_fence = mock->next_fence;
    if (mock->auto_complete_submits) {
        q->completed_fence = mock->next_fence;
    }

    {
        MockSubmission *sub = &mock->submissions[mock->submission_count++];
        sub->queue = q->id;
        sub->fence = mock->next_fence;
        sub->ib_count = desc->ib_count;
        sub->reloc_count = desc->reloc_count;
        sub->word_offset = start_offset;
        sub->dword_count = total_dwords;
    }

    out_reply->fence = mock->next_fence;
    out_reply->submitted_ticks = mock->ticks;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_fence_wait(void *ctx, uint64_t queue, uint64_t fence,
                                           uint64_t timeout_ns, uint64_t *out_ticks)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockQueue *q;
    (void)timeout_ns;

    if (!mock || !fence) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    q = find_queue(mock, queue);
    if (!q || q->destroyed) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (fence > mock->next_fence) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (out_ticks) {
        *out_ticks = mock->ticks;
    }
    if (q->completed_fence >= fence) {
        return A840_TRANSPORT_OK;
    }
    return A840_TRANSPORT_TIMEOUT;
}

static A840TransportStatus mock_wait_any_v3(void *ctx,
                                            const struct NativeAdrenoAbiWaitAnyV3 *req,
                                            struct NativeAdrenoAbiWaitAnyReplyV3 *out_reply)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    uint32_t satisfied_mask = 0u;
    uint32_t all_mask;
    uint32_t i;
    bool wait_all;
    bool done;

    if (!mock || !req || !out_reply) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if ((mock->caps.abi_flags & NativeAdrenoParamsV3TimelineWaitAny) == 0u) {
        return A840_TRANSPORT_UNSUPPORTED;
    }
    if (req->count == 0u || req->count > 32u || (req->flags & ~UINT32_C(1)) != 0u) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    for (i = 0; i < req->count; ++i) {
        MockQueue *q = find_queue(mock, req->queues[i]);
        if (!q || q->destroyed) {
            return A840_TRANSPORT_NOT_FOUND;
        }
        if (req->fences[i] == 0u || req->fences[i] > mock->next_fence) {
            return A840_TRANSPORT_NOT_FOUND;
        }
        if (q->completed_fence >= req->fences[i]) {
            satisfied_mask |= UINT32_C(1) << i;
        }
    }

    all_mask = (req->count == 32u) ? UINT32_MAX : ((UINT32_C(1) << req->count) - 1u);
    wait_all = (req->flags & 1u) != 0u;
    done = wait_all ? (satisfied_mask == all_mask) : (satisfied_mask != 0u);

    out_reply->satisfied_mask = satisfied_mask;
    out_reply->timed_out = done ? 0u : 1u;
    out_reply->observed_ticks = mock->ticks;
    return done ? A840_TRANSPORT_OK : A840_TRANSPORT_TIMEOUT;
}

static A840TransportStatus mock_get_zero_page_v3(void *ctx,
                                                 struct NativeAdrenoAbiZeroPageReplyV3 *out_reply)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    if (!mock || !out_reply) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if ((mock->caps.abi_flags & NativeAdrenoParamsV3SharedZeroPage) == 0u) {
        return A840_TRANSPORT_UNSUPPORTED;
    }
    out_reply->gpu_va = mock->zero_page_va;
    out_reply->bytes = A840_TRANSPORT_ZERO_PAGE_BYTES;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus mock_surface_meta_v3(void *ctx,
                                                struct NativeAdrenoAbiSurfaceMetaV3 *io_meta)
{
    A840MockTransport *mock = (A840MockTransport *)ctx;
    MockBo *bo;

    if (!mock || !io_meta) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if ((mock->caps.abi_flags & NativeAdrenoParamsV3UbwcSurfaceMeta) == 0u) {
        return A840_TRANSPORT_UNSUPPORTED;
    }
    bo = find_bo(mock, io_meta->bo_handle);
    if (!bo) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (bo->freed) {
        return A840_TRANSPORT_DANGLING_BO;
    }

    if (io_meta->op == 0u) {
        *io_meta = bo->surface_meta;
        io_meta->bo_handle = bo->handle;
        io_meta->op = 0u;
        return A840_TRANSPORT_OK;
    }
    if (io_meta->op == 1u) {
        if (io_meta->ubwc_enabled > 1u || (io_meta->flags & ~UINT32_C(1)) != 0u) {
            return A840_TRANSPORT_INVALID_ARG;
        }
        if (io_meta->ubwc_enabled) {
            if (io_meta->ubwc_version != mock->caps.ubwc_version ||
                io_meta->highest_bank_bit != mock->caps.highest_bank_bit ||
                io_meta->swizzle_levels != mock->caps.ubwc_swizzle ||
                io_meta->macrotile_mode != mock->caps.macrotile_mode ||
                io_meta->meta_pitch == 0u || (io_meta->meta_pitch % 64u) != 0u ||
                io_meta->meta_offset >= bo->bytes) {
                return A840_TRANSPORT_INVALID_ARG;
            }
        }
        bo->surface_meta = *io_meta;
        bo->surface_meta.bo_handle = bo->handle;
        bo->surface_meta.op = 0u;
        return A840_TRANSPORT_OK;
    }
    return A840_TRANSPORT_INVALID_ARG;
}

static const A840BackendTransportOps k_mock_ops = {
    mock_query_caps,
    mock_bo_alloc,
    mock_bo_import,
    mock_bo_map,
    mock_bo_unmap,
    mock_bo_free,
    mock_queue_create,
    mock_queue_destroy,
    mock_queue_submit,
    mock_fence_wait,
    mock_wait_any_v3,
    mock_get_zero_page_v3,
    mock_surface_meta_v3
};

void a840_mock_transport_default_config(A840MockTransportConfig *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    out_cfg->abi_version = NativeAdrenoAbiVersionV3;
    out_cfg->abi_flags = NativeAdrenoParamsV2HostImport |
                         NativeAdrenoParamsV2OwnedCoherent |
                         NativeAdrenoParamsV2SubmitBatch |
                         NativeAdrenoParamsV3TimelineWaitAny |
                         NativeAdrenoParamsV3SharedZeroPage |
                         NativeAdrenoParamsV3UbwcSurfaceMeta;
    /* 256 MiB base leaves room for GMEM and the 64 KiB kernel zero page below. */
    out_cfg->va_start = UINT64_C(1) << 28;
    out_cfg->va_size = UINT64_C(1) << 40;
    out_cfg->max_bo_bytes = UINT64_C(1) << 28;
    out_cfg->auto_complete_submits = true;
}

A840TransportStatus a840_mock_transport_create(const A840MockTransportConfig *cfg,
                                               A840MockTransport **out_mock)
{
    A840MockTransportConfig effective;
    const A840ChipFacts *facts = a840_chip_facts_kaanapali_default();
    A840MockTransport *mock;
    MockBo *zp;
    A840TransportStatus st;

    if (!out_mock) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    *out_mock = NULL;

    if (cfg) {
        effective = *cfg;
    } else {
        a840_mock_transport_default_config(&effective);
    }

    if (effective.abi_version != NativeAdrenoAbiVersionV2 &&
        effective.abi_version != NativeAdrenoAbiVersionV3) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (effective.va_start < (UINT64_C(2) * A840_TRANSPORT_ZERO_PAGE_BYTES) ||
        (effective.va_start % A840_TRANSPORT_ZERO_PAGE_BYTES) != 0u ||
        effective.va_size < A840_TRANSPORT_ZERO_PAGE_BYTES ||
        (effective.va_size % A840_TRANSPORT_PAGE_BYTES) != 0u ||
        effective.va_start >= va_limit_48() ||
        effective.va_size > va_limit_48() - effective.va_start ||
        effective.max_bo_bytes < A840_TRANSPORT_PAGE_BYTES ||
        (effective.max_bo_bytes % A840_TRANSPORT_PAGE_BYTES) != 0u) {
        return A840_TRANSPORT_INVALID_ARG;
    }

    mock = (A840MockTransport *)calloc(1u, sizeof(*mock));
    if (!mock) {
        return A840_TRANSPORT_OOM;
    }

    mock->caps.abi_version = effective.abi_version;
    mock->caps.input_bits = A840_MOCK_VA_BITS;
    mock->caps.output_bits = A840_MOCK_OUTPUT_BITS;
    mock->caps.abi_flags = effective.abi_flags;
    mock->caps.max_bo_bytes = effective.max_bo_bytes;
    mock->caps.chip_id = (uint32_t)facts->chip_id;
    mock->caps.gmem_bytes = facts->gmem_bytes;
    mock->caps.gmem_base = facts->gmem_range_min;
    mock->caps.uche_trap_base = facts->uche_trap_base;
    mock->caps.va_start = effective.va_start;
    mock->caps.va_size = effective.va_size;
    mock->caps.highest_bank_bit = facts->highest_bank_bit;
    mock->caps.ubwc_swizzle = facts->ubwc_swizzle;
    mock->caps.macrotile_mode = facts->macrotile_mode;
    mock->caps.ubwc_version = facts->ubwc_version;
    mock->caps.device_flags = A840_TRANSPORT_DEV_CLIENT_IOVA |
                              A840_TRANSPORT_DEV_CACHED_COHERENT;
    mock->auto_complete_submits = effective.auto_complete_submits;
    mock->zero_page_va = effective.va_start - A840_TRANSPORT_ZERO_PAGE_BYTES;

    /* Kernel-owned 64 KiB read-only zero page below user VA aperture. */
    st = ensure_bo_slot(mock, &zp);
    if (st != A840_TRANSPORT_OK) {
        free(mock);
        return st;
    }
    zp->backing_mem = calloc(1u, A840_TRANSPORT_ZERO_PAGE_BYTES);
    if (!zp->backing_mem) {
        free(mock->bos);
        free(mock);
        return A840_TRANSPORT_OOM;
    }
    zp->handle = ++mock->next_bo_handle;
    zp->bytes = A840_TRANSPORT_ZERO_PAGE_BYTES;
    zp->align_log2 = 16u;
    zp->kernel_owned = true;
    zp->gpu_mapped = true;
    zp->gpu_va = mock->zero_page_va;
    zp->permissions = A840_BO_PERM_READ;
    mock->bo_count++;
    mock->zero_page_handle = zp->handle;

    *out_mock = mock;
    return A840_TRANSPORT_OK;
}

void a840_mock_transport_destroy(A840MockTransport *mock)
{
    size_t i;
    if (!mock) {
        return;
    }
    for (i = 0; i < mock->bo_count; ++i) {
        if (!mock->bos[i].freed && !mock->bos[i].imported && mock->bos[i].backing_mem) {
            free(mock->bos[i].backing_mem);
        }
    }
    free(mock->bos);
    free(mock->queues);
    free(mock->submissions);
    free(mock->captured_words);
    free(mock);
}

A840BackendTransport a840_mock_transport_interface(A840MockTransport *mock)
{
    A840BackendTransport iface;
    iface.ops = mock ? &k_mock_ops : NULL;
    iface.ctx = mock;
    return iface;
}

void a840_mock_transport_set_auto_complete(A840MockTransport *mock, bool enabled)
{
    if (mock) {
        mock->auto_complete_submits = enabled;
    }
}

A840TransportStatus a840_mock_transport_complete_queue(A840MockTransport *mock,
                                                       uint64_t queue,
                                                       uint64_t up_to_fence)
{
    MockQueue *q;
    if (!mock) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    q = find_queue(mock, queue);
    if (!q || q->destroyed) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    if (up_to_fence > q->last_submitted_fence) {
        up_to_fence = q->last_submitted_fence;
    }
    if (up_to_fence > q->completed_fence) {
        q->completed_fence = up_to_fence;
        mock->ticks += A840_MOCK_TICKS_PER_SUBMIT;
    }
    return A840_TRANSPORT_OK;
}

void a840_mock_transport_complete_all(A840MockTransport *mock)
{
    size_t i;
    if (!mock) {
        return;
    }
    for (i = 0; i < mock->queue_count; ++i) {
        if (!mock->queues[i].destroyed &&
            mock->queues[i].completed_fence < mock->queues[i].last_submitted_fence) {
            mock->queues[i].completed_fence = mock->queues[i].last_submitted_fence;
        }
    }
    mock->ticks += A840_MOCK_TICKS_PER_SUBMIT;
}

size_t a840_mock_transport_submission_count(const A840MockTransport *mock)
{
    return mock ? mock->submission_count : 0u;
}

A840TransportStatus a840_mock_transport_get_submission(const A840MockTransport *mock,
                                                       size_t index,
                                                       A840MockSubmissionView *out_view)
{
    const MockSubmission *sub;
    if (!mock || !out_view) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    if (index >= mock->submission_count) {
        return A840_TRANSPORT_OUT_OF_BOUNDS;
    }
    sub = &mock->submissions[index];
    out_view->queue = sub->queue;
    out_view->fence = sub->fence;
    out_view->ib_count = sub->ib_count;
    out_view->reloc_count = sub->reloc_count;
    out_view->words = mock->captured_words + sub->word_offset;
    out_view->dword_count = sub->dword_count;
    return A840_TRANSPORT_OK;
}

size_t a840_mock_transport_total_captured_dwords(const A840MockTransport *mock)
{
    return mock ? mock->captured_count : 0u;
}

const uint32_t *a840_mock_transport_captured_words(const A840MockTransport *mock,
                                                   size_t *out_count)
{
    if (out_count) {
        *out_count = mock ? mock->captured_count : 0u;
    }
    return mock ? mock->captured_words : NULL;
}

A840TransportStatus a840_mock_transport_dump_pm4(const A840MockTransport *mock,
                                                 const char *path)
{
    FILE *fp;
    size_t i;

    if (!mock || !path || !path[0]) {
        return A840_TRANSPORT_INVALID_ARG;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return A840_TRANSPORT_STATE_ERROR;
    }
    for (i = 0; i < mock->captured_count; ++i) {
        uint32_t w = mock->captured_words[i];
        uint8_t le[4];
        le[0] = (uint8_t)(w & UINT32_C(255));
        le[1] = (uint8_t)((w >> 8) & UINT32_C(255));
        le[2] = (uint8_t)((w >> 16) & UINT32_C(255));
        le[3] = (uint8_t)((w >> 24) & UINT32_C(255));
        if (fwrite(le, sizeof(le), 1u, fp) != 1u) {
            fclose(fp);
            return A840_TRANSPORT_STATE_ERROR;
        }
    }
    if (fclose(fp) != 0) {
        return A840_TRANSPORT_STATE_ERROR;
    }
    return A840_TRANSPORT_OK;
}

size_t a840_mock_transport_live_bo_count(const A840MockTransport *mock)
{
    return mock ? mock->live_bo_count : 0u;
}

size_t a840_mock_transport_live_queue_count(const A840MockTransport *mock)
{
    return mock ? mock->live_queue_count : 0u;
}

bool a840_mock_transport_is_bo_live(const A840MockTransport *mock, uint64_t bo_handle)
{
    const MockBo *bo = find_bo_const(mock, bo_handle);
    return bo != NULL && !bo->freed;
}

uint64_t a840_mock_transport_zero_page_handle(const A840MockTransport *mock)
{
    return mock ? mock->zero_page_handle : 0u;
}
