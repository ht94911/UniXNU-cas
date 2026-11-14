/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPreview.h"
#include <string.h>

void nezha_camera_preview_init(struct NezhaCameraPreview *preview)
{
    if (preview != NULL) isp_init(&preview->processing);
}

static bool fits(size_t stride, size_t rows, size_t bytes, size_t capacity)
{
    return rows != 0 && stride >= bytes &&
        rows - 1 <= (SIZE_MAX - bytes) / stride && (rows - 1) * stride + bytes <= capacity;
}

bool nezha_camera_preview_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output)
{
    if (preview == NULL || frame == NULL || raw == NULL || output == NULL ||
        frame->width < 4 || frame->width > 16384 || frame->width % 4 ||
        frame->height < 4 || frame->height > 16384 || frame->height % 2 ||
        frame->bayer > 3 || frame->black != 64 || frame->white != 1023 ||
        !fits(frame->stride, frame->height, (size_t)frame->width / 4 * 5, raw_bytes) ||
        output->width < 2 || output->width > 8192 || output->width % 2 ||
        output->height < 2 || output->height > 8192 || output->height % 2 ||
        output->plane[0] == NULL) return false;

    const size_t width = output->width;
    if (output->format == NezhaCameraPreviewBgra) {
        if (!fits(output->stride[0], output->height, width * 4, output->capacity[0])) return false;
    } else if (output->format == NezhaCameraPreviewNv12Full || output->format == NezhaCameraPreviewNv12Video) {
        if (output->plane[1] == NULL ||
            !fits(output->stride[0], output->height, width, output->capacity[0]) ||
            !fits(output->stride[1], output->height / 2, width, output->capacity[1])) return false;
    } else {
        return false;
    }

    struct hakios_camera_frame input;
    memset(&input, 0, sizeof(input));
    input.width = frame->width;
    input.height = frame->height;
    input.stride = frame->stride;
    input.bayer = frame->bayer;
    input.black = frame->black;
    /* No source kernel /dev interface or control command is used. */
    if (output->format == NezhaCameraPreviewBgra) {
        isp_render_bgra(&preview->processing, &input, raw, output->plane[0], output->stride[0],
            output->width, output->height, 0, output->height);
    } else {
        isp_render_nv12(&preview->processing, &input, raw, output->plane[0], output->stride[0],
            output->plane[1], output->stride[1], output->width, output->height, 0, output->height,
            output->format == NezhaCameraPreviewNv12Video);
    }
    return true;
}
