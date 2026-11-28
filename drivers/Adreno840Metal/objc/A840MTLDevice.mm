/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback full-system Adreno 840 Metal device. */
#import "A840MTLDevice.h"
#import "A840MTLCommand.h"
#import "A840MTLResource.h"
#import "A840MTLScanoutBridge.h"
#import "A840MTLSystemSPI.h"
#import "A840MTLDiagnostics.h"
#import <objc/message.h>
#import <objc/runtime.h>

#include <mutex>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


static void a840FillBaseLimits(id device)
{
    Class base = objc_getClass("_MTLDevice");
    SEL limitsSel = sel_registerName("limits");
    if (!base || !device || ![device isKindOfClass:base] || !class_getInstanceMethod(base, limitsSel)) {
        return;
    }
    uint8_t *limits = reinterpret_cast<uint8_t *(*)(id, SEL)>(objc_msgSend)(device, limitsSel);
    const ptrdiff_t at = limits - reinterpret_cast<uint8_t *>((__bridge void *)device);
    const ptrdiff_t instance = static_cast<ptrdiff_t>(class_getInstanceSize(base));
    if (!limits || at <= 0 || at >= instance) return;
    ptrdiff_t end = instance;
    bool found = false;
    unsigned ivarCount = 0u;
    Ivar *ivars = class_copyIvarList(base, &ivarCount);
    for (unsigned i = 0u; i < ivarCount; ++i) {
        const ptrdiff_t off = ivar_getOffset(ivars[i]);
        if (off == at) found = true;
        else if (off > at && off < end) end = off;
    }
    free(ivars);
    if (!found || end - at < 8) return;
    const size_t size = static_cast<size_t>(end - at);
    static const char *const kLimitSelectors[] = {
        "maxVertexBuffers", "maxFragmentBuffers", "maxComputeBuffers", "maxTileBuffers",
        "maxVertexTextures", "maxFragmentTextures", "maxComputeTextures", "maxTileTextures",
        "maxVertexSamplers", "maxFragmentSamplers", "maxComputeSamplers", "maxTileSamplers",
        "maxComputeLocalMemorySizes", "maxComputeThreadgroupMemory",
        "maxComputeThreadgroupMemoryAlignmentBytes", "maxPredicatedNestingDepth",
        "maxRasterizationRateLayerCount", "maxTextureWidth1D", "maxTextureWidth2D",
        "maxTextureHeight2D", "maxTextureWidth3D", "maxTextureHeight3D", "maxTextureDepth3D",
        "maxTextureDimensionCube", "maxTextureLayers", "maxTotalComputeThreadsPerThreadgroup",
        "maxViewportCount", "maxColorAttachments", "maxVertexAttributes",
        "maxVertexAmplificationCount", "maxBufferLength", "maxThreadgroupMemoryLength",
        "minConstantBufferAlignmentBytes",
        nullptr
    };
    Class ownCls = object_getClass(device);
    for (size_t idx = 0u; kLimitSelectors[idx] != nullptr; ++idx) {
        SEL sel = sel_registerName(kLimitSelectors[idx]);
        Method own = class_getInstanceMethod(ownCls, sel);
        Method orig = class_getInstanceMethod(base, sel);
        if (!own || !orig || method_getNumberOfArguments(orig) != 2u) continue;
        const char *enc = method_getTypeEncoding(own);
        if (!enc || enc[0] != 'Q') continue;
        auto ownGetter = reinterpret_cast<NSUInteger (*)(id, SEL)>(method_getImplementation(own));
        auto baseGetter = reinterpret_cast<NSUInteger (*)(id, SEL)>(method_getImplementation(orig));
        const uint64_t value = static_cast<uint64_t>(ownGetter(device, sel));
        for (size_t off = 0u; off + 4u <= size; off += 4u) {
            uint32_t saved = 0u;
            memcpy(&saved, limits + off, 4u);
            const uint32_t marker = 0x5a17c000u | static_cast<uint32_t>(off & 0xfffu);
            memcpy(limits + off, &marker, 4u);
            const NSUInteger seen = baseGetter(device, sel);
            memcpy(limits + off, &saved, 4u);
            if (static_cast<uint32_t>(seen) != marker || (seen >> 32) != 0u) continue;
            bool wide = false;
            if (off + 8u <= size) {
                uint32_t next = 0u, one = 1u;
                memcpy(&next, limits + off + 4u, 4u);
                memcpy(limits + off, &marker, 4u);
                memcpy(limits + off + 4u, &one, 4u);
                wide = (baseGetter(device, sel) >> 32) == 1u;
                memcpy(limits + off, &saved, 4u);
                memcpy(limits + off + 4u, &next, 4u);
            }
            if (wide) {
                memcpy(limits + off, &value, 8u);
            } else {
                const uint32_t narrow = value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
                memcpy(limits + off, &narrow, 4u);
            }
            break;
        }
    }
}

