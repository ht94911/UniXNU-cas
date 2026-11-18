// SPDX-License-Identifier: MIT
// Verify real surface imports, texture views and independent plane bytes.
#import "../NativeBridgeMetalFrontend.mm"
#import <CoreVideo/CoreVideo.h>
#include <cassert>
#include <cstdio>
#include <cstring>

static void checkPlane(id<MTLDevice> device, IOSurfaceRef surface, NSUInteger plane,
                       MTLPixelFormat format, unsigned bytesPerPixel, unsigned *checks) {
    const NSUInteger width = IOSurfaceGetWidthOfPlane(surface, plane);
    const NSUInteger height = IOSurfaceGetHeightOfPlane(surface, plane);
    const NSUInteger row = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
    const auto *expected = (const unsigned char *)IOSurfaceGetBaseAddressOfPlane(surface, plane);
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:surface plane:plane];
    assert(texture && texture.iosurface == surface && texture.iosurfacePlane == plane);
    ++*checks;
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:format];
    id<MTLTexture> nested = [view newTextureViewWithPixelFormat:format textureType:MTLTextureType2D
        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
    assert(view && nested && view.iosurface == surface && nested.iosurface == surface);
    assert(view.iosurfacePlane == plane && nested.iosurfacePlane == plane);
    assert(nested.width == width && nested.height == height);
    *checks += 2;

    // Copy through Vulkan, using the actual imported bytes and original rows.
    id<MTLBuffer> output = [device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    assert(output && command && blit);
    [blit copyFromTexture:nested sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(width, height, 1) toBuffer:output destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
    const auto *actual = (const unsigned char *)output.contents;
    for (NSUInteger y = 0; y < height; ++y)
        assert(!memcmp(actual + y * row, expected + y * row, width * bytesPerPixel));
    ++*checks;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        assert(device);
        unsigned checks = 0;
        CVPixelBufferRef pixel = nullptr;
        NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        assert(CVPixelBufferCreate(kCFAllocatorDefault, 64, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            (__bridge CFDictionaryRef)attributes, &pixel) == kCVReturnSuccess);
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel);
        assert(surface && IOSurfaceGetPlaneCount(surface) == 2);
        assert(IOSurfaceLock(surface, 0, nullptr) == 0);
        for (NSUInteger plane = 0; plane < 2; ++plane) {
            const NSUInteger width = IOSurfaceGetWidthOfPlane(surface, plane);
            const NSUInteger height = IOSurfaceGetHeightOfPlane(surface, plane);
            const NSUInteger row = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
            auto *bytes = (unsigned char *)IOSurfaceGetBaseAddressOfPlane(surface, plane);
            for (NSUInteger y = 0; y < height; ++y)
                for (NSUInteger x = 0; x < width * (plane + 1); ++x)
                    bytes[y * row + x] = (unsigned char)(17 + plane * 83 + x * 3 + y * 7);
        }
        assert(IOSurfaceUnlock(surface, 0, nullptr) == 0);
        checkPlane(device, surface, 0, MTLPixelFormatR8Unorm, 1, &checks);
        checkPlane(device, surface, 1, MTLPixelFormatRG8Unorm, 2, &checks);

        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
            width:64 height:16 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        assert(![device newTextureWithDescriptor:descriptor iosurface:surface plane:2]);
        ++checks;
        id<MTLTexture> plain = [device newTextureWithDescriptor:descriptor];
        assert(plain && !plain.iosurface && plain.iosurfacePlane == 0);
        ++checks;
        id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLTexture> fromBuffer = [buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:64];
        assert(fromBuffer && !fromBuffer.iosurface && fromBuffer.iosurfacePlane == 0);
        ++checks;
        CVPixelBufferRelease(pixel);
        printf("PASS surface planes: %u checks; real imports, nested views and Vulkan plane bytes\n", checks);
    }
    return 0;
}
