/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPixelBuffer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

static void parallel_rows(void)
{
    /* Nonuniform, padded RAW with output heights that divide into uneven
     * pairs. Compare every byte, including row padding and outer guards. */
    uint8_t raw[84 * 32];
    memset(raw, 0xcd, sizeof(raw));
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; x += 4) {
        uint8_t *group = raw + y * 84 + x / 4 * 5;
        group[4] = 0;
        for (unsigned j = 0; j < 4; ++j) {
            unsigned value = (x * 7 + y * 19 + j * 43) % 1024;
            group[j] = value >> 2;
            group[4] |= (value & 3) << (j * 2);
        }
    }
    const unsigned heights[] = {2, 6, 14, 16, 18, 1304};
    for (unsigned bayer = 0; bayer < 4; ++bayer) {
        struct NezhaCameraPreview processor;
        nezha_camera_preview_init(&processor);
        const struct NezhaCameraPreviewFrame frame = {64, 32, 84, bayer, 64, 1023};
        assert(nezha_camera_preview_update_white_balance(&processor, &frame, raw, sizeof(raw)));
        for (unsigned mode = 0; mode < 3; ++mode) for (unsigned h = 0; h < 6; ++h) {
            const unsigned width = 34, height = heights[h];
            const bool planar = mode != 0;
            const size_t stride = width * (planar ? 1 : 4) + 10;
            const size_t ybytes = stride * height, uvbytes = planar ? stride * height / 2 : 0;
            const size_t bytes = ybytes + uvbytes;
            uint8_t *serial = malloc(bytes + 32), *parallel = malloc(bytes + 32);
            assert(serial && parallel);
            memset(serial, 0xcd, bytes + 32); memset(parallel, 0xcd, bytes + 32);
            const struct NezhaCameraPreviewOutput reference = {
                mode == 0 ? NezhaCameraPreviewBgra : mode == 1 ?
                    NezhaCameraPreviewNv12Full : NezhaCameraPreviewNv12Video,
                width, height, {serial + 16, planar ? serial + 16 + ybytes : NULL},
                {stride, planar ? stride : 0}, {ybytes, uvbytes},
            };
            assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &reference));
            struct __CVBuffer pixel = {
                .width = width, .height = height, .planes = planar ? 2 : 0,
                .plane_width = {width, width / 2}, .plane_height = {height, height / 2},
                .stride = {stride, planar ? stride : 0}, .bytes = ybytes,
                .format = mode == 0 ? 0x42475241 : mode == 1 ? 0x34323066 : 0x34323076,
                .base = {parallel + 16, planar ? parallel + 16 + ybytes : NULL},
                .planar = planar,
            };
            assert(!nezha_camera_preview_pixel_buffer(&processor, &frame, raw, sizeof(raw), &pixel));
            assert(!memcmp(serial, parallel, bytes + 32));
            assert(pixel.locks == 1 && pixel.unlocks == 1 && !pixel.locked);
            memset(serial, 0xcd, bytes + 32);
            assert(!nezha_camera_preview_render_rows(&processor, &frame, raw, sizeof(raw), &reference, 1, height));
            assert(!nezha_camera_preview_render_rows(&processor, &frame, raw, sizeof(raw), &reference, 0, height + 2));
            assert(!nezha_camera_preview_render_rows(&processor, &frame, raw, sizeof(raw), &reference, 2, 2));
            for (size_t i = 0; i < bytes + 32; ++i) assert(serial[i] == 0xcd);
            free(serial); free(parallel);
        }
    }
    puts("camera row groups: real dispatch, four CFA layouts, six uneven heights and BGRA/420f/420v match every serial byte");
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
        assert(nezha_camera_still_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel)==0);
        assert(y[0]==(video ? 235:255) && uv[0]==128 && uv[1]==128);
        assert(pixel.locks==2 && pixel.unlocks==2 && pixel.attachments==0x15);
        assert(y[4]==0xcd && y[10]==0xcd && uv[4]==0xcd);
    }
    pixel.plane_width[1] = 4;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == kCVReturnInvalidArgument);
    assert(pixel.locks == 2);
    pixel.plane_width[1] = 2;
    pixel.lock_status = -7; pixel.attachments = 0;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == -7);
    assert(pixel.unlocks == 2 && !pixel.attachments);
    pixel.lock_status = 0; pixel.unlock_status = -8;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == -8);
    assert(pixel.unlocks == 3 && !pixel.attachments);
    pixel.unlock_status = 0;
    memset(y,0xcd,sizeof(y));
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,48,&pixel) == kCVReturnInvalidArgument);
    assert(pixel.unlocks == 4 && y[0] == 0xcd && !pixel.attachments);
    pixel.stride[0] = SIZE_MAX;
    assert(nezha_camera_preview_pixel_buffer(&processor,&frame,raw,sizeof(raw),&pixel) == kCVReturnInvalidArgument);
    assert(pixel.unlocks == 5 && y[0] == 0xcd);
    parallel_rows();
    puts("camera pixel buffers: graph strides, BGRA/420f/420v, unlock-before-delivery and actual lock failures passed");
    return 0;
}
