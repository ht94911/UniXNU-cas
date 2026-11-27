/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback full-system Adreno 840 Metal device. */
#import "A840MTLDevice.h"
#import "A840MTLCommand.h"
#import "A840MTLResource.h"
#import "A840MTLScanoutBridge.h"
#import "A840MTLSystemSPI.h"

#include <mutex>
#include <stdlib.h>

static char callbackKey;
static std::mutex deviceCreation;

NSError *a840_metal_error(A840RtStatus status, NSString *operation)
{
    return [NSError errorWithDomain:@"Adreno840Metal" code:status
                            userInfo:@{NSLocalizedDescriptionKey: operation}];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
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

- (NSString *)name { return @"Adreno 840"; }
- (uint64_t)registryID { return _systemRegistryID ? _systemRegistryID : (_runtime ? _runtime->device_id : 0u); }
- (NSUInteger)maxBufferLength { return (NSUInteger)_runtime->transport_caps.max_bo_bytes; }
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
    const A840FormatCaps *format = descriptor.pixelFormat <= UINT32_MAX ?
        a840_format_caps_lookup((uint32_t)descriptor.pixelFormat) : nullptr;
    if (!format) return nil;

    const BOOL isMemoryless = (descriptor.storageMode == MTLStorageModeMemoryless);
    const BOOL isDepthStencil = ((format->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL)) != 0u);
    if (isMemoryless) {
        A840UbwcTextureDesc mdesc{};
        mdesc.kind = A840_P3_TEX_MEMORYLESS;
        mdesc.storage_mode = A840_STORAGE_MEMORYLESS;
        mdesc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
        mdesc.width = (uint32_t)descriptor.width;
        mdesc.height = (uint32_t)descriptor.height;
        mdesc.array_layers = (uint32_t)(descriptor.arrayLength ? descriptor.arrayLength : 1u);
        mdesc.samples = (uint32_t)(descriptor.sampleCount ? descriptor.sampleCount : 1u);
        mdesc.tile_mode = A840_TILE_TILED;
        mdesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET;
        A840UbwcTexture *memTex = nullptr;
        @synchronized (self) {
            if (a840_ubwc_texture_create(_runtime, &mdesc, &memTex) != A840_RT_OK) return nil;
        }
        return [[A840MTLTexture alloc] initWithOwner:self memorylessUbwc:memTex descriptor:descriptor];
    }

    A840RtTextureDesc desc{};
    desc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
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
    return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
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
