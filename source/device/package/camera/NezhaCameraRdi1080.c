/* SPDX-License-Identifier: MIT */
#include "NezhaCameraRdi1080.h"

#include <stddef.h>

/* Full VFE1080 RDI0 is bus client 23. Register offsets and 256-byte units
 * follow Qualcomm CAMSS v24, camss-vfe-1080.c at 11c60fc17fd5. */
enum {
    VfeRdi0 = 0x1000 + 0x500 + 23 * 0x100,
    VfeFrameIncrement = VfeRdi0 + 0x08,
    VfeImageConfig0 = VfeRdi0 + 0x0c,
    VfeImageConfig2 = VfeRdi0 + 0x14,
    VfePackerConfig = VfeRdi0 + 0x18,
    VfeIrqSubsamplePeriod = VfeRdi0 + 0x30,
    VfeIrqSubsamplePattern = VfeRdi0 + 0x34,
    VfeFrameDropPeriod = VfeRdi0 + 0x38,
    VfeFrameDropPattern = VfeRdi0 + 0x3c,
    VfeMmuPrefetchConfig = VfeRdi0 + 0x60,
    VfeMmuPrefetchMaxOffset = VfeRdi0 + 0x64,
};

bool nezha_camera_rdi1080_layout(uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2], struct NezhaCameraRdi1080 *out)
{
    return nezha_camera_rdi1080_pool_layout(width, height, buffers,
        NezhaCameraRdi1080QueuedFrames, out);
}

bool nezha_camera_rdi1080_pool_layout(uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping *buffers, unsigned count,
    struct NezhaCameraRdi1080 *out)
{
    if (out == NULL || buffers == NULL || count < NezhaCameraRdi1080QueuedFrames ||
        count > NezhaCameraRdi1080MaxFrames || width == 0 || height == 0 || (width & 3u) != 0) {
        return false;
    }

    /* CSI-2 RAW10 has five bytes for each group of four pixels. */
    const uint64_t packed_line = (uint64_t)(width / 4u) * 5u;
    const uint64_t stride = (packed_line + 15u) & ~UINT64_C(15);
    if (stride > UINT32_MAX / height) {
        return false;
    }
    const uint64_t frame_bytes = stride * height;
    if ((frame_bytes & 255u) != 0) {
        return false;
    }

    const uint64_t limit = UINT64_C(1) << 32;
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t address = buffers[i].iova;
        const uint64_t mapped = buffers[i].mapped_bytes;
        if (address == 0 || address >= limit || (address & 255u) != 0 ||
            mapped < frame_bytes || mapped > limit - address) {
            return false;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = 0; j < i; ++j) {
            if (buffers[i].iova < buffers[j].iova + buffers[j].mapped_bytes &&
                buffers[j].iova < buffers[i].iova + buffers[i].mapped_bytes) {
                return false;
            }
        }
    }

    *out = (struct NezhaCameraRdi1080){
        .layout = {width, height, (uint32_t)packed_line, (uint32_t)stride,
                   (uint32_t)frame_bytes},
        .frame_increment = (uint32_t)(frame_bytes >> 8),
        .frame_count = count,
    };
    for (unsigned i = 0; i < count; ++i)
        out->image_address[i] = (uint32_t)(buffers[i].iova >> 8);
    return true;
}

void nezha_camera_rdi1080_program_geometry(const struct NezhaCameraRdi1080 *layout,
    void *context, void (*write)(void *, uint32_t offset, uint32_t value))
{
    write(context, VfeFrameIncrement, layout->frame_increment);
    /* Frame mode uses the payload length and MIPI packing from CSID. */
    write(context, VfeImageConfig0, 0xffff);
    write(context, VfeImageConfig2, 0xffff);
    write(context, VfePackerConfig, 0);
    write(context, VfeFrameDropPeriod, 0);
    write(context, VfeFrameDropPattern, 1);
    write(context, VfeIrqSubsamplePeriod, 0);
    write(context, VfeIrqSubsamplePattern, 1);
    /* Qualcomm v24 enables the write master's MMU prefetch before
     * WM_CFG. Keep that production sequence for this full RDI0 client. */
    write(context, VfeMmuPrefetchConfig, 1);
    write(context, VfeMmuPrefetchMaxOffset, UINT32_MAX);
}
