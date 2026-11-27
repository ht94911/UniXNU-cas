/* SPDX-License-Identifier: MIT */
#include "a840_runtime_device.h"

#include <stdlib.h>
#include <string.h>

static uint64_t g_next_device_id = 0u;

static uint64_t va_limit_48(void)
{
    return UINT64_C(1) << A840_RT_VA_BITS;
}

static bool is_power_of_two_u64(uint64_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static uint32_t log2_floor_u64(uint64_t v)
{
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        r++;
    }
    return r;
}

static bool align_up_u64(uint64_t val, uint64_t align, uint64_t *out)
{
    uint64_t rem;
    if (!align || !is_power_of_two_u64(align)) {
        return false;
    }
    rem = val & (align - 1u);
    if (rem == 0u) {
        *out = val;
        return true;
    }
    if (val > UINT64_MAX - (align - rem)) {
        return false;
    }
    *out = val + (align - rem);
    return true;
}

A840RtStatus a840_rt_status_from_emit(A840EmitStatus st)
{
    switch (st) {
    case A840_EMIT_OK:
        return A840_RT_OK;
    case A840_EMIT_INVALID_ARG:
    case A840_EMIT_ALIAS:
        return A840_RT_INVALID_ARG;
    case A840_EMIT_CAPACITY:
        return A840_RT_CAPACITY;
    case A840_EMIT_RANGE_ERROR:
        return A840_RT_RANGE_ERROR;
    case A840_EMIT_ALIGNMENT:
        return A840_RT_ALIGNMENT;
    case A840_EMIT_UNSUPPORTED:
        return A840_RT_UNSUPPORTED;
    default:
        return A840_RT_EMIT_ERROR;
    }
}

/* ---------- Deterministic 48-bit GPU VA Interval Allocator ---------- */

static A840RtStatus ensure_range_cap(A840VaRange **arr, size_t *cap, size_t needed)
{
    size_t new_cap;
    A840VaRange *grown;

    if (needed <= *cap) {
        return A840_RT_OK;
    }
    new_cap = *cap ? *cap : 16u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / sizeof(A840VaRange)) {
        return A840_RT_OOM;
    }
    grown = (A840VaRange *)realloc(*arr, new_cap * sizeof(A840VaRange));
    if (!grown) {
        return A840_RT_OOM;
    }
    *arr = grown;
    *cap = new_cap;
    return A840_RT_OK;
}

A840RtStatus a840_va_allocator_init(A840VaAllocator *alloc, uint64_t base, uint64_t size)
{
    A840RtStatus st;

    if (!alloc) {
        return A840_RT_INVALID_ARG;
    }
    memset(alloc, 0, sizeof(*alloc));

    if (!base || !size ||
        (base % A840_RT_PAGE_BYTES) != 0u ||
        (size % A840_RT_PAGE_BYTES) != 0u) {
        return A840_RT_ALIGNMENT;
    }
    if (base >= va_limit_48() || size > va_limit_48() - base) {
        return A840_RT_RANGE_ERROR;
    }

    st = ensure_range_cap(&alloc->free_ranges, &alloc->free_cap, 16u);
    if (st != A840_RT_OK) {
        return st;
    }
    st = ensure_range_cap(&alloc->alloc_ranges, &alloc->alloc_cap, 16u);
    if (st != A840_RT_OK) {
        free(alloc->free_ranges);
        memset(alloc, 0, sizeof(*alloc));
        return st;
    }

    alloc->base = base;
    alloc->total_size = size;
    alloc->free_ranges[0].start = base;
    alloc->free_ranges[0].size = size;
    alloc->free_count = 1u;
    return A840_RT_OK;
}

void a840_va_allocator_destroy(A840VaAllocator *alloc)
{
    if (!alloc) {
        return;
    }
    free(alloc->free_ranges);
    free(alloc->alloc_ranges);
    memset(alloc, 0, sizeof(*alloc));
}

A840RtStatus a840_va_allocator_alloc(A840VaAllocator *alloc,
                                     uint64_t size,
                                     uint64_t alignment,
                                     uint64_t *out_va)
{
    uint64_t aligned_size;
    size_t i;

    if (!alloc || !out_va || !size || !alloc->total_size) {
        return A840_RT_INVALID_ARG;
    }
    if (alignment == 0u) {
        alignment = A840_RT_PAGE_BYTES;
    }
    if (!is_power_of_two_u64(alignment) || alignment < A840_RT_PAGE_BYTES) {
        return A840_RT_ALIGNMENT;
    }
    if (!align_up_u64(size, A840_RT_PAGE_BYTES, &aligned_size) ||
        aligned_size > alloc->total_size) {
        return A840_RT_RANGE_ERROR;
    }

    for (i = 0; i < alloc->free_count; ++i) {
        uint64_t seg_start = alloc->free_ranges[i].start;
        uint64_t seg_size = alloc->free_ranges[i].size;
        uint64_t seg_end = seg_start + seg_size;
        uint64_t cand = 0u;
        uint64_t cand_end;
        bool has_prefix;
        bool has_suffix;
        A840RtStatus st;

        if (!align_up_u64(seg_start, alignment, &cand)) {
            continue;
        }
        if (cand >= seg_end || cand >= va_limit_48()) {
            continue;
        }
        if (aligned_size > seg_end - cand || aligned_size > va_limit_48() - cand) {
            continue;
        }
        cand_end = cand + aligned_size;
        has_prefix = cand > seg_start;
        has_suffix = cand_end < seg_end;

        /* Pre-grow both arrays so failure leaves state untouched. */
        if (has_prefix && has_suffix) {
            st = ensure_range_cap(&alloc->free_ranges, &alloc->free_cap, alloc->free_count + 1u);
            if (st != A840_RT_OK) {
                return st;
            }
        }
        st = ensure_range_cap(&alloc->alloc_ranges, &alloc->alloc_cap, alloc->alloc_count + 1u);
        if (st != A840_RT_OK) {
            return st;
        }

        if (!has_prefix && !has_suffix) {
            if (i + 1u < alloc->free_count) {
                memmove(&alloc->free_ranges[i], &alloc->free_ranges[i + 1u],
                        (alloc->free_count - i - 1u) * sizeof(A840VaRange));
            }
            alloc->free_count--;
        } else if (has_prefix && !has_suffix) {
            alloc->free_ranges[i].size = cand - seg_start;
        } else if (!has_prefix && has_suffix) {
            alloc->free_ranges[i].start = cand_end;
            alloc->free_ranges[i].size = seg_end - cand_end;
        } else {
            if (i + 1u < alloc->free_count) {
                memmove(&alloc->free_ranges[i + 2u], &alloc->free_ranges[i + 1u],
                        (alloc->free_count - i - 1u) * sizeof(A840VaRange));
            }
            alloc->free_ranges[i].start = seg_start;
            alloc->free_ranges[i].size = cand - seg_start;
            alloc->free_ranges[i + 1u].start = cand_end;
            alloc->free_ranges[i + 1u].size = seg_end - cand_end;
            alloc->free_count++;
        }

        alloc->alloc_ranges[alloc->alloc_count].start = cand;
        alloc->alloc_ranges[alloc->alloc_count].size = aligned_size;
        alloc->alloc_count++;
        alloc->used_bytes += aligned_size;

        *out_va = cand;
        return A840_RT_OK;
    }

    return A840_RT_RANGE_ERROR;
}

