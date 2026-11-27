/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 iOS 27 System Metal Device SPI implementation (zero fallback). */
#import "A840MTLSystemSPI.h"
#import "A840MTLCommand.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLLibraryAndArchive.h"
#import "A840MTLResource.h"
#import "A840MTLPipelineProvider.h"

#include "../core/a840_system_frontend_core.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"

static A840YcbcrFormat detectIOSurfaceYcbcr(IOSurfaceRef surface, MTLPixelFormat requestedFormat)
{
    if (!surface) return A840_YCBCR_NONE;
    const size_t planes = IOSurfaceGetPlaneCount(surface);
    if (planes < 2u) return A840_YCBCR_NONE;
    if (requestedFormat == MTLPixelFormatR16Unorm || requestedFormat == MTLPixelFormatRG16Unorm) {
        return A840_YCBCR_P010;
    }
    return A840_YCBCR_NV12;
}

@implementation A840MTLDevice (A840MTLSystemSPI)

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily
{
    return a840_sys_supports_gpu_family((uint32_t)gpuFamily) ? YES : NO;
}

- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet
{
    (void)featureSet;
    return NO;
}
- (BOOL)supportsPrimitiveType:(NSUInteger)type
{
    return type <= 4u;
}

- (BOOL)supportsTextureSampleCount:(NSUInteger)sampleCount
{
    return a840_sys_supports_sample_count((uint32_t)sampleCount) ? YES : NO;
}

- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
    return count >= 1u && count <= A840_VERTEX_AMP_MAX_VIEWS;
}

