/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Phase 6 zero-fallback production bundle entry: pure native Adreno 840 path,
 * zero fallback router or legacy bridge symbols. */
#import "A840MTLPipelineProvider.h"
#include "../core/a840_production_router.h"

@interface Adreno840MetalPrincipal : NSObject
+ (id<MTLDevice>)createDirectDevice:(NSError **)error;
@end

@implementation Adreno840MetalPrincipal
+ (id<MTLDevice>)createDirectDevice:(NSError **)error
{
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
