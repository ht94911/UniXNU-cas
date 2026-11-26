/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_iokit_wire.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/a840_runtime_device.h"
#include "a840_context_init.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "NativeAdreno wire records require little-endian byte order"
#endif

typedef struct WireMethod {
    size_t input_bytes, output_bytes;
    uint32_t capability;
} WireMethod;

#define RECORDS(in, out) {sizeof(in), sizeof(out), 0}
#define IN(in) {sizeof(in), 0, 0}
#define OUT(out) {0, sizeof(out), 0}
static const WireMethod methods[] = {
    [A840_WIRE_QUERY] = OUT(A840WireParams),
    [A840_WIRE_BO_ALLOC] = RECORDS(A840WireBoAlloc, A840WireBoAllocReply),
    [A840_WIRE_BO_MAP] = RECORDS(A840WireBoMap, A840WireBoMapReply),
    [A840_WIRE_BO_FREE] = IN(A840WireBoFree),
    [A840_WIRE_SUBMIT] = RECORDS(A840WireSubmit, A840WireSubmitReply),
    [A840_WIRE_WAIT_FENCE] = IN(A840WireWaitFence),
    [A840_WIRE_GET_TIMESTAMP] = OUT(A840WireTimestamp),
    [A840_WIRE_GET_STATUS] = OUT(A840WireStatus),
    [A840_WIRE_SET_METADATA] = IN(A840WireMetadata),
    [A840_WIRE_GET_METADATA] = RECORDS(A840WireMetadata, A840WireMetadata),
    [A840_WIRE_CREATE_QUEUE] = RECORDS(A840WireQueue, A840WireQueueReply),
    [A840_WIRE_CLOSE_QUEUE] = IN(A840WireQueue),
    [A840_WIRE_BO_CPU_MAP] = RECORDS(A840WireBoCpuMap, A840WireBoCpuMapReply),
    [A840_WIRE_BO_CPU_UNMAP] = IN(A840WireBoCpuUnmap),
    [A840_WIRE_GET_DEVICE_INFO] = OUT(A840WireDeviceInfo),
    [A840_WIRE_BO_IMPORT] = RECORDS(A840WireBoImport, A840WireBoAllocReply),
    [A840_WIRE_SET_PRIORITY] = IN(A840WirePriority),
    [A840_WIRE_NOTE] = IN(A840WireNote),
    [A840_WIRE_SUBMIT_BATCH] = RECORDS(A840WireSubmitBatch, A840WireSubmitBatchReply),
    [NativeAdrenoMethodWaitAnyV3] = {
        sizeof(struct NativeAdrenoAbiWaitAnyV3), sizeof(struct NativeAdrenoAbiWaitAnyReplyV3),
        NativeAdrenoParamsV3TimelineWaitAny,
    },
    [NativeAdrenoMethodGetZeroPageV3] = {
        0, sizeof(struct NativeAdrenoAbiZeroPageReplyV3), NativeAdrenoParamsV3SharedZeroPage,
    },
    [NativeAdrenoMethodMapSharedPackV3] = {
        sizeof(struct NativeAdrenoAbiSharedPackReqV3), sizeof(struct NativeAdrenoAbiSharedPackReplyV3),
        NativeAdrenoParamsV3SharedTier0Pack,
    },
    [NativeAdrenoMethodWriterPromiseV3] = {
        sizeof(struct NativeAdrenoAbiWriterPromiseV3), sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3),
        NativeAdrenoParamsV3WriterPromise,
    },
    [NativeAdrenoMethodSurfaceMetaV3] = {
        sizeof(struct NativeAdrenoAbiSurfaceMetaV3), sizeof(struct NativeAdrenoAbiSurfaceMetaV3),
        NativeAdrenoParamsV3UbwcSurfaceMeta,
    },
};
#undef OUT
#undef IN
#undef RECORDS

typedef struct WireQueue {
    struct WireQueue *next;
    uint64_t handle;
    uint32_t priority;
} WireQueue;

struct A840IokitWire {
    A840WireCallFn call;
    A840WireMapMemoryFn map_cpu;
    A840WireUnmapMemoryFn unmap_cpu;
    void *cookie;
    A840TransportCaps caps;
    struct NativeAdrenoAbiZeroPageReplyV3 zero_page;
    pthread_mutex_t submit_lock;
    WireQueue *queues;
    A840RtBuffer *context_bo;
    A840SubmitIb context_ib;
    bool submitted;
};

static const WireMethod *method_info(uint32_t method)
{
    return method > 0 && method < sizeof(methods) / sizeof(methods[0]) &&
        method != NativeAdrenoMethodMapSparseV3 ? &methods[method] : NULL;
}

A840TransportStatus a840_wire_method_sizes(uint32_t method, size_t *input, size_t *output)
{
    if (!input || !output || input == output) return A840_TRANSPORT_INVALID_ARG;
    const WireMethod *info = method_info(method);
    if (!info) return A840_TRANSPORT_UNSUPPORTED;
    *input = info->input_bytes;
    *output = info->output_bytes;
    return A840_TRANSPORT_OK;
}