- (BOOL)hasUnifiedMemory { return self.systemCaps.has_unified_memory ? YES : NO; }
- (BOOL)isLowPower { return NO; }
/* Private iOS 27 Metal SPI limits & flags (required when initLimits is empty). */
- (NSUInteger)gpuCoreCount { return 12u; }
- (NSUInteger)maxVertexAmplificationCount { return A840_VERTEX_AMP_MAX_VIEWS; }
- (NSUInteger)deviceLinearTextureAlignmentBytes { return A840_SYS_IOSURFACE_RO_ALIGN; }
- (NSUInteger)linearTextureAlignmentBytes { return A840_SYS_IOSURFACE_RO_ALIGN; }
- (NSUInteger)maxVertexBuffers { return 31u; }
- (NSUInteger)maxFragmentBuffers { return 31u; }
- (NSUInteger)maxComputeBuffers { return 31u; }
- (NSUInteger)maxTileBuffers { return 31u; }
- (NSUInteger)maxVertexTextures { return 128u; }
- (NSUInteger)maxFragmentTextures { return 128u; }
- (NSUInteger)maxComputeTextures { return 128u; }
- (NSUInteger)maxTileTextures { return 128u; }
- (NSUInteger)maxVertexSamplers { return 16u; }
- (NSUInteger)maxFragmentSamplers { return 16u; }
- (NSUInteger)maxComputeSamplers { return 16u; }
- (NSUInteger)maxTileSamplers { return 16u; }
- (NSUInteger)maxComputeLocalMemorySizes { return 31u; }
- (NSUInteger)maxComputeThreadgroupMemory { return (NSUInteger)self.systemCaps.max_threadgroup_memory_bytes; }
- (NSUInteger)maxComputeThreadgroupMemoryAlignmentBytes { return 16u; }
- (NSUInteger)maxPredicatedNestingDepth { return 0u; }
- (NSUInteger)maxRasterizationRateLayerCount { return 1u; }
- (NSUInteger)maxTextureWidth1D { return 16384u; }
- (NSUInteger)maxTextureWidth2D { return 16384u; }
- (NSUInteger)maxTextureHeight2D { return 16384u; }
- (NSUInteger)maxTextureWidth3D { return 2048u; }
- (NSUInteger)maxTextureHeight3D { return 2048u; }
- (NSUInteger)maxTextureDepth3D { return 2048u; }
- (NSUInteger)maxTextureDimensionCube { return 16384u; }
- (NSUInteger)maxTextureLayers { return 2048u; }
- (NSUInteger)maxTotalComputeThreadsPerThreadgroup { return A840_LIMIT_THREADSIZE_BASE * A840_LIMIT_MAX_WAVES; }
- (NSUInteger)maxViewportCount { return 16u; }
- (BOOL)isFramebufferReadSupported { return NO; }
- (BOOL)isLargeMRTSupported { return NO; }
- (BOOL)isRGB10A2GammaSupported { return NO; }
- (BOOL)supportsBufferWithAddressRanges { return NO; }
- (BOOL)supportsBufferWithIOSurface { return NO; }
- (BOOL)supportsCommandBufferJump { return NO; }
- (BOOL)supportsComputeCompressedTextureWrite { return NO; }
- (BOOL)supportsConditionalLoadStore { return NO; }
- (BOOL)supportsDynamicAttributeStride { return YES; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsFloat16BCubicFiltering { return YES; }
- (BOOL)supportsHeapWithAddressRanges { return NO; }
- (BOOL)supportsImageBlocks { return NO; }
- (BOOL)supportsMemoryOrderAtomics { return YES; }
- (BOOL)supportsNativeHardwareFP16 { return YES; }
- (BOOL)supportsNonSquareTileShaders { return NO; }
- (BOOL)supportsProgrammableSamplePositions { return self.systemCaps.supports_programmable_sample_positions ? YES : NO; }
- (BOOL)supportsResourceDetachBacking { return NO; }
- (BOOL)supportsSharedTextureHandles { return YES; }
- (BOOL)supportsSIMDReduction { return YES; }
- (BOOL)supportsSIMDShuffleAndFill { return YES; }
- (BOOL)supportsTexture2DMultisampleArray { return YES; }
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)supports32BitFloatFiltering { return self.systemCaps.supports_32bit_float_filtering ? YES : NO; }
- (BOOL)supports32BitMSAA { return self.systemCaps.supports_32bit_msaa ? YES : NO; }
- (BOOL)supportsQueryTextureLOD { return self.systemCaps.supports_query_texture_lod ? YES : NO; }
- (BOOL)supportsBCTextureCompression { return NO; }
- (BOOL)supportsPullModelInterpolation { return YES; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsShaderBarycentricCoordinates { return NO; }
- (BOOL)areProgrammableSamplePositionsSupported { return self.systemCaps.supports_programmable_sample_positions ? YES : NO; }
- (BOOL)areRasterOrderGroupsSupported { return self.systemCaps.supports_raster_order_groups ? YES : NO; }
- (BOOL)areBarycentricCoordsSupported { return NO; }

- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier2; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTier2; }
- (uint64_t)recommendedMaxWorkingSetSize { return self.systemCaps.recommended_max_working_set_bytes; }

- (NSUInteger)currentAllocatedSize
{
    @synchronized (self) {
        return self.runtime ? (NSUInteger)a840_va_allocator_used_bytes(&self.runtime->va_alloc) : 0u;
    }
}

- (NSUInteger)maxThreadgroupMemoryLength { return (NSUInteger)self.systemCaps.max_threadgroup_memory_bytes; }
- (NSUInteger)maxArgumentBufferSamplerCount { return 16u; }
- (NSUInteger)sparseTileSizeInBytes { return (NSUInteger)self.systemCaps.sparse_tile_bytes; }
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes { return (NSUInteger)self.systemCaps.iosurface_readonly_align_bytes; }
- (NSUInteger)iosurfaceTextureAlignmentBytes { return (NSUInteger)self.systemCaps.iosurface_readonly_align_bytes; }
- (uint64_t)dedicatedMemorySize { return 0u; }
- (uint64_t)sharedMemorySize { return self.systemCaps.recommended_max_working_set_bytes; }
- (NSUInteger)maxColorAttachments { return (NSUInteger)self.systemCaps.max_color_attachments; }
- (NSUInteger)maxVertexAttributes { return (NSUInteger)self.systemCaps.max_vertex_attributes; }

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    (void)format;
    return A840_SYS_IOSURFACE_RO_ALIGN;
}

- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format
{
    (void)format;
    return A840_SYS_IOSURFACE_RO_ALIGN;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                 iosurface:(IOSurfaceRef)iosurface
                                     plane:(NSUInteger)plane
{
    if (!descriptor || !iosurface || !self.systemFrontend) return nil;
    if (descriptor.width == 0u || descriptor.height == 0u ||
        descriptor.width > UINT32_MAX || descriptor.height > UINT32_MAX) {
        return nil;
    }

    const size_t planeCount = IOSurfaceGetPlaneCount(iosurface);
    if (planeCount > 0u && plane >= planeCount) return nil;

    A840SysIOSurfaceDesc sdesc{};
    sdesc.iosurface_id = (uint64_t)IOSurfaceGetID(iosurface);
    sdesc.plane_index = (uint32_t)plane;
    sdesc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
    sdesc.ycbcr_format = detectIOSurfaceYcbcr(iosurface, descriptor.pixelFormat);
    sdesc.width = (uint32_t)(planeCount > 0u ? IOSurfaceGetWidthOfPlane(iosurface, plane) : IOSurfaceGetWidth(iosurface));
    sdesc.height = (uint32_t)(planeCount > 0u ? IOSurfaceGetHeightOfPlane(iosurface, plane) : IOSurfaceGetHeight(iosurface));
    if (sdesc.width == 0u) sdesc.width = (uint32_t)descriptor.width;
    if (sdesc.height == 0u) sdesc.height = (uint32_t)descriptor.height;
    sdesc.bytes_per_row = (uint32_t)(planeCount > 0u ? IOSurfaceGetBytesPerRowOfPlane(iosurface, plane)
                                                     : IOSurfaceGetBytesPerRow(iosurface));
    sdesc.alloc_size = (uint64_t)IOSurfaceGetAllocSize(iosurface);
    sdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_CPU_VISIBLE;
    if (descriptor.usage & MTLTextureUsageShaderWrite) sdesc.usage_flags |= A840_TEX_USAGE_SHADER_WRITE;
    if (descriptor.usage & MTLTextureUsageRenderTarget) sdesc.usage_flags |= A840_TEX_USAGE_RENDER_TARGET;
    sdesc.is_cross_process = true;
    sdesc.for_scanout = (self.processRole == A840_SYS_ROLE_RENDER_SERVER) ||
                        ((descriptor.usage & MTLTextureUsageRenderTarget) != 0);
    sdesc.request_ubwc6 = (sdesc.ycbcr_format == A840_YCBCR_NONE) &&
                          (descriptor.storageMode == MTLStorageModePrivate);
    sdesc.framebuffer_only = false;

    A840SysSurfaceHandle surfHandle = A840_SYS_INVALID_HANDLE;
    A840SysSurfaceInfo surfInfo{};
    if (a840_sys_bind_iosurface(self.systemFrontend, &sdesc, &surfHandle, &surfInfo) != A840_SYS_OK) {
        return nil;
    }

    A840RtTextureDesc rtdesc{};
    rtdesc.mtl_pixel_format = (uint32_t)descriptor.pixelFormat;
    rtdesc.tex_type = A840_TEX_2D;
    rtdesc.width = sdesc.width;
    rtdesc.height = sdesc.height;
    rtdesc.depth = 1u;
    rtdesc.array_layers = 1u;
    rtdesc.mip_levels = 1u;
    rtdesc.samples = 1u;
    rtdesc.tile_mode = A840_TILE_LINEAR;
    rtdesc.pitch_bytes = sdesc.bytes_per_row;
    rtdesc.usage_flags = sdesc.usage_flags;

    A840RtTexture *rtTex = nullptr;
    void *basePtr = IOSurfaceGetBaseAddress(iosurface);
    const size_t allocSize = IOSurfaceGetAllocSize(iosurface);
    const size_t pageBytes = A840_RT_PAGE_BYTES;
    uintptr_t planePtr = planeCount > 0u
        ? reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddressOfPlane(iosurface, plane))
        : reinterpret_cast<uintptr_t>(basePtr);
    const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(basePtr);
    const bool canImportZeroCopy =
        basePtr != nullptr &&
        (baseAddr & (pageBytes - 1u)) == 0u &&
        allocSize > 0u &&
        planePtr >= baseAddr &&
        (planePtr - baseAddr) < allocSize &&
        ((planePtr - baseAddr) % 64u) == 0u;

    @synchronized (self) {
        if (canImportZeroCopy) {
            const uint64_t alignedAlloc = ((uint64_t)allocSize + pageBytes - 1u) & ~(uint64_t)(pageBytes - 1u);
            A840RtBufferDesc bdesc{};
            bdesc.size_bytes = alignedAlloc;
            bdesc.alignment = pageBytes;
            bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE;
            bdesc.host_import = true;
            bdesc.host_ptr = basePtr;
            A840RtBuffer *surfBo = nullptr;
            if (a840_rt_buffer_create(self.runtime, &bdesc, &surfBo) == A840_RT_OK && surfBo) {
                const uint64_t planeOff = (uint64_t)(planePtr - baseAddr);
                if (a840_rt_texture_create_on_buffer(self.runtime, surfBo, planeOff, &rtdesc, &rtTex) != A840_RT_OK) {
                    rtTex = nullptr;
                }
                a840_rt_buffer_release(surfBo);
            }
        }
        if (!rtTex) {
            if (a840_rt_texture_create(self.runtime, &rtdesc, &rtTex) != A840_RT_OK) {
                (void)a840_sys_unbind_iosurface(self.systemFrontend, surfHandle);
                return nil;
            }
        }
    }

    return [[A840MTLTexture alloc] initWithOwner:self
                                          native:rtTex
                                      descriptor:descriptor
                                       iosurface:iosurface
                                           plane:plane
                                   surfaceHandle:surfHandle];
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
    if (!descriptor) return nil;
    return [[A840MTLDepthStencilState alloc] initWithOwner:self descriptor:descriptor];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount
{
    return [[A840MTLCommandQueue alloc] initWithOwner:self maxCommandBufferCount:maxCommandBufferCount];
}

- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(MTLCommandQueueDescriptor *)descriptor
{
    const NSUInteger maxCount = descriptor ? descriptor.maxCommandBufferCount : 64u;
    return [[A840MTLCommandQueue alloc] initWithOwner:self maxCommandBufferCount:maxCount];
}

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor
{
    if (!descriptor || descriptor.size == 0u) return nil;
    return [[A840MTLHeap alloc] initWithOwner:self descriptor:descriptor];
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    (void)options;
    const NSUInteger align = A840_HEAP_MIN_ALIGN_BYTES;
    const NSUInteger size = (length + align - 1u) & ~(align - 1u);
    return MTLSizeAndAlign{size, align};
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)desc
{
    if (!desc) return MTLSizeAndAlign{0u, A840_RT_PAGE_BYTES};
    A840UbwcLayout layout{};
    if (a840_ubwc_compute_layout((uint32_t)desc.pixelFormat,
                                 (uint32_t)desc.width,
                                 (uint32_t)desc.height,
                                 (uint32_t)(desc.arrayLength ? desc.arrayLength : 1u),
                                 (uint32_t)(desc.sampleCount ? desc.sampleCount : 1u),
                                 &layout) == A840_RT_OK && layout.total_bytes > 0u) {
        return MTLSizeAndAlign{(NSUInteger)layout.total_bytes, A840_RT_PAGE_BYTES};
    }
    const NSUInteger approx = desc.width * desc.height * 4u;
    const NSUInteger aligned = (approx + A840_RT_PAGE_BYTES - 1u) & ~(NSUInteger)(A840_RT_PAGE_BYTES - 1u);
    return MTLSizeAndAlign{aligned ? aligned : A840_RT_PAGE_BYTES, A840_RT_PAGE_BYTES};
}

- (id<MTLEvent>)newEvent
{
    return [[A840MTLEvent alloc] initWithOwner:self];
}

- (id<MTLSharedEvent>)newSharedEvent
{
    return [[A840MTLSharedEvent alloc] initWithOwner:self initialValue:0u machPort:0u];
}

- (id<MTLSharedEvent>)newSharedEventWithOptions:(NSUInteger)options
{
    (void)options;
    return [self newSharedEvent];
}

- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)sharedEventHandle
{
    if (!sharedEventHandle) return nil;
    return [[A840MTLSharedEvent alloc] initWithOwner:self handle:sharedEventHandle];
}

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCount
                                                               options:(MTLResourceOptions)options
{
    if (!descriptor || maxCount == 0u || maxCount > UINT32_MAX) return nil;
    return [[A840MTLIndirectCommandBuffer alloc] initWithOwner:self
                                                    descriptor:descriptor
                                               maxCommandCount:maxCount
                                                       options:options];
}

- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor
                                                 error:(NSError **)error
{
    A840MTLBinaryArchive *arch = [[A840MTLBinaryArchive alloc] initWithOwner:self descriptor:descriptor error:error];
    if (!arch && descriptor && descriptor.url) {
        MTLBinaryArchiveDescriptor *emptyDesc = [MTLBinaryArchiveDescriptor new];
        arch = [[A840MTLBinaryArchive alloc] initWithOwner:self descriptor:emptyDesc error:error];
    }
    return arch;
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    return [A840MTLStitchedLibraryLoader libraryWithDispatchData:data device:self error:error];
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)filepath error:(NSError **)error
{
    if (!filepath.length) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Library filepath is required");
        return nil;
    }
    return [self newLibraryWithURL:[NSURL fileURLWithPath:filepath] error:error];
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                               options:(MTLCompileOptions *)options
                                 error:(NSError **)error
{
    return [A840MTLStitchedLibraryLoader libraryWithSource:source options:options device:self error:error];
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                             error:(NSError **)error
{
    return [A840MTLStitchedLibraryLoader libraryWithStitchedDescriptor:descriptor device:self error:error];
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Bundle contains no default.metallib");
        return nil;
    }
    return [self newLibraryWithURL:url error:error];
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *pointer, NSUInteger length))deallocator
{
    if (!pointer || !length) return nil;
    A840RtBufferDesc desc{};
    desc.size_bytes = length;
    desc.alignment = A840_RT_PAGE_BYTES;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE;
    desc.host_import = (((uintptr_t)pointer & (A840_RT_PAGE_BYTES - 1u)) == 0u);
    desc.host_ptr = desc.host_import ? pointer : nullptr;

    A840RtBuffer *buffer = nullptr;
    @synchronized (self) {
        if (a840_rt_buffer_create(self.runtime, &desc, &buffer) != A840_RT_OK) {
            desc.host_import = false;
            desc.host_ptr = nullptr;
            if (a840_rt_buffer_create(self.runtime, &desc, &buffer) != A840_RT_OK) {
                return nil;
            }
            (void)a840_rt_buffer_write(buffer, 0u, pointer, length);
        }
    }
    return [[A840MTLBuffer alloc] initWithOwner:self
                                         native:buffer
                                        options:options
                                  noCopyPointer:pointer
                                    deallocator:deallocator];
}

@end
#pragma clang diagnostic pop
