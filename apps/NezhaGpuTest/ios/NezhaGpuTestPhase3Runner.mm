/* SPDX-License-Identifier: MIT */
/* Source delivery only; Metal 3.0 / Apple SDK / arm64e compilation pending Mac CI.
 * Build with -fobjc-arc and cpu_reference.cpp + cpu_reference_phase3.cpp.
 * Call NezhaGpuTestPhase3Run from the future Phase 3 app entry point; the
 * Phase 2 runner/main are intentionally independent of this source.
 *
 * Standard Metal cannot force Adreno GMEM/Sysmem, request UBWC6 or observe
 * compressed/escape blocks and MDSS negotiation. Optional native harness
 * adapters below must provide those operations and truthful per-command
 * evidence. They are integration requirements, NOT implemented driver APIs
 * or replacements for frozen contracts. Missing adapters produce FAIL.
 * The existing a840_hwtest_path_for_command_buffer hook proves raw routing.
 * CPU model bytes are NEVER uploaded as a Qualcomm compressed bitstream.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include "../ref/cpu_reference_phase3.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" const char *a840_hwtest_path_for_command_buffer(void *) __attribute__((weak_import));
/* Local harness adapter version 1. The provider fills bytes=sizeof(struct).
 * Counters must reflect work actually executed, never requested capability.
 */
struct A840Phase3Evidence {
    std::uint32_t bytes, version;
    std::uint32_t gmem_passes, sysmem_passes, memoryless_depth_passes, fbfetch_draws;
    std::uint32_t tile_width, tile_height, tiles_x, tiles_y;
    std::uint32_t ubwc_version, highest_bank_bit, swizzle_levels, macrotile_mode;
    std::uint64_t compressed_blocks, escape_blocks;
    std::uint32_t mdss_direct_ubwc, mdss_native_linear;
};
extern "C" int a840_hwtest_phase3_begin_scope(void *command, const char *request) __attribute__((weak_import));
extern "C" int a840_hwtest_phase3_evidence_for_command_buffer(void *command, A840Phase3Evidence *) __attribute__((weak_import));
/* Texture is a +0 returned MTLTexture; the caller's strong reference retains
 * it. Provider must use native UBWC6 BGRA8, ShaderRead/Write/RenderTarget usage.
 */
extern "C" void *a840_hwtest_phase3_new_ubwc_texture(void *device, std::uint32_t width,
    std::uint32_t height, unsigned for_scanout) __attribute__((weak_import));
/* Encode actual MDSS compatibility/resolve operations into command. mode 0
 * requires UBWC direct decode; mode 1 requires native linear downgrade.
 * Destination receives the exact pixels from the negotiated scanout path.
 * This does not certify display timing or physical panel presentation.
 */
extern "C" int a840_hwtest_phase3_encode_scanout(void *command, void *source, void *destination,
    unsigned mode) __attribute__((weak_import));

