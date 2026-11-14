/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPixelBuffer.h"

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

CVReturn nezha_camera_preview_pixel_buffer(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, CVPixelBufferRef pixel)
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
    const bool rendered = valid && nezha_camera_preview_render(preview, frame, raw, raw_bytes, &output);
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
