/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PIXEL_BUFFER_H
#define NEZHA_CAMERA_PIXEL_BUFFER_H

#include <CoreVideo/CoreVideo.h>
#include "NezhaCameraPreview.h"

/* Render one completed, CPU-owned RAW10 frame into the capture graph's
 * existing pixel buffer. The caller keeps both buffers alive until return.
 * Success includes unlocking the output; only then may it be delivered.
 * Formats are BGRA, 420f and 420v, using the pool's actual plane strides. */
CVReturn nezha_camera_preview_pixel_buffer(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, CVPixelBufferRef pixel_buffer);

#endif
