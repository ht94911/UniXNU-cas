/* SPDX-License-Identifier: MIT */
/* Source delivery only: Apple SDK/Metal 3.0 compilation is pending Mac CI.
 * Compile with -fobjc-arc; link cpu_reference.cpp, this file and main.mm
 * against Foundation and Metal.
 * Compile NezhaGpuTest.metal to a metallib and pass --metallib, or install it
 * as the default library. Runtime must supply the optional route-evidence
 * hook below after observing each command buffer's actual raw/fallback path.
 * Missing evidence is reported fallback:raw:unimplemented.global, never raw.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include "../ref/cpu_reference.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

extern "C" const char *a840_hwtest_path_for_command_buffer(void *) __attribute__((weak_import));
extern "C" id<MTLDevice> a840_metal_create_direct_device(NSError **error) __attribute__((weak_import));

using namespace nezha_gpu_test;
namespace {
void emit_console_line(const std::string &line) {
    const int fd = ::open("/dev/console", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return;
    const std::string framed = "[NezhaGpuTest] " + line + "\n";
    (void)::write(fd, framed.data(), framed.size());
    ::close(fd);
}
std::string format_nserror(NSError *err) {
    if (!err) return "nil";
    const char *dom = err.domain ? err.domain.UTF8String : "?";
    const char *desc = err.localizedDescription ? err.localizedDescription.UTF8String : "";
    return std::string(dom ? dom : "?") + ":" + std::to_string((long long)err.code) + " (" + (desc ? desc : "") + ")";
}
void need(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void command_ok(id<MTLCommandBuffer> command) {
    if (command.status != MTLCommandBufferStatusCompleted || command.error != nil) {
        const std::string msg = "Metal command failed status=" + std::to_string((unsigned long long)command.status) +
            " err=" + format_nserror(command.error);
        throw std::runtime_error(msg);
    }
}

class MetalHarness {
public:
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    std::string route;

    explicit MetalHarness(const char *metallib) {
        const char *directEnv = std::getenv("A840_DIRECT_IOKIT");
        const bool preferDirect = directEnv && directEnv[0] == '1';
        if (preferDirect) {
            need(a840_metal_create_direct_device != nullptr, "direct-iokit requested but a840_metal_create_direct_device missing");
            NSError *devErr = nil;
            device = a840_metal_create_direct_device(&devErr);
            if (!device) {
                const std::string msg = "direct-iokit device creation failed: " + format_nserror(devErr);
                emit_console_line(msg);
                throw std::runtime_error(msg);
            }
            emit_console_line(std::string("direct-iokit device opened: ") + (device.name ? device.name.UTF8String : "unknown"));
        } else {
            device = MTLCreateSystemDefaultDevice();
            if (!device && a840_metal_create_direct_device) {
                NSError *devErr = nil;
                device = a840_metal_create_direct_device(&devErr);
            }
            need(device != nil, "no MTLDevice");
        }
        queue = [device newCommandQueue]; need(queue != nil, "no command queue");
        NSError *error = nil;
        if (metallib) library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:metallib]] error:&error];
        else library = [device newDefaultLibrary];
        need(library != nil, "missing NezhaGpuTest metallib; use --metallib");
    }
    void observe(id<MTLCommandBuffer> command) {
        const char *evidence = a840_hwtest_path_for_command_buffer ?
            a840_hwtest_path_for_command_buffer((__bridge void *)command) : nullptr;
        const std::string current = evidence ? evidence : "fallback:raw:unimplemented.global";
        need(current == "raw" || current.rfind("fallback:raw:", 0) == 0, "invalid route evidence");
        if (route.empty() || route == "raw") route = current;
    }
    void finish(id<MTLCommandBuffer> command) {
        [command commit]; [command waitUntilCompleted]; command_ok(command); observe(command);
    }
    id<MTLComputePipelineState> compute(NSString *name) {
        NSError *error = nil;
        id<MTLFunction> function = [library newFunctionWithName:name]; need(function != nil, "missing compute function");
        id<MTLComputePipelineState> result = [device newComputePipelineStateWithFunction:function error:&error];
        need(result != nil, "compute pipeline creation failed"); return result;
    }
    id<MTLSamplerState> sampler(Filter filter) {
        MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
        descriptor.minFilter = descriptor.magFilter = filter == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        descriptor.normalizedCoordinates = YES;
        auto result = [device newSamplerStateWithDescriptor:descriptor]; need(result != nil, "sampler allocation"); return result;
    }
    id<MTLTexture> texture(const Image &image, MTLTextureUsage usage) {
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                           width:image.width height:image.height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared; descriptor.usage = usage;
        id<MTLTexture> result = [device newTextureWithDescriptor:descriptor]; need(result != nil, "texture allocation");
        [result replaceRegion:MTLRegionMake2D(0, 0, image.width, image.height) mipmapLevel:0
                    withBytes:image.pixels.data() bytesPerRow:image.width * sizeof(Bgra8)];
        return result;
    }
    Image read_image(id<MTLTexture> texture) {
        auto result = make_image(texture.width, texture.height, {0, 0, 0, 0});
        [texture getBytes:result.pixels.data() bytesPerRow:result.width * sizeof(Bgra8)
               fromRegion:MTLRegionMake2D(0, 0, result.width, result.height) mipmapLevel:0];
        return result;
    }
    template<typename T> id<MTLBuffer> buffer(const std::vector<T> &data) {
        auto result = [device newBufferWithBytes:data.data() length:data.size() * sizeof(T) options:MTLResourceStorageModeShared];
        need(result != nil, "buffer allocation"); return result;
    }
    template<typename T> std::vector<T> read_buffer(id<MTLBuffer> value) {
        need(value.contents != nullptr && value.length % sizeof(T) == 0, "buffer readback shape");
        std::vector<T> result(value.length / sizeof(T)); std::memcpy(result.data(), value.contents, value.length); return result;
    }
    Comparison buffer_copy() {
        Comparison total;
        for (auto count : {std::size_t(64), std::size_t(4096), std::size_t(1024 * 1024)}) {
            // 4/12-byte offsets exercise non-64B alignment while respecting
            // the Metal blit encoder's dword alignment requirement.
            for (auto offset : {std::size_t(0), std::size_t(4)}) {
                const auto source = byte_pattern(count + 64, 9), initial = byte_pattern(count + 64, 21);
                auto src = buffer(source), dst = buffer(initial);
                auto command = [queue commandBuffer]; need(command != nil, "command buffer allocation");
                auto encoder = [command blitCommandEncoder]; need(encoder != nil, "blit encoder");
                [encoder copyFromBuffer:src sourceOffset:offset toBuffer:dst destinationOffset:offset + 12 size:count];
                [encoder endEncoding]; finish(command);
                total.merge(compare_bytes(buffer_copy_reference(source, initial, offset, offset + 12, count), read_buffer<std::uint8_t>(dst)));
            }
        }
        return total;
    }
    Comparison texture_clear() {
        Comparison total;
        for (auto extent : {std::pair<std::size_t, std::size_t>{256, 256}, {1200, 2608}}) {
            const auto initial = gradient_image(extent.first, extent.second);
            const nezha_gpu_test::Rect rectangle{13, 19, extent.first - 29, extent.second - 39}; const Bgra8 color{17, 65, 193, 255};
            auto target = texture(initial, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
            auto command = [queue commandBuffer]; auto encoder = [command computeCommandEncoder]; need(encoder != nil, "clear encoder");
            const vector_uint4 rect{static_cast<unsigned>(rectangle.x), static_cast<unsigned>(rectangle.y),
                                    static_cast<unsigned>(rectangle.width), static_cast<unsigned>(rectangle.height)};
            const vector_float4 rgba{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1};
            [encoder setComputePipelineState:compute(@"clear_bgra8")]; [encoder setTexture:target atIndex:0];
            [encoder setBytes:&rect length:sizeof(rect) atIndex:0]; [encoder setBytes:&rgba length:sizeof(rgba) atIndex:1];
            [encoder dispatchThreadgroups:MTLSizeMake((rectangle.width + 7) / 8, (rectangle.height + 7) / 8, 1)
                     threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder endEncoding]; finish(command);
            total.merge(compare_images(clear_reference(initial, rectangle, color), read_image(target)));
        }
        return total;
    }
    Comparison texture_scale() {
        Comparison total; const auto source = gradient_image(256, 256);
        auto src = texture(source, MTLTextureUsageShaderRead);
        for (auto extent : {std::pair<std::size_t, std::size_t>{128, 128}, {512, 256}}) {
            for (auto filter : {Filter::Nearest, Filter::Bilinear}) {
                auto dst = texture(make_image(extent.first, extent.second, {0, 0, 0, 0}), MTLTextureUsageShaderWrite);
                auto command = [queue commandBuffer]; auto encoder = [command computeCommandEncoder]; need(encoder != nil, "scale encoder");
                [encoder setComputePipelineState:compute(@"scale_texture")]; [encoder setTexture:src atIndex:0]; [encoder setTexture:dst atIndex:1];
                [encoder setSamplerState:sampler(filter) atIndex:0];
                [encoder dispatchThreadgroups:MTLSizeMake((extent.first + 7) / 8, (extent.second + 7) / 8, 1)
                         threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [encoder endEncoding]; finish(command);
                total.merge(compare_images(scale_reference(source, extent.first, extent.second, filter), read_image(dst), filter == Filter::Nearest ? 0 : 1));
            }
        }
        return total;
    }
    Comparison add_vec() {
        constexpr std::size_t count = 4096, guards = 8;
        const auto a = vector_input(count, false), b = vector_input(count, true);
        const auto expected_values = add_vec_reference(a, b, 2);
        std::vector<float> initial(count + 2 * guards, float_guard), expected = initial;
        std::copy(expected_values.begin(), expected_values.end(), expected.begin() + guards);
        auto src_a = buffer(a), src_b = buffer(b), dst = buffer(initial);
        auto command = [queue commandBuffer]; auto encoder = [command computeCommandEncoder]; need(encoder != nil, "add encoder");
        const std::uint32_t length = count; const float multiplier = 2;
        [encoder setComputePipelineState:compute(@"add_vec")];
        [encoder setBuffer:src_a offset:0 atIndex:0]; [encoder setBuffer:src_b offset:0 atIndex:1];
        [encoder setBuffer:dst offset:guards * sizeof(float) atIndex:2];
        [encoder setBytes:&length length:sizeof(length) atIndex:3]; [encoder setBytes:&multiplier length:sizeof(multiplier) atIndex:4];
        [encoder dispatchThreadgroups:MTLSizeMake(count / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding]; finish(command); return compare_floats(expected, read_buffer<float>(dst), 1e-5);
    }
    Comparison nonuniform() {
        constexpr std::size_t width = 127, height = 83, guards = 8;
        const auto expected = nonuniform_grid_reference(width, height, guards);
        auto dst = buffer(std::vector<std::uint32_t>(expected.size(), grid_guard));
        const vector_uint2 extent{width, height};
        auto command = [queue commandBuffer]; auto encoder = [command computeCommandEncoder]; need(encoder != nil, "grid encoder");
        [encoder setComputePipelineState:compute(@"nonuniform_grid")];
        [encoder setBuffer:dst offset:guards * sizeof(std::uint32_t) atIndex:0]; [encoder setBytes:&extent length:sizeof(extent) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [encoder endEncoding]; finish(command); return compare_words(expected, read_buffer<std::uint32_t>(dst));
    }
    id<MTLRenderPipelineState> render_pipeline(bool textured) {
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"test_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:textured ? @"textured_fragment" : @"triangle_fragment"];
        need(descriptor.vertexFunction != nil && descriptor.fragmentFunction != nil, "missing render shader");
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (textured) {
            auto attachment = descriptor.colorAttachments[0]; attachment.blendingEnabled = YES;
            attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
            attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError *error = nil;
        auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error]; need(pipeline != nil, "render pipeline"); return pipeline;
    }
    Comparison render(bool textured) {
        constexpr std::size_t width = 256, height = 256; const Bgra8 clear{16, 32, 64, 255};
        const auto checker = checker_image(32, 32); Comparison total;
        const auto triangle = triangle_vertices(width, height); const auto quad = quad_vertices(width, height);
        for (auto index_type : {MTLIndexTypeUInt16, MTLIndexTypeUInt32}) {
            if (!textured && index_type == MTLIndexTypeUInt32) break;
            auto target = texture(make_image(width, height, clear), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            auto pass = [MTLRenderPassDescriptor renderPassDescriptor]; pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(clear.r / 255.0, clear.g / 255.0, clear.b / 255.0, 1);
            auto command = [queue commandBuffer]; auto encoder = [command renderCommandEncoderWithDescriptor:pass]; need(encoder != nil, "render encoder");
            [encoder setRenderPipelineState:render_pipeline(textured)]; [encoder setCullMode:MTLCullModeNone];
            [encoder setViewport:MTLViewport{0, 0, double(width), double(height), 0, 1}];
            const vector_float2 extent{width, height};
            [encoder setVertexBytes:&extent length:sizeof(extent) atIndex:1];
            if (textured) {
                [encoder setVertexBytes:quad.data() length:sizeof(quad) atIndex:0];
                auto sampled = texture(checker, MTLTextureUsageShaderRead); [encoder setFragmentTexture:sampled atIndex:0];
                [encoder setFragmentSamplerState:sampler(Filter::Nearest) atIndex:0];
                id<MTLBuffer> indices = index_type == MTLIndexTypeUInt16 ? buffer(std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3}) :
                                                                      buffer(std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:index_type indexBuffer:indices indexBufferOffset:0];
            } else {
                [encoder setVertexBytes:triangle.data() length:sizeof(triangle) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [encoder endEncoding]; finish(command);
            if (textured) total.merge(compare_images(indexed_quad_reference(width, height, checker, {0, 1, 2, 0, 2, 3}, clear), read_image(target), 1));
            else total.merge(compare_raster(triangle_reference(width, height, triangle, clear), read_image(target)));
        }
        return total;
    }
    Comparison fence_order() {
        constexpr std::size_t count = 16;
        auto initial = std::vector<std::uint32_t>(count + 1, 0); initial[0] = fence_chain_reference(count)[0];
        auto state = buffer(initial), reads = buffer(std::vector<std::uint32_t>(count, grid_guard)),
             after_reads = buffer(std::vector<std::uint32_t>(count, grid_guard));
        auto pipeline = compute(@"fence_step"), verify_pipeline = compute(@"fence_verify");
        NSMutableArray<id<MTLCommandBuffer>> *commands = [NSMutableArray array];
        struct CompletionState {
            std::vector<std::uint32_t> order;
            std::mutex mutex;
            std::condition_variable changed;
            std::size_t callbacks = 0;
        };
        const auto completions = std::make_shared<CompletionState>();
        for (std::uint32_t i = 0; i < count; ++i) {
            auto command = [queue commandBuffer]; auto encoder = [command computeCommandEncoder]; need(encoder != nil, "fence encoder");
            id<MTLFence> fence = [device newFence]; need(fence != nil, "MTLFence allocation");
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:state offset:0 atIndex:0]; [encoder setBuffer:reads offset:0 atIndex:1];
            [encoder setBytes:&i length:sizeof(i) atIndex:2];
            [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder updateFence:fence]; [encoder endEncoding];
            // A fence is scoped to encoders in this command buffer. Default
            // tracked buffers and one queue enforce the inter-buffer chain.
            auto verifier = [command computeCommandEncoder]; need(verifier != nil, "fence verify encoder");
            [verifier waitForFence:fence]; [verifier setComputePipelineState:verify_pipeline];
            [verifier setBuffer:state offset:0 atIndex:0]; [verifier setBuffer:after_reads offset:0 atIndex:1];
            [verifier setBytes:&i length:sizeof(i) atIndex:2];
            [verifier dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [verifier endEncoding];
            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                {
                    std::lock_guard<std::mutex> lock(completions->mutex);
                    if (completed.status == MTLCommandBufferStatusCompleted) completions->order.push_back(i);
                    ++completions->callbacks;
                }
                completions->changed.notify_all();
            }];
            [commands addObject:command]; [command commit];
        }
        bool successful = true;
        for (id<MTLCommandBuffer> command in commands) {
            [command waitUntilCompleted];
            successful = successful && command.status == MTLCommandBufferStatusCompleted && command.error == nil;
            observe(command);
        }
        std::unique_lock<std::mutex> lock(completions->mutex);
        need(completions->changed.wait_for(lock, std::chrono::seconds(30), [&] { return completions->callbacks == count; }),
             "command completion callbacks missing");
        need(successful, "fence chain Metal command failed");
        auto result = compare_fence_chain(read_buffer<std::uint32_t>(state), read_buffer<std::uint32_t>(reads), completions->order, count);
        const auto expected = fence_chain_reference(count);
        result.merge(compare_words(std::vector<std::uint32_t>(expected.begin() + 1, expected.end()), read_buffer<std::uint32_t>(after_reads)));
        return result;
    }
};
} // namespace

extern "C" int NezhaGpuTestRun(const char *suite, const char *selected, const char *json_out, const char *metallib) {
    @autoreleasepool {
        if (std::string(suite ? suite : "") != "phase2") { std::fprintf(stderr, "unsupported suite\n"); return 2; }
        if (selected && std::none_of(phase2_cases.begin(), phase2_cases.end(), [&](const char *name) { return std::string(name) == selected; })) {
            std::fprintf(stderr, "unknown case\n"); return 2;
        }
        NSMutableArray *records = [NSMutableArray array]; bool all_pixels = true, all_raw = true;
        try {
            MetalHarness harness(metallib);
            for (std::size_t i = 0; i < phase2_cases.size(); ++i) {
                if (selected && std::string(selected) != phase2_cases[i]) continue;
                harness.route.clear(); const auto started = std::chrono::steady_clock::now(); Comparison result;
                std::string note = "metal_measured";
                try {
                    switch (i) {
                    case 0: result = harness.buffer_copy(); break;
                    case 1: result = harness.texture_clear(); break;
                    case 2: result = harness.texture_scale(); break;
                    case 3: result = harness.add_vec(); break;
                    case 4: result = harness.nonuniform(); break;
                    case 5: result = harness.render(false); break;
                    case 6: result = harness.render(true); break;
                    case 7: result = harness.fence_order(); break;
                    default: throw std::runtime_error("invalid case");
                    }
                } catch (const std::exception &error) {
                    result.shape_equal = false; result.mismatched_px = 1; note = "metal_error";
                    std::fprintf(stderr, "%s: %s\n", phase2_cases[i], error.what());
                    emit_console_line(std::string("ERR ") + phase2_cases[i] + ": " + error.what());
                }
                if (harness.route.empty()) harness.route = "fallback:raw:unimplemented.global";
                if (harness.route == "fallback:raw:unimplemented.global") note = "route_evidence_missing";
                const auto time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
                const auto line = hwtest_line(phase2_cases[i], result, time, harness.route, note);
                std::printf("%s\n", line.c_str()); std::fflush(stdout);
                emit_console_line(line);
                all_pixels = all_pixels && result.passed(); all_raw = all_raw && harness.route == "raw";
                [records addObject:@{@"case": [NSString stringWithUTF8String:phase2_cases[i]], @"status": result.passed() ? @"PASS" : @"FAIL",
                                     @"path": [NSString stringWithUTF8String:harness.route.c_str()], @"max_err": @(result.max_err),
                                     @"mismatched_px": @(result.mismatched_px), @"time_us": @(time), @"note": [NSString stringWithUTF8String:note.c_str()]}];
            }
            if (json_out) {
                NSError *error = nil;
                NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"suite": @"phase2", @"records": records,
                    @"pixels_pass": @(all_pixels), @"all_raw": @(all_raw), @"hardware_acceptance": @NO} options:NSJSONWritingPrettyPrinted error:&error];
                need(data != nil && [data writeToFile:[NSString stringWithUTF8String:json_out] options:NSDataWritingAtomic error:&error], "cannot write JSON output");
            }
        } catch (const std::exception &error) {
            std::fprintf(stderr, "NezhaGpuTest: %s\n", error.what());
            emit_console_line(std::string("FATAL: ") + error.what());
            return 1;
        }
        emit_console_line(std::string("SUMMARY pixels_pass=") + (all_pixels ? "1" : "0") + " all_raw=" + (all_raw ? "1" : "0"));
        return all_pixels && all_raw ? 0 : 1;
    }
}
