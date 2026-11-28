/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_RESOURCE_H
#define A840_MTL_RESOURCE_H

#import <IOSurface/IOSurfaceRef.h>
#import "A840MTLDevice.h"
#include "../core/a840_heap_allocator.h"
#include "../core/a840_system_frontend_core.h"

@class A840MTLHeap;

/* Pure native Adreno 840 resource wrappers (Wave 12 zero-fallback system frontend). */
@interface A840MTLAllocation : NSObject <MTLResource>
- (instancetype)initWithOwner:(A840MTLDevice *)owner options:(MTLResourceOptions)options;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                      options:(MTLResourceOptions)options;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, strong, readonly) A840MTLHeap *owningHeap;
@property(nonatomic, readonly) A840HeapSubAlloc *heapSubAlloc;
- (void)retainInFlightUse;
- (void)releaseInFlightUse;
@end

@interface A840MTLBuffer : A840MTLAllocation <MTLBuffer>
- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options
                noCopyPointer:(void *)noCopyPointer
                  deallocator:(void (^)(void *, NSUInteger))deallocator;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                      options:(MTLResourceOptions)options;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                       native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options;
@property(nonatomic, readonly) A840RtBuffer *native;
@property(nonatomic, readonly) void *noCopyPointer;
@end

@interface A840MTLTexture : A840MTLAllocation <MTLTexture>
- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
                    iosurface:(IOSurfaceRef)iosurface
                        plane:(NSUInteger)plane
                surfaceHandle:(A840SysSurfaceHandle)surfaceHandle;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
               memorylessUbwc:(A840UbwcTexture *)memoryless
                   descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
               memorylessUbwc:(A840UbwcTexture *)memoryless
                   descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                   descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                         heap:(A840MTLHeap *)heap
                     subAlloc:(A840HeapSubAlloc *)subAlloc
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor
                       buffer:(A840MTLBuffer *)buffer
                 bufferOffset:(NSUInteger)bufferOffset
            bufferBytesPerRow:(NSUInteger)bufferBytesPerRow;
@property(nonatomic, readonly) A840RtTexture *native;
@property(nonatomic, readonly) A840UbwcTexture *memorylessNative;
@property(nonatomic, readonly) A840SysSurfaceHandle surfaceHandle;
@property(nonatomic, readonly) IOSurfaceRef iosurface;
@property(nonatomic, readonly) NSUInteger iosurfacePlane;
@property(nonatomic, readonly) BOOL isB3a8Plane0;
- (void)configureB3a8Plane0Offset:(size_t)plane0Offset
                        plane0Row:(size_t)plane0Row
                     plane1Offset:(size_t)plane1Offset
                        plane1Row:(size_t)plane1Row;
- (void)syncB3a8CpuContentIfNeeded;
- (void)markB3a8GpuWriteTarget;
@end

@interface A840MTLSamplerState : NSObject <MTLSamplerState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLSamplerDescriptor *)descriptor;
@property(nonatomic, readonly) const uint32_t *descriptorWords;
@property(nonatomic, readonly) MTLSamplerDescriptor *descriptor;
@end

@interface A840MTLFence : NSObject <MTLFence>
- (instancetype)initWithOwner:(A840MTLDevice *)owner;
@end

@interface A840MTLComputePipelineState : NSObject <MTLComputePipelineState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner function:(id<MTLFunction>)function
                   descriptor:(const A840RtComputePipelineDesc *)descriptor plan:(id)plan;
@property(nonatomic, readonly) const A840RtComputePipelineDesc *nativeDescriptor;
@property(nonatomic, strong, readonly) id plan;
@end

@interface A840MTLRenderPipelineState : NSObject <MTLRenderPipelineState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner configuration:(MTLRenderPipelineDescriptor *)configuration
                   descriptor:(const A840RtGraphicsProgramDesc *)descriptor plan:(id)plan;
@property(nonatomic, readonly) const A840RtGraphicsProgramDesc *nativeDescriptor;
@property(nonatomic, strong, readonly) MTLRenderPipelineDescriptor *configuration;
@property(nonatomic, strong, readonly) id plan;
@end

#endif /* A840_MTL_RESOURCE_H */
