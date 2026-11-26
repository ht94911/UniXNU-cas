/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI. */
#import "A840MTLPipelineProvider.h"

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
    return "Adreno840Metal-Phase2-Native-gen80200";
}
