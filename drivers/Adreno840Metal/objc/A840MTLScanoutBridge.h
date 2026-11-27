/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_SCANOUT_BRIDGE_H
#define A840_MTL_SCANOUT_BRIDGE_H

#import <IOSurface/IOSurfaceRef.h>
#import "A840MTLDevice.h"
#include "../core/a840_system_frontend_core.h"

@class A840MTLCommandBuffer, A840MTLTexture;

/* Pure native CARenderServer / NativeMobileDisplay 120Hz display scanout bridge:
 *   IOSurface -> UBWC 6.0 / Linear negotiation -> GPU completion / Early Writer Promise
 *   (ABI v3 Method 24/25) -> MDSS display refresh notification. */
@interface A840MTLScanoutBridge : NSObject
- (instancetype)initWithDevice:(A840MTLDevice *)device;
@property(nonatomic, weak, readonly) A840MTLDevice *device;
@property(nonatomic, readonly) uint64_t presentedFrames;
@property(nonatomic, readonly) uint64_t earlyPromisesSubmitted;
@property(nonatomic, readonly) uint32_t targetRefreshHz;

- (BOOL)registerSurfaceMetadataForTexture:(A840MTLTexture *)texture;
- (BOOL)notifyDisplayHandoffForSurface:(uint64_t)surfaceId
                             queueId:(uint64_t)queueId
                               fence:(uint64_t)fence
                        earlyPromise:(BOOL)earlyPromise;
- (int)encodePhase3ScanoutOnCommand:(A840MTLCommandBuffer *)command
                             source:(A840MTLTexture *)source
                        destination:(A840MTLTexture *)destination
                               mode:(unsigned)mode;
@end

#ifdef __cplusplus
extern "C" {
#endif
void a840_mtl_install_phase3_hwtest_adapters(void);
#ifdef __cplusplus
}
#endif

#endif /* A840_MTL_SCANOUT_BRIDGE_H */
