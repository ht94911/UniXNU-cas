/* SPDX-License-Identifier: MIT */
#ifndef A840_RUNTIME_DEVICE_H
#define A840_RUNTIME_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840Caps.h"
#include "chip_facts_a840.h"
#include "a840_emit_ops.h"
#include "a840_emit_3d.h"
#include "a840_backend_transport.h"
#include "a840_dualpath_router.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_RT_PAGE_BYTES = 4096u,
    A840_RT_LARGE_ALIGN_BYTES = 65536u,
    A840_RT_BORDER_COLOR_BYTES = 65536u,
    A840_RT_VA_BITS = 48u
};

typedef enum A840RtStatus {
    A840_RT_OK = 0,
    A840_RT_INVALID_ARG = 1,
    A840_RT_OOM = 2,
    A840_RT_UNSUPPORTED = 3,
    A840_RT_RANGE_ERROR = 4,
    A840_RT_ALIGNMENT = 5,
    A840_RT_WRONG_DEVICE = 6,
    A840_RT_STATE_ERROR = 7,
    A840_RT_CAPACITY = 8,
    A840_RT_TRANSPORT_ERROR = 9,
    A840_RT_EMIT_ERROR = 10,
    A840_RT_FALLBACK_REQUIRED = 11,
    A840_RT_TIMEOUT = 12
} A840RtStatus;

enum {
    A840_BUF_USAGE_READ = 1u << 0,
    A840_BUF_USAGE_WRITE = 1u << 1,
    A840_BUF_USAGE_SHADER = 1u << 2,
    A840_BUF_USAGE_CPU_VISIBLE = 1u << 3
};

enum {
    A840_TEX_USAGE_SHADER_READ = 1u << 0,
    A840_TEX_USAGE_SHADER_WRITE = 1u << 1,
    A840_TEX_USAGE_RENDER_TARGET = 1u << 2,
    A840_TEX_USAGE_CPU_VISIBLE = 1u << 3
};

/* Deterministic 48-bit GPU VA interval allocator with coalescing free-list. */
typedef struct A840VaRange {
    uint64_t start;
    uint64_t size;
} A840VaRange;

typedef struct A840VaAllocator {
    uint64_t base;
    uint64_t total_size;
    uint64_t used_bytes;

    A840VaRange *free_ranges;
    size_t free_count;
    size_t free_cap;

    A840VaRange *alloc_ranges;
    size_t alloc_count;
    size_t alloc_cap;
} A840VaAllocator;

typedef struct A840RtDevice {
    uint64_t device_id;
    A840ChipFacts chip_facts;
    A840BackendTransport transport;
    A840TransportCaps transport_caps;
    A840DualPathRouter router;
    A840VaAllocator va_alloc;

    /* Global 64 KiB read-only zero page (ABI v3 kernel-owned or v2 fallback BO). */
    uint64_t zero_page_bo;
    uint64_t zero_page_va;
    uint64_t zero_page_bytes;
    bool zero_page_owned;

    /* Global sampler border color table BO. */
    uint64_t border_color_bo;
    uint64_t border_color_va;
    uint64_t border_color_bytes;
    void *border_color_cpu;
    uint32_t border_color_cpu_token;

    uint64_t live_buffers;
    uint64_t live_textures;
    uint64_t live_queues;
} A840RtDevice;

typedef struct A840RtBufferDesc {
    uint64_t size_bytes;
    uint64_t alignment;  /* 0 defaults to 4 KiB; must be power-of-two >= 4 KiB */
    uint32_t usage_flags;/* Bitmask of A840_BUF_USAGE_* */
    bool host_import;
    void *host_ptr;      /* Required and 4 KiB aligned when host_import is true */
} A840RtBufferDesc;

typedef struct A840RtBuffer {
    A840RtDevice *device;
    uint64_t device_id;
    uint32_t refcount;

    uint64_t bo_handle;
    uint64_t gpu_va;
    uint64_t size_bytes;
    uint64_t alloc_bytes;
    uint64_t alignment;
    uint32_t usage_flags;

    void *cpu_ptr;
    uint32_t cpu_token;
    bool cpu_mapped;
    bool imported;
} A840RtBuffer;

