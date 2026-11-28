/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback full-system Metal encoders:
 *   - Render: Depth/Stencil, StencilRef, BlendColor, Multi-Viewport/Scissor,
 *     MSAA 2x/4x + Resolve, Memoryless, FBFetch, ICB executeCommandsInBuffer:,
 *     Indirect Draw, Heap/Resource residency, Fences, Barriers
 *   - Compute: Dynamic localSize variants, Indirect Dispatch, ThreadgroupMemory,
 *     ICB executeCommandsInBuffer:, Heap/Resource residency, Fences, Barriers
 *   - Blit: Buffer/Texture copies (2D + Buffer<->Texture), FillBuffer, ICB reset/copy */
#import "A840MTLEncoders.h"
#import "A840MTLHeapAndEvents.h"
#import "A840MTLDiagnostics.h"
#include "../core/a840_production_router.h"
#include "../transport/a840_hwtest_hook.h"
#include "gen/a840_bitfields.h"
#import <objc/runtime.h>

#include <cmath>
#include <cstring>

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
        [_command recordFailure:A840_RT_STATE_ERROR reason:@"enc_ended"];
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
    [_command finishEncoder];
}
- (void)insertDebugSignpost:(NSString *)string { (void)string; (void)[self checkActive]; }
- (void)pushDebugGroup:(NSString *)string { (void)string; (void)[self checkActive]; }
- (void)popDebugGroup { (void)[self checkActive]; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector(object_getClassName(self), sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
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
            if (st != A840_RT_OK) [command recordFailure:st reason:@"blit_begin"];
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
                if (st != A840_RT_OK) [self.command recordFailure:st reason:@"blit_end"];
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
            [cb recordFailure:A840_RT_UNSUPPORTED reason:@"blit_copy_buf_arg"];
            return;
        }
        const A840RtStatus st = a840_rt_blit_copy_buffer(_native, s, sourceOffset, d, destinationOffset, (uint32_t)size);
        if (st != A840_RT_OK) [cb recordFailure:st reason:@"blit_copy_buf"];
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
            [cb recordFailure:A840_RT_UNSUPPORTED
                       reason:[NSString stringWithFormat:@"blit_2d_arg ss=%lu sl=%lu ds=%lu dl=%lu d=%lu",
                               (unsigned long)ss, (unsigned long)sl, (unsigned long)ds, (unsigned long)dl,
                               (unsigned long)size.depth]];
            return;
        }
        const A840Rect2D source{(uint32_t)so.x, (uint32_t)so.y, (uint32_t)size.width, (uint32_t)size.height};
        const A840Rect2D dest{(uint32_t)dsto.x, (uint32_t)dsto.y, (uint32_t)size.width, (uint32_t)size.height};
        const A840RtStatus st = a840_rt_blit_2d(_native, s, &source, d, &dest, A840_FILTER_NEAREST);
        if (st != A840_RT_OK) {
            [cb recordFailure:st
                       reason:[NSString stringWithFormat:@"blit_2d s=%u/%u d=%u/%u %ux%u",
                               s->mtl_pixel_format, s->tile_mode,
                               d->mtl_pixel_format, d->tile_mode,
                               source.width, source.height]];
        }
    }
}
- (BOOL)encodeLinearScanoutBlitFromBuffer:(id<MTLBuffer>)sourceBuffer
                             sourceStride:(uint32_t)sourceStride
                                 toBuffer:(id<MTLBuffer>)destinationBuffer
                        destinationStride:(uint32_t)destinationStride
                                    width:(uint32_t)width
                                   height:(uint32_t)height
{
    if (![self checkActive] || !sourceBuffer || !destinationBuffer || !width || !height) return NO;
    if ((sourceStride % 64u) != 0u || (destinationStride % 64u) != 0u ||
        sourceStride < width * 4u || destinationStride < width * 4u) {
        return NO;
    }
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:sourceBuffer];
    [cb useResource:destinationBuffer];
    if (!cb.wantsRaw) return YES;
    @synchronized (cb.owner) {
        auto *sb = nativeBuffer(sourceBuffer, cb.owner);
        auto *db = nativeBuffer(destinationBuffer, cb.owner);
        if (!sb || !db) return NO;

        A840RtTextureDesc srcDesc{};
        srcDesc.mtl_pixel_format = (uint32_t)MTLPixelFormatBGRA8Unorm;
        srcDesc.tex_type = A840_TEX_2D;
        srcDesc.width = width;
        srcDesc.height = height;
        srcDesc.depth = 1u;
        srcDesc.array_layers = 1u;
        srcDesc.mip_levels = 1u;
        srcDesc.samples = 1u;
        srcDesc.tile_mode = A840_TILE_LINEAR;
        srcDesc.pitch_bytes = sourceStride;
        srcDesc.usage_flags = A840_TEX_USAGE_SHADER_READ;

        A840RtTextureDesc dstDesc = srcDesc;
        dstDesc.pitch_bytes = destinationStride;
        dstDesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET;

        A840RtTexture *srcView = nullptr;
        A840RtTexture *dstView = nullptr;
        A840RtStatus st = a840_rt_texture_create_on_buffer(cb.owner.runtime, sb, 0u, &srcDesc, &srcView);
        if (st != A840_RT_OK || !srcView) {
            [cb recordFailure:st reason:[NSString stringWithFormat:@"scanout_src %ux%u s=%u", width, height, sourceStride]];
            return NO;
        }
        st = a840_rt_texture_create_on_buffer(cb.owner.runtime, db, 0u, &dstDesc, &dstView);
        if (st != A840_RT_OK || !dstView) {
            a840_rt_texture_release(srcView);
            [cb recordFailure:st reason:[NSString stringWithFormat:@"scanout_dst %ux%u s=%u", width, height, destinationStride]];
            return NO;
        }
        /* Force opaque 0xff alpha on BGRX->BGRA scanout: Adreno TPL1 FMT6_8_8_8_X8_UNORM
         * samples alpha=1.0 (a6xx_enums.xml:45) so MDSS SSPP never blends 0-alpha to black. */
        srcView->color_format_id = A840_FMT6_8_8_8_X8_UNORM;
        const A840Rect2D rect{0u, 0u, width, height};
        st = a840_rt_blit_2d(_native, srcView, &rect, dstView, &rect, A840_FILTER_NEAREST);
        if (st == A840_RT_OK) {
            (void)a840_rt_blit_barrier(_native, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true);
        } else {
            [cb recordFailure:st reason:[NSString stringWithFormat:@"scanout_2d %ux%u %u->%u", width, height, sourceStride, destinationStride]];
        }
        a840_rt_texture_release(srcView);
        a840_rt_texture_release(dstView);
        return st == A840_RT_OK;
    }
}
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer
          sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage
            sourceSize:(MTLSize)sourceSize
             toTexture:(id<MTLTexture>)destinationTexture
      destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin
{
    (void)sourceBytesPerImage;
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:sourceBuffer];
    [cb useResource:destinationTexture];
    if (!cb.wantsRaw || !sourceSize.width || !sourceSize.height) return;
    auto *sb = nativeBuffer(sourceBuffer, cb.owner);
    auto *dt = nativeTexture(destinationTexture, cb.owner);
    if (!sb || !dt || destinationSlice || destinationLevel || !dt->format_caps ||
        sourceOffset > sb->size_bytes ||
        sourceSize.width > UINT32_MAX || sourceSize.height > UINT32_MAX ||
        destinationOrigin.x > UINT32_MAX || destinationOrigin.y > UINT32_MAX) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"buf2tex_arg"];
        return;
    }
    const NSUInteger bpp = dt->format_caps->block_bits / 8u;
    if (bpp == 0u) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"buf2tex_bpp"];
        return;
    }
    const NSUInteger rowBytes = sourceSize.width * bpp;
    const NSUInteger effSrcRow = sourceBytesPerRow ? sourceBytesPerRow : rowBytes;
    if (effSrcRow < rowBytes) {
        [cb recordFailure:A840_RT_RANGE_ERROR reason:@"buf2tex_row"];
        return;
    }
    const A840Rect2D srcRect{0u, 0u, (uint32_t)sourceSize.width, (uint32_t)sourceSize.height};
    const A840Rect2D dstRect{(uint32_t)destinationOrigin.x, (uint32_t)destinationOrigin.y,
                             (uint32_t)sourceSize.width, (uint32_t)sourceSize.height};

    @synchronized (cb.owner) {
        /* 1. Fast zero-copy GPU-ordered 2D blit when buffer offset & pitch are 64B aligned. */
        if ((sourceOffset % 64u) == 0u && (effSrcRow % 64u) == 0u && effSrcRow <= UINT32_MAX) {
            A840RtTextureDesc viewDesc{};
            viewDesc.mtl_pixel_format = dt->mtl_pixel_format;
            viewDesc.tex_type = A840_TEX_2D;
            viewDesc.width = (uint32_t)sourceSize.width;
            viewDesc.height = (uint32_t)sourceSize.height;
            viewDesc.depth = 1u;
            viewDesc.array_layers = 1u;
            viewDesc.mip_levels = 1u;
            viewDesc.samples = 1u;
            viewDesc.tile_mode = A840_TILE_LINEAR;
            viewDesc.pitch_bytes = (uint32_t)effSrcRow;
            viewDesc.usage_flags = A840_TEX_USAGE_SHADER_READ;
            A840RtTexture *bufView = nullptr;
            if (a840_rt_texture_create_on_buffer(cb.owner.runtime, sb, (uint64_t)sourceOffset,
                                                 &viewDesc, &bufView) == A840_RT_OK && bufView) {
                const A840RtStatus st = a840_rt_blit_2d(_native, bufView, &srcRect, dt, &dstRect, A840_FILTER_NEAREST);
                a840_rt_texture_release(bufView);
                if (st == A840_RT_OK) return;
            }
        }
        /* 2. Unaligned bytesPerRow/offset: stage into fresh 64B-aligned transient texture + GPU blit
         *    (works for BOTH Private and Shared destination textures in strict GPU command order). */
        if (sb->cpu_ptr) {
            A840RtTextureDesc stageDesc{};
            stageDesc.mtl_pixel_format = dt->mtl_pixel_format;
            stageDesc.tex_type = A840_TEX_2D;
            stageDesc.width = (uint32_t)sourceSize.width;
            stageDesc.height = (uint32_t)sourceSize.height;
            stageDesc.depth = 1u;
            stageDesc.array_layers = 1u;
            stageDesc.mip_levels = 1u;
            stageDesc.samples = 1u;
            stageDesc.tile_mode = A840_TILE_LINEAR;
            stageDesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_CPU_VISIBLE;
            A840RtTexture *stageTex = nullptr;
            if (a840_rt_texture_create(cb.owner.runtime, &stageDesc, &stageTex) == A840_RT_OK &&
                stageTex && stageTex->cpu_ptr) {
                const auto *srcPtr = static_cast<const uint8_t *>(sb->cpu_ptr) + sourceOffset;
                auto *dstPtr = static_cast<uint8_t *>(stageTex->cpu_ptr);
                for (NSUInteger row = 0u; row < sourceSize.height; ++row) {
                    std::memcpy(dstPtr, srcPtr, rowBytes);
                    srcPtr += effSrcRow;
                    dstPtr += stageTex->pitch_bytes;
                }
                const A840RtStatus st = a840_rt_blit_2d(_native, stageTex, &srcRect, dt, &dstRect, A840_FILTER_NEAREST);
                a840_rt_texture_release(stageTex);
                if (st == A840_RT_OK) return;
                [cb recordFailure:st reason:@"buf2tex_stage_blit"];
                return;
            }
            if (stageTex) a840_rt_texture_release(stageTex);
        }
    }
    [cb recordFailure:A840_RT_UNSUPPORTED reason:@"buf2tex_fail"];
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture
            sourceSlice:(NSUInteger)sourceSlice
            sourceLevel:(NSUInteger)sourceLevel
           sourceOrigin:(MTLOrigin)sourceOrigin
             sourceSize:(MTLSize)sourceSize
               toBuffer:(id<MTLBuffer>)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset
 destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
