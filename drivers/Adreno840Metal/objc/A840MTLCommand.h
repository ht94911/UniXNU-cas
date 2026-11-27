/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_COMMAND_H
#define A840_MTL_COMMAND_H

#import "A840MTLResource.h"

@class A840MTLCommandBuffer, A840MTLHeap;

@interface A840MTLCommandQueue : NSObject <MTLCommandQueue>
- (instancetype)initWithOwner:(A840MTLDevice *)owner;
- (instancetype)initWithOwner:(A840MTLDevice *)owner maxCommandBufferCount:(NSUInteger)maxCount;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840RtQueue *native;
@property(nonatomic, readonly) NSUInteger maxCommandBufferCount;
@end

@interface A840MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(A840MTLCommandQueue *)queue;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840RtCmdBuffer *native;
@property(nonatomic, readonly) BOOL wantsRaw;
- (BOOL)beginEncoder;
- (void)finishEncoder;
- (void)abandonEncoder;
- (void)useResource:(id<MTLResource>)resource;
- (void)useHeap:(id<MTLHeap>)heap;
- (void)recordFailure:(A840RtStatus)status;
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)notePhase3RenderPass:(BOOL)gmem
             memorylessDepth:(BOOL)memorylessDepth
                fbfetchDraws:(uint32_t)fbfetchDraws
                       tileW:(uint32_t)tileW
                       tileH:(uint32_t)tileH
                      tilesX:(uint32_t)tilesX
                      tilesY:(uint32_t)tilesY;
- (void)notePhase3UbwcExecuted:(BOOL)wroteBlocks;
- (void)notePhase3ScanoutMode:(unsigned)mode;
@end

#endif /* A840_MTL_COMMAND_H */
