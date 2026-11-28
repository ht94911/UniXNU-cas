/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback native resource wrappers: IOSurface, Heap aliasing,
 * Texture Views, Memoryless attachments, and probe006 sampler clamping. */
#import "A840MTLResource.h"
#import "A840MTLCommand.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLDiagnostics.h"

#include <cmath>
#include <cstring>
#include "NativeMetalTranslate.h"

extern "C" void sys_dcache_flush(void *start, size_t length);

@protocol A840MTLPlanQuery <NSObject>
@optional
- (NSUInteger)imageblockCellBytes;
@end

static const uint8_t *a840B3a8Xr10Lut()
{
    static uint8_t lut[1024];
    static bool ready = false;
    if (!ready) {
        for (int c = 0; c < 1024; ++c) {
            int v = ((c - 384) * 255 + 255) / 510;
            if (c <= 384) v = 0;
            else if (c >= 894) v = 255;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lut[c] = static_cast<uint8_t>(v);
        }
        ready = true;
    }
    return lut;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

@implementation A840MTLAllocation {
    MTLResourceOptions _options;
    MTLPurgeableState _purgeableState;
    int32_t _inFlightCount;
    BOOL _pendingAliasable;
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
- (BOOL)isAliasable
{
    if (!_heapSubAlloc) return NO;
    @synchronized (_owner) {
        return (_pendingAliasable || a840_heap_suballoc_is_aliasable(_heapSubAlloc)) ? YES : NO;
    }
}

- (void)retainInFlightUse
{
    if (!_heapSubAlloc) return;
    @synchronized (_owner) {
        _inFlightCount++;
    }
}

- (void)releaseInFlightUse
{
    if (!_heapSubAlloc) return;
    @synchronized (_owner) {
        if (_inFlightCount > 0) _inFlightCount--;
        if (_inFlightCount == 0 && _pendingAliasable) {
            _pendingAliasable = NO;
            (void)a840_heap_suballoc_make_aliasable(_heapSubAlloc);
        }
    }
}

- (void)makeAliasable
{
    if (!_heapSubAlloc) return;
    @synchronized (_owner) {
        if (_inFlightCount > 0) {
            _pendingAliasable = YES;
        } else {
            (void)a840_heap_suballoc_make_aliasable(_heapSubAlloc);
        }
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
- (void)setResponsibleProcess:(int)pid { (void)pid; }
- (void)waitUntilComplete
{
    [A840MTLCommandBuffer waitForInFlightResource:self];
}
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
    return [self initWithOwner:owner heap:heap subAlloc:subAlloc native:nullptr options:options];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                       native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options
{
    if ((self = [super initWithOwner:owner heap:heap subAlloc:subAlloc options:options])) {
        _native = buffer;
    }
    return self;
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
- (void *)noCopyPointer { return _noCopyPointer; }
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
    rtdesc.mtl_pixel_format = a840_canonical_backing_pixel_format((uint32_t)descriptor.pixelFormat);
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
    A840RtTexture *_iosurfaceNative;
    A840RtTexture *_b3a8Shadow;
    BOOL _isB3a8Plane0;
    size_t _b3a8Plane0Offset;
    size_t _b3a8Plane0Row;
    size_t _b3a8Plane1Offset;
    size_t _b3a8Plane1Row;
    uint32_t _lastB3a8Seed;
    BOOL _hasB3a8Seed;
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
        _iosurfaceNative = texture;
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
    return [self initWithOwner:owner native:nullptr memorylessUbwc:memoryless descriptor:descriptor];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
               memorylessUbwc:(A840UbwcTexture *)memoryless
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    if ((self = [super initWithOwner:owner options:descriptor.resourceOptions])) {
        _native = texture;
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
    return [self initWithOwner:owner heap:heap subAlloc:subAlloc native:nullptr descriptor:descriptor];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
{
    if ((self = [super initWithOwner:owner heap:heap subAlloc:subAlloc options:descriptor.resourceOptions])) {
        _native = texture;
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

static uint32_t a840MapSwizzleChannel(MTLTextureSwizzle ch, uint32_t fallback)
{
    switch (ch) {
    case MTLTextureSwizzleZero: return A840_SWIZZLE_ZERO;
    case MTLTextureSwizzleOne: return A840_SWIZZLE_ONE;
    case MTLTextureSwizzleRed: return A840_SWIZZLE_R;
    case MTLTextureSwizzleGreen: return A840_SWIZZLE_G;
    case MTLTextureSwizzleBlue: return A840_SWIZZLE_B;
    case MTLTextureSwizzleAlpha: return A840_SWIZZLE_A;
    default: return fallback;
    }
}

static uint32_t a840MapTextureType(MTLTextureType t)
{
    switch (t) {
    case MTLTextureType1D:
    case MTLTextureType1DArray: return A840_TEX_1D;
    case MTLTextureTypeCube:
    case MTLTextureTypeCubeArray: return A840_TEX_CUBE;
    case MTLTextureType3D: return A840_TEX_3D;
    default: return A840_TEX_2D;
    }
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
    if (parent.isB3a8Plane0) {
        [parent syncB3a8CpuContentIfNeeded];
    }
    if (parent.native) {
        const uint32_t backingFmt = a840_canonical_backing_pixel_format((uint32_t)pixelFormat);
        const uint32_t a840Type = a840MapTextureType(textureType);
        const uint32_t swz[4] = {
            a840MapSwizzleChannel(swizzle.red, A840_SWIZZLE_R),
            a840MapSwizzleChannel(swizzle.green, A840_SWIZZLE_G),
            a840MapSwizzleChannel(swizzle.blue, A840_SWIZZLE_B),
            a840MapSwizzleChannel(swizzle.alpha, A840_SWIZZLE_A),
        };
        const bool isDefaultSwz = (swz[0] == A840_SWIZZLE_R && swz[1] == A840_SWIZZLE_G &&
                                   swz[2] == A840_SWIZZLE_B && swz[3] == A840_SWIZZLE_A);
        A840RtTexture *viewTex = nullptr;
        @synchronized (self.owner) {
            if (a840_rt_texture_create_view(self.owner.runtime, parent.native, backingFmt, a840Type,
                                            (uint32_t)levelRange.location, (uint32_t)(levelRange.length ?: 1u),
                                            (uint32_t)sliceRange.location, (uint32_t)(sliceRange.length ?: 1u),
                                            !isDefaultSwz, swz, &viewTex) == A840_RT_OK && viewTex) {
                _native = viewTex;
            } else {
                _native = parent.native;
                a840_rt_texture_retain(_native);
            }
        }
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
    if (_memorylessNative) {
        @synchronized (self.owner) { a840_ubwc_texture_destroy(_memorylessNative); }
        _memorylessNative = nullptr;
    }
    A840RtTexture *orig = _iosurfaceNative ? _iosurfaceNative : _native;
    _native = nullptr;
    _iosurfaceNative = nullptr;
    if (_b3a8Shadow) {
        @synchronized (self.owner) { a840_rt_texture_release(_b3a8Shadow); }
        _b3a8Shadow = nullptr;
    }
    if (orig) {
        @synchronized (self.owner) { a840_rt_texture_release(orig); }
    }
    _surfaceBoBox = nil;
    if (_iosurface) {
        CFRelease(_iosurface);
        _iosurface = nullptr;
    }
}

- (IOSurfaceRef)iosurface { return _iosurface; }
- (NSUInteger)iosurfacePlane { return _iosurfacePlane; }
- (BOOL)isB3a8Plane0 { return _isB3a8Plane0; }

- (void)configureB3a8Plane0Offset:(size_t)plane0Offset
                        plane0Row:(size_t)plane0Row
                     plane1Offset:(size_t)plane1Offset
                        plane1Row:(size_t)plane1Row
{
    _isB3a8Plane0 = YES;
    _b3a8Plane0Offset = plane0Offset;
    _b3a8Plane0Row = plane0Row;
    _b3a8Plane1Offset = plane1Offset;
    _b3a8Plane1Row = plane1Row;
    _hasB3a8Seed = NO;
    [self syncB3a8CpuContentIfNeeded];
}

- (void)markB3a8GpuWriteTarget
{
    if (!_isB3a8Plane0 || !_iosurfaceNative) return;
    @synchronized (self.owner) {
        _native = _iosurfaceNative;
        if (_iosurface) {
            _lastB3a8Seed = IOSurfaceGetSeed(_iosurface);
            _hasB3a8Seed = YES;
        }
    }
}

- (void)syncB3a8CpuContentIfNeeded
{
    if (!_isB3a8Plane0 || !_iosurface || !_iosurfaceNative) return;
    const uint32_t seed = IOSurfaceGetSeed(_iosurface);
    if (_hasB3a8Seed && seed == _lastB3a8Seed) return;
    void *basePtr = IOSurfaceGetBaseAddress(_iosurface);
    const size_t allocSize = IOSurfaceGetAllocSize(_iosurface);
    const uint32_t w = _iosurfaceNative->width;
    const uint32_t h = _iosurfaceNative->height;
    if (!basePtr || !w || !h ||
        _b3a8Plane0Offset + (size_t)h * _b3a8Plane0Row > allocSize ||
        _b3a8Plane1Offset + (size_t)h * _b3a8Plane1Row > allocSize) {
        return;
    }
    sys_dcache_flush(basePtr, allocSize);
    const auto *p0Base = static_cast<const uint8_t *>(basePtr) + _b3a8Plane0Offset;
    const auto *p1Base = static_cast<const uint8_t *>(basePtr) + _b3a8Plane1Offset;
    /* Distinguish CPU/CoreGraphics 10-bit BGR10_XR + A8 (Plane 1 non-zero AND Plane 0
     * bits 30..31 always 0) from Metal GPU-rendered BGRA8 (Plane 1 untouched = all 0). */
    uint32_t p1NonZero = 0u;
    uint32_t p0HighBits = 0u;
    const uint32_t stepY = h > 16u ? (h / 16u) : 1u;
    const uint32_t stepX = w > 16u ? (w / 16u) : 1u;
    for (uint32_t y = 0u; y < h; y += stepY) {
        const auto *row0 = reinterpret_cast<const uint32_t *>(p0Base + (size_t)y * _b3a8Plane0Row);
        const uint8_t *row1 = p1Base + (size_t)y * _b3a8Plane1Row;
        for (uint32_t x = 0u; x < w; x += stepX) {
            if (row1[x] != 0u) p1NonZero++;
            if ((row0[x] & 0xc0000000u) != 0u) p0HighBits++;
        }
    }
    @synchronized (self.owner) {
        if (p1NonZero == 0u || p0HighBits > 0u) {
            _native = _iosurfaceNative;
            _lastB3a8Seed = seed;
            _hasB3a8Seed = YES;
            return;
        }
        if (!_b3a8Shadow) {
            A840RtTextureDesc sdesc{};
            sdesc.mtl_pixel_format = _iosurfaceNative->mtl_pixel_format;
            sdesc.tex_type = A840_TEX_2D;
            sdesc.width = w;
            sdesc.height = h;
            sdesc.depth = 1u;
            sdesc.array_layers = 1u;
            sdesc.mip_levels = 1u;
            sdesc.samples = 1u;
            sdesc.tile_mode = A840_TILE_LINEAR;
            sdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_CPU_VISIBLE;
            if (a840_rt_texture_create(self.owner.runtime, &sdesc, &_b3a8Shadow) != A840_RT_OK ||
                !_b3a8Shadow || !_b3a8Shadow->cpu_ptr) {
                if (_b3a8Shadow) { a840_rt_texture_release(_b3a8Shadow); _b3a8Shadow = nullptr; }
                return;
            }
        }
        const uint8_t *lut = a840B3a8Xr10Lut();
        auto *dstBase = static_cast<uint8_t *>(_b3a8Shadow->cpu_ptr);
        const uint32_t dstPitch = _b3a8Shadow->pitch_bytes;
        for (uint32_t y = 0u; y < h; ++y) {
            const auto *src0 = reinterpret_cast<const uint32_t *>(p0Base + (size_t)y * _b3a8Plane0Row);
            const uint8_t *src1 = p1Base + (size_t)y * _b3a8Plane1Row;
            auto *dst = reinterpret_cast<uint32_t *>(dstBase + (size_t)y * dstPitch);
            for (uint32_t x = 0u; x < w; ++x) {
                const uint32_t p = src0[x];
                const uint32_t b = lut[p & 0x3ffu];
                const uint32_t g = lut[(p >> 10u) & 0x3ffu];
                const uint32_t r = lut[(p >> 20u) & 0x3ffu];
                const uint32_t a = src1[x];
                dst[x] = b | (g << 8u) | (r << 16u) | (a << 24u);
            }
        }
        _native = _b3a8Shadow;
        _lastB3a8Seed = seed;
        _hasB3a8Seed = YES;
    }
}
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
    /* Encode metal2vulkan-v2 TextureWord in high 32 bits with safe null bindless index 0
     * in low 32 bits — never return a raw GPU VA that would index past descriptor tables. */
    const uint32_t word = nmb_encode_texture_word(static_cast<uint32_t>(_viewPixelFormat));
    return MTLResourceID{static_cast<uint64_t>(word) << 32u};
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
    const NSUInteger bw = _native->format_caps->block_width ? _native->format_caps->block_width : 1u;
    const NSUInteger bh = _native->format_caps->block_height ? _native->format_caps->block_height : 1u;
    const NSUInteger blockBytes = _native->format_caps->block_bits / 8u;
    uint64_t subOffset = 0u;
    uint32_t subPitch = 0u, subW = 0u, subH = 0u;
    if (region.origin.z || region.size.depth != 1 || !bytes || !_native->cpu_ptr || blockBytes == 0u ||
        a840_rt_texture_get_subresource_layout(_native, (uint32_t)level, (uint32_t)slice,
                                               &subOffset, &subPitch, &subW, &subH) != A840_RT_OK ||
        x > subW || width > subW - x || y > subH || height > subH - y)
        [NSException raise:NSInvalidArgumentException format:@"Invalid native 2-D texture transfer"];
    if (!width || !height) return;
    const NSUInteger bx = x / bw;
    const NSUInteger by = y / bh;
    const NSUInteger blocksX = (width + bw - 1u) / bw;
    const NSUInteger blocksY = (height + bh - 1u) / bh;
    const NSUInteger copyBytes = blocksX * blockBytes;
    if (copyBytes > rowBytes || (blocksY && rowBytes > NSUIntegerMax / blocksY))
        [NSException raise:NSInvalidArgumentException format:@"Invalid native 2-D texture transfer"];
    auto *surfaceStart = static_cast<uint8_t *>(_native->cpu_ptr) + (size_t)subOffset +
                         by * (size_t)subPitch + bx * blockBytes;
    auto *surface = surfaceStart;
    auto *user = static_cast<uint8_t *>(bytes);
    for (NSUInteger row = 0; row < blocksY; ++row) {
        if (write) std::memcpy(surface, user, copyBytes);
        else std::memcpy(user, surface, copyBytes);
        surface += subPitch;
        user += rowBytes;
    }
    if (write && _iosurface) {
        sys_dcache_flush(surfaceStart, blocksY * (size_t)subPitch);
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
    /* Adreno 840 has no native cubic sampler filter; match integration088
     * (NativeMetalObjects.rs:2396) by mapping MTLSamplerMinMagFilterBicubic (2) -> Linear (1)
     * so RenderBox / CoreImage / iconservicesagent never abort on nil sampler states. */
    const uint32_t minF = (uint32_t)descriptor.minFilter >= A840_FILTER_LINEAR
                              ? A840_FILTER_LINEAR : A840_FILTER_NEAREST;
    const uint32_t magF = norm
                              ? ((uint32_t)descriptor.magFilter >= A840_FILTER_LINEAR
                                     ? A840_FILTER_LINEAR : A840_FILTER_NEAREST)
                              : minF;
    /* Map MTLSamplerBorderColor to Mesa bcolor_builtin index + A8XX FASTBORDERCOLOR
     * (a6xx_enums.xml: 0=0_0_0_0, 1=0_0_0_1, 3=1_1_1_1; tu_sampler.cc:64-113). */
    uint32_t bcolorIdx = 0u;
    uint32_t fastBorder = 0u;
    switch (descriptor.borderColor) {
    case MTLSamplerBorderColorOpaqueBlack:
        bcolorIdx = 2u;
        fastBorder = 1u;
        break;
    case MTLSamplerBorderColorOpaqueWhite:
        bcolorIdx = 4u;
        fastBorder = 3u;
        break;
    case MTLSamplerBorderColorTransparentBlack:
    default:
        bcolorIdx = 0u;
        fastBorder = 0u;
        break;
    }
    const A840SampDescInput input{
        minF,
        magF,
        mipFilter,
        addressMode(descriptor.sAddressMode), addressMode(descriptor.tAddressMode), addressMode(descriptor.rAddressMode),
        maxAniso, minLod, maxLod, 0.0f, cmpFunc,
        /* metal2vulkan-v2 scales pixel-coordinate dynamic samplers in SPIR-V using
         * SamplerWord bit 0; keep TPL1 hardware sampler normalized to avoid double-scaling. */
        bcolorIdx, 0u,
    };
    if (a840_encode_samp_const(&input, _words) != A840_EMIT_OK) return nil;
    /* A8XX_TEX_SAMP_2: bit 0 = FASTBORDERCOLOREN, bits 1..2 = FASTBORDERCOLOR. */
    _words[2] |= 1u | ((fastBorder & 3u) << 1u);
    _owner = owner;
    MTLSamplerDescriptor *stored = [descriptor copy];
    if (stored) {
        if ((uint32_t)stored.minFilter > A840_FILTER_LINEAR) stored.minFilter = MTLSamplerMinMagFilterLinear;
        if ((uint32_t)stored.magFilter > A840_FILTER_LINEAR) stored.magFilter = MTLSamplerMinMagFilterLinear;
    }
    _descriptor = stored;
    return self;
}
- (id<MTLDevice>)device { return _owner; }
- (NSString *)label { return _descriptor.label; }
- (MTLResourceID)gpuResourceID
{
    const uint32_t word = _descriptor ? nmb_encode_sampler_word(
        _descriptor.normalizedCoordinates ? true : false,
        static_cast<uint32_t>(_descriptor.minFilter),
        static_cast<uint32_t>(_descriptor.magFilter),
        static_cast<uint32_t>(_descriptor.mipFilter),
        static_cast<uint32_t>(_descriptor.sAddressMode),
        static_cast<uint32_t>(_descriptor.tAddressMode),
        static_cast<uint32_t>(_descriptor.rAddressMode),
        static_cast<uint32_t>(_descriptor.borderColor),
        static_cast<uint32_t>(_descriptor.compareFunction)) : 0u;
    return MTLResourceID{static_cast<uint64_t>(word) << 32u};
}
- (const uint32_t *)descriptorWords { return _words; }
- (MTLSamplerDescriptor *)descriptor { return _descriptor; }
- (NSUInteger)pixelFormat { return MTLPixelFormatInvalid; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLSamplerState", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
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
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLFence", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
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
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)imageblockDimensions
{
    if (_plan && [_plan respondsToSelector:@selector(imageblockCellBytes)]) {
        const NSUInteger cellBytes = [(id<A840MTLPlanQuery>)_plan imageblockCellBytes];
        if (cellBytes > 0u) {
            const NSUInteger w = imageblockDimensions.width ? imageblockDimensions.width : 1u;
            const NSUInteger h = imageblockDimensions.height ? imageblockDimensions.height : 1u;
            return ((cellBytes * w * h) + 15u) & ~static_cast<NSUInteger>(15u);
        }
    }
    return 0u;
}
- (BOOL)supportIndirectCommandBuffers { return YES; }
- (const A840RtComputePipelineDesc *)nativeDescriptor { return _plan && _descriptor.stage ? &_descriptor : nullptr; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLComputePipelineState", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
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
    if (_plan && [_plan respondsToSelector:@selector(imageblockCellBytes)]) {
        const NSUInteger cellBytes = [(id<A840MTLPlanQuery>)_plan imageblockCellBytes];
        if (cellBytes > 0u) {
            const NSUInteger w = imageblockDimensions.width ? imageblockDimensions.width : 1u;
            const NSUInteger h = imageblockDimensions.height ? imageblockDimensions.height : 1u;
            return ((cellBytes * w * h) + 15u) & ~static_cast<NSUInteger>(15u);
        }
    }
    return 0u;
}
- (const A840RtGraphicsProgramDesc *)nativeDescriptor
{
    return _plan && _descriptor.emit_desc.vs.stage && _descriptor.emit_desc.fs.stage ? &_descriptor : nullptr;
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLRenderPipelineState", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

#pragma clang diagnostic pop