static char callbackKey;
static std::mutex deviceCreation;

NSError *a840_metal_error(A840RtStatus status, NSString *operation)
{
    return [NSError errorWithDomain:@"Adreno840Metal" code:status
                            userInfo:@{NSLocalizedDescriptionKey: operation}];
}

uint32_t a840_canonical_backing_pixel_format(uint32_t mtl_pixel_format)
{
    switch (mtl_pixel_format) {
    case 550u: /* MTLPixelFormatRGB10A8_2P_XR10 ('b3a8' plane 0 = 4 B/px) */
    case 554u: /* MTLPixelFormatBGR10_XR */
        return (uint32_t)MTLPixelFormatBGRA8Unorm;
    case 551u: /* MTLPixelFormatRGB10A8_2P_XR10_sRGB */
    case 555u: /* MTLPixelFormatBGR10_XR_sRGB */
        return (uint32_t)MTLPixelFormatBGRA8Unorm_sRGB;
    case 552u: /* MTLPixelFormatBGRA10_XR (8 B/px) */
    case 553u: /* MTLPixelFormatBGRA10_XR_sRGB (8 B/px) */
        return (uint32_t)MTLPixelFormatRGBA16Float;
    default:
        return mtl_pixel_format;
    }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@interface A840MTLResourceGroup : NSObject
@property(nonatomic, copy) NSArray *resources;
@end
@implementation A840MTLResourceGroup
@end

@implementation A840MTLDevice {
    A840IokitTransport *_transport;
    A840ProdRouter _prodRouter;
    A840SysFrontend *_systemFrontend;
    A840SysDeviceCaps _systemCaps;
    A840SysProcRole _processRole;
    uint64_t _systemRegistryID;
    A840MTLScanoutBridge *_scanoutBridge;
    BOOL _ownsContext;
}

+ (void)initialize
{
    if (self == [A840MTLDevice class]) {
        Protocol *spi = objc_getProtocol("MTLDeviceSPI");
        if (spi) {
            class_addProtocol(self, spi);
        }
    }
}

- (instancetype)initWithTransport:(A840IokitTransport *)transport
                 pipelineProvider:(id<A840MTLPipelineProvider>)provider
                      processRole:(A840SysProcRole)processRole
                       registryID:(uint64_t)registryID
                            error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    if (!transport) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"A NativeAdreno transport is required");
        return nil;
    }
    _transport = transport;
    _pipelineProvider = provider;
    _processRole = processRole;
    _systemRegistryID = registryID;
    (void)a840_prod_router_init(&_prodRouter);
    const auto backend = _transport.backend;
    A840RtStatus status;
    {
        std::lock_guard<std::mutex> lock(deviceCreation);
        status = a840_rt_device_create(&backend, A840_ROUTE_PHASE4_ALL, &_runtime);
    }
    if (status == A840_RT_OK && _transport.wire) {
        _ownsContext = a840_wire_attach_context(_transport.wire, _runtime) == A840_TRANSPORT_OK;
        if (!_ownsContext) status = A840_RT_TRANSPORT_ERROR;
    }
    if (status != A840_RT_OK) {
        if (error) *error = a840_metal_error(status, @"Could not initialize the NativeAdreno connection");
        return nil;
    }
    a840_rt_device_set_bo_pool_enabled(_runtime, true);
    a840_sys_query_caps(_runtime, &_systemCaps);
    A840SysFrontendConfig feCfg{};
    a840_sys_frontend_default_config(&feCfg, processRole);
    feCfg.registry_id = registryID ? registryID : _runtime->device_id;
    if (a840_sys_frontend_create(_runtime, &feCfg, &_systemFrontend) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_OOM, @"Could not initialize native system frontend core");
        return nil;
    }
    _workQueue = dispatch_queue_create("org.unixnu.adreno840.submit", DISPATCH_QUEUE_SERIAL);
    _callbackQueue = dispatch_queue_create("org.unixnu.adreno840.callbacks", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_callbackQueue, &callbackKey, (__bridge void *)self, nullptr);
    _scanoutBridge = [[A840MTLScanoutBridge alloc] initWithDevice:self];
    a840FillBaseLimits(self);
    return self;
}