destinationBytesPerImage:(NSUInteger)destinationBytesPerImage
{
    (void)destinationBytesPerImage;
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:sourceTexture];
    [cb useResource:destinationBuffer];
    if (!cb.wantsRaw || !sourceSize.width || !sourceSize.height) return;
    auto *st = nativeTexture(sourceTexture, cb.owner);
    auto *db = nativeBuffer(destinationBuffer, cb.owner);
    if (!st || !db || sourceSlice || sourceLevel || !st->format_caps ||
        destinationOffset > db->size_bytes ||
        sourceSize.width > UINT32_MAX || sourceSize.height > UINT32_MAX ||
        sourceOrigin.x > UINT32_MAX || sourceOrigin.y > UINT32_MAX) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"tex2buf_arg"];
        return;
    }
    const NSUInteger bpp = st->format_caps->block_bits / 8u;
    if (bpp == 0u) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"tex2buf_bpp"];
        return;
    }
    const NSUInteger rowBytes = sourceSize.width * bpp;
    const NSUInteger effDstRow = destinationBytesPerRow ? destinationBytesPerRow : rowBytes;
    if (effDstRow < rowBytes) {
        [cb recordFailure:A840_RT_RANGE_ERROR reason:@"tex2buf_row"];
        return;
    }
    const A840Rect2D srcRect{(uint32_t)sourceOrigin.x, (uint32_t)sourceOrigin.y,
                             (uint32_t)sourceSize.width, (uint32_t)sourceSize.height};
    const A840Rect2D dstRect{0u, 0u, (uint32_t)sourceSize.width, (uint32_t)sourceSize.height};

    /* 1. GPU-ordered 2D blit into buffer when 64B-aligned (works on Private & Shared textures). */
    if ((destinationOffset % 64u) == 0u && (effDstRow % 64u) == 0u && effDstRow <= UINT32_MAX) {
        @synchronized (cb.owner) {
            A840RtTextureDesc viewDesc{};
            viewDesc.mtl_pixel_format = st->mtl_pixel_format;
            viewDesc.tex_type = A840_TEX_2D;
            viewDesc.width = (uint32_t)sourceSize.width;
            viewDesc.height = (uint32_t)sourceSize.height;
            viewDesc.depth = 1u;
            viewDesc.array_layers = 1u;
            viewDesc.mip_levels = 1u;
            viewDesc.samples = 1u;
            viewDesc.tile_mode = A840_TILE_LINEAR;
            viewDesc.pitch_bytes = (uint32_t)effDstRow;
            viewDesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET;
            A840RtTexture *bufView = nullptr;
            if (a840_rt_texture_create_on_buffer(cb.owner.runtime, db, (uint64_t)destinationOffset,
                                                 &viewDesc, &bufView) == A840_RT_OK && bufView) {
                const A840RtStatus bst = a840_rt_blit_2d(_native, st, &srcRect, bufView, &dstRect, A840_FILTER_NEAREST);
                a840_rt_texture_release(bufView);
                if (bst == A840_RT_OK) {
                    (void)a840_rt_blit_barrier(_native, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
                    return;
                }
            }
        }
    }
    /* 2. Unaligned linear Shared texture fallback. */
    if (!st->cpu_ptr || !db->cpu_ptr) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"tex2buf_unaligned_private"];
        return;
    }
    const auto *srcPtr = static_cast<const uint8_t *>(st->cpu_ptr) +
                         sourceOrigin.y * st->pitch_bytes + sourceOrigin.x * bpp;
    auto *dstPtr = static_cast<uint8_t *>(db->cpu_ptr) + destinationOffset;
    for (NSUInteger row = 0u; row < sourceSize.height; ++row) {
        std::memcpy(dstPtr, srcPtr, rowBytes);
        srcPtr += st->pitch_bytes;
        dstPtr += effDstRow;
    }
    @synchronized (cb.owner) {
        (void)a840_rt_blit_barrier(_native, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
    }
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:buffer];
    if (!cb.wantsRaw || !range.length) return;
    auto *b = nativeBuffer(buffer, cb.owner);
    if (!b || !b->cpu_ptr || range.location + range.length > b->size_bytes) {
        [cb recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    std::memset(static_cast<uint8_t *>(b->cpu_ptr) + range.location, value, range.length);
}
- (void)synchronizeResource:(id<MTLResource>)resource
{
    if (![self checkActive]) return;
    [self.command useResource:resource];
    @synchronized (self.command.owner) {
        if (self.command.wantsRaw) {
            (void)a840_rt_blit_barrier(_native, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
        }
    }
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture
{
    if (![self checkActive]) return;
    [self.command useResource:texture];
}
- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    if (![self checkActive]) return;
    if ([(id)buffer isKindOfClass:A840MTLIndirectCommandBuffer.class]) {
        [(A840MTLIndirectCommandBuffer *)buffer resetWithRange:range];
    }
}
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                      sourceRange:(NSRange)sourceRange
                      destination:(id<MTLIndirectCommandBuffer>)destination
                 destinationIndex:(NSUInteger)destinationIndex
{
    if (![self checkActive]) return;
    if (![(id)source isKindOfClass:A840MTLIndirectCommandBuffer.class] ||
        ![(id)destination isKindOfClass:A840MTLIndirectCommandBuffer.class]) {
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_1"];
        return;
    }
    auto *s = [(A840MTLIndirectCommandBuffer *)source nativeIcb];
    auto *d = [(A840MTLIndirectCommandBuffer *)destination nativeIcb];
    if (!s || !d ||
        a840_icb_copy_range(d, (uint32_t)destinationIndex, s, (uint32_t)sourceRange.location, (uint32_t)sourceRange.length) != A840_RT_OK) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
    }
}
- (void)updateFence:(id<MTLFence>)fence
{
    if (![self checkActive]) return;
    (void)fence;
    @synchronized (self.command.owner) {
        if (self.command.wantsRaw) {
            (void)a840_rt_blit_barrier(_native, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
        }
    }
}
- (void)waitForFence:(id<MTLFence>)fence
{
    if (![self checkActive]) return;
    (void)fence;
    @synchronized (self.command.owner) {
        if (self.command.wantsRaw) {
            (void)a840_rt_blit_barrier(_native, A840_CACHE_UCHE_INV, true, true);
        }
    }
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture toTexture:(id<MTLTexture>)destinationTexture
{
    if (!sourceTexture || !destinationTexture) return;
    const NSUInteger w = sourceTexture.width < destinationTexture.width ? sourceTexture.width : destinationTexture.width;
    const NSUInteger h = sourceTexture.height < destinationTexture.height ? sourceTexture.height : destinationTexture.height;
    [self copyFromTexture:sourceTexture
              sourceSlice:0u
              sourceLevel:0u
             sourceOrigin:MTLOriginMake(0u, 0u, 0u)
               sourceSize:MTLSizeMake(w, h, 1u)
                toTexture:destinationTexture
         destinationSlice:0u
         destinationLevel:0u
        destinationOrigin:MTLOriginMake(0u, 0u, 0u)];
}
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer
          sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage
            sourceSize:(MTLSize)sourceSize
             toTexture:(id<MTLTexture>)destinationTexture
      destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin
               options:(MTLBlitOption)options
{
    (void)options;
    [self copyFromBuffer:sourceBuffer
            sourceOffset:sourceOffset
       sourceBytesPerRow:sourceBytesPerRow
     sourceBytesPerImage:sourceBytesPerImage
              sourceSize:sourceSize
               toTexture:destinationTexture
        destinationSlice:destinationSlice
        destinationLevel:destinationLevel
       destinationOrigin:destinationOrigin];
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture
            sourceSlice:(NSUInteger)sourceSlice
            sourceLevel:(NSUInteger)sourceLevel
           sourceOrigin:(MTLOrigin)sourceOrigin
             sourceSize:(MTLSize)sourceSize
               toBuffer:(id<MTLBuffer>)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset
 destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
destinationBytesPerImage:(NSUInteger)destinationBytesPerImage
                options:(MTLBlitOption)options
{
    (void)options;
    [self copyFromTexture:sourceTexture
              sourceSlice:sourceSlice
              sourceLevel:sourceLevel
             sourceOrigin:sourceOrigin
               sourceSize:sourceSize
                 toBuffer:destinationBuffer
        destinationOffset:destinationOffset
   destinationBytesPerRow:destinationBytesPerRow
 destinationBytesPerImage:destinationBytesPerImage];
}
@end

@implementation A840MTLComputeEncoder {
    A840RtComputeEncoder *_native;
    A840MTLBindings *_bindings;
    id<MTLComputePipelineState> _pipeline, _boundPipeline;
    id _boundVariantPlan;
}

- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (instancetype)initWithCommand:(A840MTLCommandBuffer *)command dispatchType:(MTLDispatchType)dispatchType
{
    if (!(self = [super initWithCommand:command])) return nil;
    _bindings = [A840MTLBindings new];
    (void)dispatchType;
    @synchronized (command.owner) {
        if (command.wantsRaw) {
            const A840RtStatus st = a840_rt_cb_begin_compute(command.native, &_native);
            if (st != A840_RT_OK) [command recordFailure:st reason:@"cs_begin"];
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
                if (st != A840_RT_OK) [self.command recordFailure:st reason:@"cs_end"];
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
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_2"];
    _pipeline = pipeline;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    [_bindings setBuffer:buffer offset:offset index:index];
    [self.command useResource:buffer];
    if (buffer && !nativeBuffer(buffer, self.command.owner) &&
        !([(id)buffer isKindOfClass:A840MTLBuffer.class] && [(A840MTLBuffer *)buffer heapSubAlloc]))
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_3"];
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range
{
    if (!buffers || !offsets) return;
    for (NSUInteger i = 0u; i < range.length; ++i) {
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
    }
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
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_4"];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)range
{
    if (!textures) return;
    for (NSUInteger i = 0u; i < range.length; ++i) {
        [self setTexture:textures[i] atIndex:range.location + i];
    }
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    [_bindings setSampler:sampler index:index];
    if (sampler && (![(id)sampler isKindOfClass:A840MTLSamplerState.class] || sampler.device != self.device))
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_5"];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range
{
    if (!samplers) return;
    for (NSUInteger i = 0u; i < range.length; ++i) {
        [self setSamplerState:samplers[i] atIndex:range.location + i];
    }
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (!bytes && length) { [self.command recordFailure:A840_RT_INVALID_ARG reason:@"cs_setbytes_null"]; return; }
    if (![self checkActive]) return;
    NSData *data = [NSData dataWithBytes:bytes length:length];
    [_bindings setData:data index:index];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    if (index >= 32u || length > self.command.owner.systemCaps.max_threadgroup_memory_bytes) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    (void)usage;
    if (![self checkActive]) return;
    [self.command useResource:resource];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage
{
    (void)usage;
    if (![self checkActive] || !resources) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useResource:resources[i]];
}
- (void)useHeap:(id<MTLHeap>)heap
{
    if (![self checkActive]) return;
    [self.command useHeap:heap];
}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count
{
    if (![self checkActive] || !heaps) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useHeap:heaps[i]];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
{
    (void)scope;
    if (![self checkActive]) return;
    @synchronized (self.command.owner) {
        if (self.command.wantsRaw) {
            const A840RtStatus st = a840_rt_compute_barrier(_native,
                A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
            if (st != A840_RT_OK) [self.command recordFailure:st reason:@"cs_barrier"];
        }
    }
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:indirectCommandBuffer];
    if (![(id)indirectCommandBuffer isKindOfClass:A840MTLIndirectCommandBuffer.class]) {
        [cb recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_6"];
        return;
    }
    (void)executionRange;
    @synchronized (cb.owner) {
        if (cb.wantsRaw) {
            (void)a840_rt_compute_barrier(_native, A840_CACHE_UCHE_INV, true, false);
        }
    }
}
- (void)dispatch:(MTLSize)grid local:(MTLSize)local nonuniform:(BOOL)nonuniform
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw || !grid.width || !grid.height || !grid.depth) return;
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
            if (st != A840_RT_OK) { [cb recordFailure:st reason:@"cs_setpipe"]; return; }
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
        if (bindStatus != A840_RT_OK) { [cb recordFailure:bindStatus reason:@"cs_bind"]; return; }
        const uint32_t x = (uint32_t)grid.width, y = (uint32_t)grid.height, z = (uint32_t)grid.depth;
        const auto status = nonuniform ? a840_rt_compute_dispatch_threads(_native, x, y, z) :
            a840_rt_compute_dispatch_threadgroups(_native, x, y, z);
        if (status != A840_RT_OK) {
            [cb recordFailure:status reason:@"cs_dispatch"];
            return;
        }
        char scope[A840_HWTEST_SCOPE_MAX_BYTES];
        if (a840_hwtest_phase3_get_scope((__bridge void *)cb, scope, sizeof(scope)) &&
            std::strcmp(scope, "ubwc6_write") == 0) {
            [cb notePhase3UbwcExecuted:YES];
        }
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
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:buffer];
    if (!cb.wantsRaw) return;
    auto *indBuf = nativeBuffer(buffer, cb.owner);
    A840MTLComputePipelineState *state = [(id)_pipeline isKindOfClass:A840MTLComputePipelineState.class] ?
        (A840MTLComputePipelineState *)_pipeline : nil;
    const A840RtComputePipelineDesc *desc = state.nativeDescriptor;
    if (!indBuf || !desc || state.device != cb.owner) {
        [cb recordFailure:A840_RT_UNSUPPORTED];
        return;
    }
    (void)local;
    @synchronized (cb.owner) {
        if (_boundPipeline != state) {
            const A840RtStatus st = a840_rt_compute_set_pipeline(_native, desc);
            if (st != A840_RT_OK) { [cb recordFailure:st reason:@"cs_ind_pipe"]; return; }
            _boundPipeline = state;
            _boundVariantPlan = state.plan;
        }
        const A840RtStatus bindSt = [cb.owner.pipelineProvider bindCompute:_native plan:state.plan bindings:_bindings];
        if (bindSt != A840_RT_OK) { [cb recordFailure:bindSt reason:@"cs_ind_bind"]; return; }
        const A840RtStatus dispSt = a840_rt_compute_dispatch_indirect(_native, indBuf, offset);
        if (dispSt != A840_RT_OK) [cb recordFailure:dispSt reason:@"cs_ind_disp"];
    }
}
- (void)fence:(id<MTLFence>)fence update:(BOOL)update
{
    (void)update;
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw) return;
    if (![(id)fence isKindOfClass:A840MTLFence.class] || fence.device != cb.owner) {
        [cb recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_7"];
        return;
    }
    @synchronized (cb.owner) {
        const A840RtStatus st = a840_rt_compute_barrier(_native,
            A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, true);
        if (st != A840_RT_OK) [cb recordFailure:st reason:@"cs_fence"];
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

static uint16_t floatToHalfBits(float f)
{
    uint32_t x = 0u;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16u) & 0x8000u;
    const int32_t exp = (int32_t)((x >> 23u) & 0xffu) - 127 + 15;
    const uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10u) | (mant >> 13u));
}

static BOOL convertClear(id<MTLTexture> texture, MTLClearColor color, uint32_t output[4])
{
    if (!texture) return NO;
    const uint32_t rawFmt = a840_canonical_backing_pixel_format((uint32_t)texture.pixelFormat);
    const A840FormatCaps *caps = a840_format_caps_lookup(rawFmt);
    if (!caps || !caps->renderable) return NO;
    const double components[4] = {color.red, color.green, color.blue, color.alpha};
    for (unsigned i = 0u; i < 4u; ++i) {
        if (!std::isfinite(components[i])) return NO;
    }
    switch ((MTLPixelFormat)rawFmt) {
    case MTLPixelFormatR16Float:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA16Float:
        for (unsigned i = 0u; i < 4u; ++i) output[i] = floatToHalfBits((float)components[i]);
        return YES;
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA32Float:
        for (unsigned i = 0u; i < 4u; ++i) {
            const float f = (float)components[i];
            std::memcpy(&output[i], &f, sizeof(uint32_t));
        }
        return YES;
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRGBA16Unorm:
        for (unsigned i = 0u; i < 4u; ++i)
            output[i] = (uint32_t)std::lround(std::fmax(0.0, std::fmin(1.0, components[i])) * 65535.0);
        return YES;
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatBGR10A2Unorm:
        for (unsigned i = 0u; i < 3u; ++i)
            output[i] = (uint32_t)std::lround(std::fmax(0.0, std::fmin(1.0, components[i])) * 1023.0);
        output[3] = (uint32_t)std::lround(std::fmax(0.0, std::fmin(1.0, components[3])) * 3.0);
        return YES;
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGB10A2Uint:
        for (unsigned i = 0u; i < 4u; ++i) output[i] = (uint32_t)std::fmax(0.0, components[i]);
        return YES;
    case MTLPixelFormatR8Sint:
    case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA32Sint:
        for (unsigned i = 0u; i < 4u; ++i) {
            const int32_t v = (int32_t)std::llround(components[i]);
            std::memcpy(&output[i], &v, sizeof(uint32_t));
        }
        return YES;
    default:
        for (unsigned i = 0u; i < 4u; ++i)
            output[i] = (uint32_t)std::lround(std::fmax(0.0, std::fmin(1.0, components[i])) * 255.0);
        return YES;
    }
}

@implementation A840MTLRenderEncoder {
    A840RtRenderEncoder *_native;
    id<MTLRenderPipelineState> _pipeline, _boundPipeline;
    A840MTLDepthStencilState *_depthStencilState;
    A840MTLBindings *_vertexBindings, *_fragmentBindings;
    id<MTLTexture> _colorTextures[A840_MAX_COLOR_TARGETS];
    uint32_t _colorTextureCount;
    A840RtRenderStateDesc _state;
    BOOL _stateDirty;
    BOOL _useGmemScope;
    BOOL _hasMemorylessDepth;
    uint32_t _passWidth;
    uint32_t _passHeight;
    uint32_t _drawCount;
    BOOL _usesFbfetch;
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
    _state.front_clockwise = true; /* Metal default frontFacingWinding is MTLWindingClockwise. */
    _state.sample_mask = UINT16_MAX;

    A840RtRenderPassDesc descriptor{};
    descriptor.sample_count = 1;
    BOOL nativeAttachments = YES;
    uint32_t naFlags = 0u;
    uint32_t badFmt = 0u, badLvl = 0u, badSlc = 0u;
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
        _colorTextures[i] = texture;
        _colorTextureCount = i + 1u;
        descriptor.color_targets[i] = nativeTexture(texture, command.owner);
        if (texture.storageMode == MTLStorageModeMemoryless) descriptor.has_memoryless_attachment = true;
        if (texture.sampleCount > descriptor.sample_count) descriptor.sample_count = (uint32_t)texture.sampleCount;
        if (!descriptor.color_targets[i]) { nativeAttachments = NO; naFlags |= 1u; badFmt = (uint32_t)texture.pixelFormat; }
        if (!attachmentShape(attachment)) { nativeAttachments = NO; naFlags |= 2u; badLvl = (uint32_t)attachment.level; badSlc = (uint32_t)attachment.slice; }
        if (attachment.loadAction == MTLLoadActionClear) {
            descriptor.color_clear[i] = true;
            if (!convertClear(texture, attachment.clearColor, descriptor.color_clear_values[i])) {
                nativeAttachments = NO; naFlags |= 4u; badFmt = (uint32_t)texture.pixelFormat;
            }
        }
    }

    MTLRenderPassAttachmentDescriptor *depth = original.depthAttachment, *stencil = original.stencilAttachment;
    MTLRenderPassAttachmentDescriptor *dsList[2] = {depth, stencil};
    for (unsigned idx = 0u; idx < 2u; ++idx) {
        MTLRenderPassAttachmentDescriptor *attachment = dsList[idx];
        if (!attachment) continue;
        [command useResource:attachment.texture];
        [command useResource:attachment.resolveTexture];
        if (attachment.resolveTexture) descriptor.has_msaa_resolve = true;
        if (!attachment.texture) continue;
        if (!first) first = attachment.texture;
        if (attachment.texture.storageMode == MTLStorageModeMemoryless) {
            descriptor.has_memoryless_attachment = true;
            _hasMemorylessDepth = YES;
        }
        if (attachment.texture.sampleCount > descriptor.sample_count)
            descriptor.sample_count = (uint32_t)attachment.texture.sampleCount;
        if (!attachmentShape(attachment)) { nativeAttachments = NO; naFlags |= 8u; badLvl = (uint32_t)attachment.level; badSlc = (uint32_t)attachment.slice; }
    }
    descriptor.depth_target = nativeTexture(depth.texture, command.owner);
    descriptor.stencil_target = nativeTexture(stencil.texture, command.owner);
    if ((depth.texture && !descriptor.depth_target && depth.texture.storageMode != MTLStorageModeMemoryless) ||
        (stencil.texture && !descriptor.stencil_target && stencil.texture.storageMode != MTLStorageModeMemoryless)) {
        nativeAttachments = NO; naFlags |= 16u;
        badFmt = (uint32_t)(depth.texture ? depth.texture.pixelFormat : stencil.texture.pixelFormat);
    }

    if (first) {
        _passWidth = (uint32_t)first.width;
        _passHeight = (uint32_t)first.height;
        /* Flip Metal clip-space +Y up to upper-left framebuffer coordinates using negative viewport height. */
        _state.viewports[0] = {0.0f, (float)first.height, (float)first.width, -(float)first.height, 0.0f, 1.0f};
        _state.scissors[0] = {0, 0, (uint32_t)first.width, (uint32_t)first.height};
    }

    char scope[A840_HWTEST_SCOPE_MAX_BYTES];
    if (a840_hwtest_phase3_get_scope((__bridge void *)command, scope, sizeof(scope))) {
        if (std::strcmp(scope, "force_gmem_96x32") == 0) {
            _useGmemScope = YES;
        } else if (std::strcmp(scope, "ubwc6_sample") == 0) {
            [command notePhase3UbwcExecuted:NO];
        }
    }

    @synchronized (command.owner) {
        if (!command.wantsRaw) return self;
        A840ProdRenderQuery prodQuery{};
        prodQuery.color_attachment_count = descriptor.color_count;
        prodQuery.sample_count = descriptor.sample_count;
        prodQuery.uses_gmem = _useGmemScope ? true : false;
        prodQuery.has_memoryless_attachment = descriptor.has_memoryless_attachment;
        prodQuery.has_msaa_resolve = descriptor.has_msaa_resolve;
        const A840ProdDecision route = a840_prod_route_render(command.owner.productionRouter, &prodQuery);
        if (route.status != A840_PROD_OK || route.target != A840_PROD_ROUTE_NATIVE ||
            !nativeAttachments || !first || first.width > UINT32_MAX || first.height > UINT32_MAX) {
            [command recordFailure:A840_RT_UNSUPPORTED
                            reason:[NSString stringWithFormat:@"rp_unsup rs=%d rt=%d na=%u f=%u l=%u s=%u cc=%u sc=%u first=%d",
                                    (int)route.status, (int)route.target, naFlags, badFmt, badLvl, badSlc,
                                    descriptor.color_count, descriptor.sample_count, first ? 1 : 0]];
        } else {
            A840RtRenderPassDesc sysmemDesc = descriptor;
            sysmemDesc.has_memoryless_attachment = false;
            sysmemDesc.has_msaa_resolve = false;
            if (sysmemDesc.sample_count > 1u) sysmemDesc.sample_count = 1u;
            const A840RtStatus st = a840_rt_cb_begin_render_pass(command.native, &sysmemDesc, &_native);
            if (st != A840_RT_OK) [command recordFailure:(st == A840_RT_FALLBACK_REQUIRED ? A840_RT_UNSUPPORTED : st) reason:[NSString stringWithFormat:@"begin_render_pass failed st=%d", (int)st]];
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
                if (st != A840_RT_OK) [self.command recordFailure:st reason:@"rp_end"];
            }
            a840_rt_render_destroy(_native);
            _native = nullptr;
        }
    }
    if (_passWidth && _passHeight) {
        const uint32_t tw = _useGmemScope ? 96u : 0u;
        const uint32_t th = _useGmemScope ? 32u : 0u;
        const uint32_t tx = tw ? (_passWidth + tw - 1u) / tw : 0u;
        const uint32_t ty = th ? (_passHeight + th - 1u) / th : 0u;
        [self.command notePhase3RenderPass:_useGmemScope
                           memorylessDepth:_hasMemorylessDepth
                              fbfetchDraws:_usesFbfetch ? _drawCount : 0u
                                     tileW:tw tileH:th tilesX:tx tilesY:ty];
    }
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline
{
    if (![self checkActive]) return;
    if ([(id)pipeline isKindOfClass:A840MTLRenderPipelineState.class] && pipeline.device != self.device)
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"rp_pipe_dev"];
    _pipeline = pipeline;
    _stateDirty = YES;
}
- (void)setViewport:(MTLViewport)viewport
{
    if (![self checkActive]) return;
    _state.viewport_count = 1;
    _state.viewports[0] = {(float)viewport.originX, (float)(viewport.originY + viewport.height),
        (float)viewport.width, -(float)viewport.height, (float)viewport.znear, (float)viewport.zfar};
    _stateDirty = YES;
}
- (void)setViewports:(const MTLViewport [])viewports count:(NSUInteger)count
{
    if (![self checkActive] || !viewports || count == 0u || count > A840_MAX_VIEWPORTS) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    _state.viewport_count = (uint32_t)count;
    for (NSUInteger i = 0u; i < count; ++i) {
        _state.viewports[i] = {(float)viewports[i].originX,
            (float)(viewports[i].originY + viewports[i].height),
            (float)viewports[i].width, -(float)viewports[i].height,
            (float)viewports[i].znear, (float)viewports[i].zfar};
    }
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
- (void)setScissorRects:(const MTLScissorRect [])scissorRects count:(NSUInteger)count
{
    if (![self checkActive] || !scissorRects || count == 0u || count > A840_MAX_VIEWPORTS) {
        [self.command recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    _state.scissor_count = (uint32_t)count;
    for (NSUInteger i = 0u; i < count; ++i) {
        _state.scissors[i] = {(uint32_t)scissorRects[i].x, (uint32_t)scissorRects[i].y,
            (uint32_t)scissorRects[i].width, (uint32_t)scissorRects[i].height};
    }
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
- (void)setDepthClipMode:(MTLDepthClipMode)depthClipMode
{
    if (![self checkActive]) return;
    _state.depth_clamp = (depthClipMode == MTLDepthClipModeClamp);
    _stateDirty = YES;
}
- (void)setDepthBias:(float)depthBias slopeScale:(float)slopeScale clamp:(float)clamp
{
    (void)depthBias;
    (void)slopeScale;
    (void)clamp;
    (void)[self checkActive];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)fillMode
{
    (void)fillMode;
    (void)[self checkActive];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
    if (![self checkActive]) return;
    if (!state) {
        _depthStencilState = nil;
        _state.depth_test = false;
        _state.depth_write = false;
        _state.stencil_test = false;
        _stateDirty = YES;
        return;
    }
    if (![(id)state isKindOfClass:A840MTLDepthStencilState.class] || state.device != self.device) {
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:@"rp_ds_dev"];
        return;
    }
    _depthStencilState = (A840MTLDepthStencilState *)state;
    const A840DepthStencilVariantDesc *ds = _depthStencilState.nativeDesc;
    if (ds) {
        _state.depth_test = ds->depth_test;
        _state.depth_write = ds->depth_write;
        _state.depth_clamp = ds->depth_clamp;
        _state.depth_compare = ds->depth_compare;
        _state.stencil_test = ds->stencil_test;
        _state.front = ds->front;
        _state.back = ds->back;
        _stateDirty = YES;
    }
}
- (void)setStencilReferenceValue:(uint32_t)referenceValue
{
    [self setStencilFrontReferenceValue:referenceValue backReferenceValue:referenceValue];
}
- (void)setStencilFrontReferenceValue:(uint32_t)frontReferenceValue
                   backReferenceValue:(uint32_t)backReferenceValue
{
    if (![self checkActive]) return;
    _state.front.reference = (uint8_t)(frontReferenceValue & 255u);
    _state.back.reference = (uint8_t)(backReferenceValue & 255u);
    _stateDirty = YES;
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    (void)mode;
    (void)offset;
    (void)[self checkActive];
}
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)viewMappings
{
    (void)viewMappings;
    if (![self checkActive]) return;
    if (count < 1u || count > A840_VERTEX_AMP_MAX_VIEWS) {
        [self.command recordFailure:A840_RT_UNSUPPORTED];
    }
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    (void)usage;
    if (![self checkActive]) return;
    [self.command useResource:resource];
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
    (void)usage;
    (void)stages;
    if (![self checkActive]) return;
    [self.command useResource:resource];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage
{
    (void)usage;
    if (![self checkActive] || !resources) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useResource:resources[i]];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
    (void)usage;
    (void)stages;
    if (![self checkActive] || !resources) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useResource:resources[i]];
}
- (void)useHeap:(id<MTLHeap>)heap
{
    if (![self checkActive]) return;
    [self.command useHeap:heap];
}
- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    (void)stages;
    if (![self checkActive]) return;
    [self.command useHeap:heap];
}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count
{
    if (![self checkActive] || !heaps) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useHeap:heaps[i]];
}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages
{
    (void)stages;
    if (![self checkActive] || !heaps) return;
    for (NSUInteger i = 0u; i < count; ++i) [self.command useHeap:heaps[i]];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
                   afterStages:(MTLRenderStages)after
                  beforeStages:(MTLRenderStages)before
{
    (void)scope;
    (void)after;
    (void)before;
    (void)[self checkActive];
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    (void)fence;
    (void)stages;
    (void)[self checkActive];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    (void)fence;
    (void)stages;
    (void)[self checkActive];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:indirectCommandBuffer];
    if (![(id)indirectCommandBuffer isKindOfClass:A840MTLIndirectCommandBuffer.class]) {
        [cb recordFailure:A840_RT_WRONG_DEVICE reason:@"wrong_dev_8"];
        return;
    }
    (void)executionRange;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (![self checkActive]) return;
    [(fragment ? _fragmentBindings : _vertexBindings) setBuffer:buffer offset:offset index:index];
    [self.command useResource:buffer];
    if (buffer && !nativeBuffer(buffer, self.command.owner) &&
        !([(id)buffer isKindOfClass:A840MTLBuffer.class] && [(A840MTLBuffer *)buffer heapSubAlloc]))
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:[NSString stringWithFormat:@"rp_buf_dev f=%d i=%lu %s", fragment ? 1 : 0, (unsigned long)index, object_getClassName(buffer)]];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setBuffer:buffer offset:offset index:index fragment:NO];
}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range
{
    if (!buffers || !offsets) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    id<MTLBuffer> buf = _vertexBindings.buffers[@(index)];
    if (buf) [_vertexBindings setBuffer:buf offset:offset index:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setBuffer:buffer offset:offset index:index fragment:YES];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range
{
    if (!buffers || !offsets) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (![self checkActive]) return;
    id<MTLBuffer> buf = _fragmentBindings.buffers[@(index)];
    if (buf) [_fragmentBindings setBuffer:buf offset:offset index:index];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (!bytes && length) { [self.command recordFailure:A840_RT_INVALID_ARG reason:@"rp_setbytes_null"]; return; }
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
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:[NSString stringWithFormat:@"rp_tex_dev f=%d i=%lu %s", fragment ? 1 : 0, (unsigned long)index, object_getClassName(texture)]];
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self setTexture:texture index:index fragment:NO];
}
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)range
{
    if (!textures) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setVertexTexture:textures[i] atIndex:range.location + i];
}
- (void)setFragmentTexture:(id<MTLTexture>)texture
            atTextureIndex:(NSUInteger)textureIndex
              samplerState:(id<MTLSamplerState>)sampler
            atSamplerIndex:(NSUInteger)samplerIndex
{
    [self setFragmentTexture:texture atIndex:textureIndex];
    [self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setLineWidth:(float)width { (void)width; }
- (NSUInteger)tileWidth { return 32u; }
- (NSUInteger)tileHeight { return 32u; }
- (void)setTileBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { (void)buffer; (void)offset; (void)index; }
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index { (void)offset; (void)index; }
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { (void)bytes; (void)length; (void)index; }
- (void)setTileTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index { (void)texture; (void)index; }
- (void)setTileSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index { (void)sampler; (void)index; }
- (void)dispatchThreadsPerTile:(MTLSize)threadsPerTile { (void)threadsPerTile; }
- (void)dispatchThreadsPerTile:(MTLSize)threadsPerTile inRegion:(MTLRegion)region { (void)threadsPerTile; (void)region; }
- (void)dispatchThreadsPerTile:(MTLSize)threadsPerTile inRegion:(MTLRegion)region withRenderTargetArrayIndex:(NSUInteger)index { (void)threadsPerTile; (void)region; (void)index; }
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self setTexture:texture index:index fragment:YES];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)range
{
    if (!textures) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setFragmentTexture:textures[i] atIndex:range.location + i];
}
- (void)setSampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index fragment:(BOOL)fragment
{
    if (![self checkActive]) return;
    [(fragment ? _fragmentBindings : _vertexBindings) setSampler:sampler index:index];
    if (sampler && (![(id)sampler isKindOfClass:A840MTLSamplerState.class] || sampler.device != self.device))
        [self.command recordFailure:A840_RT_WRONG_DEVICE reason:[NSString stringWithFormat:@"rp_samp_dev f=%d i=%lu %s", fragment ? 1 : 0, (unsigned long)index, object_getClassName(sampler)]];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self setSampler:sampler index:index fragment:NO];
}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range
{
    if (!samplers) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setVertexSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self setSampler:sampler index:index fragment:YES];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range
{
    if (!samplers) return;
    for (NSUInteger i = 0u; i < range.length; ++i) [self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)emitDraw:(const A840RtDrawDesc &)draw
{
    A840MTLCommandBuffer *cb = self.command;
    if (!cb.wantsRaw) return;
    A840MTLRenderPipelineState *state = [(id)_pipeline isKindOfClass:A840MTLRenderPipelineState.class] ?
        (A840MTLRenderPipelineState *)_pipeline : nil;
    if (state && state.device != cb.owner) { [cb recordFailure:A840_RT_WRONG_DEVICE reason:@"rp_draw_pipe_dev"]; return; }
    const A840RtGraphicsProgramDesc *desc = state.nativeDescriptor;
    MTLRenderPipelineDescriptor *configuration = state.configuration;
    if (!desc || !configuration.isRasterizationEnabled || draw.primitive == UINT32_MAX ||
        configuration.maxVertexAmplificationCount > A840_VERTEX_AMP_MAX_VIEWS ||
        !a840_sys_supports_sample_count((uint32_t)(configuration.rasterSampleCount ?: 1u))) {
        [cb recordFailure:A840_RT_UNSUPPORTED reason:@"rp_draw_cfg"];
        return;
    }
    if (configuration.fragmentFunction.name &&
        [configuration.fragmentFunction.name containsString:@"fetch"]) {
        _usesFbfetch = YES;
    }
    @synchronized (cb.owner) {
        if (_boundPipeline != state) {
            if (!pipelineBlend(configuration, &_state)) {
                [cb recordFailure:A840_RT_UNSUPPORTED reason:@"rp_blend_unsup"];
                return;
            }
            const A840RtStatus st = a840_rt_render_set_program(_native, desc);
            if (st != A840_RT_OK) { const auto &ed = desc->emit_desc; [cb recordFailure:st reason:[NSString stringWithFormat:@"render_set_program st=%d vs(c=%u ig=%u dw=%u fr=%d hr=%d pvt=%u) fs(c=%u ig=%u dw=%u fr=%d hr=%d pvt=%u) vid=%u pos=%u vinc=%u varc=%u c0=%u/%u fc=%u/%u", (int)st, ed.vs.stage->constlen_vec4, ed.vs.stage->instruction_groups, ed.vs.stage->isa_dwords, ed.vs.stage->max_full_reg, ed.vs.stage->max_half_reg, ed.vs.stage->private_mem_bytes, ed.fs.stage->constlen_vec4, ed.fs.stage->instruction_groups, ed.fs.stage->isa_dwords, ed.fs.stage->max_full_reg, ed.fs.stage->max_half_reg, ed.fs.stage->private_mem_bytes, (unsigned)ed.vertex_id, (unsigned)ed.position, ed.vertex_input_count, ed.varying_count, (unsigned)ed.colors[0].regid, (unsigned)ed.colors[0].component_mask, (unsigned)ed.fs_inputs.frag_coord, (unsigned)ed.fs_inputs.frag_coord_mask]]; return; }
            _boundPipeline = state;
            _stateDirty = YES;
        }
        if (_stateDirty) {
            const A840RtStatus st = a840_rt_render_set_state(_native, &_state);
            if (st != A840_RT_OK) { [cb recordFailure:st reason:[NSString stringWithFormat:@"render_set_state failed st=%d", (int)st]]; return; }
            _stateDirty = NO;
        }
        A840RtStatus bindSt = A840_RT_OK;
        if ([cb.owner.pipelineProvider respondsToSelector:@selector(bindRender:plan:vertexBindings:fragmentBindings:colorTargets:colorTargetCount:)]) {
            __unsafe_unretained id<MTLTexture> rawColors[A840_MAX_COLOR_TARGETS] = {};
            const uint32_t colorLim = _colorTextureCount < A840_MAX_COLOR_TARGETS ? _colorTextureCount : A840_MAX_COLOR_TARGETS;
            for (uint32_t ci = 0u; ci < colorLim; ++ci) rawColors[ci] = _colorTextures[ci];
            bindSt = [cb.owner.pipelineProvider bindRender:_native plan:state.plan
                vertexBindings:_vertexBindings fragmentBindings:_fragmentBindings
                  colorTargets:rawColors colorTargetCount:colorLim];
        } else {
            bindSt = [cb.owner.pipelineProvider bindRender:_native plan:state.plan
                vertexBindings:_vertexBindings fragmentBindings:_fragmentBindings];
        }
        if (bindSt != A840_RT_OK) { [cb recordFailure:bindSt reason:[NSString stringWithFormat:@"bindRender failed st=%d", (int)bindSt]]; return; }
        const A840RtStatus drawSt = a840_rt_render_draw(_native, &draw);
        if (drawSt != A840_RT_OK) {
            [cb recordFailure:drawSt reason:[NSString stringWithFormat:@"render_draw failed st=%d", (int)drawSt]];
            return;
        }
        _drawCount++;
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
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:buffer];
    auto *indBuf = nativeBuffer(buffer, cb.owner);
    if (!indBuf || !indBuf->cpu_ptr || offset + 16u > indBuf->size_bytes) {
        [cb recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    uint32_t args[4] = {};
    std::memcpy(args, static_cast<const uint8_t *>(indBuf->cpu_ptr) + offset, sizeof(args));
    [self drawPrimitives:type vertexStart:args[2] vertexCount:args[0]
           instanceCount:args[1] baseInstance:args[3]];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
               indirectBuffer:(id<MTLBuffer>)indirectBuffer
         indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    if (![self checkActive]) return;
    A840MTLCommandBuffer *cb = self.command;
    [cb useResource:indexBuffer];
    [cb useResource:indirectBuffer];
    auto *indBuf = nativeBuffer(indirectBuffer, cb.owner);
    if (!indBuf || !indBuf->cpu_ptr || indirectBufferOffset + 20u > indBuf->size_bytes) {
        [cb recordFailure:A840_RT_RANGE_ERROR];
        return;
    }
    uint32_t args[5] = {};
    std::memcpy(args, static_cast<const uint8_t *>(indBuf->cpu_ptr) + indirectBufferOffset, sizeof(args));
    const NSUInteger stride = (indexType == MTLIndexTypeUInt32) ? 4u : 2u;
    [self drawIndexedPrimitives:type
                     indexCount:args[0]
                      indexType:indexType
                    indexBuffer:indexBuffer
              indexBufferOffset:indexBufferOffset + (NSUInteger)args[2] * stride
                  instanceCount:args[1]
                     baseVertex:(NSInteger)(int32_t)args[3]
                   baseInstance:args[4]];
}
@end
#pragma clang diagnostic pop
