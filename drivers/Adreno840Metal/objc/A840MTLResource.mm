/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback native resource wrappers: IOSurface, Heap aliasing,
 * Texture Views, Memoryless attachments, and probe006 sampler clamping. */
#import "A840MTLResource.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLDiagnostics.h"

#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

@implementation A840MTLAllocation {
    MTLResourceOptions _options;
    MTLPurgeableState _purgeableState;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner options:(MTLResourceOptions)options
{
    if ((self = [super init])) {
        _owner = owner;
        _options = options;
        _purgeableState = MTLPurgeableStateNonVolatile;
    }
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                      options:(MTLResourceOptions)options
{
    if ((self = [self initWithOwner:owner options:options])) {
        _owningHeap = heap;
        _heapSubAlloc = subAlloc;
    }
    return self;
}

- (void)dealloc
{
    if (_heapSubAlloc) {
        @synchronized (_owner) {
            a840_heap_suballoc_release(_heapSubAlloc);
        }
        _heapSubAlloc = nullptr;
    }
}

- (id<MTLDevice>)device { return _owner; }
- (MTLResourceOptions)resourceOptions { return _options; }
- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return (_options & MTLResourceCPUCacheModeWriteCombined) ? MTLCPUCacheModeWriteCombined : MTLCPUCacheModeDefaultCache;
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return (_options & MTLResourceHazardTrackingModeUntracked) ? MTLHazardTrackingModeUntracked : MTLHazardTrackingModeTracked;
}
- (id<MTLHeap>)heap { return _owningHeap; }
- (NSUInteger)heapOffset { return _heapSubAlloc ? (NSUInteger)_heapSubAlloc->offset : 0u; }
- (BOOL)isAliasable { return _heapSubAlloc ? (a840_heap_suballoc_is_aliasable(_heapSubAlloc) ? YES : NO) : NO; }

- (void)makeAliasable
{
    if (!_heapSubAlloc) return;
    @synchronized (_owner) {
        (void)a840_heap_suballoc_make_aliasable(_heapSubAlloc);
    }
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (_owningHeap) {
        return [_owningHeap setPurgeableState:state];
    }
    const MTLPurgeableState prev = _purgeableState;
    if (state != MTLPurgeableStateKeepCurrent) {
        _purgeableState = state;
    }
    return prev;
}
- (uint64_t)protectionOptions { return 0u; }
- (int)setOwnerWithIdentity:(unsigned)identity
{
    (void)identity;
    return 46; /* KERN_NOT_SUPPORTED */
}
- (void)waitUntilComplete {}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector(object_getClassName(self), sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@implementation A840MTLBuffer {
    void *_noCopyPointer;
    void (^_deallocator)(void *, NSUInteger);
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options
{
    if ((self = [super initWithOwner:owner options:options])) _native = buffer;
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options
                noCopyPointer:(void *)noCopyPointer
                  deallocator:(void (^)(void *, NSUInteger))deallocator
{
    if ((self = [super initWithOwner:owner options:options])) {
        _native = buffer;
        _noCopyPointer = noCopyPointer;
        _deallocator = [deallocator copy];
    }
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                      options:(MTLResourceOptions)options
{
    return [super initWithOwner:owner heap:heap subAlloc:subAlloc options:options];
}

- (void)dealloc
{
    const NSUInteger len = self.length;
    if (_native) {
        @synchronized (self.owner) { a840_rt_buffer_release(_native); }
        _native = nullptr;
    }
    if (_deallocator && _noCopyPointer) {
        _deallocator(_noCopyPointer, len);
    }
}

- (NSUInteger)length
{
    if (_native) return (NSUInteger)_native->size_bytes;
    if (self.heapSubAlloc) return (NSUInteger)self.heapSubAlloc->size_bytes;
    return 0u;
}
- (NSUInteger)allocatedSize
{
    if (_native) return (NSUInteger)_native->alloc_bytes;
    if (self.heapSubAlloc) return (NSUInteger)self.heapSubAlloc->size_bytes;
    return 0u;
}
- (MTLGPUAddress)gpuAddress
{
    if (_native) return _native->gpu_va;
    if (self.heapSubAlloc) return self.heapSubAlloc->gpu_va;
    return 0u;
}
- (void *)contents
{
    if (_native) return _native->cpu_ptr;
    if (self.heapSubAlloc) return self.heapSubAlloc->cpu_ptr;
    return nullptr;
}
- (void)didModifyRange:(NSRange)range { (void)range; }
- (IOSurfaceRef)iosurface { return nullptr; }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)bytesPerRow
{
    if (!descriptor || !_native || !self.owner || !self.owner.runtime) return nil;
    if (descriptor.width == 0u || descriptor.height == 0u ||
        descriptor.width > UINT32_MAX || descriptor.height > UINT32_MAX ||
        bytesPerRow > UINT32_MAX) {
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
    rtdesc.pitch_bytes = (uint32_t)bytesPerRow;
    rtdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_CPU_VISIBLE;
    if (descriptor.usage & MTLTextureUsageShaderWrite) rtdesc.usage_flags |= A840_TEX_USAGE_SHADER_WRITE;
    if (descriptor.usage & MTLTextureUsageRenderTarget) rtdesc.usage_flags |= A840_TEX_USAGE_RENDER_TARGET;

    A840RtTexture *rtTex = nullptr;
    @synchronized (self.owner) {
        if (a840_rt_texture_create_on_buffer(self.owner.runtime, _native, (uint64_t)offset, &rtdesc, &rtTex) != A840_RT_OK) {
            return nil;
        }
    }
    return [[A840MTLTexture alloc] initWithOwner:self.owner
                                          native:rtTex
                                      descriptor:descriptor
                                          buffer:self
                                    bufferOffset:offset
                               bufferBytesPerRow:bytesPerRow];
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)bytesPerRow
                                   bytesPerImage:(NSUInteger)bytesPerImage
{
    (void)bytesPerImage;
    return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}
@end

@implementation A840MTLTexture {
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _iosurface;
    NSUInteger _iosurfacePlane;
    A840MTLTexture *_parentTexture;
    NSUInteger _parentRelativeLevel;
    NSUInteger _parentRelativeSlice;
    MTLPixelFormat _viewPixelFormat;
    MTLTextureType _viewTextureType;
    NSRange _viewLevelRange;
    NSRange _viewSliceRange;
    MTLTextureSwizzleChannels _swizzle;
    A840MTLBuffer *_backingBuffer;
    NSUInteger _bufferOffset;
    NSUInteger _bufferBytesPerRow;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    return [self initWithOwner:owner native:texture descriptor:descriptor
                     iosurface:nullptr plane:0u surfaceHandle:A840_SYS_INVALID_HANDLE];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
                    iosurface:(IOSurfaceRef)iosurface
                        plane:(NSUInteger)plane
                surfaceHandle:(A840SysSurfaceHandle)surfaceHandle
{
    if ((self = [super initWithOwner:owner options:descriptor.resourceOptions])) {
        _native = texture;
        _descriptor = [descriptor copy];
        _surfaceHandle = surfaceHandle;
        _iosurface = iosurface ? (IOSurfaceRef)CFRetain(iosurface) : nullptr;
        _iosurfacePlane = plane;
        _viewPixelFormat = descriptor.pixelFormat;
        _viewTextureType = descriptor.textureType;
        _viewLevelRange = NSMakeRange(0u, descriptor.mipmapLevelCount ?: 1u);
        _viewSliceRange = NSMakeRange(0u, descriptor.arrayLength ?: 1u);
        _swizzle = descriptor.swizzle;
    }
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
               memorylessUbwc:(A840UbwcTexture *)memoryless
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    if ((self = [super initWithOwner:owner options:descriptor.resourceOptions])) {
        _memorylessNative = memoryless;
        _descriptor = [descriptor copy];
        _viewPixelFormat = descriptor.pixelFormat;
        _viewTextureType = descriptor.textureType;
        _viewLevelRange = NSMakeRange(0u, descriptor.mipmapLevelCount ?: 1u);
        _viewSliceRange = NSMakeRange(0u, descriptor.arrayLength ?: 1u);
        _swizzle = descriptor.swizzle;
    }
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    if ((self = [super initWithOwner:owner heap:heap subAlloc:subAlloc options:descriptor.resourceOptions])) {
        _descriptor = [descriptor copy];
        _viewPixelFormat = descriptor.pixelFormat;
        _viewTextureType = descriptor.textureType;
        _viewLevelRange = NSMakeRange(0u, descriptor.mipmapLevelCount ?: 1u);
        _viewSliceRange = NSMakeRange(0u, descriptor.arrayLength ?: 1u);
        _swizzle = descriptor.swizzle;
    }
    return self;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
                       buffer:(A840MTLBuffer *)buffer
                 bufferOffset:(NSUInteger)bufferOffset
            bufferBytesPerRow:(NSUInteger)bufferBytesPerRow
{
    if ((self = [self initWithOwner:owner native:texture descriptor:descriptor
                          iosurface:nullptr plane:0u surfaceHandle:A840_SYS_INVALID_HANDLE])) {
        _backingBuffer = buffer;
        _bufferOffset = bufferOffset;
        _bufferBytesPerRow = bufferBytesPerRow;
    }
    return self;
}

- (instancetype)initViewOfParent:(A840MTLTexture *)parent
                     pixelFormat:(MTLPixelFormat)pixelFormat
                     textureType:(MTLTextureType)textureType
                          levels:(NSRange)levelRange
                          slices:(NSRange)sliceRange
                         swizzle:(MTLTextureSwizzleChannels)swizzle
{
    if (!(self = [super initWithOwner:parent.owner options:parent.resourceOptions])) return nil;
    _parentTexture = parent;
    _native = parent.native;
    if (_native) {
        @synchronized (self.owner) { a840_rt_texture_retain(_native); }
    }
    _descriptor = [parent->_descriptor copy];
    _iosurface = parent.iosurface ? (IOSurfaceRef)CFRetain(parent.iosurface) : nullptr;
    _iosurfacePlane = parent.iosurfacePlane;
    _surfaceHandle = A840_SYS_INVALID_HANDLE;
    _viewPixelFormat = pixelFormat;
    _viewTextureType = textureType;
    _viewLevelRange = levelRange;
    _viewSliceRange = sliceRange;
    _parentRelativeLevel = levelRange.location;
    _parentRelativeSlice = sliceRange.location;
    _swizzle = swizzle;
    return self;
}

- (void)dealloc
{
    if (_surfaceHandle != A840_SYS_INVALID_HANDLE && self.owner && self.owner.systemFrontend) {
        (void)a840_sys_unbind_iosurface(self.owner.systemFrontend, _surfaceHandle);
        _surfaceHandle = A840_SYS_INVALID_HANDLE;
    }
    if (_iosurface) {
        CFRelease(_iosurface);
        _iosurface = nullptr;
    }
    if (_memorylessNative) {
        @synchronized (self.owner) { a840_ubwc_texture_destroy(_memorylessNative); }
        _memorylessNative = nullptr;
    }
    if (_native) {
        @synchronized (self.owner) { a840_rt_texture_release(_native); }
        _native = nullptr;
    }
}

- (IOSurfaceRef)iosurface { return _iosurface; }
- (NSUInteger)iosurfacePlane { return _iosurfacePlane; }
- (MTLTextureType)textureType { return _viewTextureType; }
- (MTLPixelFormat)pixelFormat { return _viewPixelFormat; }
- (NSUInteger)width
{
    const NSUInteger shifted = _descriptor.width >> _parentRelativeLevel;
    return shifted ? shifted : 1u;
}
- (NSUInteger)height
{
    const NSUInteger shifted = _descriptor.height >> _parentRelativeLevel;
    return shifted ? shifted : 1u;
}
- (NSUInteger)depth { return _descriptor.depth ?: 1u; }
- (NSUInteger)mipmapLevelCount { return _viewLevelRange.length ?: 1u; }
- (NSUInteger)sampleCount { return _descriptor.sampleCount ?: 1u; }
- (NSUInteger)arrayLength { return _viewSliceRange.length ?: 1u; }
- (NSUInteger)allocatedSize
{
    if (_native) return (NSUInteger)_native->alloc_bytes;
    if (self.heapSubAlloc) return (NSUInteger)self.heapSubAlloc->size_bytes;
    return 0u;
}
- (MTLTextureUsage)usage { return _descriptor.usage; }
- (MTLStorageMode)storageMode { return _descriptor.storageMode; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)allowGPUOptimizedContents { return _descriptor.allowGPUOptimizedContents; }
- (MTLTextureSwizzleChannels)swizzle { return _swizzle; }
- (MTLResourceID)gpuResourceID
{
    if (_native) return MTLResourceID{_native->gpu_va};
    if (self.heapSubAlloc) return MTLResourceID{self.heapSubAlloc->gpu_va};
    return MTLResourceID{0u};
}
- (id<MTLTexture>)parentTexture { return _parentTexture; }
- (NSUInteger)parentRelativeLevel { return _parentRelativeLevel; }
- (NSUInteger)parentRelativeSlice { return _parentRelativeSlice; }
- (id<MTLBuffer>)buffer { return _backingBuffer; }
- (NSUInteger)bufferOffset { return _bufferOffset; }
- (NSUInteger)bufferBytesPerRow { return _bufferBytesPerRow; }
- (NSUInteger)rotation { return 0u; }

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
{
    return [self newTextureViewWithPixelFormat:pixelFormat
                                   textureType:_viewTextureType
                                        levels:NSMakeRange(0u, self.mipmapLevelCount)
                                        slices:NSMakeRange(0u, self.arrayLength)
                                       swizzle:_swizzle];
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
                                    textureType:(MTLTextureType)textureType
                                         levels:(NSRange)levelRange
                                         slices:(NSRange)sliceRange
{
    return [self newTextureViewWithPixelFormat:pixelFormat
                                   textureType:textureType
                                        levels:levelRange
                                        slices:sliceRange
                                       swizzle:_swizzle];
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
                                    textureType:(MTLTextureType)textureType
                                         levels:(NSRange)levelRange
                                         slices:(NSRange)sliceRange
                                        swizzle:(MTLTextureSwizzleChannels)swizzle
{
    if (!a840_sys_supports_pixel_format((uint32_t)pixelFormat)) return nil;
    if (levelRange.location + levelRange.length > self.mipmapLevelCount ||
        sliceRange.location + sliceRange.length > self.arrayLength ||
        levelRange.length == 0u || sliceRange.length == 0u) {
        return nil;
    }
    const NSRange absLevels = NSMakeRange(_viewLevelRange.location + levelRange.location, levelRange.length);
    const NSRange absSlices = NSMakeRange(_viewSliceRange.location + sliceRange.location, sliceRange.length);
    return [[A840MTLTexture alloc] initViewOfParent:self
                                        pixelFormat:pixelFormat
                                        textureType:textureType
                                             levels:absLevels
                                             slices:absSlices
                                            swizzle:swizzle];
}

- (void)transfer:(MTLRegion)region level:(NSUInteger)level slice:(NSUInteger)slice
          bytes:(void *)bytes rowBytes:(NSUInteger)rowBytes
          write:(BOOL)write
{
    const auto x = region.origin.x, y = region.origin.y;
    const auto width = region.size.width, height = region.size.height;
    if (!_native || !_native->format_caps)
        [NSException raise:NSInvalidArgumentException format:@"Invalid native 2-D texture transfer"];
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
- (MTLResourceID)gpuResourceID { return MTLResourceID{(uint64_t)(uintptr_t)(__bridge void *)self}; }
- (const uint32_t *)descriptorWords { return _words; }
- (MTLSamplerDescriptor *)descriptor { return _descriptor; }
- (NSUInteger)pixelFormat { return MTLPixelFormatInvalid; }
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
- (BOOL)supportIndirectCommandBuffers { return YES; }
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
- (BOOL)supportIndirectCommandBuffers { return _configuration.supportIndirectCommandBuffers; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return A840_LIMIT_THREADSIZE_BASE * A840_LIMIT_MAX_WAVES; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0u; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)imageblockDimensions
{
    (void)imageblockDimensions;
    return 0u;
}
- (const A840RtGraphicsProgramDesc *)nativeDescriptor
{
    return _plan && _descriptor.emit_desc.vs.stage && _descriptor.emit_desc.fs.stage ? &_descriptor : nullptr;
}
@end

#pragma clang diagnostic pop
