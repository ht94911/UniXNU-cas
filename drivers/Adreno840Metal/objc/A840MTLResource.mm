/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Phase 6 zero-fallback native resource wrappers with probe006 sampler clamping. */
#import "A840MTLResource.h"

#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

@implementation A840MTLAllocation {
    MTLResourceOptions _options;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner options:(MTLResourceOptions)options
{
    if ((self = [super init])) {
        _owner = owner;
        _options = options;
    }
    return self;
}

- (id<MTLDevice>)device { return _owner; }
- (MTLResourceOptions)resourceOptions { return _options; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode
{
    return _options & MTLResourceCPUCacheModeWriteCombined ? MTLCPUCacheModeWriteCombined : MTLCPUCacheModeDefaultCache;
}
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable { [self doesNotRecognizeSelector:_cmd]; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (state != MTLPurgeableStateKeepCurrent && state != MTLPurgeableStateNonVolatile)
        [self doesNotRecognizeSelector:_cmd];
    return MTLPurgeableStateNonVolatile;
}
@end

@implementation A840MTLBuffer

- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options
{
    if ((self = [super initWithOwner:owner options:options])) _native = buffer;
    return self;
}

- (void)dealloc { @synchronized (self.owner) { a840_rt_buffer_release(_native); } }
- (NSUInteger)length { return (NSUInteger)_native->size_bytes; }
- (NSUInteger)allocatedSize { return (NSUInteger)_native->alloc_bytes; }
- (MTLGPUAddress)gpuAddress { return _native->gpu_va; }
- (void *)contents { return _native->cpu_ptr; }
@end

@implementation A840MTLTexture {
    MTLTextureDescriptor *_descriptor;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    if ((self = [super initWithOwner:owner options:descriptor.resourceOptions])) {
        _native = texture;
        _descriptor = [descriptor copy];
    }
    return self;
}

- (void)dealloc { @synchronized (self.owner) { a840_rt_texture_release(_native); } }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return _descriptor.pixelFormat; }
- (NSUInteger)width { return _descriptor.width; }
- (NSUInteger)height { return _descriptor.height; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)allocatedSize { return (NSUInteger)_native->alloc_bytes; }
- (MTLTextureUsage)usage { return _descriptor.usage; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)allowGPUOptimizedContents { return NO; }
- (id<MTLTexture>)parentTexture { return nil; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (id<MTLBuffer>)buffer { return nil; }
- (NSUInteger)bufferOffset { return 0; }
- (NSUInteger)bufferBytesPerRow { return 0; }

- (void)transfer:(MTLRegion)region level:(NSUInteger)level slice:(NSUInteger)slice
          bytes:(void *)bytes rowBytes:(NSUInteger)rowBytes
          write:(BOOL)write
{
    const auto x = region.origin.x, y = region.origin.y;
    const auto width = region.size.width, height = region.size.height;
    const NSUInteger pixelBytes = _native->format_caps->block_bits / 8u;
    if (level || slice || region.origin.z || region.size.depth != 1 || !bytes || !_native->cpu_ptr ||
        x > self.width || width > self.width - x || y > self.height || height > self.height - y ||
        width * pixelBytes > rowBytes || (height && rowBytes > NSUIntegerMax / height))
        [NSException raise:NSInvalidArgumentException format:@"Invalid native 2-D texture transfer"];
    if (!width || !height) return;
    auto *surface = static_cast<uint8_t *>(_native->cpu_ptr) + y * _native->pitch_bytes + x * pixelBytes;
    auto *user = static_cast<uint8_t *>(bytes);
    for (NSUInteger row = 0; row < height; ++row) {
        if (write) std::memcpy(surface, user, width * pixelBytes);
        else std::memcpy(user, surface, width * pixelBytes);
        surface += _native->pitch_bytes;
        user += rowBytes;
    }
}

- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)rowBytes
{
    [self transfer:region level:level slice:0 bytes:const_cast<void *>(bytes) rowBytes:rowBytes write:YES];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
            withBytes:(const void *)bytes bytesPerRow:(NSUInteger)rowBytes bytesPerImage:(NSUInteger)imageBytes
{
    (void)imageBytes;
    [self transfer:region level:level slice:slice bytes:const_cast<void *>(bytes) rowBytes:rowBytes write:YES];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)rowBytes fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
    [self transfer:region level:level slice:0 bytes:bytes rowBytes:rowBytes write:NO];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)rowBytes bytesPerImage:(NSUInteger)imageBytes
     fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
    (void)imageBytes;
    [self transfer:region level:level slice:slice bytes:bytes rowBytes:rowBytes write:NO];
}
@end

static uint32_t addressMode(MTLSamplerAddressMode mode)
{
    switch (mode) {
    case MTLSamplerAddressModeRepeat: return A840_ADDRESS_REPEAT;
    case MTLSamplerAddressModeClampToEdge: return A840_ADDRESS_CLAMP_TO_EDGE;
    case MTLSamplerAddressModeMirrorRepeat: return A840_ADDRESS_MIRROR_REPEAT;
    case MTLSamplerAddressModeMirrorClampToEdge: return A840_ADDRESS_MIRROR_CLAMP;
    case MTLSamplerAddressModeClampToZero: return A840_ADDRESS_CLAMP_TO_BORDER;
    case MTLSamplerAddressModeClampToBorderColor: return A840_ADDRESS_CLAMP_TO_BORDER;
    default: return UINT32_MAX;
    }
}

