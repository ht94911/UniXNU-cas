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

#import "A840MTLDiagnostics.h"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

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


namespace {

/* Exact NativeGOPWire.h hand-off ABI (shared with NativeLegacyDisplay in XNU). */
enum {
    kNgpVersion = 1u,
    kNgpBgrx8888 = 1u,
    kNgpMethodMode = 0u,
    kNgpMethodWaitSwap = 2u,
    kNgpMethodCompleteSwap = 3u,
    kNgpOpenTypeHandoff = 0x4148u,
    kNgpTunnelBase = 0x7000u,
    kNgpTunnelMagic = 0x4e544e4cu,
    kNgpTunnelProbe = 0xffffffffu,
    kNgpMemoryScanoutToken = 0x5343u,
    kNgpMapInhibitCache = 0x101u
};

struct A840NgpMode {
    uint32_t version;
    uint32_t format;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    uint64_t bytes;
};

struct A840NgpWait {
    uint64_t timeout_ms;
};

struct A840NgpSwap {
    uint64_t sequence;
    uint64_t address;
    uint64_t bytes;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved; /* mapping id */
};

struct A840NgpComplete {
    uint64_t sequence;
    uint32_t status;
    uint32_t reserved;
};

struct A840GopThreadArgs {
    void *bridgeRef;
    void *deviceRef;
    unsigned port;
};

struct A840GopLink {
    unsigned conn = 0u;
    uint32_t selector = 0u;
    int (*callFn)(unsigned, uint32_t, const void *, size_t, void *, size_t *) = nullptr;
    int (*mapFn)(unsigned, uint32_t, unsigned, uint64_t *, uint64_t *, uint32_t) = nullptr;
    int (*openFn)(unsigned, unsigned, uint32_t, unsigned *) = nullptr;
    int (*closeFn)(unsigned) = nullptr;

    bool probe()
    {
        const uint32_t probeBuf[2] = {kNgpTunnelMagic, kNgpTunnelProbe};
        for (uint32_t sel = 0u; sel < 256u; ++sel) {
            if (callFn(conn, sel, probeBuf, sizeof(probeBuf), nullptr, nullptr) == 0) {
                selector = sel;
                return true;
            }
        }
        return false;
    }

