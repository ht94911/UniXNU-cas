/* SPDX-License-Identifier: MIT */
/* Real Apple Metal.framework + CoreImage + IOSurface + MTL*SPI reflection host self-test.
 * Links the exact production A840MTL*.mm frontend against a840_mock_transport + host Mesa ir3
 * + host NativeMetalTranslate on macOS CI to catch ObjC SPI, CoreImage, completionQueue,
 * MTLFunctionConstantValues, MTLVertexDescriptor, and cross-library bugs BEFORE touching the phone. */
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#import "A840MTLCommand.h"
#import "A840MTLDevice.h"
#import "A840MTLEncoders.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLLibraryAndArchive.h"
#import "A840MTLPipelineProvider.h"
#import "A840MTLResource.h"
#import "A840MTLSystemSPI.h"
#include "a840_backend_transport.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>

static int gFailures = 0;

#define REQUIRE(cond, msg)                                                                  \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "SELFTEST FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            ++gFailures;                                                                    \
        }                                                                                   \
    } while (0)

static void checkSelectors(Class cls, const char *clsName, const char *const *sels)
{
    for (size_t i = 0u; sels[i] != nullptr; ++i) {
        SEL sel = sel_registerName(sels[i]);
        if (![cls instancesRespondToSelector:sel]) {
            std::fprintf(stderr, "MISSING SELECTOR: -[%s %s]\n", clsName, sels[i]);
            ++gFailures;
        }
    }
}

static void logProtocolReflection(Class cls, const char *clsName, const char *protoName)
{
    Protocol *proto = objc_getProtocol(protoName);
    if (!proto) return;
    unsigned int reqCount = 0u, optCount = 0u;
    struct objc_method_description *req = protocol_copyMethodDescriptionList(proto, YES, YES, &reqCount);
    struct objc_method_description *opt = protocol_copyMethodDescriptionList(proto, NO, YES, &optCount);
    unsigned int hitReq = 0u, hitOpt = 0u;
    for (unsigned int i = 0u; i < reqCount; ++i) {
        if ([cls instancesRespondToSelector:req[i].name]) ++hitReq;
    }
    for (unsigned int i = 0u; i < optCount; ++i) {
        if ([cls instancesRespondToSelector:opt[i].name]) ++hitOpt;
    }
    std::printf("PROTOCOL_REFLECT %s vs %s: required=%u/%u optional=%u/%u\n",
                clsName, protoName, hitReq, reqCount, hitOpt, optCount);
    free(req);
    free(opt);
}