A840TransportStatus a840_wire_call(A840IokitWire *wire, uint32_t method,
    const void *input, size_t input_bytes, void *output, size_t output_bytes)
{
    if (!wire) return A840_TRANSPORT_INVALID_ARG;
    const WireMethod *info = method_info(method);
    if (!info) return A840_TRANSPORT_UNSUPPORTED;
    if (input_bytes != info->input_bytes || output_bytes != info->output_bytes ||
        (input_bytes && !input) || (output_bytes && !output))
        return A840_TRANSPORT_INVALID_ARG;
    if (info->capability && !(wire->caps.abi_flags & info->capability))
        return A840_TRANSPORT_UNSUPPORTED;
    return wire->call(wire->cookie, method, input, input_bytes, output, output_bytes);
}

static bool gpu_span(uint64_t va, uint64_t bytes, uint64_t alignment, uint32_t bits)
{
    const uint64_t ceiling = UINT64_C(1) << bits;
    return va && va < ceiling && bytes && bytes <= ceiling - va && va % alignment == 0;
}

static A840TransportStatus discover_caps(A840IokitWire *wire)
{
    A840WireParams params = {0};
    A840WireDeviceInfo info = {0};
    A840TransportStatus status = a840_wire_call(wire, A840_WIRE_QUERY,
        NULL, 0, &params, sizeof(params));
    if (status != A840_TRANSPORT_OK) return status;
    if ((params.abi_version != NativeAdrenoAbiVersionV2 && params.abi_version != NativeAdrenoAbiVersionV3) ||
        params.input_bits < 32 || params.input_bits > 48 ||
        params.output_bits < 32 || params.output_bits > 48 || !params.max_bo_bytes)
        return A840_TRANSPORT_UNSUPPORTED;
    status = a840_wire_call(wire, A840_WIRE_GET_DEVICE_INFO, NULL, 0, &info, sizeof(info));
    if (status != A840_TRANSPORT_OK) return status;
    if (info.reserved || !info.chip_id || info.va_size % A840_TRANSPORT_PAGE_BYTES ||
        !gpu_span(info.va_start, info.va_size, A840_TRANSPORT_PAGE_BYTES, params.input_bits))
        return A840_TRANSPORT_STATE_ERROR;

    wire->caps = (A840TransportCaps){
        .abi_version = params.abi_version,
        .input_bits = params.input_bits,
        .output_bits = params.output_bits,
        .abi_flags = params.flags,
        .max_bo_bytes = params.max_bo_bytes,
        .chip_id = info.chip_id,
        .gmem_bytes = info.gmem_bytes,
        .gmem_base = info.gmem_base,
        .uche_trap_base = info.uche_trap_base,
        .va_start = info.va_start,
        .va_size = info.va_size,
        .highest_bank_bit = info.highest_bank_bit,
        .ubwc_swizzle = info.ubwc_swizzle,
        .macrotile_mode = info.macrotile_mode,
        .ubwc_version = info.ubwc_version,
        .device_flags = info.flags,
    };

    if (!(params.flags & NativeAdrenoParamsV3SharedZeroPage)) return A840_TRANSPORT_OK;
    status = a840_wire_call(wire, NativeAdrenoMethodGetZeroPageV3,
        NULL, 0, &wire->zero_page, sizeof(wire->zero_page));
    if (status == A840_TRANSPORT_UNSUPPORTED) {
        wire->caps.abi_flags &= ~NativeAdrenoParamsV3SharedZeroPage;
        return A840_TRANSPORT_OK; /* The core will allocate the v2 zero BO. */
    }
    if (status != A840_TRANSPORT_OK) return status;
    const uint64_t page = wire->zero_page.gpu_va, bytes = wire->zero_page.bytes;
    if (bytes < A840_TRANSPORT_ZERO_PAGE_BYTES || bytes % A840_TRANSPORT_PAGE_BYTES ||
        !gpu_span(page, bytes, A840_TRANSPORT_ZERO_PAGE_BYTES, params.input_bits))
        return A840_TRANSPORT_STATE_ERROR;

    /* The core takes one contiguous allocation window. If the kernel's
     * mapping is inside it, hand the core the larger unmapped side. */
    const uint64_t end = info.va_start + info.va_size;
    const uint64_t page_end = page + bytes;
    if (page < end && page_end > info.va_start) {
        const uint64_t low = page > info.va_start ? page - info.va_start : 0;
        const uint64_t high = page_end < end ? end - page_end : 0;
        if (!low && !high) return A840_TRANSPORT_STATE_ERROR;
        if (high > low) wire->caps.va_start = page_end;
        wire->caps.va_size = high > low ? high : low;
    }
    return A840_TRANSPORT_OK;
}

