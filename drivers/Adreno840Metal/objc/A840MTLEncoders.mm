/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Phase 6 zero-fallback production encoders: pure native Adreno 840 emission,
 * probe006 negative-height viewport Y-flip + dynamic compute localSize variants,
 * zero DualPathRouter/fallback replay branches, fail-closed on unsupported ops. */
#import "A840MTLEncoders.h"
#include "../core/a840_production_router.h"
#include "gen/a840_bitfields.h"

#include <cmath>

@interface A840MTLBindings ()
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index;
- (void)setTexture:(id<MTLTexture>)texture index:(NSUInteger)index;
- (void)setSampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index;
- (void)setData:(NSData *)data index:(NSUInteger)index;
@end

@implementation A840MTLBindings {
    NSMutableDictionary<NSNumber *, id<MTLBuffer>> *_bufferEntries;
    NSMutableDictionary<NSNumber *, NSNumber *> *_offsetEntries;
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *_textureEntries;
    NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *_samplerEntries;
    NSMutableDictionary<NSNumber *, NSData *> *_byteEntries;
}
- (instancetype)init
{
    if ((self = [super init])) {
        _bufferEntries = [NSMutableDictionary new];
        _offsetEntries = [NSMutableDictionary new];
        _textureEntries = [NSMutableDictionary new];
        _samplerEntries = [NSMutableDictionary new];
        _byteEntries = [NSMutableDictionary new];
    }
    return self;
}
- (NSDictionary<NSNumber *, id<MTLBuffer>> *)buffers { return [_bufferEntries copy]; }
- (NSDictionary<NSNumber *, NSNumber *> *)bufferOffsets { return [_offsetEntries copy]; }
- (NSDictionary<NSNumber *, id<MTLTexture>> *)textures { return [_textureEntries copy]; }
- (NSDictionary<NSNumber *, id<MTLSamplerState>> *)samplers { return [_samplerEntries copy]; }
- (NSDictionary<NSNumber *, NSData *> *)inlineBytes { return [_byteEntries copy]; }
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index
{
    NSNumber *key = @(index);
    _bufferEntries[key] = buffer;
    _offsetEntries[key] = buffer ? @(offset) : nil;
    [_byteEntries removeObjectForKey:key];
}
- (void)setTexture:(id<MTLTexture>)texture index:(NSUInteger)index { _textureEntries[@(index)] = texture; }
- (void)setSampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index { _samplerEntries[@(index)] = sampler; }
- (void)setData:(NSData *)data index:(NSUInteger)index
{
    NSNumber *key = @(index);
    [_bufferEntries removeObjectForKey:key];
    [_offsetEntries removeObjectForKey:key];
    _byteEntries[key] = data;
}
@end

static A840RtBuffer *nativeBuffer(id<MTLBuffer> buffer, A840MTLDevice *owner)
{
    if (![(id)buffer isKindOfClass:A840MTLBuffer.class]) return nullptr;
    A840MTLBuffer *native = (A840MTLBuffer *)buffer;
    return native.owner == owner ? native.native : nullptr;
}
static A840RtTexture *nativeTexture(id<MTLTexture> texture, A840MTLDevice *owner)
{
    if (![(id)texture isKindOfClass:A840MTLTexture.class]) return nullptr;
    A840MTLTexture *native = (A840MTLTexture *)texture;
    return native.owner == owner ? native.native : nullptr;
}

@interface A840MTLEncoder ()
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command;
@property(nonatomic, strong, readonly) A840MTLCommandBuffer *command;
- (BOOL)checkActive;
- (void)endNative;
@end

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@implementation A840MTLEncoder {
    BOOL _ended;
}
@synthesize label = _label;

- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command
{
    if (!(self = [super init]) || ![command beginEncoder]) return nil;
    _command = command;
    return self;
}
- (void)dealloc { if (!_ended) [_command abandonEncoder]; }
- (id<MTLDevice>)device { return _command.owner; }
- (BOOL)checkActive
{
    if (_ended) {
        [_command recordFailure:A840_RT_STATE_ERROR];
        return NO;
    }
    return YES;
}
- (void)endNative { }
- (void)endEncoding
{
    if (_ended) return;
    [self endNative];
    _ended = YES;
    [_command finishEncoderWithReplay:nil];
}
- (void)insertDebugSignpost:(NSString *)string { (void)string; (void)[self checkActive]; }
- (void)pushDebugGroup:(NSString *)string { (void)string; (void)[self checkActive]; }
- (void)popDebugGroup { (void)[self checkActive]; }
@end

@implementation A840MTLBlitEncoder {
    A840RtBlitEncoder *_native;
}
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command
{
    if (!(self = [super initWithCommand:command])) return nil;
    @synchronized (command.owner) {
        if (command.wantsRaw) {
            const A840RtStatus st = a840_rt_cb_begin_blit(command.native, &_native);
            if (st != A840_RT_OK) [command recordFailure:st];
        }
    }
    return self;
}
- (void)dealloc
{
    @synchronized (self.command.owner) { a840_rt_blit_destroy(_native); }
}
- (void)endNative
{
    @synchronized (self.command.owner) {
        if (_native) {
            if (self.command.wantsRaw) {
                const A840RtStatus st = a840_rt_blit_end(_native);
                if (st != A840_RT_OK) [self.command recordFailure:st];
            }
            a840_rt_blit_destroy(_native);
            _native = nullptr;
        }
    }
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)sourceOffset
              toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:src];
    [cb useResource:dst];
    if (!cb.wantsRaw || !size) return;
    @synchronized (cb.owner) {
        auto *s = nativeBuffer(src, cb.owner), *d = nativeBuffer(dst, cb.owner);
        if (!s || !d || size > UINT32_MAX || (sourceOffset | destinationOffset | size) % 4) {
            [cb recordFailure:A840_RT_UNSUPPORTED];
            return;
        }
        const A840RtStatus st = a840_rt_blit_copy_buffer(_native, s, sourceOffset, d, destinationOffset, (uint32_t)size);
        if (st != A840_RT_OK) [cb recordFailure:st];
    }
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dsto
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:src];
    [cb useResource:dst];
    if (!cb.wantsRaw || !size.width || !size.height) return;
    @synchronized (cb.owner) {
        auto *s = nativeTexture(src, cb.owner), *d = nativeTexture(dst, cb.owner);
        if (ss || sl || ds || dl || so.z || dsto.z || size.depth != 1 ||
            !s || !d || so.x > UINT32_MAX || so.y > UINT32_MAX || dsto.x > UINT32_MAX ||
            dsto.y > UINT32_MAX || size.width > UINT32_MAX || size.height > UINT32_MAX) {
            [cb recordFailure:A840_RT_UNSUPPORTED];
            return;
        }
        const A840Rect2D source{(uint32_t)so.x, (uint32_t)so.y, (uint32_t)size.width, (uint32_t)size.height};
        const A840Rect2D dest{(uint32_t)dsto.x, (uint32_t)dsto.y, (uint32_t)size.width, (uint32_t)size.height};
        const A840RtStatus st = a840_rt_blit_2d(_native, s, &source, d, &dest, A840_FILTER_NEAREST);
        if (st != A840_RT_OK) [cb recordFailure:st];
    }
}
@end

@implementation A840MTLComputeEncoder {
    A840RtComputeEncoder *_native;
    A840MTLBindings *_bindings;
    id<MTLComputePipelineState> _pipeline, _boundPipeline;
    id _boundVariantPlan;
}

- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command dispatchType:(MTLDispatchType)dispatchType
{
    if (!(self = [super initWithCommand:command])) return nil;
    _bindings = [A840MTLBindings new];
    if (dispatchType != MTLDispatchTypeSerial) {
        [command recordFailure:A840_RT_UNSUPPORTED];
        return self;
    }
    @synchronized (command.owner) {
        if (command.wantsRaw) {
            const A840RtStatus st = a840_rt_cb_begin_compute(command.native, &_native);
            if (st != A840_RT_OK) [command recordFailure:st];
        }
    }
    return self;
}
- (void)dealloc
{
    @synchronized (self.command.owner) { a840_rt_compute_destroy(_native); }
}
- (void)endNative
{
    @synchronized (self.command.owner) {
        if (_native) {
            if (self.command.wantsRaw) {
                const A840RtStatus st = a840_rt_compute_end(_native);
                if (st != A840_RT_OK) [self.command recordFailure:st];
            }
            a840_rt_compute_destroy(_native);
            _native = nullptr;
        }
    }
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline
{
    if (![self checkActive]) return;
    if ([(id)pipeline isKindOfClass:A840MTLComputePipelineState.class] && pipeline.device != self.device)
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
    _pipeline = pipeline;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    [_bindings setBuffer:buffer offset:offset index:index];
    [self.command useResource:buffer];
    if (buffer && !nativeBuffer(buffer, self.command.owner))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    id<MTLBuffer> buffer = _bindings.buffers[@(index)];
    if (buffer) [_bindings setBuffer:buffer offset:offset index:index];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    [_bindings setTexture:texture index:index];
    [self.command useResource:texture];
    if (texture && !nativeTexture(texture, self.command.owner))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    [_bindings setSampler:sampler index:index];
    if (sampler && (![(id)sampler isKindOfClass:A840MTLSamplerState.class] || sampler.device != self.device))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (!bytes && length) { [self.command recordFailure:A840_RT_INVALID_ARG]; return; }
    if (![self checkActive]) return;
    NSData *data = [NSData dataWithBytes:bytes length:length];
    [_bindings setData:data index:index];
}

- (void)dispatch:(MTLSize)grid local:(MTLSize)local nonuniform:(BOOL)nonuniform
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw) return;
    A840MTLComputePipelineState *state = [(id)_pipeline isKindOfClass:A840MTLComputePipelineState.class] ?
        (A840MTLComputePipelineState *)_pipeline : nil;
    const A840RtComputePipelineDesc *desc = state.nativeDescriptor;
    A840RtComputePipelineDesc variantDesc{};
    id activePlan = state.plan;
    if (desc && state.device == cb.owner &&
        (desc->stage->local_size[0] != local.width ||
         desc->stage->local_size[1] != local.height ||
         desc->stage->local_size[2] != local.depth) &&
        [cb.owner.pipelineProvider respondsToSelector:@selector(prepareComputeVariantForPlan:localSize:forDevice:descriptor:)]) {
        @synchronized (cb.owner) {
            id variantPlan = [cb.owner.pipelineProvider prepareComputeVariantForPlan:state.plan
                                                                           localSize:local
                                                                           forDevice:cb.owner
                                                                          descriptor:&variantDesc];
            if (variantPlan && variantDesc.stage) {
                activePlan = variantPlan;
                desc = &variantDesc;
            }
        }
    }
    if (!desc || state.device != cb.owner || desc->stage->local_size[0] != local.width ||
        desc->stage->local_size[1] != local.height || desc->stage->local_size[2] != local.depth ||
        grid.width > UINT32_MAX || grid.height > UINT32_MAX || grid.depth > UINT32_MAX) {
        [cb recordFailure:A840_RT_UNSUPPORTED];
        return;
    }
    @synchronized (cb.owner) {
        if (_boundPipeline != state || _boundVariantPlan != activePlan) {
            const A840RtStatus st = a840_rt_compute_set_pipeline(_native, desc);
            if (st != A840_RT_OK) { [cb recordFailure:st]; return; }
            _boundPipeline = state;
            _boundVariantPlan = activePlan;
        }
        A840RtStatus bindStatus = A840_RT_OK;
        if ([cb.owner.pipelineProvider respondsToSelector:@selector(bindCompute:plan:bindings:gridSize:threadgroupSize:nonuniform:)]) {
            bindStatus = [cb.owner.pipelineProvider bindCompute:_native plan:activePlan bindings:_bindings
                                                       gridSize:grid threadgroupSize:local nonuniform:nonuniform];
        } else {
            bindStatus = [cb.owner.pipelineProvider bindCompute:_native plan:activePlan bindings:_bindings];
        }
        if (bindStatus != A840_RT_OK) { [cb recordFailure:bindStatus]; return; }
        const uint32_t x = (uint32_t)grid.width, y = (uint32_t)grid.height, z = (uint32_t)grid.depth;
        const auto status = nonuniform ? a840_rt_compute_dispatch_threads(_native, x, y, z) :
            a840_rt_compute_dispatch_threadgroups(_native, x, y, z);
        if (status != A840_RT_OK) [cb recordFailure:status];
    }
}
- (void)dispatchThreadgroups:(MTLSize)grid threadsPerThreadgroup:(MTLSize)local
{
    [self dispatch:grid local:local nonuniform:NO];
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)local
{
    [self dispatch:grid local:local nonuniform:YES];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset
                         threadsPerThreadgroup:(MTLSize)local
{
    (void)offset;
    (void)local;
    if (![self checkActive]) return;
    [self.command useResource:buffer];
    [self.command recordFailure:A840_RT_UNSUPPORTED];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    (void)length;
    (void)index;
    if (![self checkActive]) return;
    [self.command recordFailure:A840_RT_UNSUPPORTED];
}
- (void)fence:(id<MTLFence>)fence update:(BOOL)update
{
    (void)update;
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw) return;
    if (![(id)fence isKindOfClass:A840MTLFence.class] || fence.device != cb.owner) {
        [cb recordFailure:A840_RT_WRONG_DEVICE];
        return;
    }
    @synchronized (cb.owner) {
        const A840RtStatus st = a840_rt_compute_barrier(_native,
            A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true);
        if (st != A840_RT_OK) [cb recordFailure:st];
    }
}
- (void)updateFence:(id<MTLFence>)fence { [self fence:fence update:YES]; }
- (void)waitForFence:(id<MTLFence>)fence { [self fence:fence update:NO]; }
@end