static void runSelectorCompletenessAudit(void)
{
    static const char *const kDeviceSels[] = {
        "initLimits", "initFeatureQueries", "initGPUFamilySupport", "initWorkarounds",
        "name", "productName", "vendorName", "registryID", "maxBufferLength",
        "hasUnifiedMemory", "isLowPower", "isHeadless", "isRemovable",
        "dedicatedMemorySize", "sharedMemorySize", "recommendedMaxWorkingSetSize",
        "supportsFamily:", "supportsFeatureSet:", "supportsTextureSampleCount:",
        "supportsPrimitiveType:", "supportsExtendedXR10Formats", "supportsConditionalLoadStore",
        "isLargeMRTSupported", "supportsComputeCompressedTextureWrite",
        "gpuCoreCount", "maxVertexAmplificationCount", "deviceLinearTextureAlignmentBytes",
        "linearTextureAlignmentBytes", "iosurfaceTextureAlignmentBytes",
        "iosurfaceReadOnlyTextureAlignmentBytes",
        "maxVertexBuffers", "maxFragmentBuffers", "maxComputeBuffers", "maxTileBuffers",
        "maxVertexTextures", "maxFragmentTextures", "maxComputeTextures", "maxTileTextures",
        "maxVertexSamplers", "maxFragmentSamplers", "maxComputeSamplers", "maxTileSamplers",
        "maxComputeLocalMemorySizes", "maxComputeThreadgroupMemory",
        "maxComputeThreadgroupMemoryAlignmentBytes", "maxPredicatedNestingDepth",
        "maxRasterizationRateLayerCount", "maxTextureWidth1D", "maxTextureWidth2D",
        "maxTextureHeight2D", "maxTextureWidth3D", "maxTextureHeight3D", "maxTextureDepth3D",
        "maxTextureDimensionCube", "maxTextureLayers", "maxTotalComputeThreadsPerThreadgroup",
        "maxViewportCount", "maxColorAttachments", "maxVertexAttributes",
        "newCommandQueue", "newCommandQueueWithMaxCommandBufferCount:",
        "newBufferWithLength:options:", "newBufferWithBytes:length:options:",
        "newBufferWithBytesNoCopy:length:options:deallocator:",
        "newTextureWithDescriptor:", "newTextureWithDescriptor:iosurface:plane:",
        "newSamplerStateWithDescriptor:", "newDepthStencilStateWithDescriptor:",
        "newHeapWithDescriptor:", "newEvent", "newSharedEvent", "newFence",
        "newLibraryWithData:error:", "newLibraryWithURL:error:", "newLibraryWithFile:error:",
        "newComputePipelineStateWithFunction:error:",
        "newComputePipelineStateWithFunction:options:reflection:error:",
        "newComputePipelineStateWithFunction:completionHandler:",
        "newComputePipelineStateWithFunction:options:completionHandler:",
        "newRenderPipelineStateWithDescriptor:error:",
        "newRenderPipelineStateWithDescriptor:options:reflection:error:",
        "newRenderPipelineStateWithDescriptor:completionHandler:",
        "newRenderPipelineStateWithDescriptor:options:completionHandler:",
        "newRenderPipelineStateWithTileDescriptor:options:reflection:error:",
        "newRenderPipelineStateWithTileDescriptor:options:completionHandler:",
        "newPipelineLibraryWithFilePath:error:", "newResourceGroupFromResources:count:",
        "newTextureLayoutWithDescriptor:isHeapOrBufferBacked:",
        "newTextureWithBytesNoCopy:length:descriptor:deallocator:",
        "requiredLinearTextureBytesPerRowForDescriptor:",
        nullptr
    };
    static const char *const kQueueSels[] = {
        "device", "label", "setLabel:", "commandBuffer",
        "commandBufferWithUnretainedReferences", "commandBufferWithDescriptor:",
        "completionQueue", "setCompletionQueue:", "setSubmissionQueue:",
        "getGPUPriority", "setGPUPriority:", "setGPUPriority:offset:",
        "getBackgroundGPUPriority", "setBackgroundGPUPriority:", "setBackgroundGPUPriority:offset:",
        "isProfilingEnabled", "setProfilingEnabled:", "skipRender", "setSkipRender:",
        "executionEnabled", "setExecutionEnabled:", "insertDebugCaptureBoundary", "finish",
        nullptr
    };
    static const char *const kCommandBufferSels[] = {
        "device", "commandQueue", "status", "error", "commit", "enqueue",
        "waitUntilScheduled", "waitUntilCompleted", "commitAndWaitUntilSubmitted",
        "addScheduledHandler:", "addCompletedHandler:",
        "blitCommandEncoder", "computeCommandEncoder", "renderCommandEncoderWithDescriptor:",
        "kernelStartTime", "kernelEndTime", "GPUStartTime", "GPUEndTime",
        "protectionOptions", "setProtectionOptions:",
        "pushDebugGroup:", "popDebugGroup",
        "encodeCacheHintTag:resourceGroups:count:", "encodeCacheHintFinalize:resourceGroups:count:",
        nullptr
    };
    static const char *const kBufferSels[] = {
        "contents", "length", "gpuAddress", "iosurface", "protectionOptions",
        "setOwnerWithIdentity:", "waitUntilComplete", "didModifyRange:",
        "newTextureWithDescriptor:offset:bytesPerRow:",
        "newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:",
        nullptr
    };
    static const char *const kTextureSels[] = {
        "iosurface", "iosurfacePlane", "buffer", "bufferOffset", "bufferBytesPerRow",
        "rotation", "protectionOptions", "setOwnerWithIdentity:", "waitUntilComplete",
        "newTextureViewWithPixelFormat:",
        nullptr
    };
    static const char *const kLibSels[] = {
        "type", "functionNames", "newFunctionWithName:",
        "newFunctionWithName:constantValues:error:",
        "newFunctionWithName:constantValues:completionHandler:",
        "newFunctionWithDescriptor:error:",
        "newFunctionWithDescriptor:completionHandler:",
        "externFunctionNames", "newExternFunctionWithName:",
        "overrideTriple", "setOverrideTriple:", "shaderValidationEnabled", "setShaderValidationEnabled:",
        nullptr
    };
    static const char *const kFuncSels[] = {
        "name", "functionType", "options", "functionConstantsDictionary", nullptr
    };
    static const char *const kRenderEncSels[] = {
        "setRenderPipelineState:", "setVertexBuffer:offset:atIndex:",
        "setFragmentTexture:atIndex:",
        "setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:",
        "setLineWidth:", "tileWidth", "tileHeight",
        "setTileBuffer:offset:atIndex:", "dispatchThreadsPerTile:",
        "drawPrimitives:vertexStart:vertexCount:", "endEncoding",
        nullptr
    };
    static const char *const kBlitEncSels[] = {
        "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:",
        "copyFromTexture:toTexture:",
        "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:",
        "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:",
        "endEncoding",
        nullptr
    };

    checkSelectors(A840MTLDevice.class, "A840MTLDevice", kDeviceSels);
    checkSelectors(A840MTLCommandQueue.class, "A840MTLCommandQueue", kQueueSels);
    checkSelectors(A840MTLCommandBuffer.class, "A840MTLCommandBuffer", kCommandBufferSels);
    checkSelectors(A840MTLBuffer.class, "A840MTLBuffer", kBufferSels);
    checkSelectors(A840MTLTexture.class, "A840MTLTexture", kTextureSels);
    checkSelectors(A840MTLLibrary.class, "A840MTLLibrary", kLibSels);
    checkSelectors(A840MTLFunction.class, "A840MTLFunction", kFuncSels);
    checkSelectors(A840MTLRenderEncoder.class, "A840MTLRenderEncoder", kRenderEncSels);
    checkSelectors(A840MTLBlitEncoder.class, "A840MTLBlitEncoder", kBlitEncSels);

    logProtocolReflection(A840MTLDevice.class, "A840MTLDevice", "MTLDeviceSPI");
    logProtocolReflection(A840MTLCommandQueue.class, "A840MTLCommandQueue", "MTLCommandQueueSPI");
    logProtocolReflection(A840MTLCommandBuffer.class, "A840MTLCommandBuffer", "MTLCommandBufferSPI");
    logProtocolReflection(A840MTLLibrary.class, "A840MTLLibrary", "MTLLibrarySPI");
    logProtocolReflection(A840MTLFunction.class, "A840MTLFunction", "MTLFunctionSPI");
    logProtocolReflection(A840MTLTexture.class, "A840MTLTexture", "MTLTextureSPI");
    logProtocolReflection(A840MTLBuffer.class, "A840MTLBuffer", "MTLBufferSPI");
}

