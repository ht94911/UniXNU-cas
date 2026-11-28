/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 iOS 27 System Metal Device SPI implementation (zero fallback). */
#import "A840MTLSystemSPI.h"
#import "A840MTLCommand.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLLibraryAndArchive.h"
#import "A840MTLResource.h"
#import "A840MTLPipelineProvider.h"
#import "A840MTLDiagnostics.h"
#include <stdio.h>

#include "../core/a840_system_frontend_core.h"
#include <dlfcn.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"

static bool isB3a8Surface(IOSurfaceRef surface)
{
    return surface && IOSurfaceGetPixelFormat(surface) == 0x62336138u /* 'b3a8' */;
}

static bool computeB3a8Planes(IOSurfaceRef surface, size_t allocSize, size_t planes[4])
{
    if (!isB3a8Surface(surface) || IOSurfaceGetPlaneCount(surface) != 2u) return false;
    if (IOSurfaceGetBytesPerRowOfPlane(surface, 0u) && IOSurfaceGetBytesPerRowOfPlane(surface, 1u)) {
        return false;
    }
    const size_t w = IOSurfaceGetWidth(surface);
    const size_t h = IOSurfaceGetHeight(surface);
    const size_t row0 = (w * 4u + 63u) & ~(size_t)63u;
    const size_t row1 = (w + 63u) & ~(size_t)63u;
    if (!w || !h || row0 * h + row1 * h > allocSize) return false;
    planes[0] = 0u;
    planes[1] = row0;
    planes[2] = row0 * h;
    planes[3] = row1;
    return true;
}

static A840YcbcrFormat detectIOSurfaceYcbcr(IOSurfaceRef surface, MTLPixelFormat requestedFormat)
{
    if (!surface) return A840_YCBCR_NONE;
    const uint32_t rawFmt = (uint32_t)requestedFormat;
    /* Two-plane 'b3a8' (RGB10A8_2P_XR10, 550/551) is extended-range RGB+A, NOT YCbCr. */
    if (isB3a8Surface(surface) || (rawFmt >= 550u && rawFmt <= 555u)) {
        return A840_YCBCR_NONE;
    }
    const size_t planes = IOSurfaceGetPlaneCount(surface);
    if (planes < 2u) return A840_YCBCR_NONE;
    const uint32_t fourcc = IOSurfaceGetPixelFormat(surface);
    if (fourcc == 0x78343230u /* 'x420' */ || fourcc == 0x78663230u /* 'xf20' */ ||
        fourcc == 0x70343230u /* 'p420' */ ||
        requestedFormat == MTLPixelFormatR16Unorm || requestedFormat == MTLPixelFormatRG16Unorm) {
        return A840_YCBCR_P010;
    }
    if (fourcc == 0x34323076u /* '420v' */ || fourcc == 0x34323066u /* '420f' */ ||
        fourcc == 0x79343230u /* 'y420' */ ||
        requestedFormat == MTLPixelFormatR8Unorm || requestedFormat == MTLPixelFormatRG8Unorm) {
        return A840_YCBCR_NV12;
    }
    return A840_YCBCR_NONE;
}

@implementation A840MTLDevice (A840MTLSystemSPI)

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily
{
    const NSInteger fam = (NSInteger)gpuFamily;
    if (fam >= 1003 && fam < 2000) {
        Dl_info caller{};
        if (dladdr(__builtin_return_address(0), &caller) &&
            caller.dli_fname && strstr(caller.dli_fname, "/RenderBox.framework/")) {
            return NO;
        }
    }
    /* Match proven iOS 27 QuartzCore/SpringBoard compatibility set (Apple1..3, Common1..2).
     * Advertising Apple4+ causes QuartzCore to select XR10/YCbCr12/tile paths unsupported on 8-bit GOP scanout. */
    return (fam == 1001 || fam == 1002 || fam == 1003 || fam == 3001 || fam == 3002) ? YES : NO;
}

- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet
{
    static const uint8_t kFamilies[] = {1, 2, 1, 2, 3, 1, 2, 3, 1, 2, 3, 4, 1, 2, 3, 4, 5};
    if ((NSUInteger)featureSet >= sizeof(kFamilies)) return NO;
    return [self supportsFamily:(MTLGPUFamily)(1000u + kFamilies[(NSUInteger)featureSet])];
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
    return count == 1u;
}

- (BOOL)supportsTextureWriteRoundingMode:(NSUInteger)mode
{
    return mode <= 2u;
}

- (BOOL)supportsRasterizationRateMapWithLayerCount:(NSUInteger)layerCount
{
    (void)layerCount;
    return NO;
}