A840RtStatus a840_va_allocator_free(A840VaAllocator *alloc, uint64_t va, uint64_t size)
{
    uint64_t aligned_size;
    size_t idx = SIZE_MAX;
    size_t pos;
    size_t i;
    A840RtStatus st;

    if (!alloc || !va || !size) {
        return A840_RT_INVALID_ARG;
    }
    if (!align_up_u64(size, A840_RT_PAGE_BYTES, &aligned_size)) {
        return A840_RT_RANGE_ERROR;
    }

    for (i = 0; i < alloc->alloc_count; ++i) {
        if (alloc->alloc_ranges[i].start == va &&
            alloc->alloc_ranges[i].size == aligned_size) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        return A840_RT_INVALID_ARG;
    }

    st = ensure_range_cap(&alloc->free_ranges, &alloc->free_cap, alloc->free_count + 1u);
    if (st != A840_RT_OK) {
        return st;
    }

    /* Remove from alloc list. */
    if (idx + 1u < alloc->alloc_count) {
        memmove(&alloc->alloc_ranges[idx], &alloc->alloc_ranges[idx + 1u],
                (alloc->alloc_count - idx - 1u) * sizeof(A840VaRange));
    }
    alloc->alloc_count--;
    alloc->used_bytes -= aligned_size;

    /* Insert into sorted free list. */
    pos = 0u;
    while (pos < alloc->free_count && alloc->free_ranges[pos].start < va) {
        pos++;
    }
    if (pos < alloc->free_count) {
        memmove(&alloc->free_ranges[pos + 1u], &alloc->free_ranges[pos],
                (alloc->free_count - pos) * sizeof(A840VaRange));
    }
    alloc->free_ranges[pos].start = va;
    alloc->free_ranges[pos].size = aligned_size;
    alloc->free_count++;

    /* Coalesce with next neighbor. */
    if (pos + 1u < alloc->free_count &&
        alloc->free_ranges[pos].start + alloc->free_ranges[pos].size ==
            alloc->free_ranges[pos + 1u].start) {
        alloc->free_ranges[pos].size += alloc->free_ranges[pos + 1u].size;
        if (pos + 2u < alloc->free_count) {
            memmove(&alloc->free_ranges[pos + 1u], &alloc->free_ranges[pos + 2u],
                    (alloc->free_count - pos - 2u) * sizeof(A840VaRange));
        }
        alloc->free_count--;
    }
    /* Coalesce with previous neighbor. */
    if (pos > 0u &&
        alloc->free_ranges[pos - 1u].start + alloc->free_ranges[pos - 1u].size ==
            alloc->free_ranges[pos].start) {
        alloc->free_ranges[pos - 1u].size += alloc->free_ranges[pos].size;
        if (pos + 1u < alloc->free_count) {
            memmove(&alloc->free_ranges[pos], &alloc->free_ranges[pos + 1u],
                    (alloc->free_count - pos - 1u) * sizeof(A840VaRange));
        }
        alloc->free_count--;
    }

    return A840_RT_OK;
}

uint64_t a840_va_allocator_used_bytes(const A840VaAllocator *alloc)
{
    return alloc ? alloc->used_bytes : 0u;
}

uint64_t a840_va_allocator_free_bytes(const A840VaAllocator *alloc)
{
    return alloc ? (alloc->total_size - alloc->used_bytes) : 0u;
}

size_t a840_va_allocator_free_block_count(const A840VaAllocator *alloc)
{
    return alloc ? alloc->free_count : 0u;
}

/* ---------- Runtime Device ---------- */

static bool transport_valid(const A840BackendTransport *t)
{
    if (!t || !t->ops) {
        return false;
    }
    return t->ops->query_caps && t->ops->bo_alloc && t->ops->bo_map &&
           t->ops->bo_unmap && t->ops->bo_free && t->ops->queue_create &&
           t->ops->queue_destroy && t->ops->queue_submit && t->ops->fence_wait;
}

