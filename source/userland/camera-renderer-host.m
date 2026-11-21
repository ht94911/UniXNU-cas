/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
static bool original_flag;
static CGSize up_limit,down_limit;
static unsigned support_calls,init_calls;
static id received_pool;
static bool received_fractional,received_compressed;
@interface FigM2MController : NSObject
+ (CGSize)maxUpscale;
+ (CGSize)maxDownscale;
@end
@implementation FigM2MController
+ (CGSize)maxUpscale {return up_limit;}
+ (CGSize)maxDownscale {return down_limit;}
@end
@interface TestRenderer : NSObject
+ (bool)supportsAcceleratedBorderFill;
- (id)initWithIntermediateBufferPool:(id)pool fractionalSourceRectEnabled:(bool)fractional compressedFormatEnabled:(bool)compressed;
@end
@implementation TestRenderer
+ (bool)supportsAcceleratedBorderFill {++support_calls;return original_flag;}
- (id)initWithIntermediateBufferPool:(id)pool fractionalSourceRectEnabled:(bool)fractional compressedFormatEnabled:(bool)compressed {
 ++init_calls;received_pool=pool;received_fractional=fractional;received_compressed=compressed;return self;
}
@end
#include "NezhaCameraRenderer.m"
int main(void) {@autoreleasepool {
 original_supports=method_getImplementation(class_getClassMethod([TestRenderer class],@selector(supportsAcceleratedBorderFill)));
 original_initializer=method_getImplementation(class_getInstanceMethod([TestRenderer class],@selector(initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:)));
 assert(!scaler_available(Nil));
 NSObject *pool=[NSObject new];
 const CGSize limits[]={{0,0},{8,8},{0,8},{NAN,8},{INFINITY,8}};
 unsigned cases=0;
 for(unsigned limit=0;limit<5;++limit)for(unsigned flag=0;flag<2;++flag){
  up_limit=down_limit=limits[limit];original_flag=flag;const bool usable=limit==1;
  support_calls=0;const bool selected=supports_accelerated((id)[TestRenderer class],@selector(supportsAcceleratedBorderFill));
  assert(selected==(original_flag&&usable)&&support_calls==1);++cases;
  for(unsigned fractional=0;fractional<2;++fractional)for(unsigned compressed=0;compressed<2;++compressed){
   TestRenderer *renderer=[TestRenderer alloc];init_calls=0;
   id result=initialize_renderer(renderer,@selector(initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:),pool,fractional,compressed);
   assert(result==renderer&&init_calls==1&&received_pool==pool&&received_fractional==(bool)fractional&&received_compressed==(compressed&&usable));
   [renderer release];++cases;
  }
 }
 [pool release];printf("PASS %u production renderer forwarding cases: original result, pool, fractional flag, zero/missing/positive scale capabilities\n",cases);
}}
