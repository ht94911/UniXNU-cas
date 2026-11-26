/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_ENCODERS_H
#define A840_MTL_ENCODERS_H

#import "A840MTLCommand.h"

/* Per-stage Metal slots exposed to the pipeline's binding plan. Inline byte
 * arguments are owned copies. A buffer and inline bytes never share a slot. */
@interface A840MTLBindings : NSObject
@property(nonatomic, readonly) NSDictionary<NSNumber *, id<MTLBuffer>> *buffers;
@property(nonatomic, readonly) NSDictionary<NSNumber *, NSNumber *> *bufferOffsets;
@property(nonatomic, readonly) NSDictionary<NSNumber *, id<MTLTexture>> *textures;
@property(nonatomic, readonly) NSDictionary<NSNumber *, id<MTLSamplerState>> *samplers;
@property(nonatomic, readonly) NSDictionary<NSNumber *, NSData *> *inlineBytes;
@end

@interface A840MTLEncoder : NSObject <MTLCommandEncoder>
@end

@interface A840MTLBlitEncoder : A840MTLEncoder <MTLBlitCommandEncoder>
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command;
@end

@interface A840MTLComputeEncoder : A840MTLEncoder <MTLComputeCommandEncoder>
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command dispatchType:(MTLDispatchType)dispatchType;
@end

@interface A840MTLRenderEncoder : A840MTLEncoder <MTLRenderCommandEncoder>
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command pass:(MTLRenderPassDescriptor *)pass;
@end

#endif /* A840_MTL_ENCODERS_H */