static BOOL attachmentShape(MTLRenderPassAttachmentDescriptor *attachment)
{
    return attachment.level == 0 && attachment.slice == 0 && attachment.depthPlane == 0;
}

static uint32_t primitiveType(MTLPrimitiveType primitive)
{
    switch (primitive) {
    case MTLPrimitiveTypePoint: return A840_PRIM_POINTS;
    case MTLPrimitiveTypeLine: return A840_PRIM_LINES;
    case MTLPrimitiveTypeLineStrip: return A840_PRIM_LINE_STRIP;
    case MTLPrimitiveTypeTriangle: return A840_PRIM_TRIANGLES;
    case MTLPrimitiveTypeTriangleStrip: return A840_PRIM_TRIANGLE_STRIP;
    default: return UINT32_MAX;
    }
}

static uint8_t blendFactor(MTLBlendFactor factor)
{
    switch (factor) {
    case MTLBlendFactorZero: return A840_FACTOR_ZERO;
    case MTLBlendFactorOne: return A840_FACTOR_ONE;
    case MTLBlendFactorSourceColor: return A840_FACTOR_SRC_COLOR;
    case MTLBlendFactorOneMinusSourceColor: return A840_FACTOR_ONE_MINUS_SRC_COLOR;
    case MTLBlendFactorSourceAlpha: return A840_FACTOR_SRC_ALPHA;
    case MTLBlendFactorOneMinusSourceAlpha: return A840_FACTOR_ONE_MINUS_SRC_ALPHA;
    case MTLBlendFactorDestinationColor: return A840_FACTOR_DST_COLOR;
    case MTLBlendFactorOneMinusDestinationColor: return A840_FACTOR_ONE_MINUS_DST_COLOR;
    case MTLBlendFactorDestinationAlpha: return A840_FACTOR_DST_ALPHA;
    case MTLBlendFactorOneMinusDestinationAlpha: return A840_FACTOR_ONE_MINUS_DST_ALPHA;
    case MTLBlendFactorSourceAlphaSaturated: return A840_FACTOR_SRC_ALPHA_SATURATE;
    case MTLBlendFactorBlendColor: return A840_FACTOR_CONSTANT_COLOR;
    case MTLBlendFactorOneMinusBlendColor: return A840_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case MTLBlendFactorBlendAlpha: return A840_FACTOR_CONSTANT_ALPHA;
    case MTLBlendFactorOneMinusBlendAlpha: return A840_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return UINT8_MAX;
    }
}

