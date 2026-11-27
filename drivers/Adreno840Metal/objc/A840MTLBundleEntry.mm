/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12.1 iOS 27 full-system zero-fallback Metal plugin bundle entry:
 *   IOAcceleratorES -> MetalPluginName -> NSBundle -> principal class ->
 *   NativeAdrenoMetalDevice : _MTLDevice -initWithAcceleratorPort:
 * Gated strictly on sibling NativeAdrenoMetal.publish marker; zero fallback. */
#import <CoreFoundation/CoreFoundation.h>
#import "A840MTLDevice.h"
#import "A840MTLDiagnostics.h"
#import "A840MTLPipelineProvider.h"
#import "A840MTLScanoutBridge.h"
#import "A840MTLSystemSPI.h"
#include "../core/a840_production_router.h"
#include "../core/a840_system_frontend_core.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if TARGET_OS_OSX
@interface IOGPUMetalDevice : NSObject
@end
@interface NativeAdrenoMetalDevice : IOGPUMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#else
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#endif

namespace {

constexpr uint32_t kNativeOpenType = 16711u; /* 'AG' */

static BOOL resolveSiblingPath(const char *siblingName, char *outPath, size_t outCap)
{
    if (!siblingName || !outPath || outCap == 0u) return NO;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void *>(&resolveSiblingPath), &info) || !info.dli_fname) {
        return NO;
    }
    snprintf(outPath, outCap, "%s", info.dli_fname);
    char *slash = strrchr(outPath, '/');
    if (!slash) return NO;
    *slash = '\0';
    slash = strrchr(outPath, '/');
    if (!slash) return NO;
    snprintf(slash + 1, outCap - static_cast<size_t>((slash + 1) - outPath), "%s", siblingName);
    return access(outPath, R_OK) == 0 ? YES : NO;
}

static BOOL isProcessAllowedByClientsFile(const char *procName)
{
    if (!procName || !procName[0]) return NO;
    char clientsPath[1024] = {};
    if (!resolveSiblingPath("NativeAdrenoMetal.clients", clientsPath, sizeof(clientsPath))) {
        return NO;
    }
    FILE *f = fopen(clientsPath, "r");
    if (!f) return NO;
    char text[1024] = {};
    const size_t n = fread(text, 1u, sizeof(text) - 1u, f);
    fclose(f);
    text[n] = '\0';
    char *save = nullptr;
    for (char *tok = strtok_r(text, " ,\t\r\n", &save); tok; tok = strtok_r(nullptr, " ,\t\r\n", &save)) {
        if (strcmp(tok, "*") == 0 || strcmp(tok, procName) == 0) return YES;
        const size_t len = strlen(tok);
        if (len > 0u && tok[len - 1u] == '*' && strncmp(procName, tok, len - 1u) == 0) return YES;
    }
    return NO;
}

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
    const char *procName = getprogname() ?: "";
    const BOOL isRenderServer = (policy->proc_role == A840_SYS_ROLE_RENDER_SERVER);
    const BOOL isHwTest = (strcmp(procName, "NezhaGpuTest") == 0);
    if (!isRenderServer && !isHwTest && !isProcessAllowedByClientsFile(procName)) {
        return nil;
    }

    a840diag::installExceptionHandler();

    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return nil;
    auto retainFn = reinterpret_cast<int (*)(unsigned)>(dlsym(iokit, "IOObjectRetain"));
    auto openFn = reinterpret_cast<kern_return_t (*)(mach_port_t, task_port_t, uint32_t, mach_port_t *)>(
        dlsym(iokit, "IOServiceOpen"));
    auto entryIdFn = reinterpret_cast<kern_return_t (*)(mach_port_t, uint64_t *)>(
        dlsym(iokit, "IORegistryEntryGetRegistryEntryID"));
    if (!openFn) return nil;

    /* Retain accelerator port for progress markers and backboardd GOP scanout loop. */
    if (retainFn) retainFn(port);
    a840diag::setAcceleratorPort(port);
    a840diag::markPort(port, 0x6800u);

    /* Ensure plain linear 8-bit BGRA IOSurfaces/framebuffers (no Apple-proprietary AFBC/XR10 compression). */
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    if (isRenderServer) {
        setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    }

    uint64_t registryID = A840_KAANAPALI_CHIP_ID;
    if (entryIdFn) {
        (void)entryIdFn(port, &registryID);
    }

    mach_port_t connection = MACH_PORT_NULL;
    if (openFn(port, mach_task_self(), kNativeOpenType, &connection) != KERN_SUCCESS ||
        connection == MACH_PORT_NULL) {
        a840diag::markPort(port, 0x68f1u);
        return nil;
    }

    NSError *error = nil;
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithConnection:connection
                                                                    takesOwnership:YES
                                                                             error:&error];
    if (!transport) {
        a840diag::markPort(port, 0x68f2u);
        return nil;
    }

    A840MTLNativePipelineProvider *provider = [[A840MTLNativePipelineProvider alloc] init];
    if (!provider) return nil;

    A840MTLDevice *device = [[A840MTLDevice alloc] initWithTransport:transport
                                                    pipelineProvider:provider
                                                         processRole:policy->proc_role
                                                          registryID:registryID
                                                               error:&error];
    if (!device) {
        a840diag::markPort(port, 0x68f2u);
        return nil;
    }
    a840_mtl_install_phase3_hwtest_adapters();
    a840diag::markPort(port, 0x6802u);

    if (isRenderServer) {
        [device.scanoutBridge startGopScanoutLoopWithAcceleratorPort:port];
    }
    a840diag::markPort(port, 0x6803u);
    return device;
}

} /* namespace */

@implementation NativeAdrenoMetalDevice

- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}

- (id)initWithAcceleratorPort:(unsigned)port
{
    self = [super init];
    if (!self || !port) return nil;
    static id<MTLDevice> sCachedDevice = nil;
    static BOOL sAttempted = NO;
    @synchronized ([NativeAdrenoMetalDevice class]) {
        if (sAttempted) return sCachedDevice;
        sAttempted = YES;
        A840SysPublishPolicy policy{};
        if (!resolvePublishPolicy(&policy)) {
            return nil;
        }
        sCachedDevice = createSystemDeviceForPort(port, &policy);
        return sCachedDevice;
    }
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
