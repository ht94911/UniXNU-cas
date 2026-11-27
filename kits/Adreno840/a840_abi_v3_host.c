/* SPDX-License-Identifier: MIT */
#include "a840_abi_v3_host.h"
#include <string.h>

#define V2_FLAGS (NativeAdrenoParamsV2HostImport | NativeAdrenoParamsV2OwnedCoherent | \
    NativeAdrenoParamsV2MemoryPressure | NativeAdrenoParamsV2SubmitBatch)
#define V3_CORE_FLAGS (NativeAdrenoParamsV3TimelineWaitAny | NativeAdrenoParamsV3SharedZeroPage | \
    NativeAdrenoParamsV3SharedTier0Pack | NativeAdrenoParamsV3WriterPromise | NativeAdrenoParamsV3UbwcSurfaceMeta)
#define V3_KNOWN_FLAGS (V3_CORE_FLAGS | NativeAdrenoParamsV3SparseVmBind | NativeAdrenoParamsV3PreemptRings)

static A840AbiV3Status state_valid(const A840AbiV3State *st)
{
    if (st == NULL)
        return A840_ABI_V3_INVALID_ARG;
    return st->initialized == NativeAdrenoAbiVersionV3 ?
        A840_ABI_V3_OK : A840_ABI_V3_STATE_CONFLICT;
}

A840AbiV3Status a840_abi_v3_init(A840AbiV3State *st, uint32_t cap_flags)
{
    if (st == NULL || (cap_flags & ~(V2_FLAGS | V3_KNOWN_FLAGS)) != 0)
        return A840_ABI_V3_INVALID_ARG;
    memset(st, 0, sizeof(*st));
    st->initialized = NativeAdrenoAbiVersionV3;
    st->cap_flags = cap_flags & (V2_FLAGS | V3_CORE_FLAGS);
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_check_method(const A840AbiV3State *st, uint32_t method)
{
    uint32_t flag;
    A840AbiV3Status status = state_valid(st);
    if (status != A840_ABI_V3_OK)
        return status;
    switch (method) {
    case NativeAdrenoMethodWaitAnyV3: flag = NativeAdrenoParamsV3TimelineWaitAny; break;
    case NativeAdrenoMethodGetZeroPageV3: flag = NativeAdrenoParamsV3SharedZeroPage; break;
    case NativeAdrenoMethodMapSharedPackV3: flag = NativeAdrenoParamsV3SharedTier0Pack; break;
    case NativeAdrenoMethodWriterPromiseV3: flag = NativeAdrenoParamsV3WriterPromise; break;
    case NativeAdrenoMethodSurfaceMetaV3: flag = NativeAdrenoParamsV3UbwcSurfaceMeta; break;
    default: return A840_ABI_V3_UNSUPPORTED;
    }
    return (st->cap_flags & flag) != 0 ? A840_ABI_V3_OK : A840_ABI_V3_UNSUPPORTED;
}

A840AbiV3Status a840_abi_v3_validate_envelope(uint32_t method,
    size_t input_bytes, size_t output_bytes)
{
    size_t input, output;
    switch (method) {
    case NativeAdrenoMethodWaitAnyV3:
        input = sizeof(struct NativeAdrenoAbiWaitAnyV3);
        output = sizeof(struct NativeAdrenoAbiWaitAnyReplyV3); break;
    case NativeAdrenoMethodGetZeroPageV3:
        input = 0; output = sizeof(struct NativeAdrenoAbiZeroPageReplyV3); break;
    case NativeAdrenoMethodMapSharedPackV3:
        input = sizeof(struct NativeAdrenoAbiSharedPackReqV3);
        output = sizeof(struct NativeAdrenoAbiSharedPackReplyV3); break;
    case NativeAdrenoMethodWriterPromiseV3:
        input = sizeof(struct NativeAdrenoAbiWriterPromiseV3);
        output = sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3); break;
    case NativeAdrenoMethodSurfaceMetaV3:
        input = output = sizeof(struct NativeAdrenoAbiSurfaceMetaV3); break;
    default:
        return A840_ABI_V3_UNSUPPORTED;
    }
    if (input_bytes > NativeAdrenoAbiEnvelopeBytes || output_bytes > NativeAdrenoAbiEnvelopeBytes)
        return A840_ABI_V3_RANGE;
    return input_bytes == input && output_bytes == output ?
        A840_ABI_V3_OK : A840_ABI_V3_INVALID_ARG;
}