static uint32_t clampAnisoPow2(NSUInteger raw)
{
    if (raw <= 1u) return 1u;
    if (raw >= 16u) return 16u;
    uint32_t p = 1u;
    while ((p << 1u) <= raw && p < 16u) p <<= 1u;
    return p;
}

@implementation A840MTLSamplerState {
    A840MTLDevice *_owner;
    MTLSamplerDescriptor *_descriptor;
    uint32_t _words[A840_SAMP_CONST_DWORDS];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLSamplerDescriptor *)descriptor
{
    if (!(self = [super init])) return nil;
    const BOOL norm = descriptor.normalizedCoordinates;
    /* Apple MTLSamplerDescriptor defaults lodMaxClamp to FLT_MAX; clamp to A8XX fixed-point max. */
    const float kMaxLod = 4095.0f / 256.0f;
    float minLod = norm ? descriptor.lodMinClamp : 0.0f;
    float maxLod = norm ? descriptor.lodMaxClamp : 0.0f;
    if (!(minLod >= 0.0f)) minLod = 0.0f;
    if (minLod > kMaxLod) minLod = kMaxLod;
    if (!(maxLod >= minLod)) maxLod = minLod;
    if (maxLod > kMaxLod) maxLod = kMaxLod;
    const uint32_t mipFilter = norm ? (uint32_t)descriptor.mipFilter : A840_MIP_NONE;
    const uint32_t maxAniso = norm ? clampAnisoPow2(descriptor.maxAnisotropy) : 1u;
    const uint32_t cmpFunc = (!norm || descriptor.compareFunction == MTLCompareFunctionNever)
        ? A840_COMPARE_DISABLED : (uint32_t)descriptor.compareFunction;
    const A840SampDescInput input{
        (uint32_t)descriptor.minFilter,
        norm ? (uint32_t)descriptor.magFilter : (uint32_t)descriptor.minFilter,
        mipFilter,
        addressMode(descriptor.sAddressMode), addressMode(descriptor.tAddressMode), addressMode(descriptor.rAddressMode),
        maxAniso, minLod, maxLod, 0.0f, cmpFunc,
        0, norm ? 0u : 1u,
    };
    if (a840_encode_samp_const(&input, _words) != A840_EMIT_OK) return nil;
    _owner = owner;
    _descriptor = [descriptor copy];
    return self;
}
- (id<MTLDevice>)device { return _owner; }
- (NSString *)label { return _descriptor.label; }
- (MTLResourceID)gpuResourceID { return MTLResourceID{}; }
- (const uint32_t *)descriptorWords { return _words; }
- (MTLSamplerDescriptor *)descriptor { return _descriptor; }
@end

@implementation A840MTLFence {
    A840MTLDevice *_owner;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
{
    if ((self = [super init])) _owner = owner;
    return self;
}
- (id<MTLDevice>)device { return _owner; }
@end

@implementation A840MTLComputePipelineState {
    A840MTLDevice *_owner;
    id<MTLFunction> _function;
    A840RtComputePipelineDesc _descriptor;
}
- (instancetype)initWithOwner:(A840MTLDevice *)owner function:(id<MTLFunction>)function
                   descriptor:(const A840RtComputePipelineDesc *)descriptor plan:(id)plan
{
    if ((self = [super init])) {
        _owner = owner;
        _function = function;
        _descriptor = *descriptor;
        _plan = plan;
    }
    return self;
}
- (id<MTLDevice>)device { return _owner; }
- (NSString *)label { return _function.name; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return A840_LIMIT_THREADSIZE_BASE * A840_LIMIT_MAX_WAVES; }
- (NSUInteger)threadExecutionWidth { return A840_LIMIT_THREADSIZE_BASE; }
- (NSUInteger)staticThreadgroupMemoryLength { return _descriptor.stage ? _descriptor.stage->shared_mem_bytes : 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (const A840RtComputePipelineDesc *)nativeDescriptor { return _plan && _descriptor.stage ? &_descriptor : nullptr; }
@end

@implementation A840MTLRenderPipelineState {
    A840MTLDevice *_owner;
    A840RtGraphicsProgramDesc _descriptor;
}
- (instancetype)initWithOwner:(A840MTLDevice *)owner configuration:(MTLRenderPipelineDescriptor *)configuration
                   descriptor:(const A840RtGraphicsProgramDesc *)descriptor plan:(id)plan
{
    if ((self = [super init])) {
        _owner = owner;
        _configuration = [configuration copy];
        _descriptor = *descriptor;
        _plan = plan;
    }
    return self;
}
- (id<MTLDevice>)device { return _owner; }
- (NSString *)label { return _configuration.label; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (const A840RtGraphicsProgramDesc *)nativeDescriptor
{
    return _plan && _descriptor.emit_desc.vs.stage && _descriptor.emit_desc.fs.stage ? &_descriptor : nullptr;
}
@end

#pragma clang diagnostic pop
