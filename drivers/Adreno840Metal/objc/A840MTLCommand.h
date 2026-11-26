/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_COMMAND_H
#define A840_MTL_COMMAND_H

#import "A840MTLResource.h"

@class A840MTLCommandBuffer;
typedef BOOL (^A840MTLReplay)(id<MTLCommandBuffer> backend);

@interface A840MTLCommandQueue : NSObject <MTLCommandQueue>
- (instancetype)initWithOwner:(A840MTLDevice *)owner;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840RtQueue *native;
- (id<MTLCommandQueue>)fallbackQueue;
@end

@interface A840MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(A840MTLCommandQueue *)queue;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840RtCmdBuffer *native;
@property(nonatomic, readonly) BOOL wantsRaw;
- (BOOL)beginEncoder;
- (void)finishEncoderWithReplay:(A840MTLReplay)replay;
- (void)abandonEncoder;
- (void)useResource:(id<MTLResource>)resource;
- (BOOL)acceptStatus:(A840RtStatus)status fallback:(A840FallbackPredicate)fallback;
- (void)requireFallback:(A840FallbackPredicate)reason;
- (void)recordFailure:(A840RtStatus)status;
@end

#endif /* A840_MTL_COMMAND_H */