using namespace nezha_gpu_test;
namespace p3 = nezha_gpu_test::phase3;
namespace {
void need(bool good, const char *message) { if (!good) throw std::runtime_error(message); }
class Harness {
public:
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    std::string route;
    explicit Harness(const char *metallib) {
        device = MTLCreateSystemDefaultDevice(); need(device != nil, "no Metal device");
        queue = [device newCommandQueue]; need(queue != nil, "no command queue");
        NSError *error = nil;
        library = metallib ? [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:metallib]] error:&error] :
                             [device newDefaultLibrary];
        need(library != nil, "missing Phase 3 Metal library");
    }
    id<MTLCommandBuffer> command(const char *scope = nullptr) {
        auto cb = [queue commandBuffer]; need(cb != nil, "command allocation");
        if (scope) need(a840_hwtest_phase3_begin_scope &&
            a840_hwtest_phase3_begin_scope((__bridge void *)cb, scope) == 0, "Phase 3 native scope adapter unavailable");
        return cb;
    }
    void finish(id<MTLCommandBuffer> cb) {
        [cb commit]; [cb waitUntilCompleted];
        need(cb.status == MTLCommandBufferStatusCompleted && cb.error == nil, "Metal command failed");
        const char *value = a840_hwtest_path_for_command_buffer ? a840_hwtest_path_for_command_buffer((__bridge void *)cb) : nullptr;
        const std::string current = value ? value : "fallback:raw:unimplemented.global";
        need(current == "raw" || current.rfind("fallback:raw:", 0) == 0, "invalid native route evidence");
        if (route.empty() || route == "raw") route = current;
    }
    A840Phase3Evidence evidence(id<MTLCommandBuffer> cb) {
        A840Phase3Evidence value{}; value.bytes = sizeof(value); value.version = 1;
        need(a840_hwtest_phase3_evidence_for_command_buffer &&
            a840_hwtest_phase3_evidence_for_command_buffer((__bridge void *)cb, &value) == 0,
            "Phase 3 executed feature evidence unavailable");
        need(value.bytes == sizeof(value) && value.version == 1, "Phase 3 evidence version mismatch"); return value;
    }
    void require_tiles(const A840Phase3Evidence &actual, unsigned width, unsigned height) {
        need(actual.gmem_passes > 0 && actual.sysmem_passes == 0 && actual.tile_width == 96 && actual.tile_height == 32 &&
             actual.tiles_x == (width + 95) / 96 && actual.tiles_y == (height + 31) / 32,
             "requested 96x32 GMEM Tile grid was not executed");
    }
    id<MTLTexture> texture(std::size_t w, std::size_t h, MTLPixelFormat format = MTLPixelFormatBGRA8Unorm,
                           MTLStorageMode storage = MTLStorageModePrivate) {
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        descriptor.storageMode = storage; descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        if (storage == MTLStorageModeMemoryless) descriptor.usage = MTLTextureUsageRenderTarget;
        auto result = [device newTextureWithDescriptor:descriptor]; need(result != nil, "texture allocation"); return result;
    }
    id<MTLTexture> ubwc_texture(std::size_t w, std::size_t h, bool scanout) {
        need(a840_hwtest_phase3_new_ubwc_texture != nullptr, "native UBWC allocation adapter unavailable");
        id<MTLTexture> result = (__bridge id<MTLTexture>)a840_hwtest_phase3_new_ubwc_texture((__bridge void *)device,
            static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), scanout ? 1u : 0u);
        need(result != nil && result.width == w && result.height == h && result.pixelFormat == MTLPixelFormatBGRA8Unorm,
             "native UBWC texture invalid"); return result;
    }
    void upload(id<MTLTexture> target, const Image &image) {
        const std::size_t row = (image.width * 4 + 255) & ~std::size_t(255);
        auto staging = [device newBufferWithLength:row * image.height options:MTLResourceStorageModeShared];
        need(staging != nil && staging.contents != nullptr, "upload buffer");
        std::memset(staging.contents, 0, staging.length);
        for (std::size_t y = 0; y < image.height; ++y) std::memcpy(static_cast<unsigned char *>(staging.contents) + y * row,
            image.pixels.data() + y * image.width, image.width * 4);
        auto cb = command(); auto blit = [cb blitCommandEncoder]; need(blit != nil, "upload blit");
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * image.height
                 sourceSize:MTLSizeMake(image.width, image.height, 1) toTexture:target destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding]; finish(cb);
    }
    Image readback(id<MTLTexture> source) {
        const std::size_t row = (source.width * 4 + 255) & ~std::size_t(255);
        auto staging = [device newBufferWithLength:row * source.height options:MTLResourceStorageModeShared];
        need(staging != nil && staging.contents != nullptr, "readback buffer");
        auto cb = command(); auto blit = [cb blitCommandEncoder]; need(blit != nil, "readback blit");
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(source.width, source.height, 1) toBuffer:staging destinationOffset:0
                 destinationBytesPerRow:row destinationBytesPerImage:row * source.height];
        [blit endEncoding]; finish(cb);
        auto result = make_image(source.width, source.height, {0, 0, 0, 0});
        for (std::size_t y = 0; y < result.height; ++y) std::memcpy(result.pixels.data() + y * result.width,
            static_cast<const unsigned char *>(staging.contents) + y * row, result.width * 4);
        return result;
    }
    id<MTLRenderPipelineState> pipeline(NSString *fragment, bool scene = false, bool depth = false, bool blend = false) {
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:scene ? @"p3_scene_vertex" : @"p3_fullscreen_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:fragment];
        need(descriptor.vertexFunction != nil && descriptor.fragmentFunction != nil, "missing Phase 3 shader");
        auto color = descriptor.colorAttachments[0]; color.pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (depth) descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        if (blend) {
            color.blendingEnabled = YES; color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne; color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError *error = nil; auto result = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        need(result != nil, "Phase 3 render pipeline failed"); return result;
    }
    id<MTLRenderCommandEncoder> encoder(id<MTLCommandBuffer> cb, id<MTLTexture> target,
                                       id<MTLTexture> depth = nil, bool load = false) {
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor]; auto color = pass.colorAttachments[0];
        color.texture = target; color.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear; color.storeAction = MTLStoreActionStore;
        color.clearColor = MTLClearColorMake(p3::scene_clear.r / 255.0, p3::scene_clear.g / 255.0, p3::scene_clear.b / 255.0, 1);
        if (depth) {
            pass.depthAttachment.texture = depth; pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = 1.0; pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        }
        auto result = [cb renderCommandEncoderWithDescriptor:pass]; need(result != nil, "Phase 3 render encoder");
        [result setCullMode:MTLCullModeNone]; [result setViewport:MTLViewport{0, 0, double(target.width), double(target.height), 0, 1}]; return result;
    }
    Image render_scene(const std::vector<p3::Triangle> &scene, bool memoryless, const char *scope) {
        auto target = texture(288, 96); id<MTLTexture> depth = memoryless ?
            texture(288, 96, MTLPixelFormatDepth32Float, MTLStorageModeMemoryless) : nil;
        auto cb = command(scope); auto draw = encoder(cb, target, depth);
        [draw setRenderPipelineState:pipeline(@"p3_flat_fragment", true, memoryless, !memoryless)];
        if (memoryless) {
            auto descriptor = [MTLDepthStencilDescriptor new]; descriptor.depthCompareFunction = MTLCompareFunctionLess; descriptor.depthWriteEnabled = YES;
            auto state = [device newDepthStencilStateWithDescriptor:descriptor]; need(state != nil, "depth state"); [draw setDepthStencilState:state];
        }
        const vector_float2 extent{288, 96}; [draw setVertexBytes:&extent length:sizeof(extent) atIndex:1];
        for (const auto &triangle : scene) {
            [draw setVertexBytes:triangle.vertices.data() length:sizeof(triangle.vertices) atIndex:0];
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [draw endEncoding]; finish(cb); const auto actual = evidence(cb);
        if (std::string(scope) == "force_sysmem") need(actual.sysmem_passes > 0 && actual.gmem_passes == 0, "Sysmem scope not executed");
        else require_tiles(actual, 288, 96);
        if (memoryless) need(actual.memoryless_depth_passes > 0, "Memoryless depth was not executed");
        return readback(target);
    }
    Comparison equivalence() {
        const auto scene = p3::overlap_scene(); const auto expected = p3::raster_linear(288, 96, scene).color;
        const auto sysmem = render_scene(scene, false, "force_sysmem"), gmem = render_scene(scene, false, "force_gmem_96x32");
        auto result = compare_images(expected, sysmem, 0); result.merge(compare_images(expected, gmem, 0));
        result.merge(compare_images(sysmem, gmem, 0)); return result;
    }
    Comparison memoryless() { return compare_images(p3::raster_linear(288, 96, p3::depth_scene(), true).color,
        render_scene(p3::depth_scene(), true, "force_gmem_96x32"), 0); }
    Comparison tile_boundary() {
        auto target = texture(1200, 2608); auto cb = command("force_gmem_96x32"); auto draw = encoder(cb, target);
        [draw setRenderPipelineState:pipeline(@"p3_boundary_fragment")];
        const vector_uint2 extent{1200, 2608}; [draw setFragmentBytes:&extent length:sizeof(extent) atIndex:0];
        [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [draw endEncoding]; finish(cb);
        require_tiles(evidence(cb), 1200, 2608);
        return p3::compare_tiles(p3::boundary_image(), readback(target)).pixels;
    }
    Comparison programmable_blend() {
        const auto initial = p3::boundary_image(288, 96); auto target = texture(288, 96); upload(target, initial);
        auto cb = command("force_gmem_96x32"); auto draw = encoder(cb, target, nil, true);
        [draw setRenderPipelineState:pipeline(@"p3_fetch_fragment")];
        for (std::uint32_t pass = 0; pass < 4; ++pass) {
            [draw setFragmentBytes:&pass length:sizeof(pass) atIndex:0];
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [draw endEncoding]; finish(cb); const auto actual = evidence(cb);
        require_tiles(actual, 288, 96);
        need(actual.fbfetch_draws == 4, "four on-chip framebuffer fetch draws not observed");
        return compare_images(p3::glass_linear(initial), readback(target), 0);
    }
    A840Phase3Evidence write_ubwc(id<MTLTexture> target) {
        auto cb = command("ubwc6_write"); auto compute = [cb computeCommandEncoder]; need(compute != nil, "UBWC write encoder");
        NSError *error = nil; auto function = [library newFunctionWithName:@"p3_ubwc_write"];
        need(function != nil, "missing UBWC write shader");
        auto state = [device newComputePipelineStateWithFunction:function error:&error]; need(state != nil, "UBWC write pipeline");
        [compute setComputePipelineState:state]; [compute setTexture:target atIndex:0];
        [compute dispatchThreads:MTLSizeMake(target.width, target.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [compute endEncoding]; finish(cb); auto actual = evidence(cb);
        need(actual.ubwc_version == 6 && actual.highest_bank_bit == 16 && actual.swizzle_levels == 6 && actual.macrotile_mode == 1,
             "UBWC6 HBB16 configuration not observed");
        need(actual.compressed_blocks > 0 && actual.escape_blocks > 0, "UBWC compressed and escape branches not observed"); return actual;
    }
    Comparison ubwc_roundtrip() {
        auto source = ubwc_texture(256, 128, false); (void)write_ubwc(source); auto target = texture(256, 128);
        auto cb = command("ubwc6_sample"); auto draw = encoder(cb, target);
        [draw setRenderPipelineState:pipeline(@"p3_ubwc_sample_fragment")]; [draw setFragmentTexture:source atIndex:0];
        auto descriptor = [MTLSamplerDescriptor new]; descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterNearest;
        descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        auto nearest = [device newSamplerStateWithDescriptor:descriptor]; need(nearest != nil, "UBWC sampler");
        [draw setFragmentSamplerState:nearest atIndex:0]; [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [draw endEncoding]; finish(cb); need(evidence(cb).ubwc_version == 6, "UBWC sampling not observed");
        return compare_images(p3::ubwc_pattern(), readback(target), 0);
    }
    Comparison scanout_equivalence() {
        auto source = ubwc_texture(1200, 2608, true); (void)write_ubwc(source);
        need(a840_hwtest_phase3_encode_scanout != nullptr, "MDSS scanout adapter unavailable");
        std::array<Image, 2> images;
        for (unsigned mode = 0; mode < 2; ++mode) {
            auto target = texture(1200, 2608); auto cb = command(mode ? "mdss_linear" : "mdss_direct");
            need(a840_hwtest_phase3_encode_scanout((__bridge void *)cb, (__bridge void *)source, (__bridge void *)target, mode) == 0,
                 "MDSS encoding failed");
            finish(cb); const auto actual = evidence(cb);
            need(mode ? actual.mdss_native_linear > 0 && actual.mdss_direct_ubwc == 0 :
                        actual.mdss_direct_ubwc > 0 && actual.mdss_native_linear == 0, "MDSS negotiated path mismatch");
            images[mode] = readback(target);
        }
        const auto expected = p3::ubwc_pattern(1200, 2608); auto result = compare_images(expected, images[0], 0);
        result.merge(compare_images(expected, images[1], 0)); result.merge(compare_images(images[0], images[1], 0)); return result;
    }
};
} // namespace

extern "C" int NezhaGpuTestPhase3Run(const char *selected, const char *json_out, const char *metallib) {
    @autoreleasepool {
        if (selected && std::none_of(p3::cases.begin(), p3::cases.end(), [&](const char *name) { return std::string(name) == selected; })) {
            std::fprintf(stderr, "unknown Phase 3 case\n"); return 2;
        }
        try {
            Harness harness(metallib); NSMutableArray *records = [NSMutableArray array]; bool failed = false;
            for (std::size_t i = 0; i < p3::cases.size(); ++i) {
                if (selected && std::string(p3::cases[i]) != selected) continue;
                harness.route.clear(); Comparison result; std::string note = "metal_phase3_pixels";
                const auto started = std::chrono::steady_clock::now();
                try {
                    switch (i) {
                    case 0: result = harness.equivalence(); break;
                    case 1: result = harness.memoryless(); break;
                    case 2: result = harness.tile_boundary(); break;
                    case 3: result = harness.programmable_blend(); break;
                    case 4: result = harness.ubwc_roundtrip(); break;
                    case 5: result = harness.scanout_equivalence(); break;
                    }
                } catch (const std::exception &error) {
                    std::fprintf(stderr, "%s: %s\n", p3::cases[i], error.what());
                    result.shape_equal = false; result.mismatched_px = 1; note = "phase3_execution_or_evidence_failed";
                }
                const std::string route = harness.route.empty() ? "fallback:raw:unimplemented.global" : harness.route;
                failed = failed || !result.passed() || result.max_err != 0 || route != "raw";
                const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
                const auto line = hwtest_line(p3::cases[i], result, elapsed, route, note); std::puts(line.c_str());
                [records addObject:@{@"case": [NSString stringWithUTF8String:p3::cases[i]], @"status": result.passed() ? @"PASS" : @"FAIL",
                    @"path": [NSString stringWithUTF8String:route.c_str()], @"max_err": @(result.max_err), @"mismatched_px": @(result.mismatched_px),
                    @"time_us": @(elapsed), @"note": [NSString stringWithUTF8String:note.c_str()]}];
            }
            if (json_out) {
                NSError *error = nil; NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"suite": @"phase3", @"records": records,
                    @"device_verified": @NO, @"physical_scanout_verified": @NO} options:NSJSONWritingPrettyPrinted error:&error];
                need(data != nil && [data writeToFile:[NSString stringWithUTF8String:json_out] options:NSDataWritingAtomic error:&error], "Phase 3 JSON write failed");
            }
            return failed ? 1 : 0;
        } catch (const std::exception &error) { std::fprintf(stderr, "Phase 3 Runner: %s\n", error.what()); return 1; }
    }
}