A840RtStatus a840_rt_device_create(const A840BackendTransport *transport,
                                   uint32_t initial_route_categories,
                                   A840RtDevice **out_device)
{
    A840RtDevice *dev;
    A840TransportStatus tst;
    A840RtStatus st;
    A840BoAllocDesc alloc_desc;
    A840BoMapDesc map_desc;
    A840BoMapReply map_reply;

    if (!out_device) {
        return A840_RT_INVALID_ARG;
    }
    *out_device = NULL;
    if (!transport_valid(transport)) {
        return A840_RT_INVALID_ARG;
    }

    dev = (A840RtDevice *)calloc(1u, sizeof(*dev));
    if (!dev) {
        return A840_RT_OOM;
    }
    dev->transport = *transport;

    tst = dev->transport.ops->query_caps(dev->transport.ctx, &dev->transport_caps);
    if (tst != A840_TRANSPORT_OK) {
        free(dev);
        return A840_RT_TRANSPORT_ERROR;
    }

    dev->chip_facts = *a840_chip_facts_kaanapali_default();
    if (a840_chip_facts_apply_runtime(&dev->chip_facts,
                                      (uint64_t)dev->transport_caps.chip_id,
                                      dev->transport_caps.gmem_bytes,
                                      dev->transport_caps.highest_bank_bit) != 0) {
        free(dev);
        return A840_RT_UNSUPPORTED;
    }
    if (a840_chip_facts_validate(&dev->chip_facts, NULL, 0u) != 0) {
        free(dev);
        return A840_RT_UNSUPPORTED;
    }

    st = a840_va_allocator_init(&dev->va_alloc,
                                dev->transport_caps.va_start,
                                dev->transport_caps.va_size);
    if (st != A840_RT_OK) {
        free(dev);
        return st;
    }

    a840_router_init(&dev->router, initial_route_categories);

    /* Acquire global 64 KiB read-only zero page (ABI v3 preferred, v2 fallback BO). */
    if ((dev->transport_caps.abi_flags & NativeAdrenoParamsV3SharedZeroPage) != 0u &&
        dev->transport.ops->get_zero_page_v3 != NULL) {
        struct NativeAdrenoAbiZeroPageReplyV3 zp_reply;
        memset(&zp_reply, 0, sizeof(zp_reply));
        tst = dev->transport.ops->get_zero_page_v3(dev->transport.ctx, &zp_reply);
        if (tst != A840_TRANSPORT_OK || !zp_reply.gpu_va ||
            (zp_reply.gpu_va % A840_RT_LARGE_ALIGN_BYTES) != 0u ||
            zp_reply.bytes < A840_RT_LARGE_ALIGN_BYTES) {
            a840_va_allocator_destroy(&dev->va_alloc);
            free(dev);
            return A840_RT_TRANSPORT_ERROR;
        }
        dev->zero_page_va = zp_reply.gpu_va;
        dev->zero_page_bytes = zp_reply.bytes;
        dev->zero_page_owned = false;
    } else {
        st = a840_va_allocator_alloc(&dev->va_alloc, A840_RT_LARGE_ALIGN_BYTES,
                                     A840_RT_LARGE_ALIGN_BYTES, &dev->zero_page_va);
        if (st != A840_RT_OK) {
            a840_va_allocator_destroy(&dev->va_alloc);
            free(dev);
            return st;
        }
        memset(&alloc_desc, 0, sizeof(alloc_desc));
        alloc_desc.bytes = A840_RT_LARGE_ALIGN_BYTES;
        alloc_desc.align_log2 = 16u;
        tst = dev->transport.ops->bo_alloc(dev->transport.ctx, &alloc_desc, &dev->zero_page_bo);
        if (tst != A840_TRANSPORT_OK) {
            a840_va_allocator_free(&dev->va_alloc, dev->zero_page_va, A840_RT_LARGE_ALIGN_BYTES);
            a840_va_allocator_destroy(&dev->va_alloc);
            free(dev);
            return A840_RT_TRANSPORT_ERROR;
        }
        memset(&map_desc, 0, sizeof(map_desc));
        map_desc.handle = dev->zero_page_bo;
        map_desc.gpu_va = dev->zero_page_va;
        map_desc.flags = A840_BO_MAP_GPU;
        map_desc.permissions = A840_BO_PERM_READ;
        map_desc.shareability = A840_BO_SHARE_INNER;
        tst = dev->transport.ops->bo_map(dev->transport.ctx, &map_desc, &map_reply);
        if (tst != A840_TRANSPORT_OK) {
            dev->transport.ops->bo_free(dev->transport.ctx, dev->zero_page_bo);
            a840_va_allocator_free(&dev->va_alloc, dev->zero_page_va, A840_RT_LARGE_ALIGN_BYTES);
            a840_va_allocator_destroy(&dev->va_alloc);
            free(dev);
            return A840_RT_TRANSPORT_ERROR;
        }
        dev->zero_page_bytes = A840_RT_LARGE_ALIGN_BYTES;
        dev->zero_page_owned = true;
    }

    /* Allocate global Border Color BO (64 KiB, 64 KiB aligned, GPU RO + CPU RW). */
    st = a840_va_allocator_alloc(&dev->va_alloc, A840_RT_BORDER_COLOR_BYTES,
                                 A840_RT_LARGE_ALIGN_BYTES, &dev->border_color_va);
    if (st != A840_RT_OK) {
        a840_rt_device_destroy(dev);
        return st;
    }
    memset(&alloc_desc, 0, sizeof(alloc_desc));
    alloc_desc.bytes = A840_RT_BORDER_COLOR_BYTES;
    alloc_desc.align_log2 = 16u;
    tst = dev->transport.ops->bo_alloc(dev->transport.ctx, &alloc_desc, &dev->border_color_bo);
    if (tst != A840_TRANSPORT_OK) {
        a840_va_allocator_free(&dev->va_alloc, dev->border_color_va, A840_RT_BORDER_COLOR_BYTES);
        dev->border_color_va = 0u;
        a840_rt_device_destroy(dev);
        return A840_RT_TRANSPORT_ERROR;
    }
    memset(&map_desc, 0, sizeof(map_desc));
    map_desc.handle = dev->border_color_bo;
    map_desc.gpu_va = dev->border_color_va;
    map_desc.flags = A840_BO_MAP_GPU | A840_BO_MAP_CPU;
    map_desc.permissions = A840_BO_PERM_READ;
    map_desc.writeback = 1u;
    map_desc.shareability = A840_BO_SHARE_INNER;
    tst = dev->transport.ops->bo_map(dev->transport.ctx, &map_desc, &map_reply);
    if (tst != A840_TRANSPORT_OK) {
        dev->transport.ops->bo_free(dev->transport.ctx, dev->border_color_bo);
        dev->border_color_bo = 0u;
        a840_va_allocator_free(&dev->va_alloc, dev->border_color_va, A840_RT_BORDER_COLOR_BYTES);
        dev->border_color_va = 0u;
        a840_rt_device_destroy(dev);
        return A840_RT_TRANSPORT_ERROR;
    }
    dev->border_color_bytes = A840_RT_BORDER_COLOR_BYTES;
    dev->border_color_cpu = map_reply.cpu_ptr;
    dev->border_color_cpu_token = map_reply.cpu_token;

    dev->device_id = ++g_next_device_id;
    *out_device = dev;
    return A840_RT_OK;
}

void a840_rt_device_destroy(A840RtDevice *device)
{
    A840BoUnmapDesc unmap;

    if (!device) {
        return;
    }
    if (device->border_color_bo) {
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = device->border_color_bo;
        unmap.flags = A840_BO_UNMAP_GPU |
                      (device->border_color_cpu ? A840_BO_UNMAP_CPU : 0u);
        unmap.cpu_token = device->border_color_cpu_token;
        device->transport.ops->bo_unmap(device->transport.ctx, &unmap);
        device->transport.ops->bo_free(device->transport.ctx, device->border_color_bo);
        if (device->border_color_va) {
            a840_va_allocator_free(&device->va_alloc, device->border_color_va,
                                   A840_RT_BORDER_COLOR_BYTES);
        }
    }
    if (device->zero_page_owned && device->zero_page_bo) {
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = device->zero_page_bo;
        unmap.flags = A840_BO_UNMAP_GPU;
        device->transport.ops->bo_unmap(device->transport.ctx, &unmap);
        device->transport.ops->bo_free(device->transport.ctx, device->zero_page_bo);
        if (device->zero_page_va) {
            a840_va_allocator_free(&device->va_alloc, device->zero_page_va,
                                   A840_RT_LARGE_ALIGN_BYTES);
        }
    }
    a840_va_allocator_destroy(&device->va_alloc);
    free(device);
}