typedef struct A840RtTextureDesc {
    uint32_t mtl_pixel_format;
    uint32_t tex_type;    /* A840_TEX_1D / 2D / CUBE / 3D */
    uint32_t width;
    uint32_t height;
    uint32_t depth;       /* >= 1; for 3D depth slices, else 1 */
    uint32_t array_layers;/* >= 1; for CUBE must be multiple of 6 (0 defaults to 6/1) */
    uint32_t mip_levels;  /* 0 defaults to 1 */
    uint32_t samples;     /* 0 defaults to 1 */
    uint32_t tile_mode;   /* A840_TILE_LINEAR or A840_TILE_TILED */
    uint32_t pitch_bytes; /* 0 for automatic aligned pitch */
    uint32_t usage_flags; /* Bitmask of A840_TEX_USAGE_* */
    bool custom_swizzle;
    uint32_t swizzle[4];  /* Used only when custom_swizzle is true */
} A840RtTextureDesc;

typedef struct A840RtTexture {
    A840RtDevice *device;
    uint64_t device_id;
    uint32_t refcount;

    uint64_t bo_handle;
    uint64_t gpu_va;
    uint64_t alloc_bytes;
    void *cpu_ptr;
    uint32_t cpu_token;
    bool cpu_mapped;

    uint32_t mtl_pixel_format;
    const A840FormatCaps *format_caps;
    uint32_t tex_type;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t samples;
    uint32_t tile_mode;
    uint32_t pitch_bytes;
    uint32_t pitch_align_log2;
    uint64_t slice_stride_bytes;
    uint32_t usage_flags;

    uint32_t format_id;
    uint32_t swap;
    uint32_t color_format_id;
    uint32_t color_swap;
    bool srgb;
    uint32_t swizzle[4];

    /* Auto-encoded 64-byte A8XX hardware texture descriptor. */
    uint32_t tex_const[A840_TEX_CONST_DWORDS];
} A840RtTexture;

/* Standalone VA allocator API. */
A840RtStatus a840_va_allocator_init(A840VaAllocator *alloc, uint64_t base, uint64_t size);
void a840_va_allocator_destroy(A840VaAllocator *alloc);
A840RtStatus a840_va_allocator_alloc(A840VaAllocator *alloc,
                                     uint64_t size,
                                     uint64_t alignment,
                                     uint64_t *out_va);
A840RtStatus a840_va_allocator_free(A840VaAllocator *alloc, uint64_t va, uint64_t size);
uint64_t a840_va_allocator_used_bytes(const A840VaAllocator *alloc);
uint64_t a840_va_allocator_free_bytes(const A840VaAllocator *alloc);
size_t a840_va_allocator_free_block_count(const A840VaAllocator *alloc);

/* Runtime Device API. */
A840RtStatus a840_rt_device_create(const A840BackendTransport *transport,
                                   uint32_t initial_route_categories,
                                   A840RtDevice **out_device);
void a840_rt_device_destroy(A840RtDevice *device);

const A840ChipFacts *a840_rt_device_chip_facts(const A840RtDevice *device);
const A840TransportCaps *a840_rt_device_caps(const A840RtDevice *device);
A840DualPathRouter *a840_rt_device_router(A840RtDevice *device);
uint64_t a840_rt_device_zero_page_va(const A840RtDevice *device);
uint64_t a840_rt_device_border_color_va(const A840RtDevice *device);
uint64_t a840_rt_device_border_color_bo(const A840RtDevice *device);

bool a840_rt_device_owns_buffer(const A840RtDevice *device, const A840RtBuffer *buffer);
bool a840_rt_device_owns_texture(const A840RtDevice *device, const A840RtTexture *texture);

/* Runtime Buffer API (refcounted). */
A840RtStatus a840_rt_buffer_create(A840RtDevice *device,
                                   const A840RtBufferDesc *desc,
                                   A840RtBuffer **out_buffer);
void a840_rt_buffer_retain(A840RtBuffer *buffer);
void a840_rt_buffer_release(A840RtBuffer *buffer);
A840RtStatus a840_rt_buffer_write(A840RtBuffer *buffer,
                                  uint64_t offset,
                                  const void *data,
                                  size_t bytes);

/* Runtime Texture API (refcounted). */
A840RtStatus a840_rt_texture_create(A840RtDevice *device,
                                    const A840RtTextureDesc *desc,
                                    A840RtTexture **out_texture);
void a840_rt_texture_retain(A840RtTexture *texture);
void a840_rt_texture_release(A840RtTexture *texture);

A840RtStatus a840_rt_texture_as_surface2d(const A840RtTexture *texture,
                                          A840Surface2D *out_surface);
A840RtStatus a840_rt_texture_as_attachment(const A840RtTexture *texture,
                                           A840AttachmentStorage *out_storage);

/* Utility status mapping from emit layer. */
A840RtStatus a840_rt_status_from_emit(A840EmitStatus st);

#ifdef __cplusplus
}
#endif

#endif /* A840_RUNTIME_DEVICE_H */
