/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI. */
#import "A840MTLDevice.h"
#import "A840MTLCommand.h"
#import "A840MTLResource.h"

#include <mutex>

static char callbackKey;
static std::mutex deviceCreation;

NSError *a840_metal_error(A840RtStatus status, NSString *operation)
{
    return [NSError errorWithDomain:@"Adreno840Metal" code:status
                            userInfo:@{NSLocalizedDescriptionKey: operation}];
}

/* Outside the Phase 2 selectors below, read-only queries and resource
 * factories on the device can use its explicitly supplied delegate. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@implementation A840MTLDevice {
    A840IokitTransport *_transport;
    A840MTLFallbackFactory _fallbackFactory;
    id<MTLDevice> _fallbackDevice;
    BOOL _fallbackAttempted, _ownsContext;
}

- (instancetype)initWithTransport:(A840IokitTransport *)transport
                   fallbackFactory:(A840MTLFallbackFactory)fallback
                  pipelineProvider:(id<A840MTLPipelineProvider>)provider
                             error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    if (!transport) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"A NativeAdreno transport is required");
        return nil;
    }
    _transport = transport;
    _fallbackFactory = [fallback copy];
    _pipelineProvider = provider;
    const auto backend = _transport.backend;
    A840RtStatus status;
    {
        // The core assigns the device identity from a process-local counter.
        std::lock_guard<std::mutex> lock(deviceCreation);
        status = a840_rt_device_create(&backend, A840_ROUTE_ALL, &_runtime);
    }
    if (status == A840_RT_OK && _transport.wire) {
        _ownsContext = a840_wire_attach_context(_transport.wire, _runtime) == A840_TRANSPORT_OK;
        if (!_ownsContext) status = A840_RT_TRANSPORT_ERROR;
    }
    if (status != A840_RT_OK) {
        if (error) *error = a840_metal_error(status, @"Could not initialize the NativeAdreno connection");
        return nil;
    }
    _workQueue = dispatch_queue_create("org.unixnu.adreno840.submit", DISPATCH_QUEUE_SERIAL);
    _callbackQueue = dispatch_queue_create("org.unixnu.adreno840.callbacks", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_callbackQueue, &callbackKey, (__bridge void *)self, nullptr);
    return self;
}

- (void)dealloc
{
    if (_ownsContext) a840_wire_detach_context(_transport.wire);
    a840_rt_device_destroy(_runtime);
}

- (BOOL)isCallbackThread { return dispatch_get_specific(&callbackKey) == (__bridge void *)self; }

- (id<MTLDevice>)fallbackDevice
{
    @synchronized (self) {
        if (!_fallbackAttempted) {
            _fallbackAttempted = YES;
            id<MTLDevice> candidate = _fallbackFactory ? _fallbackFactory() : nil;
            if (candidate != self) _fallbackDevice = candidate;
            _fallbackFactory = nil;
        }
        return _fallbackDevice;
    }
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    id<MTLDevice> delegate = self.fallbackDevice;
    return [delegate respondsToSelector:selector] ? delegate : [super forwardingTargetForSelector:selector];
}

- (NSString *)name { return @"Adreno 840"; }
- (uint64_t)registryID { return _runtime->device_id; }
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
    if ((options & MTLResourceStorageModeMask) != MTLResourceStorageModeShared)
        return [self.fallbackDevice newBufferWithLength:length options:options];
    const A840RtBufferDesc desc{length, 0, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE |
        A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
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
    if ((options & MTLResourceStorageModeMask) != MTLResourceStorageModeShared)
        return [self.fallbackDevice newBufferWithBytes:bytes length:length options:options];
    A840MTLBuffer *buffer = (A840MTLBuffer *)[self newBufferWithLength:length options:options];
    if (!buffer || a840_rt_buffer_write(buffer.native, 0, bytes, length) != A840_RT_OK) return nil;
    return buffer;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
    if (!descriptor) return nil;
    const A840FormatCaps *format = descriptor.pixelFormat <= UINT32_MAX ?
        a840_format_caps_lookup((uint32_t)descriptor.pixelFormat) : nullptr;
    if (!format || format->block_width != 1 || format->block_height != 1 ||
        (format->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_REQUIRES_XR_CODEC)) ||
        descriptor.textureType != MTLTextureType2D || descriptor.storageMode != MTLStorageModeShared ||
        descriptor.depth != 1 || descriptor.arrayLength != 1 || descriptor.mipmapLevelCount != 1 ||
        descriptor.sampleCount != 1 || descriptor.width > UINT32_MAX || descriptor.height > UINT32_MAX)
        return [self.fallbackDevice newTextureWithDescriptor:descriptor];

    A840RtTextureDesc desc{};
    desc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
    desc.tex_type = A840_TEX_2D;
    desc.width = (uint32_t)descriptor.width;
    desc.height = (uint32_t)descriptor.height;
    desc.depth = desc.array_layers = desc.mip_levels = desc.samples = 1;
    desc.tile_mode = A840_TILE_LINEAR;
    desc.usage_flags = A840_TEX_USAGE_CPU_VISIBLE;
    if (descriptor.usage == MTLTextureUsageUnknown || descriptor.usage & MTLTextureUsageShaderRead)
        desc.usage_flags |= A840_TEX_USAGE_SHADER_READ;
    if (descriptor.usage & MTLTextureUsageShaderWrite) desc.usage_flags |= A840_TEX_USAGE_SHADER_WRITE;
    if (descriptor.usage & MTLTextureUsageRenderTarget) desc.usage_flags |= A840_TEX_USAGE_RENDER_TARGET;
    A840RtTexture *texture = nullptr;
    A840RtStatus status;
    @synchronized (self) { status = a840_rt_texture_create(_runtime, &desc, &texture); }
    if (status == A840_RT_UNSUPPORTED) return [self.fallbackDevice newTextureWithDescriptor:descriptor];
    if (status != A840_RT_OK) return nil;
    return [[A840MTLTexture alloc] initWithOwner:self native:texture descriptor:descriptor];
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
    if (!descriptor) return nil;
    id<MTLSamplerState> sampler = [[A840MTLSamplerState alloc] initWithOwner:self descriptor:descriptor];
    return sampler ?: [self.fallbackDevice newSamplerStateWithDescriptor:descriptor];
}

- (id<MTLLibrary>)newDefaultLibrary
{
    if ([_pipelineProvider respondsToSelector:@selector(defaultLibraryForDevice:)]) {
        id<MTLLibrary> library = [_pipelineProvider defaultLibraryForDevice:self];
        if (library) return library;
    }
    return [self.fallbackDevice newDefaultLibrary];
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if ([_pipelineProvider respondsToSelector:@selector(libraryWithURL:forDevice:error:)])
        return [_pipelineProvider libraryWithURL:url forDevice:self error:error];
    id<MTLDevice> delegate = self.fallbackDevice;
    if (delegate) return [delegate newLibraryWithURL:url error:error];
    if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"No Metal library provider");
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
    NSError *delegateError = nil;
    if (pipeline.nativeDescriptor || [pipeline fallbackPipeline:&delegateError]) return pipeline;
    if (error) *error = delegateError ?: a840_metal_error(A840_RT_UNSUPPORTED, @"No compatible compute pipeline");
    return nil;
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
    NSError *delegateError = nil;
    if (pipeline.nativeDescriptor || [pipeline fallbackPipeline:&delegateError]) return pipeline;
    if (error) *error = delegateError ?: a840_metal_error(A840_RT_UNSUPPORTED, @"No compatible render pipeline");
    return nil;
}
@end
#pragma clang diagnostic pop

id<MTLDevice> a840_metal_create_device(mach_port_t connection, BOOL ownsConnection,
    A840MTLFallbackFactory fallback, id<A840MTLPipelineProvider> provider, NSError **error)
{
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithConnection:connection
        takesOwnership:ownsConnection error:error];
    if (!transport) return nil;
    return [[A840MTLDevice alloc] initWithTransport:transport fallbackFactory:fallback
                                   pipelineProvider:provider error:error];
}