    int method(uint32_t m, const void *inBuf, size_t inBytes, void *outBuf, size_t outBytes)
    {
        uint8_t req[8 + 128] = {};
        if (inBytes > 128u) return -1;
        const uint32_t hdr[2] = {kNgpTunnelMagic, m};
        std::memcpy(req, hdr, 8u);
        if (inBytes && inBuf) std::memcpy(req + 8u, inBuf, inBytes);
        size_t o = outBytes;
        return callFn(conn, selector, req, 8u + inBytes, outBytes ? outBuf : nullptr, outBytes ? &o : nullptr);
    }
};

static void *a840GopScanoutThreadMain(void *rawArg)
{
    A840GopThreadArgs *args = static_cast<A840GopThreadArgs *>(rawArg);
    A840MTLScanoutBridge *bridge = (__bridge_transfer A840MTLScanoutBridge *)args->bridgeRef;
    A840MTLDevice *device = (__bridge_transfer A840MTLDevice *)args->deviceRef;
    const unsigned port = args->port;
    delete args;

    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    sleep(2);

    @autoreleasepool {
        a840diag::markPort(port, 0x6700u);
        void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
        if (!iokit) {
            a840diag::reportText("P0 no-iokit");
            return nullptr;
        }
        A840GopLink link{};
        link.openFn = reinterpret_cast<int (*)(unsigned, unsigned, uint32_t, unsigned *)>(dlsym(iokit, "IOServiceOpen"));
        link.closeFn = reinterpret_cast<int (*)(unsigned)>(dlsym(iokit, "IOServiceClose"));
        link.callFn = reinterpret_cast<int (*)(unsigned, uint32_t, const void *, size_t, void *, size_t *)>(
            dlsym(iokit, "IOConnectCallStructMethod"));
        link.mapFn = reinterpret_cast<int (*)(unsigned, uint32_t, unsigned, uint64_t *, uint64_t *, uint32_t)>(
            dlsym(iokit, "IOConnectMapMemory64"));
        if (!link.openFn || !link.callFn || !link.mapFn) {
            a840diag::reportText("P0 no-syms");
            return nullptr;
        }
        const int openRc = link.openFn(port, mach_task_self(), kNgpOpenTypeHandoff, &link.conn);
        if (openRc != 0 || !link.conn) {
            char msg[64] = {};
            snprintf(msg, sizeof(msg), "P0 e%x", (unsigned)openRc);
            a840diag::reportText(msg);
            return nullptr;
        }
        a840diag::markPort(port, 0x6701u);
        if (!link.probe()) {
            a840diag::reportText("P1 no-sel");
            if (link.closeFn) link.closeFn(link.conn);
            return nullptr;
        }
        a840diag::markPort(port, 0x6710u);
        A840NgpMode mode{};
        const int modeRc = link.method(kNgpTunnelBase + kNgpMethodMode, nullptr, 0u, &mode, sizeof(mode));
        a840diag::markPort(port, 0x6711u);
        if (modeRc != 0 || mode.version != kNgpVersion || mode.format != kNgpBgrx8888 ||
            mode.width == 0u || mode.height == 0u || mode.stride < mode.width * 4u ||
            mode.bytes < mode.stride * mode.height) {
            char msg[64] = {};
            snprintf(msg, sizeof(msg), "P2 e%x v%u f%u", (unsigned)modeRc, mode.version, mode.format);
            a840diag::reportText(msg);
            if (link.closeFn) link.closeFn(link.conn);
            return nullptr;
        }
        a840diag::markPort(port, 0x6712u);
        uint64_t scanoutAddr = 0u, scanoutLen = 0u;
        const int mapRc = link.mapFn(link.conn, kNgpMemoryScanoutToken, mach_task_self(),
                                     &scanoutAddr, &scanoutLen, kNgpMapInhibitCache);
        a840diag::markPort(port, 0x6713u);
        if (mapRc != 0 || !scanoutAddr || scanoutLen < mode.bytes) {
            char msg[64] = {};
            snprintf(msg, sizeof(msg), "P3 map e%x", (unsigned)mapRc);
            a840diag::reportText(msg);
            if (link.closeFn) link.closeFn(link.conn);
            return nullptr;
        }
        const size_t page = (size_t)getpagesize();
        const size_t alignedScanoutBytes = ((size_t)mode.bytes + page - 1u) & ~(page - 1u);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> targetBuf = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)scanoutAddr
                                                            length:alignedScanoutBytes
                                                           options:MTLResourceStorageModeShared
                                                       deallocator:nil];
        if (!queue || !targetBuf) {
            a840diag::reportText("P3 no-target-buf");
            if (link.closeFn) link.closeFn(link.conn);
            return nullptr;
        }
        if (![(id)targetBuf isKindOfClass:A840MTLBuffer.class] ||
            !((A840MTLBuffer *)targetBuf).native ||
            !((A840MTLBuffer *)targetBuf).native->imported) {
            a840diag::reportText("P3 target-not-imported");
            if (link.closeFn) link.closeFn(link.conn);
            return nullptr;
        }
        a840diag::markPort(port, 0x6715u);
        a840diag::reportText("P4 serving-native-a840");

        NSMutableDictionary<NSNumber *, id<MTLBuffer>> *srcCache = [NSMutableDictionary new];
        NSMutableDictionary<NSNumber *, NSNumber *> *srcAddrMap = [NSMutableDictionary new];
        uint64_t frames = 0u, failures = 0u;