- (instancetype)initWithTransport:(A840IokitTransport *)transport
                 pipelineProvider:(id<A840MTLPipelineProvider>)provider
                            error:(NSError **)error
{
    const A840SysProcRole role = a840_sys_classify_process(getprogname());
    return [self initWithTransport:transport
                  pipelineProvider:provider
                       processRole:role
                        registryID:0u
                             error:error];
}

- (void)dealloc
{
    if (_systemFrontend) {
        a840_sys_frontend_destroy(_systemFrontend);
        _systemFrontend = nullptr;
    }
    if (_ownsContext) a840_wire_detach_context(_transport.wire);
    a840_rt_device_destroy(_runtime);
}

- (A840IokitWire *)wire { return _transport.wire; }
- (A840ProdRouter *)productionRouter { return &_prodRouter; }
- (A840SysFrontend *)systemFrontend { return _systemFrontend; }
- (A840SysDeviceCaps)systemCaps { return _systemCaps; }
- (A840SysProcRole)processRole { return _processRole; }
- (A840MTLScanoutBridge *)scanoutBridge { return _scanoutBridge; }
- (BOOL)isCallbackThread { return dispatch_get_specific(&callbackKey) == (__bridge void *)self; }

/* _MTLAddDevice invokes these four hooks during plugin registration on iOS 27.
 * Overriding them as empty prevents _MTLDevice from probing nonexistent Apple GPU tables and aborting. */
- (void)initLimits { a840FillBaseLimits(self); }
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}

- (BOOL)respondsToSelector:(SEL)selector
{
    /* Inherited _MTLDevice targetDeviceInfo is NULL without Apple IOGPU. */
    if (selector == sel_registerName("targetDeviceInfo")) return NO;
    return [super respondsToSelector:selector];
}

