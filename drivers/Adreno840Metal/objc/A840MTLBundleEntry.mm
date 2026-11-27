/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 iOS 27 full-system zero-fallback Metal plugin bundle entry:
 *   IOAcceleratorES -> MetalPluginName -> NSBundle -> principal class ->
 *   NativeAdrenoMetalDevice : _MTLDevice -initWithAcceleratorPort:
 * Gated strictly on sibling NativeAdrenoMetal.publish marker; zero fallback. */
#import "A840MTLDevice.h"
#import "A840MTLPipelineProvider.h"
#import "A840MTLScanoutBridge.h"
#import "A840MTLSystemSPI.h"
#include "../core/a840_production_router.h"
#include "../core/a840_system_frontend_core.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#if TARGET_OS_OSX
@interface IOGPUMetalDevice : NSObject
@end
@interface NativeAdrenoMetalDevice : IOGPUMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#else
@interface _MTLDevice : NSObject
@end
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#endif

namespace {

constexpr uint32_t kNativeOpenType = 16711u; /* 'AG' */

static BOOL resolvePublishPolicy(A840SysPublishPolicy *outPolicy)
{
    if (!outPolicy) return NO;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void *>(&resolvePublishPolicy), &info) || !info.dli_fname) {
        return NO;
    }
    A840SysPublishPolicy probe{};
    (void)a840_sys_resolve_publish_marker(info.dli_fname, getprogname(), true, &probe);
    const bool markerReadable = (probe.marker_path[0] != '\0') && (access(probe.marker_path, R_OK) == 0);
    return a840_sys_resolve_publish_marker(info.dli_fname, getprogname(), markerReadable, outPolicy) == A840_SYS_OK;
}

static id<MTLDevice> createSystemDeviceForPort(unsigned port, const A840SysPublishPolicy *policy)
{
    if (!port || !policy || !policy->allow_device_creation) return nil;
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return nil;
    auto openFn = reinterpret_cast<kern_return_t (*)(mach_port_t, task_port_t, uint32_t, mach_port_t *)>(
        dlsym(iokit, "IOServiceOpen"));
    auto entryIdFn = reinterpret_cast<kern_return_t (*)(mach_port_t, uint64_t *)>(
        dlsym(iokit, "IORegistryEntryGetRegistryEntryID"));
    if (!openFn) return nil;

    uint64_t registryID = A840_KAANAPALI_CHIP_ID;
    if (entryIdFn) {
        (void)entryIdFn(port, &registryID);
    }

    mach_port_t connection = MACH_PORT_NULL;
    if (openFn(port, mach_task_self(), kNativeOpenType, &connection) != KERN_SUCCESS ||
        connection == MACH_PORT_NULL) {
        return nil;
    }

    NSError *error = nil;
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithConnection:connection
                                                                    takesOwnership:YES
                                                                             error:&error];
    if (!transport) return nil;

    A840MTLNativePipelineProvider *provider = [[A840MTLNativePipelineProvider alloc] init];
    if (!provider) return nil;

    A840MTLDevice *device = [[A840MTLDevice alloc] initWithTransport:transport
                                                    pipelineProvider:provider
                                                         processRole:policy->proc_role
                                                          registryID:registryID
                                                               error:&error];
    if (!device) return nil;
    a840_mtl_install_phase3_hwtest_adapters();
    return device;
}

} /* namespace */

@implementation NativeAdrenoMetalDevice

- (id)initWithAcceleratorPort:(unsigned)port
{
    self = [super init];
    if (!self || !port) return nil;
    A840SysPublishPolicy policy{};
    if (!resolvePublishPolicy(&policy)) {
        return nil;
    }
    return createSystemDeviceForPort(port, &policy);
}

@end

@interface Adreno840MetalPrincipal : NSObject
+ (id<MTLDevice>)createDirectDevice:(NSError **)error;
@end

@implementation Adreno840MetalPrincipal
+ (id<MTLDevice>)createDirectDevice:(NSError **)error
{
    a840_mtl_install_phase3_hwtest_adapters();
    return a840_metal_create_direct_device(error);
}
@end

extern "C" __attribute__((visibility("default")))
const char *a840_metal_bundle_version(void)
{
    return a840_prod_bundle_footprint().bundle_version;
}

extern "C" __attribute__((visibility("default")))
A840ProdBundleFootprint a840_metal_bundle_footprint(void)
{
    return a840_prod_bundle_footprint();
}