- (BOOL)hasUnifiedMemory { return self.systemCaps.has_unified_memory ? YES : NO; }
- (BOOL)isLowPower { return NO; }
/* Private iOS 27 Metal SPI limits & flags (aligned with iOS 27 system capability contract). */
- (NSUInteger)gpuCoreCount { return 8u; }
- (NSUInteger)maxVertexAmplificationCount { return 1u; }
- (NSUInteger)minConstantBufferAlignmentBytes { return 4u; }
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes { return A840_SYS_IOSURFACE_RO_ALIGN; }
- (NSUInteger)deviceLinearTextureAlignmentBytes { return A840_SYS_IOSURFACE_RO_ALIGN; }
- (NSUInteger)linearTextureAlignmentBytes { return A840_SYS_IOSURFACE_RO_ALIGN; }
- (NSUInteger)maxVertexBuffers { return 31u; }
- (NSUInteger)maxFragmentBuffers { return 31u; }
- (NSUInteger)maxComputeBuffers { return 31u; }
- (NSUInteger)maxTileBuffers { return 0u; }
- (NSUInteger)maxVertexTextures { return 128u; }
- (NSUInteger)maxFragmentTextures { return 128u; }
- (NSUInteger)maxComputeTextures { return 128u; }
- (NSUInteger)maxTileTextures { return 0u; }
- (NSUInteger)maxVertexSamplers { return 16u; }
- (NSUInteger)maxFragmentSamplers { return 16u; }
- (NSUInteger)maxComputeSamplers { return 16u; }
- (NSUInteger)maxTileSamplers { return 0u; }
- (NSUInteger)maxComputeLocalMemorySizes { return 31u; }
- (NSUInteger)maxComputeThreadgroupMemory { return (NSUInteger)self.systemCaps.max_threadgroup_memory_bytes; }
- (NSUInteger)maxComputeThreadgroupMemoryAlignmentBytes { return 16u; }
- (NSUInteger)maxPredicatedNestingDepth { return 0u; }
- (NSUInteger)maxRasterizationRateLayerCount { return 0u; }
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
- (BOOL)isFramebufferReadSupported { return YES; }
- (BOOL)isLargeMRTSupported { return YES; }
- (BOOL)isRGB10A2GammaSupported { return NO; }
- (BOOL)supportsBufferWithAddressRanges { return NO; }
- (BOOL)supportsBufferWithIOSurface { return NO; }
- (BOOL)supportsCommandBufferJump { return NO; }
- (BOOL)supportsComputeCompressedTextureWrite { return NO; }
- (BOOL)supportsConditionalLoadStore { return NO; }
- (BOOL)supportsDynamicAttributeStride { return NO; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsYCBCRFormats12 { return NO; }
- (BOOL)supportsYCBCRPackedFormats12 { return NO; }
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsFloat16BCubicFiltering { return NO; }
- (BOOL)supportsHeapWithAddressRanges { return NO; }
- (BOOL)supportsImageBlocks { return NO; }
- (BOOL)supportsMemoryOrderAtomics { return NO; }
- (BOOL)supportsNativeHardwareFP16 { return YES; }
- (BOOL)supportsNonSquareTileShaders { return NO; }
- (BOOL)supportsProgrammableSamplePositions { return NO; }
- (BOOL)supportsResourceDetachBacking { return NO; }
- (BOOL)supportsSharedTextureHandles { return NO; }
- (BOOL)supportsSIMDGroup { return YES; }
- (BOOL)supportsSIMDReduction { return YES; }
- (BOOL)supportsSIMDShuffleAndFill { return YES; }
- (BOOL)supportsSamplerReductionMode { return YES; }
- (BOOL)supportsTexture2DMultisampleArray { return YES; }
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)supports32BitFloatFiltering { return self.systemCaps.supports_32bit_float_filtering ? YES : NO; }
- (BOOL)supports32BitMSAA { return self.systemCaps.supports_32bit_msaa ? YES : NO; }
- (BOOL)supportsQueryTextureLOD { return self.systemCaps.supports_query_texture_lod ? YES : NO; }
- (BOOL)supportsBCTextureCompression { return NO; }
- (BOOL)supportsPullModelInterpolation { return NO; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsShaderBarycentricCoordinates { return NO; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supportsRasterOrderGroups { return NO; }
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
    const size_t iosAllocSize = IOSurfaceGetAllocSize(iosurface);
    size_t b3a8Layout[4] = {};
    const bool hasB3a8Layout = computeB3a8Planes(iosurface, iosAllocSize, b3a8Layout);
    const uint32_t backingFmt = a840_canonical_backing_pixel_format(sdesc.mtl_pixel_format);

    sdesc.bytes_per_row = (uint32_t)(planeCount > 0u ? IOSurfaceGetBytesPerRowOfPlane(iosurface, plane)
                                                     : IOSurfaceGetBytesPerRow(iosurface));
    if (sdesc.bytes_per_row == 0u && hasB3a8Layout && plane < 2u) {
        sdesc.bytes_per_row = (uint32_t)b3a8Layout[plane == 0u ? 1u : 3u];
    }
    if (sdesc.bytes_per_row == 0u && sdesc.width > 0u) {
        const A840FormatCaps *fc = a840_format_caps_lookup(backingFmt);
        const uint32_t bpp = (fc && fc->block_bits >= 8u) ? (fc->block_bits / 8u) : ((plane == 0u) ? 4u : 1u);
        sdesc.bytes_per_row = ((sdesc.width * bpp) + 63u) & ~63u;
    }
    sdesc.alloc_size = (uint64_t)iosAllocSize;
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
    const A840SysStatus bindSt = a840_sys_bind_iosurface(self.systemFrontend, &sdesc, &surfHandle, &surfInfo);
    if (bindSt != A840_SYS_OK) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840SURF bind st=%d f=%u %ux%u p=%u/%zu r=%u",
                 (int)bindSt, sdesc.mtl_pixel_format, sdesc.width, sdesc.height,
                 sdesc.plane_index, planeCount, sdesc.bytes_per_row);
        a840diag::reportText(msg);
        return nil;
    }

    A840RtTextureDesc rtdesc{};
    rtdesc.mtl_pixel_format = backingFmt;
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
    const size_t allocSize = iosAllocSize;
    const size_t pageBytes = A840_RT_PAGE_BYTES;
    const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(basePtr);
    uintptr_t planePtr = planeCount > 0u
        ? reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddressOfPlane(iosurface, plane))
        : baseAddr;
    if (hasB3a8Layout && plane < 2u && (planePtr < baseAddr || (planePtr - baseAddr) >= allocSize)) {
        planePtr = baseAddr + b3a8Layout[plane == 0u ? 0u : 2u];
    }
    const bool canImportZeroCopy =
        basePtr != nullptr &&
        (baseAddr & (pageBytes - 1u)) == 0u &&
        allocSize > 0u &&
        planePtr >= baseAddr &&
        (planePtr - baseAddr) < allocSize &&
        ((planePtr - baseAddr) % 64u) == 0u;

    @synchronized (self) {
        A840RtStatus importBoSt = A840_RT_UNSUPPORTED;
        A840RtStatus importTexSt = A840_RT_UNSUPPORTED;
        if (canImportZeroCopy) {
            const uint64_t alignedAlloc = ((uint64_t)allocSize + pageBytes - 1u) & ~(uint64_t)(pageBytes - 1u);
            A840RtBufferDesc bdesc{};
            bdesc.size_bytes = alignedAlloc;
            bdesc.alignment = pageBytes;
            bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE;
            bdesc.host_import = true;
            bdesc.host_ptr = basePtr;
            A840RtBuffer *surfBo = nullptr;
            importBoSt = a840_rt_buffer_create(self.runtime, &bdesc, &surfBo);
            if (importBoSt == A840_RT_OK && surfBo) {
                const uint64_t planeOff = (uint64_t)(planePtr - baseAddr);
                importTexSt = a840_rt_texture_create_on_buffer(self.runtime, surfBo, planeOff, &rtdesc, &rtTex);
                if (importTexSt != A840_RT_OK) {
                    rtTex = nullptr;
                }
                a840_rt_buffer_release(surfBo);
            }
        }
        if (!rtTex) {
            char nmsg[128] = {};
            snprintf(nmsg, sizeof(nmsg), "A840SURF nocopy c=%d bo=%d tx=%d f=%u %ux%u r=%u",
                     canImportZeroCopy ? 1 : 0, (int)importBoSt, (int)importTexSt,
                     rtdesc.mtl_pixel_format, rtdesc.width, rtdesc.height, rtdesc.pitch_bytes);
            a840diag::reportText(nmsg);
            const A840RtStatus texSt = a840_rt_texture_create(self.runtime, &rtdesc, &rtTex);
            if (texSt != A840_RT_OK) {
                char msg[128] = {};
                snprintf(msg, sizeof(msg), "A840SURF tex st=%d f=%u %ux%u r=%u",
                         (int)texSt, rtdesc.mtl_pixel_format, rtdesc.width, rtdesc.height, rtdesc.pitch_bytes);
                a840diag::reportText(msg);
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
    return [[A840MTLBinaryArchive alloc] initWithOwner:self descriptor:descriptor error:error];
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

- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLLibrary> lib = [self newLibraryWithSource:source options:options error:&err];
    completionHandler(lib, err);
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                             error:(NSError **)error
{
    return [A840MTLStitchedLibraryLoader libraryWithStitchedDescriptor:descriptor device:self error:error];
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                          destinationBinaryArchive:(id<MTLBinaryArchive>)archive
                                             error:(NSError **)error
{
    (void)archive;
    return [self newLibraryWithStitchedDescriptor:descriptor error:error];
}

- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                       completionHandler:(MTLNewLibraryCompletionHandler)completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLLibrary> lib = [self newLibraryWithStitchedDescriptor:descriptor error:&err];
    completionHandler(lib, err);
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
