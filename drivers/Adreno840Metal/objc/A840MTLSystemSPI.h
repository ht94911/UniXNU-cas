/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_SYSTEM_SPI_H
#define A840_MTL_SYSTEM_SPI_H

#import <IOSurface/IOSurfaceRef.h>
#import "A840MTLDevice.h"

/* iOS 27 CARenderServer / QuartzCore / SpringBoard / Camera MTLDevice + MTLDeviceSPI
 * interface category on A840MTLDevice. Pure native Adreno 840 implementation backed by
 * a840_system_frontend_core and Adreno840CapsContract.md. */
@interface A840MTLDevice (A840MTLSystemSPI)

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily;
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet;
- (BOOL)supportsTextureSampleCount:(NSUInteger)sampleCount;
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count;

@property(nonatomic, readonly) BOOL hasUnifiedMemory;
@property(nonatomic, readonly, getter=isLowPower) BOOL lowPower;
@property(nonatomic, readonly, getter=isHeadless) BOOL headless;
@property(nonatomic, readonly, getter=isRemovable) BOOL removable;
@property(nonatomic, readonly, getter=isDepth24Stencil8PixelFormatSupported) BOOL depth24Stencil8PixelFormatSupported;
@property(nonatomic, readonly) BOOL supports32BitFloatFiltering;
@property(nonatomic, readonly) BOOL supports32BitMSAA;
@property(nonatomic, readonly) BOOL supportsQueryTextureLOD;
@property(nonatomic, readonly) BOOL supportsBCTextureCompression;
@property(nonatomic, readonly) BOOL supportsPullModelInterpolation;
@property(nonatomic, readonly) BOOL supportsDynamicLibraries;
@property(nonatomic, readonly) BOOL supportsRenderDynamicLibraries;
@property(nonatomic, readonly) BOOL supportsRaytracing;
@property(nonatomic, readonly) BOOL supportsPrimitiveMotionBlur;
@property(nonatomic, readonly) BOOL supportsFunctionPointers;
@property(nonatomic, readonly) BOOL supportsShaderBarycentricCoordinates;
@property(nonatomic, readonly) BOOL areProgrammableSamplePositionsSupported;
@property(nonatomic, readonly) BOOL areRasterOrderGroupsSupported;
@property(nonatomic, readonly) BOOL areBarycentricCoordsSupported;

@property(nonatomic, readonly) MTLArgumentBuffersTier argumentBuffersSupport;
@property(nonatomic, readonly) MTLReadWriteTextureTier readWriteTextureSupport;
@property(nonatomic, readonly) uint64_t recommendedMaxWorkingSetSize;
@property(nonatomic, readonly) NSUInteger currentAllocatedSize;
@property(nonatomic, readonly) NSUInteger maxThreadgroupMemoryLength;
@property(nonatomic, readonly) NSUInteger maxArgumentBufferSamplerCount;
@property(nonatomic, readonly) NSUInteger sparseTileSizeInBytes;
@property(nonatomic, readonly) NSUInteger iosurfaceReadOnlyTextureAlignmentBytes;
@property(nonatomic, readonly) NSUInteger iosurfaceTextureAlignmentBytes;
@property(nonatomic, readonly) uint64_t dedicatedMemorySize;
@property(nonatomic, readonly) uint64_t sharedMemorySize;
@property(nonatomic, readonly) NSUInteger maxColorAttachments;
@property(nonatomic, readonly) NSUInteger maxVertexAttributes;

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format;
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format;

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                 iosurface:(IOSurfaceRef)iosurface
                                     plane:(NSUInteger)plane;

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor;
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount;
- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(MTLCommandQueueDescriptor *)descriptor;

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor;
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)desc;

- (id<MTLEvent>)newEvent;
- (id<MTLSharedEvent>)newSharedEvent;
- (id<MTLSharedEvent>)newSharedEventWithOptions:(NSUInteger)options;
- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)sharedEventHandle;

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCount
                                                               options:(MTLResourceOptions)options;

- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor
                                                 error:(NSError **)error;

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)filepath error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                               options:(MTLCompileOptions *)options
                                 error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                             error:(NSError **)error;
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error;

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *pointer, NSUInteger length))deallocator;

@end

#endif /* A840_MTL_SYSTEM_SPI_H */