        for (;;) {
            @autoreleasepool {
                A840NgpWait waitReq{500u};
                A840NgpSwap swap{};
                if (frames == 0u && failures == 0u) a840diag::markPort(port, 0x6720u);
                const int waitRc = link.method(kNgpTunnelBase + kNgpMethodWaitSwap,
                                               &waitReq, sizeof(waitReq), &swap, sizeof(swap));
                if (waitRc != 0) {
                    if ((unsigned)waitRc == 0xe00002d6u /* kIOReturnTimeout */) continue;
                    char msg[64] = {};
                    snprintf(msg, sizeof(msg), "PX wait e%x f%llu", (unsigned)waitRc, (unsigned long long)frames);
                    a840diag::reportText(msg);
                    break;
                }
                a840diag::markPort(port, 0x6730u);
                bool ok = false;
                if (swap.address && swap.bytes >= swap.stride * swap.height &&
                    swap.width <= mode.width && swap.height <= mode.height && swap.stride >= swap.width * 4u) {
                    const size_t alignedSrcBytes = ((size_t)swap.bytes + page - 1u) & ~(page - 1u);
                    id<MTLBuffer> srcBuf = nil;
                    if (swap.reserved != 0u) {
                        NSNumber *key = @(swap.reserved);
                        NSNumber *prevAddr = srcAddrMap[key];
                        if (prevAddr && prevAddr.unsignedLongLongValue == swap.address) {
                            srcBuf = srcCache[key];
                        }
                    }
                    if (!srcBuf) {
                        srcBuf = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)swap.address
                                                           length:alignedSrcBytes
                                                          options:MTLResourceStorageModeShared
                                                      deallocator:nil];
                        if (srcBuf && swap.reserved != 0u) {
                            if (srcCache.count >= 16u) {
                                [srcCache removeAllObjects];
                                [srcAddrMap removeAllObjects];
                            }
                            NSNumber *key = @(swap.reserved);
                            srcCache[key] = srcBuf;
                            srcAddrMap[key] = @(swap.address);
                        }
                    }
                    if (srcBuf) {
                        id<MTLCommandBuffer> cb = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                        if (cb && blit) {
                            if (swap.stride == mode.stride && swap.width == mode.width) {
                                const NSUInteger copyBytes = (NSUInteger)(swap.stride * swap.height) & ~(NSUInteger)3u;
                                [blit copyFromBuffer:srcBuf sourceOffset:0u
                                            toBuffer:targetBuf destinationOffset:0u
                                                size:copyBytes];
                            } else {
                                const NSUInteger rowCopy = ((NSUInteger)swap.width * 4u) & ~(NSUInteger)3u;
                                for (uint32_t y = 0u; y < swap.height; ++y) {
                                    [blit copyFromBuffer:srcBuf sourceOffset:(NSUInteger)(y * swap.stride)
                                                toBuffer:targetBuf destinationOffset:(NSUInteger)(y * mode.stride)
                                                    size:rowCopy];
                                }
                            }
                            [blit endEncoding];
                            [cb commit];
                            [cb waitUntilCompleted];
                            ok = (cb.status == MTLCommandBufferStatusCompleted && cb.error == nil);
                        }
                    }
                }
                a840diag::markPort(port, 0x6731u);
                A840NgpComplete done{swap.sequence, ok ? 0u : 0xe00002bcu, 0u};
                const int compRc = link.method(kNgpTunnelBase + kNgpMethodCompleteSwap,
                                               &done, sizeof(done), nullptr, 0u);
                if (ok) ++frames; else ++failures;
                if (frames == 1u || (frames % 10u) == 0u || (!ok && failures <= 3u)) {
                    char msg[96] = {};
                    if (ok) {
                        snprintf(msg, sizeof(msg), "PF %llu/%llu", (unsigned long long)frames, (unsigned long long)failures);
                    } else {
                        snprintf(msg, sizeof(msg), "PF %llu/%llu fail %ux%u s=%llu/%llu f=%u",
                                 (unsigned long long)frames, (unsigned long long)failures,
                                 swap.width, swap.height,
                                 (unsigned long long)swap.stride, (unsigned long long)mode.stride,
                                 swap.format);
                    }
                    a840diag::reportText(msg);
                }
                if (compRc != 0) break;
                (void)bridge;
            }
        }
        if (link.closeFn && link.conn) link.closeFn(link.conn);
    }
    return nullptr;
}

} /* namespace */

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

- (void)startGopScanoutLoopWithAcceleratorPort:(unsigned)port
{
    A840MTLDevice *dev = _device;
    if (!dev || !port) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel)) return;
    auto *args = new A840GopThreadArgs{
        (__bridge_retained void *)self,
        (__bridge_retained void *)dev,
        port
    };
    pthread_t th{};
    pthread_attr_t attr{};
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1u << 20);
    if (pthread_create(&th, &attr, a840GopScanoutThreadMain, args) != 0) {
        (void)(__bridge_transfer A840MTLScanoutBridge *)args->bridgeRef;
        (void)(__bridge_transfer A840MTLDevice *)args->deviceRef;
        delete args;
        started.store(false, std::memory_order_release);
    }
    pthread_attr_destroy(&attr);
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
