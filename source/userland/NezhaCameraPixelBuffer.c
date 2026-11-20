/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPixelBuffer.h"
#include <dispatch/dispatch.h>
#include <stdio.h>

/* Graph-owned BufferPool and Lock/Unlock sequence follow pinned HakiOS
 * camera_device.c. Pixel processing stays in NezhaCameraPreview. These
 * public CoreVideo exports are present in iPhone14,4 24A446, UUID
 * d444941e-cfd4-3912-b203-d8364d71b4b4. */
enum {
    PixelBgra = 0x42475241, PixelNv12Full = 0x34323066,
    PixelNv12Video = 0x34323076,
};

static bool plane_capacity(size_t stride, size_t rows, size_t *capacity)
{
    if (!stride || !rows || rows > SIZE_MAX / stride) return false;
    *capacity = stride * rows;
    return true;
}

/* The first few actual graph buffers distinguish malformed Y/UV content
 * from a later display import. Read only after all row groups complete,
 * while the existing CoreVideo write lock is still held. */
static void observe_output(const struct NezhaCameraPreviewOutput *output)
{
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)>=4) return;
    const size_t x=(output->width/2)&~(size_t)1, y=output->height/2;
    const size_t bytes=output->format==NezhaCameraPreviewBgra ? 4u:1u;
    const uint8_t *first=output->plane[0]+y*output->stride[0]+x*bytes;
    uint8_t values[4]={first[0],0,0,0};
    if (bytes==4) for (unsigned i=1;i<4;++i) values[i]=first[i];
    else {
        const uint8_t *uv=output->plane[1]+(y/2)*output->stride[1]+x;
        values[1]=uv[0];values[2]=uv[1];
    }
    unsigned low=255,high=0;
    for (unsigned row=0;row<4;++row) {
        const size_t at=(size_t)row*(output->height-1)/3;
        const uint8_t *line=output->plane[0]+at*output->stride[0];
        for (unsigned column=0;column<4;++column) {
            const uint8_t v=line[(size_t)column*(output->width-1)/3*bytes];
            if(v<low)low=v;
            if(v>high)high=v;
        }
    }
    fprintf(stderr,"nezha-camera: rendered pixel format=%u size=%ux%u stride=%llu/%llu center=%u/%u/%u/%u first-plane=%u..%u\n",
        (unsigned)output->format,output->width,output->height,
        (unsigned long long)output->stride[0],(unsigned long long)output->stride[1],
        values[0],values[1],values[2],values[3],low,high);
}

/* HakiOS f128f1d camera_device.c render_piece/deliver uses eight
 * synchronous row groups on the global queue. Keep one completed RAW and
 * a frame-local ISP snapshot alive until all groups finish. CoreVideo
 * lock/unlock and the caller's output callback remain on the serial path. */
struct PreviewRows {
    const struct NezhaCameraPreview *preview;
    const struct NezhaCameraPreviewFrame *frame;
    const uint8_t *raw;
    size_t raw_bytes;
    const struct NezhaCameraPreviewOutput *output;
    uint32_t pieces;
    bool completed[8];
};

static void preview_rows(void *context, size_t index)
{
    struct PreviewRows *job = context;
    const uint32_t pairs = job->output->height / 2;
    const uint32_t first = (uint32_t)(pairs * index / job->pieces) * 2;
    const uint32_t end = (uint32_t)(pairs * (index + 1) / job->pieces) * 2;
    job->completed[index] = nezha_camera_preview_render_rows(job->preview, job->frame,
        job->raw, job->raw_bytes, job->output, first, end);
}

static bool render_preview(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output)
{
    const struct NezhaCameraPreview snapshot = *preview;
    struct PreviewRows job = {
        .preview = &snapshot, .frame = frame, .raw = raw, .raw_bytes = raw_bytes,
        .output = output, .pieces = output->height < 16 ? output->height / 2 : 8,
    };
    dispatch_apply_f(job.pieces, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0),
        &job, preview_rows);
    for (unsigned i = 0; i < job.pieces; ++i) if (!job.completed[i]) return false;
    return true;
}

static CVReturn pixel_buffer(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, CVPixelBufferRef pixel, bool still)
{
    if (!preview || !frame || !raw || !pixel) return kCVReturnInvalidArgument;
    const size_t width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    const uint32_t format = CVPixelBufferGetPixelFormatType(pixel);
    if (width < 2 || width > 8192 || width % 2 ||
        height < 2 || height > 8192 || height % 2) return kCVReturnInvalidArgument;
    const bool nv12 = format == PixelNv12Full || format == PixelNv12Video;
    if (format != PixelBgra && !nv12) return kCVReturnInvalidArgument;
    if (nv12) {
        if (!CVPixelBufferIsPlanar(pixel) || CVPixelBufferGetPlaneCount(pixel) != 2 ||
            CVPixelBufferGetWidthOfPlane(pixel, 0) != width ||
            CVPixelBufferGetHeightOfPlane(pixel, 0) != height ||
            CVPixelBufferGetWidthOfPlane(pixel, 1) != width / 2 ||
            CVPixelBufferGetHeightOfPlane(pixel, 1) != height / 2) return kCVReturnInvalidArgument;
    } else if (CVPixelBufferIsPlanar(pixel)) {
        return kCVReturnInvalidArgument;
    }

    CVReturn status = CVPixelBufferLockBaseAddress(pixel, 0);
    if (status != kCVReturnSuccess) return status;
    struct NezhaCameraPreviewOutput output = {
        .format = !nv12 ? NezhaCameraPreviewBgra : format == PixelNv12Full ?
            NezhaCameraPreviewNv12Full : NezhaCameraPreviewNv12Video,
        .width = (uint32_t)width, .height = (uint32_t)height,
    };
    bool valid;
    if (nv12) {
        for (unsigned i = 0; i < 2; ++i) {
            output.plane[i] = CVPixelBufferGetBaseAddressOfPlane(pixel, i);
            output.stride[i] = CVPixelBufferGetBytesPerRowOfPlane(pixel, i);
        }
        valid = plane_capacity(output.stride[0], height, &output.capacity[0]) &&
            plane_capacity(output.stride[1], height / 2, &output.capacity[1]);
    } else {
        output.plane[0] = CVPixelBufferGetBaseAddress(pixel);
        output.stride[0] = CVPixelBufferGetBytesPerRow(pixel);
        output.capacity[0] = CVPixelBufferGetDataSize(pixel);
        valid = true;
    }
    const bool rendered = valid && (still ?
        nezha_camera_still_render(preview, frame, raw, raw_bytes, &output) :
        render_preview(preview, frame, raw, raw_bytes, &output));
    if (rendered) observe_output(&output);
    status = CVPixelBufferUnlockBaseAddress(pixel, 0);
    if (!rendered) return kCVReturnInvalidArgument;
    if (status != kCVReturnSuccess) return status;
    if (nv12) CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey,
        kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey,
        kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey,
        kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);
    return kCVReturnSuccess;
}

CVReturn nezha_camera_preview_pixel_buffer(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, CVPixelBufferRef pixel)
{
    return pixel_buffer(preview, frame, raw, raw_bytes, pixel, false);
}

CVReturn nezha_camera_still_pixel_buffer(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, CVPixelBufferRef pixel)
{
    return pixel_buffer(preview, frame, raw, raw_bytes, pixel, true);
}
