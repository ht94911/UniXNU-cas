/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_RESOURCE_H
#define A840_MTL_RESOURCE_H

#import "A840MTLDevice.h"

/* The submission worker copies CPU-visible native resources into the selected
 * Metal device's resources before replay and reconciles them after the fence. */
@protocol A840MTLMirror <NSObject>
- (BOOL)uploadToFallback;
- (void)downloadFromFallback;
- (id<MTLResource>)fallbackResource;
@end

@interface A840MTLAllocation : NSObject <MTLResource, A840MTLMirror>
- (instancetype)initWithOwner:(A840MTLDevice *)owner options:(MTLResourceOptions)options;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@end

@interface A840MTLBuffer : A840MTLAllocation <MTLBuffer>
- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtBuffer *)buffer
                      options:(MTLResourceOptions)options;
@property(nonatomic, readonly) A840RtBuffer *native;
@end

@interface A840MTLTexture : A840MTLAllocation <MTLTexture>
- (instancetype)initWithOwner:(A840MTLDevice *)owner native:(A840RtTexture *)texture
                   descriptor:(MTLTextureDescriptor *)descriptor;
@property(nonatomic, readonly) A840RtTexture *native;
@end

@interface A840MTLSamplerState : NSObject <MTLSamplerState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLSamplerDescriptor *)descriptor;
@property(nonatomic, readonly) const uint32_t *descriptorWords;
- (id<MTLSamplerState>)fallbackSampler;
@end

@interface A840MTLFence : NSObject <MTLFence>
- (instancetype)initWithOwner:(A840MTLDevice *)owner;
- (id<MTLFence>)fallbackFence;
@end

@interface A840MTLComputePipelineState : NSObject <MTLComputePipelineState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner function:(id<MTLFunction>)function
                   descriptor:(const A840RtComputePipelineDesc *)descriptor plan:(id)plan;
@property(nonatomic, readonly) const A840RtComputePipelineDesc *nativeDescriptor;
@property(nonatomic, strong, readonly) id plan;
- (id<MTLComputePipelineState>)fallbackPipeline:(NSError **)error;
@end

@interface A840MTLRenderPipelineState : NSObject <MTLRenderPipelineState>
- (instancetype)initWithOwner:(A840MTLDevice *)owner configuration:(MTLRenderPipelineDescriptor *)configuration
                   descriptor:(const A840RtGraphicsProgramDesc *)descriptor plan:(id)plan;
@property(nonatomic, readonly) const A840RtGraphicsProgramDesc *nativeDescriptor;
@property(nonatomic, strong, readonly) MTLRenderPipelineDescriptor *configuration;
@property(nonatomic, strong, readonly) id plan;
- (id<MTLRenderPipelineState>)fallbackPipeline:(NSError **)error;
@end

#endif /* A840_MTL_RESOURCE_H */