const A840ChipFacts *a840_rt_device_chip_facts(const A840RtDevice *device)
{
    return device ? &device->chip_facts : NULL;
}

const A840TransportCaps *a840_rt_device_caps(const A840RtDevice *device)
{
    return device ? &device->transport_caps : NULL;
}

A840DualPathRouter *a840_rt_device_router(A840RtDevice *device)
{
    return device ? &device->router : NULL;
}

uint64_t a840_rt_device_zero_page_va(const A840RtDevice *device)
{
    return device ? device->zero_page_va : 0u;
}

uint64_t a840_rt_device_border_color_va(const A840RtDevice *device)
{
    return device ? device->border_color_va : 0u;
}

uint64_t a840_rt_device_border_color_bo(const A840RtDevice *device)
{
    return device ? device->border_color_bo : 0u;
}

bool a840_rt_device_owns_buffer(const A840RtDevice *device, const A840RtBuffer *buffer)
{
    return device != NULL && buffer != NULL &&
           buffer->device == device && buffer->device_id == device->device_id;
}

bool a840_rt_device_owns_texture(const A840RtDevice *device, const A840RtTexture *texture)
{
    return device != NULL && texture != NULL &&
           texture->device == device && texture->device_id == device->device_id;
}

/* ---------- Runtime Buffer ---------- */

A840RtStatus a840_rt_buffer_create(A840RtDevice *device,
                                   const A840RtBufferDesc *desc,
                                   A840RtBuffer **out_buffer)
{
    uint64_t align;
    uint64_t alloc_bytes;
    uint32_t align_log2;
    uint64_t gpu_va = 0u;
    uint64_t bo_handle = 0u;
    A840RtBuffer *buf;
    A840RtStatus st;
    A840TransportStatus tst;
    A840BoMapDesc map_desc;
    A840BoMapReply map_reply;

    if (!out_buffer) {
        return A840_RT_INVALID_ARG;
    }
    *out_buffer = NULL;
    if (!device || !desc || desc->size_bytes == 0u) {
        return A840_RT_INVALID_ARG;
    }

    align = desc->alignment ? desc->alignment : A840_RT_PAGE_BYTES;
    if (!is_power_of_two_u64(align) || align < A840_RT_PAGE_BYTES) {
        return A840_RT_ALIGNMENT;
    }
    align_log2 = log2_floor_u64(align);
    if (align_log2 > 24u) {
        return A840_RT_RANGE_ERROR;
    }
    if (!align_up_u64(desc->size_bytes, A840_RT_PAGE_BYTES, &alloc_bytes) ||
        alloc_bytes > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }
    if (desc->host_import) {
        if (!device->transport.ops->bo_import) {
            return A840_RT_UNSUPPORTED;
        }
        if (!desc->host_ptr || ((uintptr_t)desc->host_ptr % A840_RT_PAGE_BYTES) != 0u) {
            return A840_RT_ALIGNMENT;
        }
    }

    st = a840_va_allocator_alloc(&device->va_alloc, alloc_bytes, align, &gpu_va);
    if (st != A840_RT_OK) {
        return st;
    }

    if (desc->host_import) {
        A840BoImportDesc imp;
        imp.host_ptr = desc->host_ptr;
        imp.bytes = alloc_bytes;
        tst = device->transport.ops->bo_import(device->transport.ctx, &imp, &bo_handle);
    } else {
        A840BoAllocDesc alloc_desc;
        alloc_desc.bytes = alloc_bytes;
        alloc_desc.align_log2 = align_log2;
        alloc_desc.contiguous = 0u;
        tst = device->transport.ops->bo_alloc(device->transport.ctx, &alloc_desc, &bo_handle);
    }
    if (tst != A840_TRANSPORT_OK) {
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return (tst == A840_TRANSPORT_UNSUPPORTED) ? A840_RT_UNSUPPORTED : A840_RT_TRANSPORT_ERROR;
    }

    memset(&map_desc, 0, sizeof(map_desc));
    memset(&map_reply, 0, sizeof(map_reply));
    map_desc.handle = bo_handle;
    map_desc.gpu_va = gpu_va;
    map_desc.flags = A840_BO_MAP_GPU;
    if (!desc->host_import && (desc->usage_flags & A840_BUF_USAGE_CPU_VISIBLE) != 0u) {
        map_desc.flags |= A840_BO_MAP_CPU;
    }
    map_desc.permissions = A840_BO_PERM_READ |
                           ((desc->usage_flags & A840_BUF_USAGE_WRITE) != 0u ? A840_BO_PERM_WRITE : 0u);
    map_desc.writeback = (device->transport_caps.abi_flags & NativeAdrenoParamsV2OwnedCoherent) ? 1u : 0u;
    map_desc.shareability = A840_BO_SHARE_INNER;

    tst = device->transport.ops->bo_map(device->transport.ctx, &map_desc, &map_reply);
    if (tst != A840_TRANSPORT_OK) {
        device->transport.ops->bo_free(device->transport.ctx, bo_handle);
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return A840_RT_TRANSPORT_ERROR;
    }

    buf = (A840RtBuffer *)calloc(1u, sizeof(*buf));
    if (!buf) {
        A840BoUnmapDesc unmap;
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = bo_handle;
        unmap.flags = A840_BO_UNMAP_GPU |
                      ((map_desc.flags & A840_BO_MAP_CPU) ? A840_BO_UNMAP_CPU : 0u);
        unmap.cpu_token = map_reply.cpu_token;
        device->transport.ops->bo_unmap(device->transport.ctx, &unmap);
        device->transport.ops->bo_free(device->transport.ctx, bo_handle);
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return A840_RT_OOM;
    }

    buf->device = device;
    buf->device_id = device->device_id;
    buf->refcount = 1u;
    buf->bo_handle = bo_handle;
    buf->gpu_va = gpu_va;
    buf->size_bytes = desc->size_bytes;
    buf->alloc_bytes = alloc_bytes;
    buf->alignment = align;
    buf->usage_flags = desc->usage_flags;
    buf->imported = desc->host_import;
    if (desc->host_import) {
        buf->cpu_ptr = desc->host_ptr;
        buf->cpu_mapped = false;
    } else {
        buf->cpu_ptr = map_reply.cpu_ptr;
        buf->cpu_token = map_reply.cpu_token;
        buf->cpu_mapped = (map_desc.flags & A840_BO_MAP_CPU) != 0u;
    }

    device->live_buffers++;
    *out_buffer = buf;
    return A840_RT_OK;
}

