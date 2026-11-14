/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPixelBuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct __CFString { unsigned id; };
static const struct __CFString keys[] = {{0}, {1}, {2}, {3}, {4}, {5}};
const CFStringRef kCVImageBufferYCbCrMatrixKey = &keys[0];
const CFStringRef kCVImageBufferYCbCrMatrix_ITU_R_601_4 = &keys[1];
const CFStringRef kCVImageBufferColorPrimariesKey = &keys[2];
const CFStringRef kCVImageBufferColorPrimaries_ITU_R_709_2 = &keys[3];
const CFStringRef kCVImageBufferTransferFunctionKey = &keys[4];
const CFStringRef kCVImageBufferTransferFunction_sRGB = &keys[5];
struct __CVBuffer {
    size_t width, height, planes, plane_width[2], plane_height[2], stride[2], bytes;
    uint32_t format;
    uint8_t *base[2];
    unsigned locks, unlocks, attachments;
    CVReturn lock_status, unlock_status;
    Boolean planar, locked;
};

size_t CVPixelBufferGetWidth(CVPixelBufferRef p) { return p->width; }
size_t CVPixelBufferGetHeight(CVPixelBufferRef p) { return p->height; }
uint32_t CVPixelBufferGetPixelFormatType(CVPixelBufferRef p) { return p->format; }
Boolean CVPixelBufferIsPlanar(CVPixelBufferRef p) { return p->planar; }
size_t CVPixelBufferGetPlaneCount(CVPixelBufferRef p) { return p->planes; }
size_t CVPixelBufferGetWidthOfPlane(CVPixelBufferRef p, size_t i) { assert(i < 2); return p->plane_width[i]; }
size_t CVPixelBufferGetHeightOfPlane(CVPixelBufferRef p, size_t i) { assert(i < 2); return p->plane_height[i]; }
size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef p, size_t i) { assert(p->locked && i < 2); return p->stride[i]; }
void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef p, size_t i) { assert(p->locked && i < 2); return p->base[i]; }
size_t CVPixelBufferGetBytesPerRow(CVPixelBufferRef p) { assert(p->locked); return p->stride[0]; }
void *CVPixelBufferGetBaseAddress(CVPixelBufferRef p) { assert(p->locked); return p->base[0]; }
size_t CVPixelBufferGetDataSize(CVPixelBufferRef p) { assert(p->locked); return p->bytes; }
CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef p, CVPixelBufferLockFlags flags)
{
    assert(!flags && !p->locked); ++p->locks;
    if (!p->lock_status) p->locked = 1;
    return p->lock_status;
}
CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef p, CVPixelBufferLockFlags flags)
{
    assert(!flags && p->locked); ++p->unlocks; p->locked = 0;
    return p->unlock_status;
}
void CVBufferSetAttachment(CVPixelBufferRef p, CFStringRef key, CFTypeRef value, CVAttachmentMode mode)
{
    assert(!p->locked && mode == kCVAttachmentMode_ShouldPropagate);
    assert((key == &keys[0] && value == &keys[1] && p->planar) ||
        (key == &keys[2] && value == &keys[3]) || (key == &keys[4] && value == &keys[5]));
    p->attachments |= 1u << key->id;
}

int main(void)
{
    struct NezhaCameraPreview processor;
    nezha_camera_preview_init(&processor);
    /* White BGGR frame, two padded input rows per output row. */
    uint8_t raw[52]; memset(raw, 255, sizeof(raw));
    const struct NezhaCameraPreviewFrame frame = {8, 4, 13, 3, 64, 1023};
    uint8_t bgra[40], y[12], uv[6];
    struct __CVBuffer pixel = {.width=4, .height=2, .stride={20,0},
        .bytes=sizeof(bgra), .format=0x42475241, .base={bgra,NULL}};
    memset(bgra, 0xcd, sizeof(bgra));
    assert(nezha_camera_preview_pixel_buffer(&processor, &frame, raw, sizeof(raw), &pixel) == 0);
    assert(pixel.locks == 1 && pixel.unlocks == 1 && pixel.attachments == 0x14);
    for (unsigned row=0; row<2; ++row) {
        for (unsigned i=0; i<16; ++i) assert(bgra[row*20+i] == 255);
        for (unsigned i=16; i<20; ++i) assert(bgra[row*20+i] == 0xcd);
    }
    for (unsigned video=0; video<2; ++video) {
        pixel=(struct __CVBuffer){.width=4, .height=2, .planar=1, .planes=2,
            .plane_width={4,2}, .plane_height={2,1}, .stride={6,6},
            .format=video ? 0x34323076 : 0x34323066, .base={y,uv}};
        memset(y,0xcd,sizeof(y)); memset(uv,0xcd,sizeof(uv));
        assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == 0);
        assert(y[0] == (video ? 235 : 255) && uv[0] == 128 && uv[1] == 128);
        assert(y[4] == 0xcd && y[10] == 0xcd && uv[4] == 0xcd);
        assert(pixel.locks == 1 && pixel.unlocks == 1 && pixel.attachments == 0x15);
    }
    pixel.plane_width[1] = 4;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == kCVReturnInvalidArgument);
    assert(pixel.locks == 1);
    pixel.plane_width[1] = 2;
    pixel.lock_status = -7; pixel.attachments = 0;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == -7);
    assert(pixel.unlocks == 1 && !pixel.attachments);
    pixel.lock_status = 0; pixel.unlock_status = -8;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == -8);
    assert(pixel.unlocks == 2 && !pixel.attachments);
    pixel.unlock_status = 0;
    memset(y,0xcd,sizeof(y));
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,48,&pixel) == kCVReturnInvalidArgument);
    assert(pixel.unlocks == 3 && y[0] == 0xcd && !pixel.attachments);
    pixel.stride[0] = SIZE_MAX;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == kCVReturnInvalidArgument);
    assert(pixel.unlocks == 4 && y[0] == 0xcd);
    puts("camera pixel buffers: graph strides, BGRA/420f/420v, unlock-before-delivery and actual lock failures passed");
    return 0;
}
