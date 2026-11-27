/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 pure native CARenderServer / MDSS 120Hz scanout bridge & Phase 3 hwtest adapters. */
#import <CoreFoundation/CoreFoundation.h>
#import "A840MTLScanoutBridge.h"
#import "A840MTLCommand.h"
#import "A840MTLEncoders.h"
#import "A840MTLResource.h"
#include "../transport/a840_hwtest_hook.h"
#include "NativeAdrenoABI_v3.h"

#include <cstring>

static void *hwtestPhase3NewUbwcTexture(void *rawDevice,
                                        uint32_t width,
                                        uint32_t height,
                                        unsigned forScanout)
{
    if (!rawDevice || width == 0u || height == 0u) return nullptr;
    id obj = (__bridge id)rawDevice;
    if (![obj isKindOfClass:A840MTLDevice.class]) return nullptr;
    A840MTLDevice *device = (A840MTLDevice *)obj;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    A840MTLTexture *tex = [device newNativeUbwcTextureWithDescriptor:desc forScanout:(forScanout != 0u)];
    if (!tex) return nullptr;
    /* Caller expects +0 bridged id<MTLTexture>; keep autoreleased in caller's pool. */
    return (void *)CFAutorelease((__bridge_retained CFTypeRef)tex);
}

static int hwtestPhase3EncodeScanout(void *rawCommand,
                                     void *rawSource,
                                     void *rawDestination,
                                     unsigned mode)
{
    if (!rawCommand || !rawSource || !rawDestination || mode > 1u) return -1;
    id cbObj = (__bridge id)rawCommand;
    id srcObj = (__bridge id)rawSource;
    id dstObj = (__bridge id)rawDestination;
    if (![cbObj isKindOfClass:A840MTLCommandBuffer.class] ||
        ![srcObj isKindOfClass:A840MTLTexture.class] ||
        ![dstObj isKindOfClass:A840MTLTexture.class]) {
        return -1;
    }
    A840MTLCommandBuffer *cb = (A840MTLCommandBuffer *)cbObj;
    A840MTLScanoutBridge *bridge = cb.owner.scanoutBridge;
    if (!bridge) return -1;
    return [bridge encodePhase3ScanoutOnCommand:cb
                                         source:(A840MTLTexture *)srcObj
                                    destination:(A840MTLTexture *)dstObj
                                           mode:mode];
}

void a840_mtl_install_phase3_hwtest_adapters(void)
{
    a840_hwtest_phase3_register_adapters(hwtestPhase3NewUbwcTexture, hwtestPhase3EncodeScanout);
}

@implementation A840MTLScanoutBridge {
    uint64_t _presentedFrames;
    uint64_t _earlyPromisesSubmitted;
}

- (instancetype)initWithDevice:(A840MTLDevice *)device
{
    if (!(self = [super init]) || !device) return nil;
    _device = device;
    a840_mtl_install_phase3_hwtest_adapters();
    return self;
}

- (uint64_t)presentedFrames { return _presentedFrames; }
- (uint64_t)earlyPromisesSubmitted { return _earlyPromisesSubmitted; }
- (uint32_t)targetRefreshHz { return A840_SYS_DEFAULT_REFRESH_HZ; }

- (BOOL)registerSurfaceMetadataForTexture:(A840MTLTexture *)texture
{
    A840MTLDevice *dev = _device;
    if (!dev || !texture || !dev.wire) return NO;
    A840SysSurfaceInfo info{};
    if (texture.surfaceHandle != A840_SYS_INVALID_HANDLE &&
        a840_sys_query_surface(dev.systemFrontend, texture.surfaceHandle, &info) == A840_SYS_OK) {
        struct NativeAdrenoAbiSurfaceMetaV3 meta = info.surface_meta;
        meta.op = 1u;
        (void)a840_wire_call(dev.wire, NativeAdrenoMethodSurfaceMetaV3,
                             &meta, sizeof(meta), &meta, sizeof(meta));
        return YES;
    }
    return NO;
}

- (BOOL)notifyDisplayHandoffForSurface:(uint64_t)surfaceId
                             queueId:(uint64_t)queueId
                               fence:(uint64_t)fence
                        earlyPromise:(BOOL)earlyPromise
{
    A840MTLDevice *dev = _device;
    if (!dev || surfaceId == 0u) return NO;
    @synchronized (self) {
        _presentedFrames++;
        if (earlyPromise && dev.processRole == A840_SYS_ROLE_RENDER_SERVER) {
            _earlyPromisesSubmitted++;
        }
    }
    if (dev.wire) {
        struct NativeAdrenoAbiWriterPromiseV3 req{};
        struct NativeAdrenoAbiWriterPromiseReplyV3 reply{};
        req.op = (fence != 0u) ? NativeAdrenoPromiseFulfill : NativeAdrenoPromiseMake;
        req.surface_count = 1u;
        req.queue = queueId;
        req.fence = fence;
        req.surface_ids[0] = surfaceId;
        (void)a840_wire_call(dev.wire, NativeAdrenoMethodWriterPromiseV3,
                             &req, sizeof(req), &reply, sizeof(reply));
    }
    return YES;
}

- (int)encodePhase3ScanoutOnCommand:(A840MTLCommandBuffer *)command
                             source:(A840MTLTexture *)source
                        destination:(A840MTLTexture *)destination
                               mode:(unsigned)mode
{
    if (!command || !source || !destination || mode > 1u) return -1;
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) return -1;
    const NSUInteger w = source.width < destination.width ? source.width : destination.width;
    const NSUInteger h = source.height < destination.height ? source.height : destination.height;
    [blit copyFromTexture:source
              sourceSlice:0u
              sourceLevel:0u
             sourceOrigin:MTLOriginMake(0u, 0u, 0u)
               sourceSize:MTLSizeMake(w, h, 1u)
                toTexture:destination
         destinationSlice:0u
         destinationLevel:0u
        destinationOrigin:MTLOriginMake(0u, 0u, 0u)];
    [blit endEncoding];
    [command notePhase3ScanoutMode:mode];
    return 0;
}

@end