void a840_rt_buffer_retain(A840RtBuffer *buffer)
{
    if (buffer && buffer->refcount > 0u) {
        buffer->refcount++;
    }
}

void a840_rt_buffer_release(A840RtBuffer *buffer)
{
    A840RtDevice *dev;
    A840BoUnmapDesc unmap;

    if (!buffer || buffer->refcount == 0u) {
        return;
    }
    buffer->refcount--;
    if (buffer->refcount > 0u) {
        return;
    }

    dev = buffer->device;
    memset(&unmap, 0, sizeof(unmap));
    unmap.handle = buffer->bo_handle;
    unmap.flags = A840_BO_UNMAP_GPU | (buffer->cpu_mapped ? A840_BO_UNMAP_CPU : 0u);
    unmap.cpu_token = buffer->cpu_token;
    dev->transport.ops->bo_unmap(dev->transport.ctx, &unmap);
    dev->transport.ops->bo_free(dev->transport.ctx, buffer->bo_handle);
    a840_va_allocator_free(&dev->va_alloc, buffer->gpu_va, buffer->alloc_bytes);
    if (dev->live_buffers > 0u) {
        dev->live_buffers--;
    }
    free(buffer);
}

A840RtStatus a840_rt_buffer_write(A840RtBuffer *buffer,
                                  uint64_t offset,
                                  const void *data,
                                  size_t bytes)
{
    if (!buffer || (!data && bytes > 0u)) {
        return A840_RT_INVALID_ARG;
    }
    if (!buffer->cpu_ptr) {
        return A840_RT_STATE_ERROR;
    }
    if (offset > buffer->size_bytes || (uint64_t)bytes > buffer->size_bytes - offset) {
        return A840_RT_RANGE_ERROR;
    }
    if (bytes > 0u) {
        memcpy((uint8_t *)buffer->cpu_ptr + (size_t)offset, data, bytes);
    }
    return A840_RT_OK;
}

/* ---------- Runtime Texture ---------- */

static uint32_t texture_element_bytes(const A840FormatCaps *caps)
{
    if ((caps->flags & A840_FORMAT_SEPARATE_DEPTH_STENCIL_PLANES) != 0u) {
        /* Separate Z32F plane on Adreno hardware (4 bytes/pixel). */
        return 4u;
    }
    return caps->block_bits / 8u;
}

static void compute_tiled_alignment(const A840FormatCaps *caps,
                                    uint32_t samples,
                                    uint32_t *out_pitch_align_log2,
                                    uint32_t *out_row_align)
{
    uint32_t elem_bytes = texture_element_bytes(caps);
    uint32_t eff_cpp = elem_bytes * samples;

    if ((caps->flags & A840_FORMAT_COMPRESSED) != 0u) {
        *out_pitch_align_log2 = 8u;
        *out_row_align = 16u;
        return;
    }

    if (eff_cpp == 1u ||
        (samples == 1u && (caps->flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) != 0u)) {
        *out_pitch_align_log2 = 7u;
        *out_row_align = 32u;
    } else if (eff_cpp == 2u) {
        *out_pitch_align_log2 = 8u;
        *out_row_align = 16u;
    } else {
        *out_pitch_align_log2 = 6u + log2_floor_u64((uint64_t)eff_cpp);
        *out_row_align = 16u;
    }
}

