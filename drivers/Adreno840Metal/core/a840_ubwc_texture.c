/* SPDX-License-Identifier: MIT */
#include "a840_ubwc_texture.h"

#include <stdlib.h>
#include <string.h>

#include "gen/a840_bitfields.h"

static bool is_pow2_u32(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static uint32_t log2_u32(uint32_t v)
{
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

static uint32_t round_up_u32(uint32_t v, uint32_t align)
{
    return (v + align - 1u) / align * align;
}

static bool round_up_u64(uint64_t v, uint64_t align, uint64_t *out)
{
    uint64_t rem;
    if (align == 0u) {
        return false;
    }
    rem = v % align;
    if (rem == 0u) {
        *out = v;
        return true;
    }
    if (v > UINT64_MAX - (align - rem)) {
        return false;
    }
    *out = v + (align - rem);
    return true;
}

static bool is_xr10_mtl_format(uint32_t mtl_fmt)
{
    return mtl_fmt == A840_MTL_FMT_BGRA10_XR ||
           mtl_fmt == A840_MTL_FMT_BGRA10_XR_SRGB ||
           mtl_fmt == A840_MTL_FMT_BGR10_XR ||
           mtl_fmt == A840_MTL_FMT_BGR10_XR_SRGB;
}

static uint32_t tiled_pitch_align_log2_for_caps(const A840FormatCaps *caps, uint32_t samples)
{
    uint32_t cpp = (caps->block_bits + 7u) / 8u;
    uint32_t eff_cpp = cpp * samples;
    bool rg8 = (caps->flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) != 0u;
    if (eff_cpp == 1u || (samples == 1u && rg8)) {
        return 7u;
    }
    if (eff_cpp == 2u) {
        return 8u;
    }
    return 6u + log2_u32(eff_cpp);
}

static uint32_t tiled_row_align_for_caps(const A840FormatCaps *caps, uint32_t samples)
{
    uint32_t cpp = (caps->block_bits + 7u) / 8u;
    uint32_t eff_cpp = cpp * samples;
    bool rg8 = (caps->flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) != 0u;
    return (eff_cpp == 1u || (samples == 1u && rg8)) ? 32u : 16u;
}

static bool mdss_accepts_ubwc6(const A840MdssScanoutCaps *caps,
                               const A840UbwcTexture *tex)
{
    uint32_t base_align;
    uint32_t pitch_align;
    size_t i;

    if (!caps || (caps->flags & A840_SCANOUT_CAP_UBWC6) == 0u || !caps->formats ||
        caps->format_count == 0u) {
        return false;
    }
    if (tex->kind != A840_P3_TEX_UBWC6 || !tex->format_caps ||
        !tex->format_caps->ubwc_supported || !tex->backing_bo) {
        return false;
    }
    if (tex->highest_bank_bit != A840_KAANAPALI_HIGHEST_BANK_BIT ||
        tex->ubwc_version != A840_LIMIT_UBWC_VERSION ||
        tex->ubwc_swizzle != A840_LIMIT_UBWC_SWIZZLE ||
        tex->macrotile_mode != A840_LIMIT_MACROTILE_MODE) {
        return false;
    }
    if (tex->width > caps->max_width || tex->height > caps->max_height) {
        return false;
    }
    base_align = caps->base_alignment ? caps->base_alignment : A840_RT_PAGE_BYTES;
    pitch_align = caps->pitch_alignment ? caps->pitch_alignment : 64u;
    if ((tex->backing_bo->gpu_va % base_align) != 0u ||
        (tex->ubwc.flag_offset % base_align) != 0u ||
        (tex->ubwc.data_offset % base_align) != 0u ||
        (tex->ubwc.flag_pitch % pitch_align) != 0u) {
        return false;
    }
    for (i = 0u; i < caps->format_count; ++i) {
        if (caps->formats[i] == tex->mtl_pixel_format) {
            return true;
        }
    }
    return false;
}

static A840RtStatus allocate_linear_resolve(A840RtDevice *device,
                                            A840UbwcTexture *tex)
{
    uint32_t cpp = tex->cpp ? tex->cpp : 4u;
    uint32_t padded_w = round_up_u32(tex->width, (uint32_t)A840_LIMIT_GMEM_ALIGN_W);
    uint32_t padded_h = round_up_u32(tex->height, (uint32_t)A840_LIMIT_GMEM_ALIGN_H);
    uint32_t pitch;
    uint64_t bytes = 0u;
    A840RtBufferDesc bdesc;
    A840RtStatus st;

    if (padded_w > UINT32_MAX / cpp) {
        return A840_RT_RANGE_ERROR;
    }
    pitch = round_up_u32(padded_w * cpp, 64u);
    if (!round_up_u64((uint64_t)pitch * (uint64_t)padded_h, A840_RT_PAGE_BYTES, &bytes)) {
        return A840_RT_RANGE_ERROR;
    }

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size_bytes = bytes;
    bdesc.alignment = A840_RT_PAGE_BYTES;
    bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    if ((tex->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) != 0u) {
        bdesc.usage_flags |= A840_BUF_USAGE_CPU_VISIBLE;
    }

    st = a840_rt_buffer_create(device, &bdesc, &tex->linear_resolve_bo);
    if (st != A840_RT_OK) {
        return st;
    }
    tex->linear_resolve_pitch = pitch;
    tex->linear_resolve_range.bo = tex->linear_resolve_bo->bo_handle;
    tex->linear_resolve_range.va = tex->linear_resolve_bo->gpu_va;
    tex->linear_resolve_range.bytes = bytes;
    return A840_RT_OK;
}

A840RtStatus a840_ubwc_compute_layout(uint32_t mtl_pixel_format,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t array_layers,
                                      uint32_t samples,
                                      A840UbwcLayout *out_layout)
{
    const A840FormatCaps *caps;
    uint32_t cpp, eff_cpp, pitch_align_log2, pitch_align, height_align;
    uint32_t padded_w, padded_h, data_pitch, bw, bh, flag_cols, flag_pitch, flag_rows;
    uint64_t data_slice = 0u, flag_slice = 0u, flag_total = 0u, data_offset = 0u, data_total = 0u;
    bool rg8;
    A840UbwcLayout lay;

    if (!out_layout || width == 0u || height == 0u ||
        width > A840_LIMIT_MAX_TEXTURE_SIZE || height > A840_LIMIT_MAX_TEXTURE_SIZE) {
        return A840_RT_INVALID_ARG;
    }
    if (array_layers == 0u) {
        array_layers = 1u;
    }
    if (array_layers > A840_LIMIT_MAX_ARRAY_LAYERS) {
        return A840_RT_RANGE_ERROR;
    }
    if (samples == 0u) {
        samples = 1u;
    }
    if (!is_pow2_u32(samples) || samples > A840_LIMIT_MAX_SAMPLES) {
        return A840_RT_RANGE_ERROR;
    }

    caps = a840_format_caps_lookup(mtl_pixel_format);
    if (!caps || !caps->ubwc_supported ||
        (caps->flags & (A840_FORMAT_COMPRESSED | A840_FORMAT_REQUIRES_XR_CODEC)) != 0u) {
        return A840_RT_UNSUPPORTED;
    }
    if (samples > 1u && (caps->msaa & samples) == 0u) {
        return A840_RT_UNSUPPORTED;
    }

    cpp = (caps->block_bits + 7u) / 8u;
    if (!is_pow2_u32(cpp) || cpp > 16u || cpp * samples > 32u) {
        return A840_RT_UNSUPPORTED;
    }
    eff_cpp = cpp * samples;

    rg8 = (caps->flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) != 0u;
    pitch_align_log2 = tiled_pitch_align_log2_for_caps(caps, samples);
    pitch_align = UINT32_C(1) << pitch_align_log2;
    height_align = tiled_row_align_for_caps(caps, samples);

    padded_w = round_up_u32(width, (uint32_t)A840_LIMIT_GMEM_ALIGN_W);
    padded_h = round_up_u32(height, height_align);
    if (padded_w > UINT32_MAX / eff_cpp) {
        return A840_RT_RANGE_ERROR;
    }
    data_pitch = round_up_u32(padded_w * eff_cpp, pitch_align);
    if ((data_pitch >> A840_MRT_PITCH_SHR) > A840_MRT_PITCH_MAX) {
        return A840_RT_RANGE_ERROR;
    }
    if (!round_up_u64((uint64_t)data_pitch * (uint64_t)padded_h, A840_RT_PAGE_BYTES, &data_slice)) {
        return A840_RT_RANGE_ERROR;
    }

    bw = (eff_cpp <= 4u) ? 16u : ((eff_cpp == 8u) ? 8u : 4u);
    if (cpp <= 2u && samples > 1u) {
        bw = 16u / samples;
    }
    bh = rg8 ? 8u : ((eff_cpp == 32u) ? 2u : 4u);
    if (rg8) {
        bw = 16u;
    }

    flag_cols = (width + bw - 1u) / bw;
    flag_pitch = round_up_u32(flag_cols, 64u);
    flag_rows = round_up_u32((height + bh - 1u) / bh, 16u);
    if ((flag_pitch >> A840_TEX_5_FLAG_BUFFER_PITCH_SHR) > A840_TEX_5_FLAG_BUFFER_PITCH_MAX ||
        (flag_pitch >> A840_2D_FLAG_PITCH_SHR) > A840_2D_FLAG_PITCH_MAX) {
        return A840_RT_RANGE_ERROR;
    }
    if (!round_up_u64((uint64_t)flag_pitch * (uint64_t)flag_rows, A840_RT_PAGE_BYTES, &flag_slice)) {
        return A840_RT_RANGE_ERROR;
    }
    if ((flag_slice >> A840_TEX_8_FLAG_ARRAY_PITCH_SHR) > A840_TEX_8_FLAG_ARRAY_PITCH_MAX ||
        (flag_slice >> A840_2D_FLAG_ARRAY_SHR) > A840_2D_FLAG_ARRAY_MAX) {
        return A840_RT_RANGE_ERROR;
    }
    if ((uint64_t)array_layers > UINT64_MAX / flag_slice ||
        (uint64_t)array_layers > UINT64_MAX / data_slice) {
        return A840_RT_RANGE_ERROR;
    }

    flag_total = flag_slice * (uint64_t)array_layers;
    if (!round_up_u64(flag_total, A840_RT_PAGE_BYTES, &data_offset)) {
        return A840_RT_RANGE_ERROR;
    }
    data_total = data_slice * (uint64_t)array_layers;
    if (data_offset > UINT64_MAX - data_total) {
        return A840_RT_RANGE_ERROR;
    }

    memset(&lay, 0, sizeof(lay));
    lay.cpp = cpp;
    lay.ubwc_block_width = bw;
    lay.ubwc_block_height = bh;
    lay.flag_pitch = flag_pitch;
    lay.flag_rows = flag_rows;
    lay.flag_slice_bytes = flag_slice;
    lay.flag_offset = 0u;
    lay.flag_total_bytes = flag_total;
    lay.data_pitch = data_pitch;
    lay.data_pitch_align_log2 = pitch_align_log2;
    lay.data_padded_height = padded_h;
    lay.data_slice_bytes = data_slice;
    lay.data_offset = data_offset;
    lay.data_total_bytes = data_total;
    lay.total_bytes = data_offset + data_total;
    *out_layout = lay;
    return A840_RT_OK;
}

static A840RtStatus create_ubwc6(A840RtDevice *device,
                                 const A840UbwcTextureDesc *desc,
                                 A840UbwcTexture *tex)
{
    uint32_t hbb = desc->highest_bank_bit ? desc->highest_bank_bit : A840_KAANAPALI_HIGHEST_BANK_BIT;
    uint32_t ubwc_ver = desc->ubwc_version ? desc->ubwc_version : (uint32_t)A840_LIMIT_UBWC_VERSION;
    uint32_t swizzle = desc->ubwc_swizzle ? desc->ubwc_swizzle : (uint32_t)A840_LIMIT_UBWC_SWIZZLE;
    uint32_t macrotile = desc->macrotile_mode ? desc->macrotile_mode : (uint32_t)A840_LIMIT_MACROTILE_MODE;
    A840UbwcLayout lay;
    A840RtBufferDesc bdesc;
    A840TexDescInput tin;
    A840RtStatus st;
    A840EmitStatus est;

    if (desc->storage_mode == A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->tile_mode != 0u && desc->tile_mode != A840_TILE_UBWC) {
        return A840_RT_INVALID_ARG;
    }
    if (hbb != A840_KAANAPALI_HIGHEST_BANK_BIT ||
        device->chip_facts.highest_bank_bit != A840_KAANAPALI_HIGHEST_BANK_BIT ||
        ubwc_ver != A840_LIMIT_UBWC_VERSION ||
        swizzle != A840_LIMIT_UBWC_SWIZZLE ||
        macrotile != A840_LIMIT_MACROTILE_MODE) {
        return A840_RT_UNSUPPORTED;
    }

    st = a840_ubwc_compute_layout(desc->mtl_pixel_format, desc->width, desc->height,
                                  tex->array_layers, tex->samples, &lay);
    if (st != A840_RT_OK) {
        return st;
    }
    if (lay.total_bytes > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    tex->format_caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    tex->cpp = lay.cpp;
    tex->tile_mode = A840_TILE_UBWC;
    tex->highest_bank_bit = hbb;
    tex->ubwc_version = ubwc_ver;
    tex->ubwc_swizzle = swizzle;
    tex->macrotile_mode = macrotile;
    tex->ubwc = lay;
    tex->is_srgb = (tex->format_caps->flags & A840_FORMAT_SRGB) != 0u;
    tex->gmem_bytes_per_pixel = lay.cpp * tex->samples;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size_bytes = lay.total_bytes;
    bdesc.alignment = A840_RT_PAGE_BYTES;
    bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    if ((desc->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) != 0u ||
        desc->storage_mode == A840_STORAGE_SHARED) {
        bdesc.usage_flags |= A840_BUF_USAGE_CPU_VISIBLE;
    }

    st = a840_rt_buffer_create(device, &bdesc, &tex->backing_bo);
    if (st != A840_RT_OK) {
        return st;
    }

    tex->flag_range.bo = tex->backing_bo->bo_handle;
    tex->flag_range.va = tex->backing_bo->gpu_va + lay.flag_offset;
    tex->flag_range.bytes = lay.flag_total_bytes;
    tex->data_range.bo = tex->backing_bo->bo_handle;
    tex->data_range.va = tex->backing_bo->gpu_va + lay.data_offset;
    tex->data_range.bytes = lay.data_total_bytes;

    memset(&tin, 0, sizeof(tin));
    tin.format_id = tex->format_caps->tiled_fmt_id;
    tin.swap = tex->format_caps->tiled_swap;
    tin.type = A840_TEX_2D;
    tin.width = desc->width;
    tin.height = desc->height;
    tin.depth = tex->array_layers;
    tin.mip_levels = 1u;
    tin.samples = tex->samples;
    tin.pitch = lay.data_pitch;
    tin.slice_stride = lay.data_slice_bytes;
    tin.gpu_va = tex->data_range.va;
    tin.tile_mode = A840_TILE_UBWC;
    tin.ubwc_meta_va = tex->flag_range.va;
    tin.ubwc_meta_pitch = lay.flag_pitch;
    tin.ubwc_meta_slice_stride = lay.flag_slice_bytes;
    tin.ubwc_block_width = lay.ubwc_block_width;
    tin.ubwc_block_height = lay.ubwc_block_height;
    tin.highest_bank_bit = hbb;
    tin.srgb = tex->is_srgb ? 1u : 0u;
    tin.pitch_align_log2 = lay.data_pitch_align_log2;
    tin.swizzle[0] = A840_SWIZZLE_R;
    tin.swizzle[1] = A840_SWIZZLE_G;
    tin.swizzle[2] = A840_SWIZZLE_B;
    tin.swizzle[3] = A840_SWIZZLE_A;

    est = a840_encode_tex_const(&tin, tex->tex_const[0]);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    tex->plane_count = 1u;
    tex->plane_mtl_formats[0] = desc->mtl_pixel_format;
    tex->plane_widths[0] = desc->width;
    tex->plane_heights[0] = desc->height;
    tex->plane_pitches[0] = lay.data_pitch;
    tex->plane_offsets[0] = lay.data_offset;
    tex->plane_bytes[0] = lay.data_total_bytes;

    tex->for_scanout = desc->for_scanout;
    if (desc->for_scanout) {
        tex->scanout_eligible = mdss_accepts_ubwc6(desc->mdss_caps, tex);
        tex->fallback_linear_required = !tex->scanout_eligible;
    }

    memset(&tex->surface_meta, 0, sizeof(tex->surface_meta));
    tex->surface_meta.bo_handle = tex->backing_bo->bo_handle;
    tex->surface_meta.op = 1u;
    tex->surface_meta.ubwc_enabled = 1u;
    tex->surface_meta.ubwc_version = ubwc_ver;
    tex->surface_meta.highest_bank_bit = hbb;
    tex->surface_meta.swizzle_levels = swizzle;
    tex->surface_meta.macrotile_mode = macrotile;
    tex->surface_meta.meta_offset = lay.flag_offset;
    tex->surface_meta.meta_pitch = lay.flag_pitch;
    tex->surface_meta.flags = tex->scanout_eligible ? A840_SCANOUT_FLAG_ELIGIBLE : 0u;

    if (device->transport.ops && device->transport.ops->surface_meta_v3 &&
        (device->transport_caps.abi_flags & NativeAdrenoParamsV3UbwcSurfaceMeta) != 0u) {
        struct NativeAdrenoAbiSurfaceMetaV3 wire = tex->surface_meta;
        if (device->transport.ops->surface_meta_v3(device->transport.ctx, &wire) ==
            A840_TRANSPORT_OK) {
            tex->surface_meta = wire;
            tex->surface_meta_negotiated = true;
        }
    }

    if (tex->for_scanout && tex->fallback_linear_required) {
        st = allocate_linear_resolve(device, tex);
        if (st != A840_RT_OK) {
            return st;
        }
    }
    return A840_RT_OK;
}

static A840RtStatus create_memoryless(const A840UbwcTextureDesc *desc,
                                      A840UbwcTexture *tex)
{
    const A840FormatCaps *caps;
    uint32_t cpp;

    if (desc->storage_mode != A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    if ((desc->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) != 0u || desc->for_scanout) {
        return A840_RT_UNSUPPORTED;
    }
    caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    if (!caps || !caps->renderable ||
        (caps->flags & (A840_FORMAT_COMPRESSED | A840_FORMAT_REQUIRES_XR_CODEC)) != 0u) {
        return A840_RT_UNSUPPORTED;
    }
    if (tex->samples > 1u && (caps->msaa & tex->samples) == 0u) {
        return A840_RT_UNSUPPORTED;
    }
    cpp = (caps->block_bits + 7u) / 8u;
    if (!is_pow2_u32(cpp) || cpp > 16u) {
        return A840_RT_UNSUPPORTED;
    }

    tex->format_caps = caps;
    tex->cpp = cpp;
    tex->tile_mode = A840_TILE_TILED;
    tex->is_srgb = (caps->flags & A840_FORMAT_SRGB) != 0u;
    tex->gmem_bytes_per_pixel = cpp * tex->samples;
    tex->backing_bo = NULL;
    tex->plane_count = 0u;
    memset(&tex->data_range, 0, sizeof(tex->data_range));
    memset(&tex->flag_range, 0, sizeof(tex->flag_range));
    return A840_RT_OK;
}

static A840RtStatus create_xr10(A840RtDevice *device,
                                const A840UbwcTextureDesc *desc,
                                A840UbwcTexture *tex)
{
    const A840FormatCaps *caps;
    uint32_t tile_mode = desc->tile_mode;
    uint32_t cpp, pitch_align_log2, pitch_align, row_align, pitch, padded_h;
    uint64_t slice_bytes = 0u, total_bytes;
    A840RtBufferDesc bdesc;
    A840TexDescInput tin;
    A840RtStatus st;
    A840EmitStatus est;

    if (desc->storage_mode == A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    if (!is_xr10_mtl_format(desc->mtl_pixel_format) ||
        tile_mode == A840_TILE_UBWC || tile_mode > A840_TILE_UBWC) {
        return A840_RT_UNSUPPORTED;
    }
    caps = a840_format_caps_lookup(desc->mtl_pixel_format);
    if (!caps || (caps->flags & A840_FORMAT_REQUIRES_XR_CODEC) == 0u) {
        return A840_RT_UNSUPPORTED;
    }

    cpp = (caps->block_bits + 7u) / 8u;
    if (tile_mode == A840_TILE_LINEAR) {
        pitch_align_log2 = 6u;
        row_align = 1u;
    } else {
        pitch_align_log2 = tiled_pitch_align_log2_for_caps(caps, 1u);
        row_align = tiled_row_align_for_caps(caps, 1u);
    }
    pitch_align = UINT32_C(1) << pitch_align_log2;
    if (desc->width > UINT32_MAX / cpp) {
        return A840_RT_RANGE_ERROR;
    }
    pitch = round_up_u32(desc->width * cpp, pitch_align);
    padded_h = round_up_u32(desc->height, row_align);
    if (!round_up_u64((uint64_t)pitch * (uint64_t)padded_h, A840_RT_PAGE_BYTES, &slice_bytes)) {
        return A840_RT_RANGE_ERROR;
    }
    if ((uint64_t)tex->array_layers > UINT64_MAX / slice_bytes) {
        return A840_RT_RANGE_ERROR;
    }
    total_bytes = slice_bytes * (uint64_t)tex->array_layers;
    if (total_bytes > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    tex->format_caps = caps;
    tex->cpp = cpp;
    tex->tile_mode = tile_mode;
    tex->is_xr10 = true;
    tex->is_srgb = (caps->flags & A840_FORMAT_SRGB) != 0u;
    tex->requires_xr_codec = true;
    tex->gmem_bytes_per_pixel = cpp;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size_bytes = total_bytes;
    bdesc.alignment = A840_RT_PAGE_BYTES;
    bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    if ((desc->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) != 0u ||
        desc->storage_mode == A840_STORAGE_SHARED) {
        bdesc.usage_flags |= A840_BUF_USAGE_CPU_VISIBLE;
    }

    st = a840_rt_buffer_create(device, &bdesc, &tex->backing_bo);
    if (st != A840_RT_OK) {
        return st;
    }

    tex->data_range.bo = tex->backing_bo->bo_handle;
    tex->data_range.va = tex->backing_bo->gpu_va;
    tex->data_range.bytes = total_bytes;

    memset(&tin, 0, sizeof(tin));
    tin.format_id = (tile_mode == A840_TILE_LINEAR) ? caps->a840_fmt_id : caps->tiled_fmt_id;
    tin.swap = (tile_mode == A840_TILE_LINEAR) ? caps->swap : caps->tiled_swap;
    tin.type = A840_TEX_2D;
    tin.width = desc->width;
    tin.height = desc->height;
    tin.depth = tex->array_layers;
    tin.mip_levels = 1u;
    tin.samples = 1u;
    tin.pitch = pitch;
    tin.slice_stride = slice_bytes;
    tin.gpu_va = tex->data_range.va;
    tin.tile_mode = tile_mode;
    tin.highest_bank_bit = device->chip_facts.highest_bank_bit;
    tin.srgb = 0u;
    tin.pitch_align_log2 = pitch_align_log2;
    tin.swizzle[0] = A840_SWIZZLE_R;
    tin.swizzle[1] = A840_SWIZZLE_G;
    tin.swizzle[2] = A840_SWIZZLE_B;
    tin.swizzle[3] = A840_SWIZZLE_A;

    est = a840_encode_tex_const(&tin, tex->tex_const[0]);
    if (est != A840_EMIT_OK) {
        return a840_rt_status_from_emit(est);
    }

    tex->plane_count = 1u;
    tex->plane_mtl_formats[0] = desc->mtl_pixel_format;
    tex->plane_widths[0] = desc->width;
    tex->plane_heights[0] = desc->height;
    tex->plane_pitches[0] = pitch;
    tex->plane_offsets[0] = 0u;
    tex->plane_bytes[0] = total_bytes;

    tex->for_scanout = desc->for_scanout;
    if (desc->for_scanout) {
        tex->scanout_eligible = false;
        tex->fallback_linear_required = true;
        st = allocate_linear_resolve(device, tex);
        if (st != A840_RT_OK) {
            return st;
        }
    }
    return A840_RT_OK;
}

static A840RtStatus create_ycbcr_multiplane(A840RtDevice *device,
                                            const A840UbwcTextureDesc *desc,
                                            A840UbwcTexture *tex)
{
    uint32_t plane_fmts[2], plane_w[2], plane_h[2], plane_pitches[2], plane_align_log2[2];
    uint64_t plane_slices[2], plane_offsets[2];
    const A840FormatCaps *pcaps[2];
    uint32_t tile_mode = desc->tile_mode;
    uint64_t total_bytes = 0u;
    A840RtBufferDesc bdesc;
    A840RtStatus st;
    uint32_t p;

    if (desc->storage_mode == A840_STORAGE_MEMORYLESS) {
        return A840_RT_INVALID_ARG;
    }
    if (tile_mode == A840_TILE_UBWC || tile_mode > A840_TILE_UBWC) {
        return A840_RT_UNSUPPORTED;
    }
    if ((desc->width & 1u) != 0u || (desc->height & 1u) != 0u ||
        desc->width < 2u || desc->height < 2u) {
        return A840_RT_INVALID_ARG;
    }
    if (desc->ycbcr_format == A840_YCBCR_NV12) {
        plane_fmts[0] = A840_MTL_FMT_R8_UNORM;
        plane_fmts[1] = A840_MTL_FMT_RG8_UNORM;
    } else if (desc->ycbcr_format == A840_YCBCR_P010) {
        plane_fmts[0] = A840_MTL_FMT_R16_UNORM;
        plane_fmts[1] = A840_MTL_FMT_RG16_UNORM;
    } else {
        return A840_RT_UNSUPPORTED;
    }

    plane_w[0] = desc->width;
    plane_h[0] = desc->height;
    plane_w[1] = desc->width / 2u;
    plane_h[1] = desc->height / 2u;

    for (p = 0u; p < 2u; ++p) {
        uint32_t cpp, pitch_align, row_align, padded_h;
        uint64_t plane_total;

        pcaps[p] = a840_format_caps_lookup(plane_fmts[p]);
        if (!pcaps[p]) {
            return A840_RT_UNSUPPORTED;
        }
        cpp = (pcaps[p]->block_bits + 7u) / 8u;
        if (tile_mode == A840_TILE_LINEAR) {
            plane_align_log2[p] = 6u;
            row_align = 1u;
        } else {
            plane_align_log2[p] = tiled_pitch_align_log2_for_caps(pcaps[p], 1u);
            row_align = tiled_row_align_for_caps(pcaps[p], 1u);
        }
        pitch_align = UINT32_C(1) << plane_align_log2[p];
        plane_pitches[p] = round_up_u32(plane_w[p] * cpp, pitch_align);
        padded_h = round_up_u32(plane_h[p], row_align);
        if (!round_up_u64((uint64_t)plane_pitches[p] * (uint64_t)padded_h,
                          A840_RT_PAGE_BYTES, &plane_slices[p])) {
            return A840_RT_RANGE_ERROR;
        }
        if ((uint64_t)tex->array_layers > UINT64_MAX / plane_slices[p]) {
            return A840_RT_RANGE_ERROR;
        }
        plane_total = plane_slices[p] * (uint64_t)tex->array_layers;
        plane_offsets[p] = total_bytes;
        if (total_bytes > UINT64_MAX - plane_total) {
            return A840_RT_RANGE_ERROR;
        }
        total_bytes += plane_total;
    }

    if (total_bytes > device->transport_caps.max_bo_bytes) {
        return A840_RT_RANGE_ERROR;
    }

    tex->format_caps = pcaps[0];
    tex->cpp = (pcaps[0]->block_bits + 7u) / 8u;
    tex->tile_mode = tile_mode;
    tex->gmem_bytes_per_pixel = tex->cpp;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size_bytes = total_bytes;
    bdesc.alignment = A840_RT_PAGE_BYTES;
    bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    if ((desc->usage_flags & A840_TEX_USAGE_CPU_VISIBLE) != 0u ||
        desc->storage_mode == A840_STORAGE_SHARED) {
        bdesc.usage_flags |= A840_BUF_USAGE_CPU_VISIBLE;
    }

    st = a840_rt_buffer_create(device, &bdesc, &tex->backing_bo);
    if (st != A840_RT_OK) {
        return st;
    }

    tex->data_range.bo = tex->backing_bo->bo_handle;
    tex->data_range.va = tex->backing_bo->gpu_va;
    tex->data_range.bytes = total_bytes;

    for (p = 0u; p < 2u; ++p) {
        A840TexDescInput tin;
        A840EmitStatus est;
        memset(&tin, 0, sizeof(tin));
        tin.format_id = (tile_mode == A840_TILE_LINEAR) ? pcaps[p]->a840_fmt_id : pcaps[p]->tiled_fmt_id;
        tin.swap = (tile_mode == A840_TILE_LINEAR) ? pcaps[p]->swap : pcaps[p]->tiled_swap;
        tin.type = A840_TEX_2D;
        tin.width = plane_w[p];
        tin.height = plane_h[p];
        tin.depth = tex->array_layers;
        tin.mip_levels = 1u;
        tin.samples = 1u;
        tin.pitch = plane_pitches[p];
        tin.slice_stride = plane_slices[p];
        tin.gpu_va = tex->backing_bo->gpu_va + plane_offsets[p];
        tin.tile_mode = tile_mode;
        tin.highest_bank_bit = device->chip_facts.highest_bank_bit;
        tin.srgb = 0u;
        tin.pitch_align_log2 = plane_align_log2[p];
        tin.swizzle[0] = A840_SWIZZLE_R;
        tin.swizzle[1] = (p == 0u) ? A840_SWIZZLE_ZERO : A840_SWIZZLE_G;
        tin.swizzle[2] = A840_SWIZZLE_ZERO;
        tin.swizzle[3] = A840_SWIZZLE_ONE;

        est = a840_encode_tex_const(&tin, tex->tex_const[p]);
        if (est != A840_EMIT_OK) {
            return a840_rt_status_from_emit(est);
        }

        tex->plane_mtl_formats[p] = plane_fmts[p];
        tex->plane_widths[p] = plane_w[p];
        tex->plane_heights[p] = plane_h[p];
        tex->plane_pitches[p] = plane_pitches[p];
        tex->plane_offsets[p] = plane_offsets[p];
        tex->plane_bytes[p] = plane_slices[p] * (uint64_t)tex->array_layers;
    }
    tex->plane_count = 2u;

    tex->for_scanout = desc->for_scanout;
    if (desc->for_scanout) {
        tex->scanout_eligible = false;
        tex->fallback_linear_required = true;
    }
    return A840_RT_OK;
}

A840RtStatus a840_ubwc_texture_create(A840RtDevice *device,
                                      const A840UbwcTextureDesc *desc,
                                      A840UbwcTexture **out_tex)
{
    A840UbwcTexture *tex;
    A840RtStatus st;

    if (!out_tex) {
        return A840_RT_INVALID_ARG;
    }
    *out_tex = NULL;
    if (!device || !desc || desc->width == 0u || desc->height == 0u ||
        desc->width > A840_LIMIT_MAX_TEXTURE_SIZE ||
        desc->height > A840_LIMIT_MAX_TEXTURE_SIZE) {
        return A840_RT_INVALID_ARG;
    }

    tex = (A840UbwcTexture *)calloc(1u, sizeof(*tex));
    if (!tex) {
        return A840_RT_OOM;
    }
    tex->device = device;
    tex->kind = desc->kind;
    tex->storage_mode = desc->storage_mode;
    tex->mtl_pixel_format = desc->mtl_pixel_format;
    tex->ycbcr_format = desc->ycbcr_format;
    tex->width = desc->width;
    tex->height = desc->height;
    tex->array_layers = desc->array_layers ? desc->array_layers : 1u;
    tex->samples = desc->samples ? desc->samples : 1u;
    tex->usage_flags = desc->usage_flags;

    if (tex->array_layers > A840_LIMIT_MAX_ARRAY_LAYERS ||
        !is_pow2_u32(tex->samples) || tex->samples > A840_LIMIT_MAX_SAMPLES) {
        free(tex);
        return A840_RT_RANGE_ERROR;
    }

    switch (desc->kind) {
    case A840_P3_TEX_UBWC6:
        st = create_ubwc6(device, desc, tex);
        break;
    case A840_P3_TEX_MEMORYLESS:
        st = create_memoryless(desc, tex);
        break;
    case A840_P3_TEX_XR10:
        st = create_xr10(device, desc, tex);
        break;
    case A840_P3_TEX_YCBCR_MULTIPLANE:
        st = create_ycbcr_multiplane(device, desc, tex);
        break;
    default:
        st = A840_RT_INVALID_ARG;
        break;
    }

    if (st != A840_RT_OK) {
        a840_ubwc_texture_destroy(tex);
        return st;
    }

    *out_tex = tex;
    return A840_RT_OK;
}

void a840_ubwc_texture_destroy(A840UbwcTexture *tex)
{
    if (!tex) {
        return;
    }
    if (tex->linear_resolve_bo) {
        a840_rt_buffer_release(tex->linear_resolve_bo);
        tex->linear_resolve_bo = NULL;
    }
    if (tex->backing_bo) {
        a840_rt_buffer_release(tex->backing_bo);
        tex->backing_bo = NULL;
    }
    free(tex);
}

A840RtStatus a840_ubwc_texture_validate_sysmem_target(const A840UbwcTexture *tex)
{
    if (!tex) {
        return A840_RT_INVALID_ARG;
    }
    if (tex->storage_mode == A840_STORAGE_MEMORYLESS ||
        tex->kind == A840_P3_TEX_MEMORYLESS || !tex->backing_bo ||
        tex->data_range.bo == 0u || tex->data_range.va == 0u) {
        return A840_RT_UNSUPPORTED;
    }
    return A840_RT_OK;
}

A840RtStatus a840_ubwc_texture_cpu_readback(const A840UbwcTexture *tex,
                                            void *dst,
                                            size_t dst_bytes)
{
    if (!tex || !dst || dst_bytes == 0u) {
        return A840_RT_INVALID_ARG;
    }
    if (tex->storage_mode == A840_STORAGE_MEMORYLESS ||
        tex->kind == A840_P3_TEX_MEMORYLESS || !tex->backing_bo) {
        return A840_RT_UNSUPPORTED;
    }
    if (!tex->backing_bo->cpu_ptr || !tex->backing_bo->cpu_mapped) {
        return A840_RT_STATE_ERROR;
    }
    if (dst_bytes > tex->data_range.bytes) {
        return A840_RT_RANGE_ERROR;
    }
    memcpy(dst,
           (const uint8_t *)tex->backing_bo->cpu_ptr + tex->ubwc.data_offset,
           dst_bytes);
    return A840_RT_OK;
}

A840RtStatus a840_ubwc_texture_fill_gmem_attachment(const A840UbwcTexture *tex,
                                                    A840GmemLoad load,
                                                    A840GmemStore store,
                                                    A840GmemAttachment *out_att,
                                                    A840GmemBinding *out_bind)
{
    A840GmemAttachment att;
    A840GmemBinding bind;

    if (!tex || !out_att || !out_bind || !tex->format_caps) {
        return A840_RT_INVALID_ARG;
    }

    memset(&att, 0, sizeof(att));
    memset(&bind, 0, sizeof(bind));

    if (tex->storage_mode == A840_STORAGE_MEMORYLESS ||
        tex->kind == A840_P3_TEX_MEMORYLESS) {
        if (load == A840_LOAD_LOAD || store != A840_STORE_DONT_CARE) {
            return A840_RT_UNSUPPORTED;
        }
        att.cpp = tex->cpp;
        att.samples = tex->samples;
        att.load = load;
        att.store = A840_STORE_DONT_CARE;
        att.memoryless = true;
        att.ubwc = false;
        bind.format = tex->format_caps->color_fmt_id;
        bind.srgb = tex->is_srgb;
        *out_att = att;
        *out_bind = bind;
        return A840_RT_OK;
    }

    if (tex->kind == A840_P3_TEX_UBWC6) {
        att.cpp = tex->cpp;
        att.samples = tex->samples;
        att.load = load;
        att.store = store;
        att.memoryless = false;
        att.ubwc = true;

        bind.format = tex->format_caps->color_fmt_id;
        bind.srgb = tex->is_srgb;
        bind.image.data = tex->data_range;
        bind.image.flags = tex->flag_range;
        bind.image.pitch = tex->ubwc.data_pitch;
        bind.image.flag_pitch = tex->ubwc.flag_pitch;
        bind.image.flag_array_pitch = (uint32_t)tex->ubwc.flag_slice_bytes;
        bind.image.swap = tex->format_caps->tiled_swap;
        bind.image.tiled = true;

        if (tex->for_scanout && tex->fallback_linear_required && tex->linear_resolve_bo) {
            bind.resolve.data = tex->linear_resolve_range;
            bind.resolve.pitch = tex->linear_resolve_pitch;
            bind.resolve.swap = tex->format_caps->color_swap;
            bind.resolve.tiled = false;
        }

        *out_att = att;
        *out_bind = bind;
        return A840_RT_OK;
    }

    return A840_RT_UNSUPPORTED;
}