A840TransportStatus a840_wire_create(A840WireCallFn call, A840WireMapMemoryFn map_cpu,
    A840WireUnmapMemoryFn unmap_cpu, void *cookie, A840IokitWire **out_wire)
{
    if (!out_wire || !call || (map_cpu == NULL) != (unmap_cpu == NULL))
        return A840_TRANSPORT_INVALID_ARG;
    A840IokitWire *wire = calloc(1, sizeof(*wire));
    if (!wire) return A840_TRANSPORT_OOM;
    wire->call = call;
    wire->map_cpu = map_cpu;
    wire->unmap_cpu = unmap_cpu;
    wire->cookie = cookie;
    A840TransportStatus status = discover_caps(wire);
    if (status != A840_TRANSPORT_OK || pthread_mutex_init(&wire->submit_lock, NULL) != 0) {
        free(wire);
        return status == A840_TRANSPORT_OK ? A840_TRANSPORT_STATE_ERROR : status;
    }
    *out_wire = wire;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus query_caps(void *ctx, A840TransportCaps *out)
{
    const A840IokitWire *wire = ctx;
    if (!wire || !out) return A840_TRANSPORT_INVALID_ARG;
    *out = wire->caps;
    return A840_TRANSPORT_OK;
}

static bool bo_size(const A840IokitWire *wire, uint64_t bytes)
{
    return bytes && bytes % A840_TRANSPORT_PAGE_BYTES == 0 && bytes <= wire->caps.max_bo_bytes;
}

static A840TransportStatus bo_handle_call(A840IokitWire *wire, uint32_t method,
    const void *request, size_t bytes, uint64_t *handle)
{
    A840WireBoAllocReply reply = {0};
    const A840TransportStatus status = a840_wire_call(wire, method, request, bytes, &reply, sizeof(reply));
    if (status != A840_TRANSPORT_OK) return status;
    if (!reply.handle) return A840_TRANSPORT_STATE_ERROR;
    *handle = reply.handle;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus bo_alloc(void *ctx, const A840BoAllocDesc *desc, uint64_t *handle)
{
    A840IokitWire *wire = ctx;
    if (!wire || !desc || !handle || !bo_size(wire, desc->bytes) ||
        desc->align_log2 < 12 || desc->align_log2 > 24 || desc->contiguous > 1)
        return A840_TRANSPORT_INVALID_ARG;
    const A840WireBoAlloc request = {desc->bytes, desc->align_log2, desc->contiguous};
    return bo_handle_call(wire, A840_WIRE_BO_ALLOC, &request, sizeof(request), handle);
}

static A840TransportStatus bo_import(void *ctx, const A840BoImportDesc *desc, uint64_t *handle)
{
    A840IokitWire *wire = ctx;
    if (!wire || !desc || !handle) return A840_TRANSPORT_INVALID_ARG;
    if (!(wire->caps.abi_flags & NativeAdrenoParamsV2HostImport))
        return A840_TRANSPORT_UNSUPPORTED;
    const uintptr_t address = (uintptr_t)desc->host_ptr;
    if (!address || address % A840_TRANSPORT_PAGE_BYTES ||
        !bo_size(wire, desc->bytes) || desc->bytes > UINTPTR_MAX - address)
        return A840_TRANSPORT_INVALID_ARG;
    const A840WireBoImport request = {(uint64_t)address, desc->bytes};
    return bo_handle_call(wire, A840_WIRE_BO_IMPORT, &request, sizeof(request), handle);
}

static A840TransportStatus release_lease(A840IokitWire *wire, uint64_t handle, uint32_t token)
{
    const A840WireBoCpuUnmap request = {handle, token, 0};
    return a840_wire_call(wire, A840_WIRE_BO_CPU_UNMAP, &request, sizeof(request), NULL, 0);
}

static A840TransportStatus release_cpu(A840IokitWire *wire, uint64_t handle, uint32_t token)
{
    const A840TransportStatus status = wire->unmap_cpu(wire->cookie, token);
    if (status != A840_TRANSPORT_OK) return status;
    return release_lease(wire, handle, token);
}

static A840TransportStatus acquire_cpu(A840IokitWire *wire, uint64_t handle, A840BoMapReply *out)
{
    if (!wire->map_cpu) return A840_TRANSPORT_UNSUPPORTED;
    const A840WireBoCpuMap request = {handle};
    A840WireBoCpuMapReply lease = {0};
    A840TransportStatus status = a840_wire_call(wire, A840_WIRE_BO_CPU_MAP,
        &request, sizeof(request), &lease, sizeof(lease));
    if (status != A840_TRANSPORT_OK) return status;
    if (lease.reserved || !lease.token || !bo_size(wire, lease.bytes)) {
        if (lease.token) (void)release_lease(wire, handle, lease.token);
        return A840_TRANSPORT_STATE_ERROR;
    }
    const bool coherent = (wire->caps.abi_flags & NativeAdrenoParamsV2OwnedCoherent) != 0;
    void *cpu = NULL;
    status = wire->map_cpu(wire->cookie, lease.token, lease.bytes, coherent, &cpu);
    if (status != A840_TRANSPORT_OK || !cpu) {
        (void)release_lease(wire, handle, lease.token);
        return status == A840_TRANSPORT_OK ? A840_TRANSPORT_STATE_ERROR : status;
    }
    out->cpu_ptr = cpu;
    out->cpu_token = lease.token;
    out->bytes = lease.bytes;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus bo_map(void *ctx, const A840BoMapDesc *desc, A840BoMapReply *out)
{
    A840IokitWire *wire = ctx;
    if (!wire || !desc || !out || !desc->handle || !desc->flags ||
        (desc->flags & ~(A840_BO_MAP_GPU | A840_BO_MAP_CPU)))
        return A840_TRANSPORT_INVALID_ARG;

    const bool gpu = (desc->flags & A840_BO_MAP_GPU) != 0;
    if (gpu && (!gpu_span(desc->gpu_va, A840_TRANSPORT_PAGE_BYTES,
                      A840_TRANSPORT_PAGE_BYTES, wire->caps.input_bits) ||
        !(desc->permissions & A840_BO_PERM_READ) ||
        (desc->permissions & ~(A840_BO_PERM_READ | A840_BO_PERM_WRITE)) || desc->writeback > 1 ||
        (desc->shareability && desc->shareability != A840_BO_SHARE_OUTER &&
            desc->shareability != A840_BO_SHARE_INNER)))
        return A840_TRANSPORT_INVALID_ARG;

    A840BoMapReply reply = {0};
    if (desc->flags & A840_BO_MAP_CPU) {
        const A840TransportStatus status = acquire_cpu(wire, desc->handle, &reply);
        if (status != A840_TRANSPORT_OK) return status;
    }
    if (gpu) {
        const bool coherent = (wire->caps.abi_flags & NativeAdrenoParamsV2OwnedCoherent) != 0;
        const uint32_t share = coherent
            ? (desc->shareability ? desc->shareability : A840_BO_SHARE_OUTER)
            : A840_BO_SHARE_OUTER;
        const A840WireBoMap request = {
            desc->handle, desc->gpu_va, desc->permissions, coherent ? desc->writeback : 0,
            share, 0,
        };
        A840WireBoMapReply mapping = {0};
        const A840TransportStatus status = a840_wire_call(wire, A840_WIRE_BO_MAP,
            &request, sizeof(request), &mapping, sizeof(mapping));
        if (status != A840_TRANSPORT_OK || mapping.iova != desc->gpu_va) {
            if (reply.cpu_ptr) (void)release_cpu(wire, desc->handle, reply.cpu_token);
            return status == A840_TRANSPORT_OK ? A840_TRANSPORT_STATE_ERROR : status;
        }
        reply.gpu_va = mapping.iova;
    }
    *out = reply;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus bo_unmap(void *ctx, const A840BoUnmapDesc *desc)
{
    A840IokitWire *wire = ctx;
    if (!wire || !desc || !desc->handle || !desc->flags ||
        (desc->flags & ~(A840_BO_UNMAP_GPU | A840_BO_UNMAP_CPU)))
        return A840_TRANSPORT_INVALID_ARG;
    /* v2 owns GPU bindings until Method 4. Core releases them with BoFree. */
    if (!(desc->flags & A840_BO_UNMAP_CPU)) return A840_TRANSPORT_OK;
    if (!desc->cpu_token || !wire->unmap_cpu) return A840_TRANSPORT_INVALID_ARG;
    return release_cpu(wire, desc->handle, desc->cpu_token);
}

static A840TransportStatus bo_free(void *ctx, uint64_t handle)
{
    if (!handle) return A840_TRANSPORT_INVALID_ARG;
    const A840WireBoFree request = {handle};
    return a840_wire_call(ctx, A840_WIRE_BO_FREE, &request, sizeof(request), NULL, 0);
}

static const unsigned priority_rank[] = {4, 2, 1, 0, 3, 5};

static uint32_t foreground_priority(const WireQueue *queues)
{
    if (!queues) return A840_QUEUE_PRIORITY_NORMAL;
    uint32_t priority = queues->priority;
    for (const WireQueue *q = queues->next; q; q = q->next)
        if (priority_rank[q->priority] > priority_rank[priority]) priority = q->priority;
    return priority;
}

static A840TransportStatus set_priority(A840IokitWire *wire, uint32_t foreground)
{
    const A840WirePriority request = {foreground, A840_QUEUE_PRIORITY_DISABLED};
    return a840_wire_call(wire, A840_WIRE_SET_PRIORITY, &request, sizeof(request), NULL, 0);
}

static A840TransportStatus close_queue(A840IokitWire *wire, uint64_t handle)
{
    const A840WireQueue request = {handle};
    return a840_wire_call(wire, A840_WIRE_CLOSE_QUEUE, &request, sizeof(request), NULL, 0);
}

static A840TransportStatus queue_create(void *ctx, uint32_t priority, uint64_t *out)
{
    A840IokitWire *wire = ctx;
    if (!wire || !out || priority > A840_QUEUE_PRIORITY_REALTIME ||
        priority == A840_QUEUE_PRIORITY_DISABLED) return A840_TRANSPORT_INVALID_ARG;
    WireQueue *q = calloc(1, sizeof(*q));
    if (!q) return A840_TRANSPORT_OOM;
    (void)pthread_mutex_lock(&wire->submit_lock);
    const A840WireQueue request = {0};
    A840WireQueueReply reply = {0};
    A840TransportStatus status = a840_wire_call(wire, A840_WIRE_CREATE_QUEUE,
        &request, sizeof(request), &reply, sizeof(reply));
    if (status == A840_TRANSPORT_OK && !reply.queue) status = A840_TRANSPORT_STATE_ERROR;
    if (status == A840_TRANSPORT_OK) {
        *q = (WireQueue){wire->queues, reply.queue, priority};
        const uint32_t desired = foreground_priority(q);
        if (desired != foreground_priority(wire->queues)) status = set_priority(wire, desired);
        if (status == A840_TRANSPORT_OK) {
            wire->queues = q;
            *out = reply.queue;
        } else {
            (void)close_queue(wire, reply.queue);
        }
    }
    (void)pthread_mutex_unlock(&wire->submit_lock);
    if (status != A840_TRANSPORT_OK) free(q);
    return status;
}

static A840TransportStatus queue_destroy(void *ctx, uint64_t handle)
{
    A840IokitWire *wire = ctx;
    if (!wire || !handle) return A840_TRANSPORT_INVALID_ARG;
    (void)pthread_mutex_lock(&wire->submit_lock);
    WireQueue **link = &wire->queues;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    A840TransportStatus status = A840_TRANSPORT_NOT_FOUND;
    if (*link) {
        const uint32_t before = foreground_priority(wire->queues);
        WireQueue *q = *link;
        status = close_queue(wire, handle);
        if (status == A840_TRANSPORT_OK) {
            *link = q->next;
            free(q);
            const uint32_t desired = foreground_priority(wire->queues);
            if (desired != before) (void)set_priority(wire, desired);
        }
    }
    (void)pthread_mutex_unlock(&wire->submit_lock);
    return status;
}

static uint64_t timestamp(A840IokitWire *wire)
{
    A840WireTimestamp reply = {0};
    return a840_wire_call(wire, A840_WIRE_GET_TIMESTAMP, NULL, 0, &reply, sizeof(reply)) ==
        A840_TRANSPORT_OK ? reply.ticks : 0;
}

static A840TransportStatus wait_fence(void *ctx, uint64_t queue, uint64_t fence,
    uint64_t timeout_ns, uint64_t *out_ticks)
{
    A840IokitWire *wire = ctx;
    (void)queue; /* v2 fences use the connection's timeline. */
    if (!wire || !fence) return A840_TRANSPORT_INVALID_ARG;
    const A840WireWaitFence request = {fence, timeout_ns};
    const A840TransportStatus status = a840_wire_call(wire, A840_WIRE_WAIT_FENCE,
        &request, sizeof(request), NULL, 0);
    if (status == A840_TRANSPORT_OK && out_ticks) *out_ticks = timestamp(wire);
    return status;
}

static uint32_t fence_mask(uint32_t count)
{
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1u;
}

static bool fences_satisfied(const struct NativeAdrenoAbiWaitAnyV3 *request, uint32_t mask)
{
    return request->flags & 1u ? mask == fence_mask(request->count) : mask != 0;
}

static bool monotonic_ns(uint64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return false;
    *out = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

static A840TransportStatus poll_v2(A840IokitWire *wire,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *out)
{
    uint64_t start = 0;
    if (request->timeout_ns && !monotonic_ns(&start)) return A840_TRANSPORT_STATE_ERROR;
    uint32_t mask = 0;
    const uint64_t step_ns = UINT64_C(1000000);
    for (;;) {
        for (uint32_t i = 0; i < request->count; ++i) {
            const uint32_t bit = UINT32_C(1) << i;
            if (mask & bit) continue;
            const A840TransportStatus status = request->fences[i]
                ? wait_fence(wire, request->queues[i], request->fences[i], 0, NULL)
                : A840_TRANSPORT_OK;
            if (status == A840_TRANSPORT_OK) mask |= bit;
            else if (status != A840_TRANSPORT_TIMEOUT) return status;
        }
        const bool done = fences_satisfied(request, mask);
        uint64_t now = start;
        if (!done && request->timeout_ns && !monotonic_ns(&now))
            return A840_TRANSPORT_STATE_ERROR;
        if (done || now - start >= request->timeout_ns) {
            *out = (struct NativeAdrenoAbiWaitAnyReplyV3){mask, done ? 0u : 1u, timestamp(wire)};
            return done ? A840_TRANSPORT_OK : A840_TRANSPORT_TIMEOUT;
        }
        /* Yield to work that may satisfy the fence. On EINTR, poll again
         * against the original deadline instead of resetting the budget. */
        const uint64_t remaining = request->timeout_ns - (now - start);
        const struct timespec nap = {0, (long)(remaining < step_ns ? remaining : step_ns)};
        if (nanosleep(&nap, NULL) && errno != EINTR) return A840_TRANSPORT_STATE_ERROR;
    }
}

static A840TransportStatus wait_any(void *ctx, const struct NativeAdrenoAbiWaitAnyV3 *request,
    struct NativeAdrenoAbiWaitAnyReplyV3 *out)
{
    A840IokitWire *wire = ctx;
    if (!wire || !request || !out || !request->count || request->count > 32 ||
        (request->flags & ~UINT32_C(1))) return A840_TRANSPORT_INVALID_ARG;

    struct NativeAdrenoAbiWaitAnyReplyV3 reply = {0};
    const A840TransportStatus status = a840_wire_call(wire, NativeAdrenoMethodWaitAnyV3,
        request, sizeof(*request), &reply, sizeof(reply));
    if (status == A840_TRANSPORT_UNSUPPORTED) return poll_v2(wire, request, out);
    if (status != A840_TRANSPORT_OK && status != A840_TRANSPORT_TIMEOUT) return status;
    if (status == A840_TRANSPORT_TIMEOUT) reply.timed_out = 1;
    const bool done = fences_satisfied(request, reply.satisfied_mask);
    if ((reply.satisfied_mask & ~fence_mask(request->count)) || reply.timed_out > 1 ||
        done == (reply.timed_out != 0)) return A840_TRANSPORT_STATE_ERROR;
    *out = reply;
    return done ? A840_TRANSPORT_OK : A840_TRANSPORT_TIMEOUT;
}

static A840TransportStatus get_zero_page(void *ctx, struct NativeAdrenoAbiZeroPageReplyV3 *out)
{
    const A840IokitWire *wire = ctx;
    if (!wire || !out) return A840_TRANSPORT_INVALID_ARG;
    if (!(wire->caps.abi_flags & NativeAdrenoParamsV3SharedZeroPage))
        return A840_TRANSPORT_UNSUPPORTED;
    *out = wire->zero_page;
    return A840_TRANSPORT_OK;
}

static A840TransportStatus surface_meta(void *ctx, struct NativeAdrenoAbiSurfaceMetaV3 *in_out)
{
    if (!ctx || !in_out || !in_out->bo_handle || in_out->op > 1)
        return A840_TRANSPORT_INVALID_ARG;
    struct NativeAdrenoAbiSurfaceMetaV3 reply = {0};
    const A840TransportStatus status = a840_wire_call(ctx, NativeAdrenoMethodSurfaceMetaV3,
        in_out, sizeof(*in_out), &reply, sizeof(reply));
    if (status != A840_TRANSPORT_OK) return status;
    if (reply.bo_handle != in_out->bo_handle || reply.op != in_out->op)
        return A840_TRANSPORT_STATE_ERROR;
    *in_out = reply;
    return A840_TRANSPORT_OK;
}

static A840WireSubmitBuffer submit_ib(const A840IokitWire *wire, const A840SubmitDesc *desc,
    bool prefix, uint32_t i)
{
    const A840SubmitIb *ib = prefix && i == 0 ? &wire->context_ib : &desc->ibs[i - (uint32_t)prefix];
    return (A840WireSubmitBuffer){ib->bo_handle, ib->gpu_va, ib->bytes};
}

static A840TransportStatus submit_one(A840IokitWire *wire, const A840SubmitDesc *desc,
    bool prefix, uint32_t count, uint64_t *fence)
{
    A840WireSubmit request = {0};
    request.buffer_count = count;
    request.queue = desc->queue;
    request.wait_fence = desc->wait_fence;
    request.wait_timeout_ns = desc->wait_fence ? desc->wait_timeout_ns : 0;
    for (uint32_t i = 0; i < count; ++i)
        request.buffers[i] = submit_ib(wire, desc, prefix, i);
    A840WireSubmitReply reply = {0};
    const A840TransportStatus status = a840_wire_call(wire, A840_WIRE_SUBMIT,
        &request, sizeof(request), &reply, sizeof(reply));
    if (status == A840_TRANSPORT_OK) {
        wire->submitted = true;
        *fence = reply.fence;
    }
    return status;
}

static A840TransportStatus batch_call(A840IokitWire *wire, const A840WireSubmitBatch *request,
    A840WireSubmitBatchReply *reply)
{
    *reply = (A840WireSubmitBatchReply){0};
    return a840_wire_call(wire, A840_WIRE_SUBMIT_BATCH, request, sizeof(*request), reply, sizeof(*reply));
}

static A840TransportStatus submit_batch(A840IokitWire *wire, const A840SubmitDesc *desc,
    bool prefix, uint32_t count, uint64_t *fence)
{
    if (!(wire->caps.abi_flags & NativeAdrenoParamsV2SubmitBatch))
        return A840_TRANSPORT_UNSUPPORTED;
    if (desc->wait_fence) {
        const A840TransportStatus waited = wait_fence(wire, desc->queue, desc->wait_fence,
            desc->wait_timeout_ns, NULL);
        if (waited != A840_TRANSPORT_OK) return waited;
    }

    A840WireSubmitBatch request = {0};
    request.queue = desc->queue;
    request.total_buffers = count;
    request.action = A840_WIRE_BATCH_BEGIN;
    A840WireSubmitBatchReply reply;
    A840TransportStatus status = batch_call(wire, &request, &reply);
    if (status != A840_TRANSPORT_OK) return status;
    if (!reply.batch) return A840_TRANSPORT_STATE_ERROR;
    request.batch = reply.batch;
    if (reply.fence) status = A840_TRANSPORT_STATE_ERROR;

    request.action = A840_WIRE_BATCH_APPEND;
    while (status == A840_TRANSPORT_OK && request.offset < count) {
        const uint32_t remaining = count - request.offset;
        request.buffer_count = remaining < A840_WIRE_MAX_BUFFERS ? remaining : A840_WIRE_MAX_BUFFERS;
        memset(request.buffers, 0, sizeof(request.buffers));
        for (uint32_t i = 0; i < request.buffer_count; ++i)
            request.buffers[i] = submit_ib(wire, desc, prefix, request.offset + i);
        status = batch_call(wire, &request, &reply);
        if (status == A840_TRANSPORT_OK) {
            request.offset += request.buffer_count;
            if (reply.batch != request.batch || reply.fence) status = A840_TRANSPORT_STATE_ERROR;
        }
    }

    request.buffer_count = 0;
    memset(request.buffers, 0, sizeof(request.buffers));
    if (status == A840_TRANSPORT_OK) {
        request.action = A840_WIRE_BATCH_COMMIT;
        status = batch_call(wire, &request, &reply);
        if (status == A840_TRANSPORT_OK) {
            wire->submitted = true;
            if (reply.batch != request.batch) return A840_TRANSPORT_STATE_ERROR;
            *fence = reply.fence;
            return A840_TRANSPORT_OK;
        }
    }
    /* On non-Busy COMMIT errors the production client already consumed the
     * token. ABORT is safe in either case. Never replay a COMMIT. */
    request.action = A840_WIRE_BATCH_ABORT;
    (void)batch_call(wire, &request, &reply);
    return status;
}

static A840TransportStatus queue_submit(void *ctx, const A840SubmitDesc *desc, A840SubmitReply *out)
{
    A840IokitWire *wire = ctx;
    if (!wire || !desc || !out || !desc->ibs || !desc->ib_count ||
        desc->ib_count > A840_TRANSPORT_MAX_SUBMIT_IBS || (desc->reloc_count && !desc->relocs))
        return A840_TRANSPORT_INVALID_ARG;

    for (uint32_t i = 0; i < desc->ib_count; ++i) {
        const A840SubmitIb *ib = &desc->ibs[i];
        if (!ib->bo_handle || ib->bytes % 4 || ib->bytes > A840_CONTEXT_AMBLE_MAX_DWORDS * UINT64_C(4) ||
            !gpu_span(ib->gpu_va, ib->bytes, 4, wire->caps.input_bits))
            return A840_TRANSPORT_INVALID_ARG;
    }

    (void)pthread_mutex_lock(&wire->submit_lock);
    /* NativeAdrenoRing::emitPrologue resets CP_THREAD_CONTROL to BOTH before
     * every submission and emits CP_RESET_CONTEXT_STATE(ALL) on context switch.
     * Prepend the pre-encoded context IB on every submission (matching tu_init_hw). */
    const bool prefix = wire->context_bo != NULL;
    const uint32_t count = desc->ib_count + (uint32_t)prefix;
    uint64_t fence = 0;
    A840TransportStatus status;
    if (count > A840_TRANSPORT_MAX_SUBMIT_IBS) status = A840_TRANSPORT_OUT_OF_BOUNDS;
    else if (count <= A840_WIRE_MAX_BUFFERS) status = submit_one(wire, desc, prefix, count, &fence);
    else status = submit_batch(wire, desc, prefix, count, &fence);
    (void)pthread_mutex_unlock(&wire->submit_lock);

    if (status != A840_TRANSPORT_OK) return status;
    if (!fence) return A840_TRANSPORT_STATE_ERROR;
    *out = (A840SubmitReply){.fence = fence};
    return A840_TRANSPORT_OK;
}

typedef struct ContextImage {
    uint32_t prefix[A840_CTX_INIT_MAX_DWORDS];
    uint32_t br[A840_BIN_PREAMBLE_MAX_DWORDS];
    uint32_t bv[A840_BIN_PREAMBLE_MAX_DWORDS];
} ContextImage;

static A840TransportStatus create_context(A840IokitWire *wire, A840RtDevice *device)
{
    const uint64_t zero = a840_rt_device_zero_page_va(device);
    if (device->zero_page_bytes < A840_TRANSPORT_ZERO_PAGE_BYTES ||
        !gpu_span(zero, A840_TRANSPORT_ZERO_PAGE_BYTES,
            A840_TRANSPORT_ZERO_PAGE_BYTES, wire->caps.input_bits))
        return A840_TRANSPORT_STATE_ERROR;

    A840ContextInitParams params = {
        .border_color_gpu_va = a840_rt_device_border_color_va(device),
        .border_color_bo = a840_rt_device_border_color_bo(device),
        .vis_stream_count = 1, /* Fixed sysmem first frame, as in tu_cmd_buffer.cc:2492-2498. */
        .enable_early_preamble_reset = 1,
    };
    if (!params.border_color_bo) return A840_TRANSPORT_STATE_ERROR;

    /* The zero page is read-only data. Emit executable ambles into their own
     * regions of the live context BO instead of pointing the CP at zeroes. */
    ContextImage image = {0};
    A840CmdStream br, bv, prefix;
    if (a840_cs_init(&br, image.br, A840_BIN_PREAMBLE_MAX_DWORDS, NULL, 0) != A840_EMIT_OK ||
        a840_cs_init(&bv, image.bv, A840_BIN_PREAMBLE_MAX_DWORDS, NULL, 0) != A840_EMIT_OK ||
        a840_cs_init(&prefix, image.prefix, A840_CTX_INIT_MAX_DWORDS, NULL, 0) != A840_EMIT_OK ||
        a840_emit_bin_preamble(&br, &params, 0) != A840_EMIT_OK ||
        a840_emit_bin_preamble(&bv, &params, 1) != A840_EMIT_OK)
        return A840_TRANSPORT_STATE_ERROR;

    const A840RtBufferDesc descriptor = {
        .size_bytes = sizeof(image), .usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE,
    };
    A840RtBuffer *bo = NULL;
    const A840RtStatus status = a840_rt_buffer_create(device, &descriptor, &bo);
    if (status != A840_RT_OK)
        return status == A840_RT_OOM ? A840_TRANSPORT_OOM : A840_TRANSPORT_STATE_ERROR;

    params.bin_preamble_br_va = bo->gpu_va + offsetof(ContextImage, br);
    params.bin_preamble_bv_va = bo->gpu_va + offsetof(ContextImage, bv);
    params.bin_preamble_br_dwords = (uint32_t)br.used_dwords;
    params.bin_preamble_bv_dwords = (uint32_t)bv.used_dwords;
    if (a840_emit_context_init(&prefix, &params) != A840_EMIT_OK ||
        a840_rt_buffer_write(bo, 0, &image, sizeof(image)) != A840_RT_OK) {
        a840_rt_buffer_release(bo);
        return A840_TRANSPORT_STATE_ERROR;
    }

    wire->context_ib = (A840SubmitIb){bo->bo_handle, bo->gpu_va, prefix.used_dwords * sizeof(uint32_t)};
    wire->context_bo = bo;
    return A840_TRANSPORT_OK;
}

A840TransportStatus a840_wire_attach_context(A840IokitWire *wire, A840RtDevice *device)
{
    if (!wire || !device || device->transport.ctx != wire ||
        device->transport.ops != a840_wire_interface(wire).ops)
        return A840_TRANSPORT_INVALID_ARG;
    (void)pthread_mutex_lock(&wire->submit_lock);
    const A840TransportStatus status = wire->context_bo || wire->submitted
        ? A840_TRANSPORT_STATE_ERROR : create_context(wire, device);
    (void)pthread_mutex_unlock(&wire->submit_lock);
    return status;
}

void a840_wire_detach_context(A840IokitWire *wire)
{
    if (!wire) return;
    (void)pthread_mutex_lock(&wire->submit_lock);
    A840RtBuffer *bo = wire->context_bo;
    wire->context_bo = NULL;
    wire->context_ib = (A840SubmitIb){0};
    (void)pthread_mutex_unlock(&wire->submit_lock);
    if (bo) a840_rt_buffer_release(bo);
}

static const A840BackendTransportOps backend_ops = {
    .query_caps = query_caps, .bo_alloc = bo_alloc, .bo_import = bo_import, .bo_map = bo_map,
    .bo_unmap = bo_unmap, .bo_free = bo_free, .queue_create = queue_create, .queue_destroy = queue_destroy,
    .queue_submit = queue_submit, .fence_wait = wait_fence, .wait_any_v3 = wait_any,
    .get_zero_page_v3 = get_zero_page, .surface_meta_v3 = surface_meta,
};

A840BackendTransport a840_wire_interface(A840IokitWire *wire)
{
    return (A840BackendTransport){wire ? &backend_ops : NULL, wire};
}

void a840_wire_destroy(A840IokitWire *wire)
{
    if (!wire) return;
    /* Owners drain queues and detach while their runtime device is alive. */
    a840_wire_detach_context(wire);
    while (wire->queues) {
        WireQueue *q = wire->queues;
        wire->queues = q->next;
        (void)close_queue(wire, q->handle);
        free(q);
    }
    (void)pthread_mutex_destroy(&wire->submit_lock);
    free(wire);
}