A840RtStatus a840_rt_texture_create(A840RtDevice *device,
                                    const A840RtTextureDesc *desc,
                                    A840RtTexture **out_texture)
{
    const A840FormatCaps *caps;
    uint32_t depth;
    uint32_t layers;
    uint32_t mips;
    uint32_t samples;
    uint32_t max_dim;
    uint32_t max_mips;
    uint32_t blocks_x;
    uint32_t blocks_y;
    uint32_t elem_bytes;
    uint32_t pitch_align_log2;
    uint32_t row_align = 1u;
    uint64_t min_row_bytes;
    uint64_t pitch_bytes;
    uint64_t padded_rows;
    uint64_t slice_stride;
    uint32_t total_slices;
    uint64_t alloc_bytes;
    uint64_t gpu_va = 0u;
    uint64_t bo_handle = 0u;
    A840TexDescInput tex_in;
    uint32_t tex_const[A840_TEX_CONST_DWORDS];
    A840EmitStatus est;
    A840TransportStatus tst;
    A840BoAllocDesc alloc_desc;
    A840BoMapDesc map_desc;
    A840BoMapReply map_reply;
    A840RtTexture *tex;
    A840RtStatus st;
    uint32_t i;

    if (!out_texture) {
        return A840_RT_INVALID_ARG;
    }
    *out_texture = NULL;
    if (!device || !desc || desc->width == 0u || desc->height == 0u) {
        return A840_RT_INVALID_ARG;
    }

    caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    if (!caps) {
        return A840_RT_UNSUPPORTED;
    }
    if ((caps->flags & A840_FORMAT_REQUIRES_XR_CODEC) != 0u) {
        return A840_RT_UNSUPPORTED;
    }
    if (desc->tile_mode != A840_TILE_LINEAR && desc->tile_mode != A840_TILE_TILED) {
        return A840_RT_UNSUPPORTED;
    }
    if (desc->tex_type > A840_TEX_3D) {
        return A840_RT_INVALID_ARG;
    }

    mips = desc->mip_levels ? desc->mip_levels : 1u;
    samples = desc->samples ? desc->samples : 1u;
    if (!is_power_of_two_u64((uint64_t)samples) || samples > A840_LIMIT_MAX_SAMPLES) {
        return A840_RT_RANGE_ERROR;
    }
    if (samples > 1u && ((caps->msaa & samples) == 0u ||
                         desc->tex_type != A840_TEX_2D || mips != 1u)) {
        return A840_RT_UNSUPPORTED;
    }

    if (desc->tex_type == A840_TEX_1D) {
        if (desc->height != 1u || desc->width > A840_LIMIT_MAX_TEXTURE_SIZE_1D) {
            return A840_RT_RANGE_ERROR;
        }
        depth = 1u;
        layers = desc->array_layers ? desc->array_layers : 1u;
        if (layers > A840_LIMIT_MAX_ARRAY_LAYERS) {
            return A840_RT_RANGE_ERROR;
        }
        total_slices = layers;
        max_dim = desc->width;
        max_mips = (uint32_t)A840_LIMIT_MAX_MIP_LEVELS_2D;
    } else if (desc->tex_type == A840_TEX_2D) {
        if (desc->width > A840_LIMIT_MAX_TEXTURE_SIZE ||
            desc->height > A840_LIMIT_MAX_TEXTURE_SIZE) {
            return A840_RT_RANGE_ERROR;
        }
        depth = 1u;
        layers = desc->array_layers ? desc->array_layers : 1u;
        if (layers > A840_LIMIT_MAX_ARRAY_LAYERS) {
            return A840_RT_RANGE_ERROR;
        }
        total_slices = layers;
        max_dim = desc->width > desc->height ? desc->width : desc->height;
        max_mips = (uint32_t)A840_LIMIT_MAX_MIP_LEVELS_2D;
    } else if (desc->tex_type == A840_TEX_CUBE) {
        if (desc->width != desc->height || desc->width > A840_LIMIT_MAX_TEXTURE_SIZE_CUBE) {
            return A840_RT_RANGE_ERROR;
        }
        depth = 1u;
        layers = desc->array_layers ? desc->array_layers : 6u;
        if ((layers % 6u) != 0u || layers > A840_LIMIT_MAX_ARRAY_LAYERS) {
            return A840_RT_INVALID_ARG;
        }
        total_slices = layers;
        max_dim = desc->width;
        max_mips = (uint32_t)A840_LIMIT_MAX_MIP_LEVELS_2D;
    } else {
        /* A840_TEX_3D */
        depth = desc->depth ? desc->depth : 1u;
        layers = 1u;
        if (desc->width > A840_LIMIT_MAX_TEXTURE_SIZE_3D ||
            desc->height > A840_LIMIT_MAX_TEXTURE_SIZE_3D ||
            depth > A840_LIMIT_MAX_TEXTURE_SIZE_3D) {
            return A840_RT_RANGE_ERROR;
        }
        total_slices = depth;
        max_dim = desc->width > desc->height ? desc->width : desc->height;
        if (depth > max_dim) {
            max_dim = depth;
        }
        max_mips = (uint32_t)A840_LIMIT_MAX_MIP_LEVELS_3D;
    }

    if (mips > max_mips || mips > log2_floor_u64((uint64_t)max_dim) + 1u) {
        return A840_RT_RANGE_ERROR;
    }

    blocks_x = (desc->width + caps->block_width - 1u) / caps->block_width;
    blocks_y = (desc->height + caps->block_height - 1u) / caps->block_height;
    elem_bytes = texture_element_bytes(caps);
    min_row_bytes = (uint64_t)blocks_x * (uint64_t)elem_bytes * (uint64_t)samples;

    if (desc->tile_mode == A840_TILE_LINEAR) {
        pitch_align_log2 = 6u;
        row_align = 1u;
    } else {
        compute_tiled_alignment(caps, samples, &pitch_align_log2, &row_align);
    }

    {
        uint64_t pitch_align = UINT64_C(1) << pitch_align_log2;
        if (desc->pitch_bytes != 0u) {
            if ((uint64_t)desc->pitch_bytes < min_row_bytes) {
                return A840_RT_RANGE_ERROR;
            }
            if ((desc->pitch_bytes % pitch_align) != 0u) {
                return A840_RT_ALIGNMENT;
            }
            pitch_bytes = (uint64_t)desc->pitch_bytes;
        } else {
            if (!align_up_u64(min_row_bytes, pitch_align, &pitch_bytes)) {
                return A840_RT_RANGE_ERROR;
            }
        }
    }
    if (pitch_bytes > UINT32_MAX) {
        return A840_RT_RANGE_ERROR;
    }

    padded_rows = ((uint64_t)blocks_y + (uint64_t)row_align - 1u) /
                  (uint64_t)row_align * (uint64_t)row_align;
    if (padded_rows > UINT64_MAX / pitch_bytes) {
        return A840_RT_RANGE_ERROR;
    }
    if (!align_up_u64(pitch_bytes * padded_rows, A840_RT_PAGE_BYTES, &slice_stride)) {
        return A840_RT_RANGE_ERROR;
    }
    if ((uint64_t)total_slices > UINT64_MAX / slice_stride) {
        return A840_RT_RANGE_ERROR;
    }
    alloc_bytes = slice_stride * (uint64_t)total_slices;
    if (alloc_bytes > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    st = a840_va_allocator_alloc(&device->va_alloc, alloc_bytes,
                                 A840_RT_PAGE_BYTES, &gpu_va);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(&alloc_desc, 0, sizeof(alloc_desc));
    alloc_desc.bytes = alloc_bytes;
    alloc_desc.align_log2 = 12u;
    tst = device->transport.ops->bo_alloc(device->transport.ctx, &alloc_desc, &bo_handle);
    if (tst != A840_TRANSPORT_OK) {
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return A840_RT_TRANSPORT_ERROR;
    }

    memset(&map_desc, 0, sizeof(map_desc));
    memset(&map_reply, 0, sizeof(map_reply));
    map_desc.handle = bo_handle;
    map_desc.gpu_va = gpu_va;
    map_desc.flags = A840_BO_MAP_GPU |
                     ((desc->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) ? A840_BO_MAP_CPU : 0u);
    map_desc.permissions = A840_BO_PERM_READ | A840_BO_PERM_WRITE;
    map_desc.writeback = (device->transport_caps.abi_flags & NativeAdrenoParamsV2OwnedCoherent) ? 1u : 0u;
    map_desc.shareability = A840_BO_SHARE_INNER;

    tst = device->transport.ops->bo_map(device->transport.ctx, &map_desc, &map_reply);
    if (tst != A840_TRANSPORT_OK) {
        device->transport.ops->bo_free(device->transport.ctx, bo_handle);
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return A840_RT_TRANSPORT_ERROR;
    }

    /* Encode 64-byte A8XX hardware texture descriptor via kits/Adreno840Emit. */
    memset(&tex_in, 0, sizeof(tex_in));
    tex_in.format_id = (desc->tile_mode == A840_TILE_LINEAR) ? caps->a840_fmt_id : caps->tiled_fmt_id;
    tex_in.swap = (desc->tile_mode == A840_TILE_LINEAR) ? caps->swap : caps->tiled_swap;
    tex_in.type = desc->tex_type;
    tex_in.width = desc->width;
    tex_in.height = desc->height;
    tex_in.depth = total_slices;
    tex_in.mip_levels = mips;
    tex_in.samples = samples;
    tex_in.pitch = (uint32_t)pitch_bytes;
    tex_in.slice_stride = slice_stride;
    tex_in.gpu_va = gpu_va;
    tex_in.tile_mode = desc->tile_mode;
    tex_in.highest_bank_bit = device->chip_facts.highest_bank_bit;
    tex_in.srgb = (caps->flags & A840_FORMAT_SRGB) ? 1u : 0u;
    tex_in.pitch_align_log2 = pitch_align_log2;
    if (desc->tex_type == A840_TEX_3D) {
        tex_in.min_slice_stride = (mips == 1u) ? slice_stride : A840_RT_PAGE_BYTES;
    }
    if (desc->custom_swizzle) {
        for (i = 0; i < 4u; ++i) {
            tex_in.swizzle[i] = desc->swizzle[i];
        }
    } else {
        tex_in.swizzle[0] = A840_SWIZZLE_R;
        tex_in.swizzle[1] = A840_SWIZZLE_G;
        tex_in.swizzle[2] = A840_SWIZZLE_B;
        tex_in.swizzle[3] = A840_SWIZZLE_A;
    }

    est = a840_encode_tex_const(&tex_in, tex_const);
    if (est != A840_EMIT_OK) {
        A840BoUnmapDesc unmap;
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = bo_handle;
        unmap.flags = A840_BO_UNMAP_GPU |
                      ((map_desc.flags & A840_BO_MAP_CPU) ? A840_BO_UNMAP_CPU : 0u);
        unmap.cpu_token = map_reply.cpu_token;
        device->transport.ops->bo_unmap(device->transport.ctx, &unmap);
        device->transport.ops->bo_free(device->transport.ctx, bo_handle);
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return a840_rt_status_from_emit(est);
    }

    tex = (A840RtTexture *)calloc(1u, sizeof(*tex));
    if (!tex) {
        A840BoUnmapDesc unmap;
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = bo_handle;
        unmap.flags = A840_BO_UNMAP_GPU |
                      ((map_desc.flags & A840_BO_MAP_CPU) ? A840_BO_UNMAP_CPU : 0u);
        unmap.cpu_token = map_reply.cpu_token;
        device->transport.ops->bo_unmap(device->transport.ctx, &unmap);
        device->transport.ops->bo_free(device->transport.ctx, bo_handle);
        a840_va_allocator_free(&device->va_alloc, gpu_va, alloc_bytes);
        return A840_RT_OOM;
    }

    tex->device = device;
    tex->device_id = device->device_id;
    tex->refcount = 1u;
    tex->bo_handle = bo_handle;
    tex->gpu_va = gpu_va;
    tex->alloc_bytes = alloc_bytes;
    tex->cpu_ptr = map_reply.cpu_ptr;
    tex->cpu_token = map_reply.cpu_token;
    tex->cpu_mapped = (map_desc.flags & A840_BO_MAP_CPU) != 0u;
    tex->mtl_pixel_format = desc->mtl_pixel_format;
    tex->format_caps = caps;
    tex->tex_type = desc->tex_type;
    tex->width = desc->width;
    tex->height = desc->height;
    tex->depth = depth;
    tex->array_layers = layers;
    tex->mip_levels = mips;
    tex->samples = samples;
    tex->tile_mode = desc->tile_mode;
    tex->pitch_bytes = (uint32_t)pitch_bytes;
    tex->pitch_align_log2 = pitch_align_log2;
    tex->slice_stride_bytes = slice_stride;
    tex->usage_flags = desc->usage_flags;
    tex->format_id = tex_in.format_id;
    tex->swap = tex_in.swap;
    tex->color_format_id = caps->color_fmt_id;
    tex->color_swap = (desc->tile_mode == A840_TILE_LINEAR) ? caps->color_swap : A840_WZYX;
    tex->srgb = tex_in.srgb != 0u;
    for (i = 0; i < 4u; ++i) {
        tex->swizzle[i] = tex_in.swizzle[i];
    }
    memcpy(tex->tex_const, tex_const, sizeof(tex_const));

    device->live_textures++;
    *out_texture = tex;
    return A840_RT_OK;
}


A840RtStatus a840_rt_texture_create_on_buffer(A840RtDevice *device,
                                              A840RtBuffer *backing_buffer,
                                              uint64_t backing_offset,
                                              const A840RtTextureDesc *desc,
                                              A840RtTexture **out_texture)
{
    const A840FormatCaps *caps;
    uint32_t blocks_x;
    uint32_t blocks_y;
    uint32_t elem_bytes;
    uint64_t min_row_bytes;
    uint64_t pitch_bytes;
    uint64_t needed_bytes;
    uint64_t slice_stride;
    A840TexDescInput tex_in;
    uint32_t tex_const[A840_TEX_CONST_DWORDS];
    A840EmitStatus est;
    A840RtTexture *tex;
    uint32_t i;

    if (!out_texture) {
        return A840_RT_INVALID_ARG;
    }
    *out_texture = NULL;
    if (!device || !backing_buffer || !desc || !a840_rt_device_owns_buffer(device, backing_buffer)) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->width == 0u || desc->height == 0u || desc->tex_type != A840_TEX_2D ||
        desc->tile_mode != A840_TILE_LINEAR) {
        return A840_RT_UNSUPPORTED;
    }
    if (desc->width > A840_LIMIT_MAX_TEXTURE_SIZE || desc->height > A840_LIMIT_MAX_TEXTURE_SIZE) {
        return A840_RT_RANGE_ERROR;
    }
    if ((backing_offset % 64u) != 0u) {
        return A840_RT_ALIGNMENT;
    }
    caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    if (!caps || (caps->flags & A840_FORMAT_REQUIRES_XR_CODEC) != 0u) {
        return A840_RT_UNSUPPORTED;
    }

    blocks_x = (desc->width + caps->block_width - 1u) / caps->block_width;
    blocks_y = (desc->height + caps->block_height - 1u) / caps->block_height;
    elem_bytes = texture_element_bytes(caps);
    min_row_bytes = (uint64_t)blocks_x * (uint64_t)elem_bytes;
    if (desc->pitch_bytes != 0u) {
        if ((uint64_t)desc->pitch_bytes < min_row_bytes) {
            return A840_RT_RANGE_ERROR;
        }
        if ((desc->pitch_bytes % 64u) != 0u) {
            return A840_RT_ALIGNMENT;
        }
        pitch_bytes = (uint64_t)desc->pitch_bytes;
    } else {
        if (!align_up_u64(min_row_bytes, 64u, &pitch_bytes)) {
            return A840_RT_RANGE_ERROR;
        }
    }
    if (pitch_bytes > UINT32_MAX || (uint64_t)blocks_y > UINT64_MAX / pitch_bytes) {
        return A840_RT_RANGE_ERROR;
    }
    needed_bytes = pitch_bytes * (uint64_t)blocks_y;
    if (backing_offset > backing_buffer->alloc_bytes ||
        needed_bytes > backing_buffer->alloc_bytes - backing_offset) {
        return A840_RT_RANGE_ERROR;
    }
    if (!align_up_u64(needed_bytes, A840_RT_PAGE_BYTES, &slice_stride)) {
        slice_stride = needed_bytes;
    }

    memset(&tex_in, 0, sizeof(tex_in));
    tex_in.format_id = caps->a840_fmt_id;
    tex_in.swap = caps->swap;
    tex_in.type = A840_TEX_2D;
    tex_in.width = desc->width;
    tex_in.height = desc->height;
    tex_in.depth = 1u;
    tex_in.mip_levels = 1u;
    tex_in.samples = 1u;
    tex_in.pitch = (uint32_t)pitch_bytes;
    tex_in.slice_stride = slice_stride;
    tex_in.gpu_va = backing_buffer->gpu_va + backing_offset;
    tex_in.tile_mode = A840_TILE_LINEAR;
    tex_in.highest_bank_bit = device->chip_facts.highest_bank_bit;
    tex_in.srgb = (caps->flags & A840_FORMAT_SRGB) ? 1u : 0u;
    tex_in.pitch_align_log2 = 6u;
    if (desc->custom_swizzle) {
        for (i = 0; i < 4u; ++i) {
            tex_in.swizzle[i] = desc->swizzle[i];
        }
    } else {
        tex_in.swizzle[0] = A840_SWIZZLE_R;
        tex_in.swizzle[1] = A840_SWIZZLE_G;
        tex_in.swizzle[2] = A840_SWIZZLE_B;
        tex_in.swizzle[3] = A840_SWIZZLE_A;
    }
    est = a840_encode_tex_const(&tex_in, tex_const);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    tex = (A840RtTexture *)calloc(1u, sizeof(*tex));
    if (!tex) {
        return A840_RT_OOM;
    }
    a840_rt_buffer_retain(backing_buffer);
    tex->device = device;
    tex->device_id = device->device_id;
    tex->refcount = 1u;
    tex->bo_handle = backing_buffer->bo_handle;
    tex->gpu_va = backing_buffer->gpu_va + backing_offset;
    tex->alloc_bytes = needed_bytes;
    tex->cpu_ptr = backing_buffer->cpu_ptr
        ? (void *)((uint8_t *)backing_buffer->cpu_ptr + (size_t)backing_offset)
        : NULL;
    tex->cpu_token = 0u;
    tex->cpu_mapped = false;
    tex->backing_buffer = backing_buffer;
    tex->backing_offset = backing_offset;
    tex->mtl_pixel_format = desc->mtl_pixel_format;
    tex->format_caps = caps;
    tex->tex_type = A840_TEX_2D;
    tex->width = desc->width;
    tex->height = desc->height;
    tex->depth = 1u;
    tex->array_layers = 1u;
    tex->mip_levels = 1u;
    tex->samples = 1u;
    tex->tile_mode = A840_TILE_LINEAR;
    tex->pitch_bytes = (uint32_t)pitch_bytes;
    tex->pitch_align_log2 = 6u;
    tex->slice_stride_bytes = slice_stride;
    tex->usage_flags = desc->usage_flags;
    tex->format_id = tex_in.format_id;
    tex->swap = tex_in.swap;
    tex->color_format_id = caps->color_fmt_id;
    tex->color_swap = caps->color_swap;
    tex->srgb = tex_in.srgb != 0u;
    for (i = 0; i < 4u; ++i) {
        tex->swizzle[i] = tex_in.swizzle[i];
    }
    memcpy(tex->tex_const, tex_const, sizeof(tex_const));

    device->live_textures++;
    *out_texture = tex;
    return A840_RT_OK;
}

