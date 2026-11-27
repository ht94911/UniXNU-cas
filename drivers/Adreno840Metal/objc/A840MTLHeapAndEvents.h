/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_HEAP_AND_EVENTS_H
#define A840_MTL_HEAP_AND_EVENTS_H

#import "A840MTLResource.h"
#include "../core/a840_system_frontend_core.h"

@interface A840MTLHeap : NSObject <MTLHeap>
- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLHeapDescriptor *)descriptor;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840SysHeapHandle heapHandle;
@property(nonatomic, readonly) A840Heap *nativeHeap;
@end

@interface A840MTLEvent : NSObject <MTLEvent>
- (instancetype)initWithOwner:(A840MTLDevice *)owner;
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       shared:(BOOL)shared
                 initialValue:(uint64_t)initialValue
                     machPort:(uint32_t)machPort;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840SysEventHandle eventHandle;
@end

@interface A840MTLSharedEvent : A840MTLEvent <MTLSharedEvent>
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                 initialValue:(uint64_t)initialValue
                     machPort:(uint32_t)machPort;
- (instancetype)initWithOwner:(A840MTLDevice *)owner handle:(MTLSharedEventHandle *)handle;
@end

@interface A840MTLDepthStencilState : NSObject <MTLDepthStencilState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLDepthStencilDescriptor *)descriptor;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840SysDepthStencilHandle depthStencilHandle;
@property(nonatomic, readonly) const A840DepthStencilVariantDesc *nativeDesc;
@end

@interface A840MTLIndirectCommandBuffer : A840MTLAllocation <MTLIndirectCommandBuffer>
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                   descriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
              maxCommandCount:(NSUInteger)maxCount
                      options:(MTLResourceOptions)options;
@property(nonatomic, readonly) A840SysIcbHandle icbHandle;
@property(nonatomic, readonly) A840IndirectCmdBuffer *nativeIcb;
@end

#endif /* A840_MTL_HEAP_AND_EVENTS_H */
