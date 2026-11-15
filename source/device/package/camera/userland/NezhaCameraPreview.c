/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPreview.h"
#include "../NezhaCameraControl.h"
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

static bool frame_valid(const struct NezhaCameraPreviewFrame *frame,
    const uint8_t *raw, size_t raw_bytes)
{
    return frame && raw && frame->width >= 4 && frame->width <= 16384 && !(frame->width % 4) &&
        frame->height >= 4 && frame->height <= 16384 && !(frame->height % 2) &&
        frame->bayer <= 3 && frame->black == 64 && frame->white == 1023 &&
        fits(frame->stride, frame->height, (size_t)frame->width / 4 * 5, raw_bytes);
}

bool nezha_camera_preview_update_white_balance(struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes)
{
    struct isp_stats statistics;
    return nezha_camera_preview_measure(preview,frame,raw,raw_bytes,&statistics);
}

bool nezha_camera_preview_measure(struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t raw_bytes,
    struct isp_stats *statistics)
{
    if (!preview || !statistics || !frame_valid(frame, raw, raw_bytes)) return false;
    const struct hakios_camera_frame input = {
        .width = frame->width, .height = frame->height, .stride = frame->stride,
        .bayer = frame->bayer, .black = frame->black,
    };
    isp_measure(&preview->processing, &input, raw, statistics);
    isp_awb(&preview->processing, statistics);
    return true;
}

bool nezha_camera_preview_auto_exposure(struct NezhaCameraPreview *preview,
    const struct hakios_camera_frame *f,const struct isp_stats *statistics,
    struct hakios_camera_command *command)
{
    if (!preview || !f || !statistics || !command || !statistics->cells ||
        !f->sequence || !f->time || !f->line_ns || f->margin!=10 ||
        f->gain_unity!=256 || f->gain<256 || f->gain>f->gain_max || f->gain_max>65280 ||
        f->frame_lines_min<=10 || f->frame_lines<f->frame_lines_min ||
        f->frame_lines_max<f->frame_lines || f->frame_lines_max>0xffffff ||
        !f->exposure_lines || f->exposure_lines>f->frame_lines-f->margin) return false;
    /* Use HakiOS's normal two-frame exposure ceiling. The actual sensor
     * limit in metadata also supports explicit long exposures; the live
     * preview controller does not jump to that full hardware limit. */
    const uint32_t normal_max=2u*f->frame_lines_min;
    preview->processing.max_frame_lines=f->frame_lines_max<normal_max ?
        f->frame_lines_max:normal_max;
    if (!isp_ae(&preview->processing,f,statistics,command)) return false;
    command->b=nezha_camera_control_gain_q8(command->b);
    if(!command->c)command->c=f->frame_lines_min;
    if(command->c<command->a+f->margin)command->c=command->a+f->margin;
    preview->processing.asked_gain=command->b;
    preview->processing.asked_frame_lines=command->c;
    return true;
}

static bool render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output, bool still)
{
    if (preview == NULL || !frame_valid(frame, raw, raw_bytes) || output == NULL ||
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
    } else if (still) {
        return isp_render_still_nv12(&preview->processing, &input, raw, output->plane[0], output->stride[0],
            output->plane[1], output->stride[1], output->width, output->height, 0, output->height,
            output->format == NezhaCameraPreviewNv12Video);
    } else {
        isp_render_nv12(&preview->processing, &input, raw, output->plane[0], output->stride[0],
            output->plane[1], output->stride[1], output->width, output->height, 0, output->height,
            output->format == NezhaCameraPreviewNv12Video);
    }
    return true;
}

bool nezha_camera_preview_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output)
{
    return render(preview, frame, raw, raw_bytes, output, false);
}

bool nezha_camera_still_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output)
{
    return render(preview, frame, raw, raw_bytes, output, true);
}