void a840_rt_texture_retain(A840RtTexture *texture)
{
    if (texture && texture->refcount > 0u) {
        texture->refcount++;
    }
}

void a840_rt_texture_release(A840RtTexture *texture)
{
    A840RtDevice *dev;
    A840BoUnmapDesc unmap;

    if (!texture || texture->refcount == 0u) {
        return;
    }
    texture->refcount--;
    if (texture->refcount > 0u) {
        return;
    }

    dev = texture->device;
    if (texture->backing_buffer != NULL) {
        a840_rt_buffer_release(texture->backing_buffer);
    } else {
        memset(&unmap, 0, sizeof(unmap));
        unmap.handle = texture->bo_handle;
        unmap.flags = A840_BO_UNMAP_GPU | (texture->cpu_mapped ? A840_BO_UNMAP_CPU : 0u);
        unmap.cpu_token = texture->cpu_token;
        dev->transport.ops->bo_unmap(dev->transport.ctx, &unmap);
        dev->transport.ops->bo_free(dev->transport.ctx, texture->bo_handle);
        a840_va_allocator_free(&dev->va_alloc, texture->gpu_va, texture->alloc_bytes);
    }
    if (dev->live_textures > 0u) {
        dev->live_textures--;
    }
    free(texture);
}

A840RtStatus a840_rt_texture_as_surface2d(const A840RtTexture *texture,
                                          A840Surface2D *out_surface)
{
    if (!texture || !out_surface) {
        return A840_RT_INVALID_ARG;
    }
    memset(out_surface, 0, sizeof(*out_surface));
    out_surface->bo_handle = texture->bo_handle;
    out_surface->gpu_va = texture->gpu_va;
    out_surface->format_id = texture->color_format_id;
    out_surface->swap = texture->color_swap;
    out_surface->width = texture->width;
    out_surface->height = texture->height;
    out_surface->pitch = texture->pitch_bytes;
    out_surface->tile_mode = texture->tile_mode;
    out_surface->srgb = texture->srgb ? 1u : 0u;
    return A840_RT_OK;
}

A840RtStatus a840_rt_texture_as_attachment(const A840RtTexture *texture,
                                           A840AttachmentStorage *out_storage)
{
    if (!texture || !out_storage) {
        return A840_RT_INVALID_ARG;
    }
    memset(out_storage, 0, sizeof(*out_storage));
    out_storage->data.bo_handle = texture->bo_handle;
    out_storage->data.gpu_va = texture->gpu_va;
    out_storage->pitch_bytes = texture->pitch_bytes;
    out_storage->layer_stride_bytes = texture->slice_stride_bytes;
    out_storage->tile_mode = texture->tile_mode;
    return A840_RT_OK;
}
