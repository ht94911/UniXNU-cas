// SPDX-License-Identifier: MIT
// Invoke the production imageblock selectors and check the host GPU's pixels.
#import "../NativeBridgeMetalFrontend.mm"
#include <cassert>
#include <cmath>

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        assert(device);
        NSData *air = [NSData dataWithContentsOfFile:@(argv[3])];
        assert(air.length);
        id<MTLLibrary> library = native_bridge_metal_library_with_air(air.bytes, air.length);
        assert(library);
        unsigned checked = 0;
        for (NSString *name in @[@"ib_half_cell", @"ib_half2_cell"]) {
            NSError *error = nil;
            id<MTLFunction> function = [library newFunctionWithName:name];
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            assert(pipeline && !error);
            auto dimensions = MTLSizeMake(8, 8, 1);
            assert([(NBComputePipeline *)pipeline imageblockMemoryLengthForDimensions:dimensions] == 512);
            assert([(NBComputePipeline *)pipeline imageblockMemoryLengthForDimensions:MTLSizeMake(0, 8, 1)] == 0);
            auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                               width:8 height:8 mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
            assert(texture);
            float pixels[8 * 8 * 4];
            for (float &value : pixels) value = -100;
            auto region = MTLRegionMake2D(0, 0, 8, 8);
            [texture replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:8 * 4 * sizeof(float)];
            id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:texture atIndex:0];
            [(NBComputeEncoder *)encoder setImageblockWidth:8 height:8];
            [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:dimensions];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            [texture getBytes:pixels bytesPerRow:8 * 4 * sizeof(float) fromRegion:region mipmapLevel:0];
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                const float *pixel = pixels + (y * 8 + x) * 4;
                assert(pixel[0] == x && pixel[1] == y && pixel[2] == 0 && pixel[3] == 0);
                ++checked;
            }
        }
        printf("PASS imageblock frontend: 2 kernels, %u pixels; production size/query and host Vulkan dispatch\n", checked);
    }
    return 0;
}