static A840AbiV3Status gpu_span(uint64_t va, uint64_t bytes, uint64_t alignment)
{
    if (va == 0 || bytes == 0)
        return A840_ABI_V3_INVALID_ARG;
    if (va >= A840_ABI_V3_GPU_VA_LIMIT || bytes > A840_ABI_V3_GPU_VA_LIMIT - va)
        return A840_ABI_V3_RANGE;
    if (va % alignment != 0 || bytes % alignment != 0)
        return A840_ABI_V3_ALIGNMENT;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_set_zero_page(A840AbiV3State *st,
    const struct NativeAdrenoAbiZeroPageReplyV3 *page, uint32_t readonly)
{
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodGetZeroPageV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (page == NULL || readonly != 1)
        return A840_ABI_V3_INVALID_ARG;
    if (page->bytes < A840_ABI_V3_ZERO_PAGE_BYTES)
        return A840_ABI_V3_RANGE;
    status = gpu_span(page->gpu_va, page->bytes, A840_ABI_V3_ZERO_PAGE_BYTES);
    if (status != A840_ABI_V3_OK)
        return status;
    st->zero_page = *page;
    st->zero_page_valid = 1;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_get_zero_page(const A840AbiV3State *st,
    struct NativeAdrenoAbiZeroPageReplyV3 *reply)
{
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodGetZeroPageV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (reply == NULL)
        return A840_ABI_V3_INVALID_ARG;
    if (!st->zero_page_valid)
        return A840_ABI_V3_NOT_FOUND;
    *reply = st->zero_page;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_set_shared_pack(A840AbiV3State *st,
    const struct NativeAdrenoAbiSharedPackReplyV3 *pack,
    uint32_t gpu_readonly, uint32_t cpu_readonly)
{
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodMapSharedPackV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (pack == NULL || gpu_readonly != 1 || cpu_readonly > 1 ||
        pack->actual_stack_key == 0)
        return A840_ABI_V3_INVALID_ARG;
    status = gpu_span(pack->gpu_va, pack->bytes, A840_ABI_V3_PAGE_BYTES);
    if (status != A840_ABI_V3_OK)
        return status;
    if (pack->cpu_va != 0) {
        if (cpu_readonly != 1)
            return A840_ABI_V3_INVALID_ARG;
        if (pack->cpu_va % A840_ABI_V3_PAGE_BYTES)
            return A840_ABI_V3_ALIGNMENT;
        if (pack->bytes > UINT64_MAX - pack->cpu_va)
            return A840_ABI_V3_RANGE;
    }
    st->shared_pack = *pack;
    st->shared_pack_valid = 1;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_map_shared_pack(const A840AbiV3State *st,
    const struct NativeAdrenoAbiSharedPackReqV3 *request,
    struct NativeAdrenoAbiSharedPackReplyV3 *reply)
{
    struct NativeAdrenoAbiSharedPackReplyV3 candidate;
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodMapSharedPackV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (request == NULL || reply == NULL || request->map_cpu > 1 || request->reserved != 0)
        return A840_ABI_V3_INVALID_ARG;
    if (!st->shared_pack_valid)
        return A840_ABI_V3_NOT_FOUND;
    if (request->expected_stack_key != st->shared_pack.actual_stack_key) {
        memset(&candidate, 0, sizeof(candidate));
        candidate.actual_stack_key = st->shared_pack.actual_stack_key;
        *reply = candidate;
        return A840_ABI_V3_STACK_KEY_MISMATCH;
    }
    if (request->map_cpu && st->shared_pack.cpu_va == 0)
        return A840_ABI_V3_NOT_FOUND;
    candidate = st->shared_pack;
    if (!request->map_cpu)
        candidate.cpu_va = 0;
    *reply = candidate;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_eval_wait_any(const A840AbiV3State *st,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    A840AbiV3FenceQuery query, void *cookie, uint64_t elapsed_ns,
    uint64_t observed_ticks, struct NativeAdrenoAbiWaitAnyReplyV3 *reply)
{
    struct NativeAdrenoAbiWaitAnyReplyV3 candidate;
    uint32_t i, wanted;
    int satisfied;
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodWaitAnyV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (request == NULL || reply == NULL || query == NULL ||
        request->count == 0 || request->count > 32 || (request->flags & ~UINT32_C(1)) != 0)
        return A840_ABI_V3_INVALID_ARG;
    candidate.satisfied_mask = 0;
    candidate.timed_out = 0;
    candidate.observed_ticks = observed_ticks;
    for (i = 0; i < request->count; ++i) {
        uint64_t completed = 0;
        status = query(cookie, request->queues[i], &completed);
        if (status != A840_ABI_V3_OK)
            return status;
        if (completed >= request->fences[i])
            candidate.satisfied_mask |= UINT32_C(1) << i;
    }
    wanted = request->count == 32 ? UINT32_MAX : (UINT32_C(1) << request->count) - 1;
    satisfied = request->flags ? candidate.satisfied_mask == wanted : candidate.satisfied_mask != 0;
    candidate.timed_out = !satisfied && elapsed_ns >= request->timeout_ns;
    *reply = candidate;
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_set_promise_observer(A840AbiV3State *st,
    A840AbiV3PromiseObserver observer, void *cookie)
{
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodWriterPromiseV3);
    if (status != A840_ABI_V3_OK)
        return status;
    st->observer = observer;
    st->observer_cookie = cookie;
    return A840_ABI_V3_OK;
}

static void notify(A840AbiV3State *st, const A840AbiV3PromiseSlot *slot)
{
    if (st->observer != NULL)
        st->observer(st->observer_cookie, slot->client_id, &slot->request, slot->state);
}

static A840AbiV3Status surface_ids(const struct NativeAdrenoAbiWriterPromiseV3 *r)
{
    uint32_t i, j;
    if (r->surface_count == 0 || r->surface_count > 16)
        return A840_ABI_V3_INVALID_ARG;
    for (i = 0; i < r->surface_count; ++i) {
        if (r->surface_ids[i] == 0)
            return A840_ABI_V3_INVALID_ARG;
        for (j = 0; j < i; ++j)
            if (r->surface_ids[i] == r->surface_ids[j])
                return A840_ABI_V3_STATE_CONFLICT;
    }
    return A840_ABI_V3_OK;
}

A840AbiV3Status a840_abi_v3_writer_promise(A840AbiV3State *st, uint64_t client_id,
    const struct NativeAdrenoAbiWriterPromiseV3 *request,
    struct NativeAdrenoAbiWriterPromiseReplyV3 *reply)
{
    struct NativeAdrenoAbiWriterPromiseV3 input;
    struct NativeAdrenoAbiWriterPromiseReplyV3 output;
    A840AbiV3PromiseSlot *slot = NULL, *terminal = NULL;
    size_t i;
    uint32_t n;
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodWriterPromiseV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (request == NULL || reply == NULL || client_id == 0)
        return A840_ABI_V3_INVALID_ARG;
    input = *request;
    status = surface_ids(&input);
    if (status != A840_ABI_V3_OK)
        return status;
    if (input.op == NativeAdrenoPromiseMake) {
        if (input.promise_token != 0 || input.fence != 0)
            return A840_ABI_V3_INVALID_ARG;
        if (st->last_promise_token == UINT64_MAX)
            return A840_ABI_V3_CAPACITY;
        for (i = 0; i < A840_ABI_V3_PROMISE_SLOTS; ++i) {
            if (st->promises[i].state == A840_ABI_V3_PROMISE_FREE) {
                slot = &st->promises[i];
                break;
            }
            if (terminal == NULL && st->promises[i].state != A840_ABI_V3_PROMISE_PENDING)
                terminal = &st->promises[i];
        }
        if (slot == NULL)
            slot = terminal;
        if (slot == NULL)
            return A840_ABI_V3_CAPACITY;
        memset(slot, 0, sizeof(*slot));
        slot->client_id = client_id;
        slot->state = A840_ABI_V3_PROMISE_PENDING;
        slot->request = input;
        slot->request.promise_token = ++st->last_promise_token;
    } else if (input.op == NativeAdrenoPromiseFulfill || input.op == NativeAdrenoPromiseCancel) {
        if (input.promise_token == 0 ||
            (input.op == NativeAdrenoPromiseFulfill ? input.fence == 0 : input.fence != 0))
            return A840_ABI_V3_INVALID_ARG;
        for (i = 0; i < A840_ABI_V3_PROMISE_SLOTS; ++i)
            if (st->promises[i].state != A840_ABI_V3_PROMISE_FREE &&
                st->promises[i].client_id == client_id &&
                st->promises[i].request.promise_token == input.promise_token) {
                slot = &st->promises[i];
                break;
            }
        if (slot == NULL)
            return A840_ABI_V3_NOT_FOUND;
        if (slot->state != A840_ABI_V3_PROMISE_PENDING ||
            slot->request.surface_count != input.surface_count)
            return A840_ABI_V3_STATE_CONFLICT;
        for (n = 0; n < input.surface_count; ++n)
            if (slot->request.surface_ids[n] != input.surface_ids[n])
                return A840_ABI_V3_STATE_CONFLICT;
        slot->request = input;
        slot->state = input.op == NativeAdrenoPromiseFulfill ?
            A840_ABI_V3_PROMISE_FULFILLED : A840_ABI_V3_PROMISE_CANCELED;
    } else {
        return A840_ABI_V3_INVALID_ARG;
    }
    output.promise_token = slot->request.promise_token;
    output.accepted = 1;
    output.reserved = 0;
    *reply = output;
    notify(st, slot);
    return A840_ABI_V3_OK;
}

size_t a840_abi_v3_pending_promises(const A840AbiV3State *st, uint64_t client_id)
{
    size_t i, count = 0;
    if (state_valid(st) != A840_ABI_V3_OK)
        return 0;
    for (i = 0; i < A840_ABI_V3_PROMISE_SLOTS; ++i)
        if (st->promises[i].state == A840_ABI_V3_PROMISE_PENDING &&
            (client_id == 0 || st->promises[i].client_id == client_id))
            ++count;
    return count;
}

size_t a840_abi_v3_cancel_client_promises(A840AbiV3State *st, uint64_t client_id)
{
    size_t i, count = 0;
    if (state_valid(st) != A840_ABI_V3_OK || client_id == 0)
        return 0;
    for (i = 0; i < A840_ABI_V3_PROMISE_SLOTS; ++i) {
        A840AbiV3PromiseSlot *slot = &st->promises[i];
        if (slot->state == A840_ABI_V3_PROMISE_FREE || slot->client_id != client_id)
            continue;
        if (slot->state == A840_ABI_V3_PROMISE_PENDING) {
            slot->state = A840_ABI_V3_PROMISE_CANCELED;
            slot->request.op = NativeAdrenoPromiseCancel;
            slot->request.fence = 0;
            notify(st, slot);
            ++count;
        }
        memset(slot, 0, sizeof(*slot));
    }
    for (i = 0; i < A840_ABI_V3_SURFACE_SLOTS; ++i)
        if (st->surfaces[i].valid && st->surfaces[i].client_id == client_id)
            memset(&st->surfaces[i], 0, sizeof(st->surfaces[i]));
    return count;
}

size_t a840_abi_v3_cancel_all_for_client(A840AbiV3State *st, uint64_t client_id)
{
    return a840_abi_v3_cancel_client_promises(st, client_id);
}

static A840AbiV3Status meta_bounds(const struct NativeAdrenoAbiSurfaceMetaV3 *m,
    uint64_t bo_bytes)
{
    if (bo_bytes == 0 || bo_bytes > A840_ABI_V3_GPU_VA_LIMIT ||
        m->meta_offset > bo_bytes || m->meta_pitch > bo_bytes - m->meta_offset)
        return A840_ABI_V3_RANGE;
    return A840_ABI_V3_OK;
}

static A840AbiV3Status meta_validate(const struct NativeAdrenoAbiSurfaceMetaV3 *m,
    uint64_t bo_bytes)
{
    if (m->ubwc_enabled > 1 || m->ubwc_version != 6 || m->highest_bank_bit != 16 ||
        m->swizzle_levels != 6 || m->macrotile_mode != 1 || (m->flags & ~UINT32_C(1)) != 0)
        return A840_ABI_V3_INVALID_ARG;
    if (m->meta_offset % A840_ABI_V3_PAGE_BYTES ||
        m->meta_pitch % A840_ABI_V3_META_PITCH_ALIGNMENT)
        return A840_ABI_V3_ALIGNMENT;
    if (m->ubwc_enabled && m->meta_pitch == 0)
        return A840_ABI_V3_INVALID_ARG;
    return meta_bounds(m, bo_bytes);
}

A840AbiV3Status a840_abi_v3_surface_meta(A840AbiV3State *st, uint64_t client_id,
    const struct NativeAdrenoAbiSurfaceMetaV3 *request, uint64_t bo_bytes,
    struct NativeAdrenoAbiSurfaceMetaV3 *reply)
{
    struct NativeAdrenoAbiSurfaceMetaV3 input, output;
    A840AbiV3SurfaceSlot *slot = NULL, *empty = NULL;
    size_t i;
    A840AbiV3Status status = a840_abi_v3_check_method(st, NativeAdrenoMethodSurfaceMetaV3);
    if (status != A840_ABI_V3_OK)
        return status;
    if (request == NULL || reply == NULL || client_id == 0 ||
        request->bo_handle == 0 || request->op > 1)
        return A840_ABI_V3_INVALID_ARG;
    input = *request;
    if (input.op == 1) {
        status = meta_validate(&input, bo_bytes);
        if (status != A840_ABI_V3_OK)
            return status;
    }
    for (i = 0; i < A840_ABI_V3_SURFACE_SLOTS; ++i) {
        if (st->surfaces[i].valid && st->surfaces[i].client_id == client_id &&
            st->surfaces[i].metadata.bo_handle == input.bo_handle)
            slot = &st->surfaces[i];
        if (!st->surfaces[i].valid && empty == NULL)
            empty = &st->surfaces[i];
    }
    if (input.op == 0) {
        if (slot == NULL)
            return A840_ABI_V3_NOT_FOUND;
        status = meta_bounds(&slot->metadata, bo_bytes);
        if (status != A840_ABI_V3_OK)
            return status;
        output = slot->metadata;
        output.op = 0;
        *reply = output;
        return A840_ABI_V3_OK;
    }
    if (slot == NULL)
        slot = empty;
    if (slot == NULL)
        return A840_ABI_V3_CAPACITY;
    memset(slot, 0, sizeof(*slot));
    slot->valid = 1;
    slot->client_id = client_id;
    slot->bo_bytes = bo_bytes;
    slot->metadata = input;
    *reply = input;
    return A840_ABI_V3_OK;
}