- (NSString *)name { return @"Adreno 840"; }
- (NSString *)productName { return @"Adreno 840"; }
- (NSString *)vendorName { return @"Qualcomm"; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLDevice", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
- (uint64_t)registryID { return _systemRegistryID ? _systemRegistryID : (_runtime ? _runtime->device_id : 0u); }
- (NSUInteger)maxBufferLength { return _runtime ? (NSUInteger)_runtime->transport_caps.max_bo_bytes : (256u * 1024u * 1024u); }
- (MTLSize)maxThreadsPerThreadgroup
{
    const NSUInteger limit = A840_LIMIT_THREADSIZE_BASE * A840_LIMIT_MAX_WAVES;
    return MTLSizeMake(limit, limit, limit);
}

- (id<MTLCommandQueue>)newCommandQueue { return [[A840MTLCommandQueue alloc] initWithOwner:self]; }
- (id<MTLFence>)newFence { return [[A840MTLFence alloc] initWithOwner:self]; }

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    if (!length) return nil;
    const MTLStorageMode mode = (MTLStorageMode)((options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
    if (mode != MTLStorageModeShared && mode != MTLStorageModePrivate) {
        return nil;
    }
    uint32_t usage = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    if (mode == MTLStorageModeShared) usage |= A840_BUF_USAGE_CPU_VISIBLE;
    const A840RtBufferDesc desc{length, 0, usage, false, nullptr};
    A840RtBuffer *buffer = nullptr;
    @synchronized (self) {
        if (a840_rt_buffer_create(_runtime, &desc, &buffer) != A840_RT_OK)
            return nil;
    }
    return [[A840MTLBuffer alloc] initWithOwner:self native:buffer options:options];
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length
                           options:(MTLResourceOptions)options
{
    if (!bytes || !length) return nil;
    A840MTLBuffer *buffer = (A840MTLBuffer *)[self newBufferWithLength:length options:options];
    if (!buffer || a840_rt_buffer_write(buffer.native, 0, bytes, length) != A840_RT_OK) return nil;
    return buffer;
}

- (A840MTLTexture *)newNativeUbwcTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                            forScanout:(BOOL)forScanout
{
    if (!descriptor || !_systemFrontend || !descriptor.width || !descriptor.height) return nil;
    A840SysIOSurfaceDesc sdesc{};
    sdesc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
    sdesc.width = (uint32_t)descriptor.width;
    sdesc.height = (uint32_t)descriptor.height;
    sdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_SHADER_WRITE | A840_TEX_USAGE_RENDER_TARGET;
    sdesc.for_scanout = forScanout ? true : false;
    sdesc.request_ubwc6 = true;
    A840SysSurfaceHandle handle = A840_SYS_INVALID_HANDLE;
    A840SysSurfaceInfo info{};
    if (a840_sys_bind_iosurface(_systemFrontend, &sdesc, &handle, &info) != A840_SYS_OK) {
        return nil;
    }

    A840RtTextureDesc rtdesc{};
    rtdesc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
    rtdesc.tex_type = A840_TEX_2D;
    rtdesc.width = (uint32_t)descriptor.width;
    rtdesc.height = (uint32_t)descriptor.height;
    rtdesc.depth = 1u;
    rtdesc.array_layers = 1u;
    rtdesc.mip_levels = 1u;
    rtdesc.samples = 1u;
    rtdesc.tile_mode = A840_TILE_LINEAR;
    rtdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_SHADER_WRITE |
                         A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_CPU_VISIBLE;
    A840RtTexture *rtTex = nullptr;
    @synchronized (self) {
        if (a840_rt_texture_create(_runtime, &rtdesc, &rtTex) != A840_RT_OK) {
            (void)a840_sys_unbind_iosurface(_systemFrontend, handle);
            return nil;
        }
    }
    return [[A840MTLTexture alloc] initWithOwner:self
                                          native:rtTex
                                      descriptor:descriptor
                                       iosurface:nullptr
                                           plane:0u
                                   surfaceHandle:handle];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
    if (!descriptor || !descriptor.width || !descriptor.height ||
        descriptor.width > UINT32_MAX || descriptor.height > UINT32_MAX) {
        return nil;
    }
    if (!a840_sys_supports_sample_count((uint32_t)(descriptor.sampleCount ? descriptor.sampleCount : 1u))) {
        return nil;
    }
    if (descriptor.pixelFormat > UINT32_MAX) return nil;
    const uint32_t backingFmt = a840_canonical_backing_pixel_format((uint32_t)descriptor.pixelFormat);
    const A840FormatCaps *format = a840_format_caps_lookup(backingFmt);
    if (!format) return nil;

    const BOOL isMemoryless = (descriptor.storageMode == MTLStorageModeMemoryless);
    const BOOL isDepthStencil = ((format->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL)) != 0u);
    if (isMemoryless) {
        A840UbwcTextureDesc mdesc{};
        mdesc.kind = A840_P3_TEX_MEMORYLESS;
        mdesc.storage_mode = A840_STORAGE_MEMORYLESS;
        mdesc.mtl_pixel_format = backingFmt;
        mdesc.width = (uint32_t)descriptor.width;
        mdesc.height = (uint32_t)descriptor.height;
        mdesc.array_layers = (uint32_t)(descriptor.arrayLength ? descriptor.arrayLength : 1u);
        mdesc.samples = (uint32_t)(descriptor.sampleCount ? descriptor.sampleCount : 1u);
        mdesc.tile_mode = A840_TILE_TILED;
        mdesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET;
        A840UbwcTexture *memTex = nullptr;
        A840RtTexture *sysmemBacking = nullptr;
        A840RtTextureDesc rdesc{};
        rdesc.mtl_pixel_format = backingFmt;
        rdesc.tex_type = A840_TEX_2D;
        rdesc.width = mdesc.width;
        rdesc.height = mdesc.height;
        rdesc.depth = 1u;
        rdesc.array_layers = mdesc.array_layers;
        rdesc.mip_levels = 1u;
        rdesc.samples = mdesc.samples;
        rdesc.tile_mode = (isDepthStencil || rdesc.samples > 1u) ? A840_TILE_TILED : A840_TILE_LINEAR;
        rdesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_SHADER_READ;
        @synchronized (self) {
            if (a840_ubwc_texture_create(_runtime, &mdesc, &memTex) != A840_RT_OK) return nil;
            if (a840_rt_texture_create(_runtime, &rdesc, &sysmemBacking) != A840_RT_OK) {
                sysmemBacking = nullptr;
            }
        }
        return [[A840MTLTexture alloc] initWithOwner:self native:sysmemBacking
                                      memorylessUbwc:memTex descriptor:descriptor];
    }

    A840RtTextureDesc desc{};
    desc.mtl_pixel_format = backingFmt;
    switch (descriptor.textureType) {
    case MTLTextureType1D:
    case MTLTextureType1DArray: desc.tex_type = A840_TEX_1D; break;
    case MTLTextureTypeCube:
    case MTLTextureTypeCubeArray: desc.tex_type = A840_TEX_CUBE; break;
    case MTLTextureType3D: desc.tex_type = A840_TEX_3D; break;
    default: desc.tex_type = A840_TEX_2D; break;
    }
    desc.width = (uint32_t)descriptor.width;
    desc.height = (uint32_t)descriptor.height;
    desc.depth = (uint32_t)(descriptor.depth ? descriptor.depth : 1u);
    desc.array_layers = (uint32_t)(descriptor.arrayLength ? descriptor.arrayLength : 1u);
    if (desc.tex_type == A840_TEX_CUBE) desc.array_layers *= 6u;
    desc.mip_levels = (uint32_t)(descriptor.mipmapLevelCount ? descriptor.mipmapLevelCount : 1u);
    desc.samples = (uint32_t)(descriptor.sampleCount ? descriptor.sampleCount : 1u);
    desc.tile_mode = (isDepthStencil || desc.samples > 1u) ? A840_TILE_TILED : A840_TILE_LINEAR;
    desc.usage_flags = (descriptor.storageMode == MTLStorageModeShared) ? A840_TEX_USAGE_CPU_VISIBLE : 0u;
    if (descriptor.usage == MTLTextureUsageUnknown || (descriptor.usage & MTLTextureUsageShaderRead))
        desc.usage_flags |= A840_TEX_USAGE_SHADER_READ;
    if (descriptor.usage & MTLTextureUsageShaderWrite) desc.usage_flags |= A840_TEX_USAGE_SHADER_WRITE;
    if (descriptor.usage & MTLTextureUsageRenderTarget) desc.usage_flags |= A840_TEX_USAGE_RENDER_TARGET;

    A840RtTexture *texture = nullptr;
    A840RtStatus status;
    @synchronized (self) { status = a840_rt_texture_create(_runtime, &desc, &texture); }
    if (status != A840_RT_OK) return nil;
    return [[A840MTLTexture alloc] initWithOwner:self native:texture descriptor:descriptor];
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
    if (!descriptor) return nil;
    return [[A840MTLSamplerState alloc] initWithOwner:self descriptor:descriptor];
}

- (id<MTLLibrary>)newDefaultLibrary
{
    if ([_pipelineProvider respondsToSelector:@selector(defaultLibraryForDevice:)])
        return [_pipelineProvider defaultLibraryForDevice:self];
    return nil;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if ([_pipelineProvider respondsToSelector:@selector(libraryWithURL:forDevice:error:)])
        return [_pipelineProvider libraryWithURL:url forDevice:self error:error];
    if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"No native Metal library provider");
    return nil;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                            error:(NSError **)error
{
    if (!function) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Compute function is required");
        return nil;
    }
    A840RtComputePipelineDesc desc{};
    id plan;
    @synchronized (self) { plan = [_pipelineProvider prepareCompute:function forDevice:self descriptor:&desc]; }
    A840MTLComputePipelineState *pipeline = [[A840MTLComputePipelineState alloc]
        initWithOwner:self function:function descriptor:&desc plan:plan];
    if (pipeline.nativeDescriptor) return pipeline;
    if (plan) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-nodesc %.40s", function.name.UTF8String ?: "?");
        a840diag::reportText(msg);
    }
    if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Native compute pipeline compilation failed");
    return nil;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                                             options:(MTLPipelineOption)options
                                                          reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
                                                               error:(NSError **)error
{
    (void)options;
    if (reflection) *reflection = nil;
    if (!descriptor || !descriptor.computeFunction) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Compute descriptor missing function");
        return nil;
    }
    if ([_pipelineProvider respondsToSelector:@selector(prepareComputeWithDescriptor:forDevice:descriptor:)]) {
        A840RtComputePipelineDesc desc{};
        id plan;
        @synchronized (self) {
            plan = [_pipelineProvider prepareComputeWithDescriptor:descriptor forDevice:self descriptor:&desc];
        }
        A840MTLComputePipelineState *pipeline = [[A840MTLComputePipelineState alloc]
            initWithOwner:self function:descriptor.computeFunction descriptor:&desc plan:plan];
        if (pipeline.nativeDescriptor) return pipeline;
        if (plan) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE cs-nodesc %.40s",
                     descriptor.computeFunction.name.UTF8String ?: "?");
            a840diag::reportText(msg);
        }
        if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Native compute pipeline compilation failed");
        return nil;
    }
    return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                                               error:(NSError **)error
{
    return [self newComputePipelineStateWithDescriptor:descriptor
                                               options:MTLPipelineOptionNone
                                            reflection:nil
                                                 error:error];
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                            completionHandler:(MTLNewComputePipelineStateCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [self newComputePipelineStateWithDescriptor:descriptor error:&err];
    completionHandler(pso, err);
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                      options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    MTLAutoreleasedComputePipelineReflection refl = nil;
    id<MTLComputePipelineState> pso = [self newComputePipelineStateWithDescriptor:descriptor
                                                                          options:options
                                                                       reflection:&refl
                                                                            error:&err];
    completionHandler(pso, refl, err);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)configuration
                                                            error:(NSError **)error
{
    if (!configuration) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Render pipeline descriptor is required");
        return nil;
    }
    A840RtGraphicsProgramDesc desc{};
    id plan;
    @synchronized (self) { plan = [_pipelineProvider prepareRender:configuration forDevice:self descriptor:&desc]; }
    A840MTLRenderPipelineState *pipeline = [[A840MTLRenderPipelineState alloc]
        initWithOwner:self configuration:configuration descriptor:&desc plan:plan];
    if (pipeline.nativeDescriptor) return pipeline;
    if (plan) {
        a840diag::reportText("A840PIPE rps-nodesc");
    }
    if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Native render pipeline compilation failed");
    return nil;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    (void)options;
    if (reflection) *reflection = nil;
    return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
                                                             error:(NSError **)error
{
    (void)options;
    if (reflection) *reflection = nil;
    return [self newComputePipelineStateWithFunction:function error:error];
}

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                          completionHandler:(MTLNewComputePipelineStateCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [self newComputePipelineStateWithFunction:function error:&err];
    completionHandler(pso, err);
}

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                    options:(MTLPipelineOption)options
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    MTLAutoreleasedComputePipelineReflection refl = nil;
    id<MTLComputePipelineState> pso = [self newComputePipelineStateWithFunction:function
                                                                        options:options
                                                                     reflection:&refl
                                                                          error:&err];
    completionHandler(pso, refl, err);
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [self newRenderPipelineStateWithDescriptor:descriptor error:&err];
    completionHandler(pso, err);
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                     options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    MTLAutoreleasedRenderPipelineReflection refl = nil;
    id<MTLRenderPipelineState> pso = [self newRenderPipelineStateWithDescriptor:descriptor
                                                                        options:options
                                                                     reflection:&refl
                                                                          error:&err];
    completionHandler(pso, refl, err);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor
                                                               options:(MTLPipelineOption)options
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                                 error:(NSError **)error
{
    (void)options;
    char msg[128] = {};
    snprintf(msg, sizeof(msg), "A840PIPE tile %.40s", descriptor.tileFunction.name.UTF8String ?: "?");
    a840diag::reportText(msg);
    if (reflection) *reflection = nil;
    if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Tile render pipelines not yet supported on Adreno 840");
    return nil;
}

- (void)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor
                                         options:(MTLPipelineOption)options
                               completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    MTLAutoreleasedRenderPipelineReflection refl = nil;
    id<MTLRenderPipelineState> pso = [self newRenderPipelineStateWithTileDescriptor:descriptor
                                                                            options:options
                                                                         reflection:&refl
                                                                              error:&err];
    completionHandler(pso, refl, err);
}