static uint8_t blendOperation(MTLBlendOperation operation)
{
    switch (operation) {
    case MTLBlendOperationAdd: return A840_BLEND_ADD;
    case MTLBlendOperationSubtract: return A840_BLEND_SUB;
    case MTLBlendOperationReverseSubtract: return A840_BLEND_REVERSE_SUB;
    case MTLBlendOperationMin: return A840_BLEND_MIN;
    case MTLBlendOperationMax: return A840_BLEND_MAX;
    default: return UINT8_MAX;
    }
}

static BOOL pipelineBlend(MTLRenderPipelineDescriptor *configuration, A840RtRenderStateDesc *out)
{
    for (unsigned i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        auto *attachment = configuration.colorAttachments[i];
        auto &blend = out->color_blends[i];
        blend = {};
        if (attachment.pixelFormat == MTLPixelFormatInvalid) continue;
        const MTLColorWriteMask mask = attachment.writeMask;
        blend.write_mask = (mask & MTLColorWriteMaskRed ? 1u : 0u) |
            (mask & MTLColorWriteMaskGreen ? 2u : 0u) |
            (mask & MTLColorWriteMaskBlue ? 4u : 0u) |
            (mask & MTLColorWriteMaskAlpha ? 8u : 0u);
        blend.enabled = attachment.isBlendingEnabled;
        if (!blend.enabled) continue;
        blend.src_rgb = blendFactor(attachment.sourceRGBBlendFactor);
        blend.dst_rgb = blendFactor(attachment.destinationRGBBlendFactor);
        blend.src_alpha = blendFactor(attachment.sourceAlphaBlendFactor);
        blend.dst_alpha = blendFactor(attachment.destinationAlphaBlendFactor);
        blend.op_rgb = blendOperation(attachment.rgbBlendOperation);
        blend.op_alpha = blendOperation(attachment.alphaBlendOperation);
        if (blend.src_rgb == UINT8_MAX || blend.dst_rgb == UINT8_MAX ||
            blend.src_alpha == UINT8_MAX || blend.dst_alpha == UINT8_MAX ||
            blend.op_rgb == UINT8_MAX || blend.op_alpha == UINT8_MAX) return NO;
    }
    return YES;
}

static BOOL convertClear(id<MTLTexture> texture, MTLClearColor color, uint32_t output[4])
{
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm && texture.pixelFormat != MTLPixelFormatRGBA8Unorm)
        return NO;
    const double components[4] = {color.red, color.green, color.blue, color.alpha};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(components[i])) return NO;
        output[i] = (uint32_t)std::lround(std::fmax(0.0, std::fmin(1.0, components[i])) * 255.0);
    }
    return YES;
}

@implementation A840MTLRenderEncoder {
    A840RtRenderEncoder *_native;
    id<MTLRenderPipelineState> _pipeline, _boundPipeline;
    A840MTLBindings *_vertexBindings, *_fragmentBindings;
    A840RtRenderStateDesc _state;
    BOOL _stateDirty;
}

- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command pass:(MTLRenderPassDescriptor *)pass
{
    MTLRenderPassDescriptor *original = [pass copy];
    if (!(self = [super initWithCommand:command])) return nil;
    _vertexBindings = [A840MTLBindings new];
    _fragmentBindings = [A840MTLBindings new];
    _stateDirty = YES;
    _state.viewport_count = _state.scissor_count = 1;
    _state.line_width = 1;
    _state.sample_mask = UINT16_MAX;

    A840RtRenderPassDesc descriptor{};
    descriptor.sample_count = 1;
    BOOL nativeAttachments = YES;
    id<MTLTexture> first = nil;
    for (unsigned i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        MTLRenderPassColorAttachmentDescriptor *attachment = original.colorAttachments[i];
        id<MTLTexture> texture = attachment.texture;
        [command useResource:texture];
        [command useResource:attachment.resolveTexture];
        if (attachment.resolveTexture) descriptor.has_msaa_resolve = true;
        if (!texture) continue;
        if (!first) first = texture;
        descriptor.color_count = i + 1;
        descriptor.color_targets[i] = nativeTexture(texture, command.owner);
        descriptor.has_memoryless_attachment |= texture.storageMode == MTLStorageModeMemoryless;
        if (texture.sampleCount > descriptor.sample_count) descriptor.sample_count = (uint32_t)texture.sampleCount;
        if (!descriptor.color_targets[i] || !attachmentShape(attachment)) nativeAttachments = NO;
        if (attachment.loadAction == MTLLoadActionClear) {
            descriptor.color_clear[i] = true;
            if (!convertClear(texture, attachment.clearColor, descriptor.color_clear_values[i]))
                nativeAttachments = NO;
        }
    }

    MTLRenderPassAttachmentDescriptor *depth = original.depthAttachment, *stencil = original.stencilAttachment;
    for (MTLRenderPassAttachmentDescriptor *attachment in @[depth, stencil]) {
        [command useResource:attachment.texture];
        [command useResource:attachment.resolveTexture];
        if (attachment.resolveTexture) descriptor.has_msaa_resolve = true;
        if (!attachment.texture) continue;
        if (!first) first = attachment.texture;
        descriptor.has_memoryless_attachment |= attachment.texture.storageMode == MTLStorageModeMemoryless;
        if (attachment.texture.sampleCount > descriptor.sample_count)
            descriptor.sample_count = (uint32_t)attachment.texture.sampleCount;
        if (attachment.loadAction == MTLLoadActionClear || !attachmentShape(attachment))
            nativeAttachments = NO;
    }
    descriptor.depth_target = nativeTexture(depth.texture, command.owner);
    descriptor.stencil_target = nativeTexture(stencil.texture, command.owner);
    if ((depth.texture && !descriptor.depth_target) || (stencil.texture && !descriptor.stencil_target))
        nativeAttachments = NO;
    if (first) {
        /* metal2vulkan leaves clip-space +Y up and expects negative viewport height (stage_io/mod.rs:104). */
        _state.viewports[0] = {0.0f, (float)first.height, (float)first.width, -(float)first.height, 0.0f, 1.0f};
        _state.scissors[0] = {0, 0, (uint32_t)first.width, (uint32_t)first.height};
    }
    @synchronized (command.owner) {
        if (!command.wantsRaw) return self;
        A840ProdRenderQuery prodQuery{};
        prodQuery.color_attachment_count = descriptor.color_count;
        prodQuery.sample_count = descriptor.sample_count;
        prodQuery.has_memoryless_attachment = descriptor.has_memoryless_attachment;
        prodQuery.has_msaa_resolve = descriptor.has_msaa_resolve;
        const A840ProdDecision route = a840_prod_route_render(command.owner.productionRouter, &prodQuery);
        if (route.status != A840_PROD_OK || route.target != A840_PROD_ROUTE_NATIVE ||
            !nativeAttachments || !first || first.width > UINT32_MAX || first.height > UINT32_MAX) {
            [command recordFailure:A840_RT_UNSUPPORTED];
        } else {
            const A840RtStatus st = a840_rt_cb_begin_render_pass(command.native, &descriptor, &_native);
            if (st != A840_RT_OK) [command recordFailure:st == A840_RT_FALLBACK_REQUIRED ? A840_RT_UNSUPPORTED : st];
        }
    }
    return self;
}
- (void)dealloc
{
    @synchronized (self.command.owner) { a840_rt_render_destroy(_native); }
}
- (void)endNative
{
    @synchronized (self.command.owner) {
        if (_native) {
            if (self.command.wantsRaw) {
                const A840RtStatus st = a840_rt_render_end_pass(_native);
                if (st != A840_RT_OK) [self.command recordFailure:st];
            }
            a840_rt_render_destroy(_native);
            _native = nullptr;
        }
    }
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline
{
    if (![self checkActive]) return;
    if ([(id)pipeline isKindOfClass:A840MTLRenderPipelineState.class] && pipeline.device != self.device)
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
    _pipeline = pipeline;
    _stateDirty = YES;
}
- (void)setViewport:(MTLViewport)viewport
{
    if (![self checkActive]) return;
    _state.viewport_count = 1;
    /* Flip Metal NDC (+Y up) into upper-left framebuffer coordinates via VK_KHR_maintenance1 negative height. */
    _state.viewports[0] = {(float)viewport.originX, (float)(viewport.originY + viewport.height),
        (float)viewport.width, -(float)viewport.height, (float)viewport.znear, (float)viewport.zfar};
    _stateDirty = YES;
}
- (void)setScissorRect:(MTLScissorRect)rect
{
    if (![self checkActive]) return;
    if (rect.x > UINT32_MAX || rect.y > UINT32_MAX || rect.width > UINT32_MAX || rect.height > UINT32_MAX) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    _state.scissor_count = 1;
    _state.scissors[0] = {(uint32_t)rect.x, (uint32_t)rect.y, (uint32_t)rect.width, (uint32_t)rect.height};
    _stateDirty = YES;
}
- (void)setCullMode:(MTLCullMode)cull
{
    if (![self checkActive]) return;
    _state.cull_front = cull == MTLCullModeFront;
    _state.cull_back = cull == MTLCullModeBack;
    _stateDirty = YES;
}
- (void)setFrontFacingWinding:(MTLWinding)winding
{
    if (![self checkActive]) return;
    _state.front_clockwise = winding == MTLWindingClockwise;
    _stateDirty = YES;
}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha
{
    if (![self checkActive]) return;
    _state.blend_color[0] = red;
    _state.blend_color[1] = green;
    _state.blend_color[2] = blue;
    _state.blend_color[3] = alpha;
    _stateDirty = YES;
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
    if (![self checkActive]) return;
    if (state) [self.command recordFailure:A840_RT_UNSUPPORTED];
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (![self checkActive]) return;
    [(fragment ? _fragmentBindings : _vertexBindings) setBuffer:buffer offset:offset index:index];
    [self.command useResource:buffer];
    if (buffer && !nativeBuffer(buffer, self.command.owner))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setBuffer:buffer offset:offset index:index fragment:NO];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setBuffer:buffer offset:offset index:index fragment:YES];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (!bytes && length) { [self.command recordFailure:A840_RT_INVALID_ARG]; return; }
    if (![self checkActive]) return;
    NSData *data = [NSData dataWithBytes:bytes length:length];
    [(fragment ? _fragmentBindings : _vertexBindings) setData:data index:index];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self setBytes:bytes length:length index:index fragment:NO];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self setBytes:bytes length:length index:index fragment:YES];
}
- (void)setTexture:(id<MTLTexture>)texture index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (![self checkActive]) return;
    [(fragment ? _fragmentBindings : _vertexBindings) setTexture:texture index:index];
    [self.command useResource:texture];
    if (texture && !nativeTexture(texture, self.command.owner))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self setTexture:texture index:index fragment:NO];
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self setTexture:texture index:index fragment:YES];
}
- (void)setSampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (![self checkActive]) return;
    [(fragment ? _fragmentBindings : _vertexBindings) setSampler:sampler index:index];
    if (sampler && (![(id)sampler isKindOfClass:A840MTLSamplerState.class] || sampler.device != self.device))
        [self.command recordFailure:A840_RT_WRONG_DEVICE];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self setSampler:sampler index:index fragment:NO];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self setSampler:sampler index:index fragment:YES];
}
- (void)emitDraw:(const A840RtDrawDesc &)draw
{
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw) return;
    A840MTLRenderPipelineState *state = [(id)_pipeline isKindOfClass:A840MTLRenderPipelineState.class] ?
        (A840MTLRenderPipelineState *)_pipeline : nil;
    if (state && state.device != cb.owner) { [cb recordFailure:A840_RT_WRONG_DEVICE]; return; }
    const A840RtGraphicsProgramDesc *desc = state.nativeDescriptor;
    MTLRenderPipelineDescriptor *configuration = state.configuration;
    if (!desc || !configuration.isRasterizationEnabled || draw.primitive == UINT32_MAX ||
        configuration.vertexFunction.patchType != MTLPatchTypeNone ||
        configuration.maxVertexAmplificationCount > 1 ||
        configuration.rasterSampleCount > 1 ||
        configuration.supportIndirectCommandBuffers) {
        [cb recordFailure:A840_RT_UNSUPPORTED];
        return;
    }
    @synchronized (cb.owner) {
        if (_boundPipeline != state) {
            if (!pipelineBlend(configuration, &_state)) {
                [cb recordFailure:A840_RT_UNSUPPORTED];
                return;
            }
            const A840RtStatus st = a840_rt_render_set_program(_native, desc);
            if (st != A840_RT_OK) { [cb recordFailure:st]; return; }
            _boundPipeline = state;
            _stateDirty = YES;
        }
        if (_stateDirty) {
            const A840RtStatus st = a840_rt_render_set_state(_native, &_state);
            if (st != A840_RT_OK) { [cb recordFailure:st]; return; }
            _stateDirty = NO;
        }
        const A840RtStatus bindSt = [cb.owner.pipelineProvider bindRender:_native plan:state.plan
            vertexBindings:_vertexBindings fragmentBindings:_fragmentBindings];
        if (bindSt != A840_RT_OK) { [cb recordFailure:bindSt]; return; }
        const A840RtStatus drawSt = a840_rt_render_draw(_native, &draw);
        if (drawSt != A840_RT_OK) [cb recordFailure:drawSt];
    }
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base
{
    if (![self checkActive]) return;
    if (start > UINT32_MAX || count > UINT32_MAX || instances > UINT32_MAX || base > UINT32_MAX) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    A840RtDrawDesc draw{};
    draw.primitive = primitiveType(type);
    draw.index_type = A840_INDEX_NONE;
    draw.count = (uint32_t)count;
    draw.instance_count = (uint32_t)instances;
    draw.first_vertex = (uint32_t)start;
    draw.first_instance = (uint32_t)base;
    if (count && instances) [self emitDraw:draw];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
              indexBufferOffset:(NSUInteger)offset
{
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indexBuffer
               indexBufferOffset:offset instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
              indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
{
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indexBuffer
              indexBufferOffset:offset instanceCount:instances baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
              indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
                     baseVertex:(NSInteger)vertex baseInstance:(NSUInteger)base
{
    if (![self checkActive]) return;
    [self.command useResource:indexBuffer];
    if (!count || !instances || !self.command.wantsRaw) return;
    A840RtBuffer *buffer = nativeBuffer(indexBuffer, self.command.owner);
    const A840IndexType encodedType = indexType == MTLIndexTypeUInt16 ? A840_INDEX_UINT16 :
        indexType == MTLIndexTypeUInt32 ? A840_INDEX_UINT32 : A840_INDEX_NONE;
    if (!buffer || encodedType == A840_INDEX_NONE || count > UINT32_MAX ||
        instances > UINT32_MAX || base > UINT32_MAX || vertex < INT32_MIN || vertex > INT32_MAX ||
        offset > indexBuffer.length || (indexBuffer.length - offset) / (NSUInteger)encodedType > UINT32_MAX) {
        [self.command recordFailure:A840_RT_UNSUPPORTED];
        return;
    }
    A840RtDrawDesc draw{};
    draw.primitive = primitiveType(type);
    draw.index_type = encodedType;
    draw.count = (uint32_t)count;
    draw.instance_count = (uint32_t)instances;
    draw.first_instance = (uint32_t)base;
    draw.base_vertex = (int32_t)vertex;
    draw.index_capacity = (uint32_t)((indexBuffer.length - offset) / (NSUInteger)encodedType);
    draw.index_buffer = buffer;
    draw.index_offset = offset;
    [self emitDraw:draw];
}
- (void)drawPrimitives:(MTLPrimitiveType)type indirectBuffer:(id<MTLBuffer>)buffer
          indirectBufferOffset:(NSUInteger)offset
{
    (void)type;
    (void)offset;
    if (![self checkActive]) return;
    [self.command useResource:buffer];
    [self.command recordFailure:A840_RT_UNSUPPORTED];
}
@end
#pragma clang diagnostic pop
