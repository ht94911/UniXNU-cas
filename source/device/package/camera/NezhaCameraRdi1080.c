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
};

bool nezha_camera_rdi1080_layout(uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2], struct NezhaCameraRdi1080 *out)
{
    if (out == NULL || buffers == NULL || width == 0 || height == 0 || (width & 3u) != 0) {
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
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t address = buffers[i].iova;
        const uint64_t mapped = buffers[i].mapped_bytes;
        if (address == 0 || address >= limit || (address & 255u) != 0 ||
            mapped < frame_bytes || mapped > limit - address) {
            return false;
        }
    }
    if (buffers[0].iova < buffers[1].iova + buffers[1].mapped_bytes &&
        buffers[1].iova < buffers[0].iova + buffers[0].mapped_bytes) {
        return false;
    }

    *out = (struct NezhaCameraRdi1080){
        .layout = {width, height, (uint32_t)packed_line, (uint32_t)stride,
                   (uint32_t)frame_bytes},
        .frame_increment = (uint32_t)(frame_bytes >> 8),
        .image_address = {(uint32_t)(buffers[0].iova >> 8),
                          (uint32_t)(buffers[1].iova >> 8)},
    };
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
}
