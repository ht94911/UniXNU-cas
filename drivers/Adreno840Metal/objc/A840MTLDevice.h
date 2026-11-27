/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_DEVICE_H
#define A840_MTL_DEVICE_H

#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import "../transport/a840_iokit_transport.h"
#include "../core/a840_production_router.h"
#include "../core/a840_runtime_cmd.h"
#include "../core/a840_system_frontend_core.h"

@class A840MTLDevice, A840MTLBindings, A840MTLTexture, A840MTLScanoutBridge;

/* The bundle's pipeline record provider supplies owned, uploaded compiler
 * output. The returned token keeps every pointed-to record and BO alive.
 * Binding methods consume Metal-indexed snapshots and call the C core's
 * Compute Bindless/Const or Render SDS interfaces. */
@protocol A840MTLPipelineProvider <NSObject>
- (id)prepareCompute:(id<MTLFunction>)function forDevice:(A840MTLDevice *)device
          descriptor:(A840RtComputePipelineDesc *)descriptor;
- (id)prepareRender:(MTLRenderPipelineDescriptor *)pipeline forDevice:(A840MTLDevice *)device
         descriptor:(A840RtGraphicsProgramDesc *)descriptor;
- (A840RtStatus)bindCompute:(A840RtComputeEncoder *)encoder plan:(id)plan
                    bindings:(A840MTLBindings *)bindings;
- (A840RtStatus)bindRender:(A840RtRenderEncoder *)encoder plan:(id)plan
            vertexBindings:(A840MTLBindings *)vertex fragmentBindings:(A840MTLBindings *)fragment;
@optional
- (A840RtStatus)bindCompute:(A840RtComputeEncoder *)encoder plan:(id)plan
                    bindings:(A840MTLBindings *)bindings
                    gridSize:(MTLSize)grid
             threadgroupSize:(MTLSize)local
                  nonuniform:(BOOL)nonuniform;
- (id<MTLLibrary>)defaultLibraryForDevice:(A840MTLDevice *)device;
- (id<MTLLibrary>)libraryWithURL:(NSURL *)url forDevice:(A840MTLDevice *)device error:(NSError **)error;
- (id)prepareComputeVariantForPlan:(id)plan localSize:(MTLSize)localSize
                         forDevice:(A840MTLDevice *)device
                        descriptor:(A840RtComputePipelineDesc *)descriptor;
- (A840RtStatus)bindRender:(A840RtRenderEncoder *)encoder plan:(id)plan
            vertexBindings:(A840MTLBindings *)vertex fragmentBindings:(A840MTLBindings *)fragment
              colorTargets:(id<MTLTexture> const __unsafe_unretained *)colorTargets
          colorTargetCount:(uint32_t)colorTargetCount;
@end

#if !TARGET_OS_OSX
@interface _MTLDevice : NSObject
@end
@interface A840MTLDevice : _MTLDevice <MTLDevice>
#else
@interface A840MTLDevice : NSObject <MTLDevice>
#endif
- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithTransport:(A840IokitTransport *)transport
                 pipelineProvider:(id<A840MTLPipelineProvider>)provider
                      processRole:(A840SysProcRole)processRole
                       registryID:(uint64_t)registryID
                            error:(NSError **)error NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithTransport:(A840IokitTransport *)transport
                 pipelineProvider:(id<A840MTLPipelineProvider>)provider
                            error:(NSError **)error;
- (A840MTLTexture *)newNativeUbwcTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                            forScanout:(BOOL)forScanout;
@property(nonatomic, readonly) A840RtDevice *runtime;
@property(nonatomic, readonly) A840IokitWire *wire;
@property(nonatomic, readonly) A840ProdRouter *productionRouter;
@property(nonatomic, readonly) A840SysFrontend *systemFrontend;
@property(nonatomic, readonly) A840SysDeviceCaps systemCaps;
@property(nonatomic, readonly) A840SysProcRole processRole;
@property(nonatomic, strong, readonly) A840MTLScanoutBridge *scanoutBridge;
@property(nonatomic, strong, readonly) id<A840MTLPipelineProvider> pipelineProvider;
@property(nonatomic, strong, readonly) dispatch_queue_t workQueue;
@property(nonatomic, strong, readonly) dispatch_queue_t callbackQueue;
- (BOOL)isCallbackThread;
@end

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default")))
id<MTLDevice> a840_metal_create_device(mach_port_t connection, BOOL ownsConnection,
    void *reserved, id<A840MTLPipelineProvider> provider, NSError **error);
#ifdef __cplusplus
}
#endif

NSError *a840_metal_error(A840RtStatus status, NSString *operation);

#endif /* A840_MTL_DEVICE_H */
