/* SPDX-License-Identifier: MIT */
#ifndef A840_UBWC_TEXTURE_H
#define A840_UBWC_TEXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "a840_emit_gmem.h"
#include "a840_runtime_device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SCANOUT_CAP_UBWC6 = 1u << 0,
    A840_SCANOUT_FLAG_ELIGIBLE = 1u << 0,
    A840_P3_MAX_PLANES = 2u
};

/* Metal pixel format IDs used by Phase 3 multi-plane YCbCr and XR10 views. */
enum {
    A840_MTL_FMT_R8_UNORM = 10u,
    A840_MTL_FMT_R16_UNORM = 20u,
    A840_MTL_FMT_RG8_UNORM = 30u,
    A840_MTL_FMT_RG16_UNORM = 60u,
    A840_MTL_FMT_RGBA8_UNORM = 70u,
    A840_MTL_FMT_BGRA8_UNORM = 80u,
    A840_MTL_FMT_BGRA8_UNORM_SRGB = 81u,
    A840_MTL_FMT_DEPTH32_FLOAT = 252u,
    A840_MTL_FMT_RGB10A8_2P_XR10 = 550u,
    A840_MTL_FMT_RGB10A8_2P_XR10_SRGB = 551u,
    A840_MTL_FMT_BGRA10_XR = 552u,
    A840_MTL_FMT_BGRA10_XR_SRGB = 553u,
    A840_MTL_FMT_BGR10_XR = 554u,
    A840_MTL_FMT_BGR10_XR_SRGB = 555u
};

typedef enum A840StorageMode {
    A840_STORAGE_PRIVATE = 0,
    A840_STORAGE_SHARED = 1,
    A840_STORAGE_MEMORYLESS = 2
} A840StorageMode;

typedef enum A840Phase3TexKind {
    A840_P3_TEX_UBWC6 = 0,
    A840_P3_TEX_MEMORYLESS = 1,
    A840_P3_TEX_XR10 = 2,
    A840_P3_TEX_YCBCR_MULTIPLANE = 3
} A840Phase3TexKind;

typedef enum A840YcbcrFormat {
    A840_YCBCR_NONE = 0,
    A840_YCBCR_NV12 = 1,
    A840_YCBCR_P010 = 2
} A840YcbcrFormat;

typedef struct A840MdssScanoutCaps {
    uint32_t flags;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t base_alignment;
    uint32_t pitch_alignment;
    const uint32_t *formats;
    size_t format_count;
} A840MdssScanoutCaps;

typedef struct A840UbwcLayout {
    uint32_t cpp;
    uint32_t ubwc_block_width;
    uint32_t ubwc_block_height;
    uint32_t flag_pitch;
    uint32_t flag_rows;
    uint64_t flag_slice_bytes;
    uint64_t flag_offset;
    uint64_t flag_total_bytes;
    uint32_t data_pitch;
    uint32_t data_pitch_align_log2;
    uint32_t data_padded_height;
    uint64_t data_slice_bytes;
    uint64_t data_offset;
    uint64_t data_total_bytes;
    uint64_t total_bytes;
} A840UbwcLayout;

typedef struct A840UbwcTextureDesc {
    A840Phase3TexKind kind;
    A840StorageMode storage_mode;
    uint32_t mtl_pixel_format;
    A840YcbcrFormat ycbcr_format;
    uint32_t width;
    uint32_t height;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tile_mode;
    uint32_t usage_flags;
    uint32_t highest_bank_bit;
    uint32_t ubwc_version;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    bool for_scanout;
    const A840MdssScanoutCaps *mdss_caps;
} A840UbwcTextureDesc;

typedef struct A840UbwcTexture {
    A840RtDevice *device;
    A840Phase3TexKind kind;
    A840StorageMode storage_mode;
    uint32_t mtl_pixel_format;
    A840YcbcrFormat ycbcr_format;
    const A840FormatCaps *format_caps;

    uint32_t width;
    uint32_t height;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t cpp;
    uint32_t tile_mode;
    uint32_t usage_flags;

    uint32_t highest_bank_bit;
    uint32_t ubwc_version;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    A840UbwcLayout ubwc;

    A840RtBuffer *backing_bo;
    A840GmemRange data_range;
    A840GmemRange flag_range;

    uint32_t plane_count;
    uint32_t plane_mtl_formats[A840_P3_MAX_PLANES];
    uint32_t plane_widths[A840_P3_MAX_PLANES];
    uint32_t plane_heights[A840_P3_MAX_PLANES];
    uint32_t plane_pitches[A840_P3_MAX_PLANES];
    uint64_t plane_offsets[A840_P3_MAX_PLANES];
    uint64_t plane_bytes[A840_P3_MAX_PLANES];

    uint32_t tex_const[A840_P3_MAX_PLANES][A840_TEX_CONST_DWORDS];

    bool is_xr10;
    bool is_srgb;
    bool requires_xr_codec;

    uint32_t gmem_bytes_per_pixel;

    bool for_scanout;
    bool scanout_eligible;
    bool fallback_linear_required;
    bool surface_meta_negotiated;
    struct NativeAdrenoAbiSurfaceMetaV3 surface_meta;
    A840RtBuffer *linear_resolve_bo;
    A840GmemRange linear_resolve_range;
    uint32_t linear_resolve_pitch;
} A840UbwcTexture;

A840RtStatus a840_ubwc_compute_layout(uint32_t mtl_pixel_format,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t array_layers,
                                      uint32_t samples,
                                      A840UbwcLayout *out_layout);

A840RtStatus a840_ubwc_texture_create(A840RtDevice *device,
                                      const A840UbwcTextureDesc *desc,
                                      A840UbwcTexture **out_tex);

void a840_ubwc_texture_destroy(A840UbwcTexture *tex);

A840RtStatus a840_ubwc_texture_validate_sysmem_target(const A840UbwcTexture *tex);
A840RtStatus a840_ubwc_texture_cpu_readback(const A840UbwcTexture *tex,
                                            void *dst,
                                            size_t dst_bytes);

A840RtStatus a840_ubwc_texture_fill_gmem_attachment(const A840UbwcTexture *tex,
                                                    A840GmemLoad load,
                                                    A840GmemStore store,
                                                    A840GmemAttachment *out_att,
                                                    A840GmemBinding *out_bind);

#ifdef __cplusplus
}
#endif

#endif /* A840_UBWC_TEXTURE_H */