- (id)newPipelineLibraryWithFilePath:(NSString *)filepath error:(NSError **)error
{
    if (error) {
        *error = a840_metal_error(A840_RT_UNSUPPORTED,
                                  [NSString stringWithFormat:@"No pipeline library on Adreno 840: %@", filepath.lastPathComponent ?: @""]);
    }
    return nil;
}

- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count
{
    A840MTLResourceGroup *group = [A840MTLResourceGroup new];
    NSMutableArray *held = [NSMutableArray arrayWithCapacity:count];
    for (NSUInteger i = 0u; i < count; ++i) {
        if (resources && resources[i]) [held addObject:resources[i]];
    }
    group.resources = held;
    return group;
}

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor isHeapOrBufferBacked:(BOOL)isHeapOrBufferBacked
{
    (void)descriptor;
    (void)isHeapOrBufferBacked;
    return nil;
}

- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)descriptor
                                deallocator:(void (^)(void *, NSUInteger))deallocator
{
    (void)bytes;
    (void)length;
    (void)descriptor;
    (void)deallocator;
    return nil;
}

- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)descriptor
{
    if (!descriptor || descriptor.width == 0u || descriptor.pixelFormat > UINT32_MAX) return 0u;
    const A840FormatCaps *caps = a840_format_caps_lookup((uint32_t)descriptor.pixelFormat);
    if (!caps || caps->block_width == 0u) return 0u;
    const NSUInteger blocksX = (descriptor.width + caps->block_width - 1u) / caps->block_width;
    const NSUInteger elemBytes = (caps->flags & A840_FORMAT_SEPARATE_DEPTH_STENCIL_PLANES) ? 4u : (caps->block_bits / 8u);
    const NSUInteger raw = blocksX * elemBytes;
    return (raw + 63u) & ~(NSUInteger)63u;
}
@end
#pragma clang diagnostic pop

id<MTLDevice> a840_metal_create_device(mach_port_t connection, BOOL ownsConnection,
    void *reserved, id<A840MTLPipelineProvider> provider, NSError **error)
{
    if (reserved != nullptr) {
        (void)a840_prod_route_request_fallback(nullptr);
        if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED,
                                             @"Non-native delegate injection is permanently forbidden");
        return nil;
    }
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithConnection:connection
        takesOwnership:ownsConnection error:error];
    if (!transport) return nil;
    return [[A840MTLDevice alloc] initWithTransport:transport
                                   pipelineProvider:provider error:error];
}
