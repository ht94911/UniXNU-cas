/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PREVIEW_H
#define NEZHA_CAMERA_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "upstream/hakios/camera_isp.h"

/* User-space processing of CPU-owned, completed MIPI RAW10. These are
 * processing descriptors, not a new kernel transport or an iOS ABI. */
struct NezhaCameraPreviewFrame {
    uint32_t width, height, stride;
    uint32_t bayer; /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
    uint32_t black, white;
};

enum NezhaCameraPreviewFormat {
    NezhaCameraPreviewBgra, NezhaCameraPreviewNv12Full, NezhaCameraPreviewNv12Video,
};

struct NezhaCameraPreviewOutput {
    enum NezhaCameraPreviewFormat format;
    uint32_t width, height;
    uint8_t *plane[2];
    size_t stride[2], capacity[2];
};

struct NezhaCameraPreview {
    struct isp processing;
};

void nezha_camera_preview_init(struct NezhaCameraPreview *preview);

/* Current stock OVX mode is BGGR, black=64, white=1023. The unchanged
 * upstream processor uses that black/white scale. Other calibrations
 * must be implemented explicitly before they can be accepted here.
 * The caller owns raw and the output for the duration of this call.
 * Acquire CPU ownership of a completed DMA frame and invalidate its cache
 * before passing it to this processor; retain its memory until return. */
bool nezha_camera_preview_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output);

#endif