static void selftestTimeoutHandler(int sig)
{
    (void)sig;
    static const char msg[] = "SELFTEST_TIMEOUT_45S: hung inside selftest\n";
    (void)write(2, msg, sizeof(msg) - 1u);
    _exit(124);
}

int main(int argc, const char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    signal(SIGALRM, selftestTimeoutHandler);
    alarm(45u);
    A840MockTransport *mock = nullptr;
    @autoreleasepool {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s <libA.metallib> <libB.metallib>\n", argv[0]);
            return 2;
        }
        std::printf("=== 1. Selector Completeness & Apple Protocol Reflection ===\n");
        runSelectorCompletenessAudit();

        std::printf("=== 2. Mock Device + Custom completionQueue + Deadlock Guard ===\n");
        REQUIRE(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK && mock != nullptr,
                "create mock transport");
        A840BackendTransport backend = a840_mock_transport_interface(mock);
        NSError *err = nil;
        id<MTLDevice> dev = a840_metal_create_mock_device(backend, &err);
        REQUIRE(dev != nil && err == nil, "a840_metal_create_mock_device");
        if (!dev) return 1;

        id<MTLCommandQueue> q = [dev newCommandQueue];
        REQUIRE(q != nil, "newCommandQueue");
        auto *a840q = (A840MTLCommandQueue *)q;
        REQUIRE([a840q getGPUPriority] == 1u, "default gpuPriority == 1");
        REQUIRE([a840q getBackgroundGPUPriority] == 3u, "default backgroundGpuPriority == 3");

        static char qKey;
        dispatch_queue_t customQ = dispatch_queue_create("org.nezha.selftest.completion", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(customQ, &qKey, (void *)0x1234, nullptr);
        a840q.completionQueue = customQ;
        REQUIRE(a840q.completionQueue == customQ, "completionQueue setter/getter");

        id<MTLBuffer> b1 = [dev newBufferWithLength:4096u options:MTLResourceStorageModeShared];
        id<MTLBuffer> b2 = [dev newBufferWithLength:4096u options:MTLResourceStorageModeShared];
        REQUIRE(b1 != nil && b2 != nil, "shared buffers");
        std::memset(b1.contents, 0xAB, 4096u);

        std::atomic<bool> schedOnCustom{false};
        std::atomic<bool> compOnCustom{false};
        std::atomic<bool> *schedPtr = &schedOnCustom;
        std::atomic<bool> *compPtr = &compOnCustom;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [cb addScheduledHandler:^(id<MTLCommandBuffer> c) {
            (void)c;
            if (dispatch_get_specific(&qKey) == (void *)0x1234) schedPtr->store(true);
        }];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (dispatch_get_specific(&qKey) == (void *)0x1234) compPtr->store(true);
            /* Must NOT deadlock when waitUntilCompleted is called inside completionQueue! */
            [c waitUntilCompleted];
        }];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:b1 sourceOffset:0u toBuffer:b2 destinationOffset:0u size:4096u];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        REQUIRE(cb.status == MTLCommandBufferStatusCompleted && cb.error == nil, "blit completed");
        REQUIRE(schedOnCustom.load() && compOnCustom.load(), "handlers ran on custom completionQueue");
        REQUIRE(cb.GPUStartTime > 0.0 && cb.GPUEndTime >= cb.GPUStartTime, "GPUStartTime/GPUEndTime populated");

        std::printf("=== 3. Function Constants + VertexDescriptor + Cross-Library VS/FS ===\n");
        NSData *libAData = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        NSData *libBData = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
        REQUIRE(libAData.length > 0u && libBData.length > 0u, "load metallibs");

        id<MTLLibrary> libA = [dev newLibraryWithData:dispatch_data_create(libAData.bytes, libAData.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&err];
        REQUIRE(libA != nil && err == nil, "newLibraryWithData libA");
        id<MTLLibrary> libB = [dev newLibraryWithData:dispatch_data_create(libBData.bytes, libBData.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&err];
        REQUIRE(libB != nil && err == nil, "newLibraryWithData libB");

        MTLFunctionConstantValues *fcVs = [MTLFunctionConstantValues new];
        bool flipY = true;
        [fcVs setConstantValue:&flipY type:MTLDataTypeBool atIndex:0u];
        id<MTLFunction> vsFn = [libA newFunctionWithName:@"vs_main" constantValues:fcVs error:&err];
        REQUIRE(vsFn != nil && err == nil, "vs_main specialized with indexed Bool constant");

        MTLFunctionConstantValues *fcFs = [MTLFunctionConstantValues new];
        float gain = 0.5f;
        [fcFs setConstantValue:&gain type:MTLDataTypeFloat withName:@"kColorGain"];
        id<MTLFunction> fsFn = [libB newFunctionWithName:@"fs_main" constantValues:fcFs error:&err];
        REQUIRE(fsFn != nil && err == nil, "fs_main specialized with named Float constant");

        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2;
        vd.attributes[0].offset = 0u;
        vd.attributes[0].bufferIndex = 0u;
        vd.layouts[0].stride = 8u;
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        vd.layouts[0].stepRate = 1u;

        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = vsFn;
        rpd.fragmentFunction = fsFn;
        rpd.vertexDescriptor = vd;
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        REQUIRE(rpso != nil && err == nil, "cross-library render PSO with constants + vertexDescriptor");

        MTLFunctionConstantValues *fcCs = [MTLFunctionConstantValues new];
        uint32_t scale = 3u;
        [fcCs setConstantValue:&scale type:MTLDataTypeUInt atIndex:1u];
        id<MTLFunction> csFn = [libA newFunctionWithName:@"cs_scale" constantValues:fcCs error:&err];
        REQUIRE(csFn != nil && err == nil, "cs_scale specialized with indexed UInt constant");
        id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:csFn error:&err];
        REQUIRE(cpso != nil && err == nil, "compute PSO with function constant");

        std::printf("=== 4. Real macOS IOSurface Zero-Copy Import + Render Submission ===\n");
        NSDictionary *surfProps = @{
            (id)kIOSurfaceWidth: @64,
            (id)kIOSurfaceHeight: @64,
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfaceBytesPerRow: @256,
            (id)kIOSurfacePixelFormat: @(0x42475241u), /* 'BGRA' */
        };
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)surfProps);
        REQUIRE(surface != nullptr, "IOSurfaceCreate");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64u height:64u mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> surfTex = [dev newTextureWithDescriptor:td iosurface:surface plane:0u];
        REQUIRE(surfTex != nil, "newTextureWithDescriptor:iosurface:plane:");
        if (surfTex && [(id)surfTex isKindOfClass:A840MTLTexture.class]) {
            A840RtTexture *rt = ((A840MTLTexture *)surfTex).native;
            REQUIRE(rt && rt->backing_buffer && rt->backing_buffer->imported,
                    "IOSurface texture uses zero-copy host_import backing buffer");
        }

        const uint64_t subsBefore = a840_mock_transport_submission_count(mock);
        id<MTLCommandBuffer> rcb = [q commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = surfTex;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 1.0);
        id<MTLRenderCommandEncoder> renc = [rcb renderCommandEncoderWithDescriptor:pass];
        float verts[6] = {0.0f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
        id<MTLBuffer> vbuf = [dev newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
        [renc setRenderPipelineState:rpso];
        [renc setVertexBuffer:vbuf offset:0u atIndex:0u];
        [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0u vertexCount:3u];
        [renc endEncoding];
        [rcb commit];
        [rcb waitUntilCompleted];
        if (rcb.status != MTLCommandBufferStatusCompleted || rcb.error != nil) {
            std::fprintf(stderr, "RCB_DIAG: status=%lu code=%ld err=%s subsBefore=%llu subsAfter=%llu\n",
                         (unsigned long)rcb.status, (long)rcb.error.code,
                         rcb.error ? rcb.error.localizedDescription.UTF8String : "none",
                         (unsigned long long)subsBefore,
                         (unsigned long long)a840_mock_transport_submission_count(mock));
        }
        REQUIRE(rcb.status == MTLCommandBufferStatusCompleted && rcb.error == nil, "cross-library render pass completed");
        REQUIRE(a840_mock_transport_submission_count(mock) > subsBefore, "real A8XX PM4 submission emitted");

        /* Also exercise real compute dispatch with function constant specialization! */
        const uint64_t subsCompBefore = a840_mock_transport_submission_count(mock);
        id<MTLCommandBuffer> ccb = [q commandBuffer];
        id<MTLComputeCommandEncoder> cenc = [ccb computeCommandEncoder];
        [cenc setComputePipelineState:cpso];
        [cenc setBuffer:b1 offset:0u atIndex:0u];
        [cenc dispatchThreadgroups:MTLSizeMake(1u, 1u, 1u) threadsPerThreadgroup:MTLSizeMake(16u, 1u, 1u)];
        [cenc endEncoding];
        [ccb commit];
        [ccb waitUntilCompleted];
        if (ccb.status != MTLCommandBufferStatusCompleted || ccb.error != nil) {
            std::fprintf(stderr, "CCB_DIAG: status=%lu code=%ld err=%s\n",
                         (unsigned long)ccb.status, (long)ccb.error.code,
                         ccb.error ? ccb.error.localizedDescription.UTF8String : "none");
        }
        REQUIRE(ccb.status == MTLCommandBufferStatusCompleted && ccb.error == nil, "specialized compute dispatch completed");
        REQUIRE(a840_mock_transport_submission_count(mock) > subsCompBefore, "real A8XX compute PM4 emitted");

        std::printf("=== 5. Real Apple CoreImage CIContext Exercise ===\n");
        @try {
            std::printf("STEP 5a: creating CIContext...\n");
            id<MTLCommandQueue> ciQ = [dev newCommandQueue];
            CIContext *ciCtx = [CIContext contextWithMTLDevice:dev
                                                       options:@{kCIContextUseSoftwareRenderer: @NO}];
            REQUIRE(ciCtx != nil, "CIContext contextWithMTLDevice: succeeded without throwing SPI exception");
            std::printf("STEP 5b: CIContext created=%d, rendering solid...\n", ciCtx != nil);
            if (ciCtx && ciQ) {
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                CIImage *solid = [CIImage imageWithColor:[CIColor colorWithRed:0.25 green:0.5 blue:0.75 alpha:1.0]];
                id<MTLCommandBuffer> cicb = [ciQ commandBuffer];
                [ciCtx render:solid
                    toMTLTexture:surfTex
                   commandBuffer:cicb
                          bounds:CGRectMake(0, 0, 64, 64)
                      colorSpace:cs];
                std::printf("STEP 5c: committing solid cicb (status=%lu)...\n", (unsigned long)cicb.status);
                [cicb commit];
                [cicb waitUntilCompleted];
                std::printf("COREIMAGE_SOLID_STATUS: status=%lu err=%s\n",
                            (unsigned long)cicb.status,
                            cicb.error ? cicb.error.localizedDescription.UTF8String : "none");

                CIFilter *blur = [CIFilter filterWithName:@"CIGaussianBlur"];
                if (blur) {
                    std::printf("STEP 5d: rendering CIGaussianBlur...\n");
                    [blur setValue:[solid imageByCroppingToRect:CGRectMake(0, 0, 64, 64)] forKey:kCIInputImageKey];
                    [blur setValue:@(4.0) forKey:kCIInputRadiusKey];
                    CIImage *blurred = blur.outputImage;
                    id<MTLCommandBuffer> bcb = [ciQ commandBuffer];
                    [ciCtx render:blurred
                        toMTLTexture:surfTex
                       commandBuffer:bcb
                              bounds:CGRectMake(0, 0, 64, 64)
                          colorSpace:cs];
                    std::printf("STEP 5e: committing blur bcb (status=%lu)...\n", (unsigned long)bcb.status);
                    [bcb commit];
                    [bcb waitUntilCompleted];
                    std::printf("COREIMAGE_BLUR_STATUS: status=%lu err=%s\n",
                                (unsigned long)bcb.status,
                                bcb.error ? bcb.error.localizedDescription.UTF8String : "none");
                }
                CGColorSpaceRelease(cs);
            }
            ciCtx = nil;
            ciQ = nil;
        } @catch (NSException *ex) {
            std::fprintf(stderr, "COREIMAGE_EXCEPTION: %s | %s\n",
                         ex.name.UTF8String ?: "?",
                         ex.reason.UTF8String ?: "?");
            if ([ex.name isEqualToString:NSInvalidArgumentException]) {
                ++gFailures;
            }
        }

        if (surface) CFRelease(surface);
        /* Release ObjC objects before tearing down mock transport */
        surfTex = nil;
        rpso = nil;
        cpso = nil;
        vsFn = nil;
        fsFn = nil;
        csFn = nil;
        libA = nil;
        libB = nil;
        vbuf = nil;
        b1 = nil;
        b2 = nil;
        q = nil;
        dev = nil;
    }
    if (mock) a840_mock_transport_destroy(mock);

    if (gFailures == 0) {
        std::printf("PASS: a840_macos_metal_selftest (0 failures)\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: a840_macos_metal_selftest (%d failures)\n", gFailures);
    return 1;
}
