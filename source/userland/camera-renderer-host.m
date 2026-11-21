/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
static bool original_flag;
static CGSize up_limit,down_limit;
static unsigned support_calls,init_calls;
static id received_pool;
static bool received_fractional,received_compressed,received_fill;
static unsigned transfer_calls;
static int transfer_status;
static CVPixelBufferRef received_source,received_destination;
static CGRect received_src,received_dst;
struct OpaqueVTPixelRotationSession;

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
- (int)doPixelRotationTransferWithSession:(struct OpaqueVTPixelRotationSession *)session sourceBuffer:(CVPixelBufferRef)source sourceRect:(CGRect)src destinationBuffer:(CVPixelBufferRef)destination destinationRect:(CGRect)dst blackFillUnalignedEdges:(bool)fill;
@end
@implementation TestRenderer
+ (bool)supportsAcceleratedBorderFill {++support_calls;return original_flag;}
- (id)initWithIntermediateBufferPool:(id)pool fractionalSourceRectEnabled:(bool)fractional compressedFormatEnabled:(bool)compressed {
 ++init_calls;received_pool=pool;received_fractional=fractional;received_compressed=compressed;return self;
}
- (int)doPixelRotationTransferWithSession:(struct OpaqueVTPixelRotationSession *)session sourceBuffer:(CVPixelBufferRef)source sourceRect:(CGRect)src destinationBuffer:(CVPixelBufferRef)destination destinationRect:(CGRect)dst blackFillUnalignedEdges:(bool)fill {
 assert(!session);++transfer_calls;received_source=source;received_destination=destination;
 received_src=src;received_dst=dst;received_fill=fill;return transfer_status;
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
 original_transfer=method_getImplementation(class_getInstanceMethod([TestRenderer class],@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:)));
 TestRenderer *renderer=[TestRenderer alloc];
 CVPixelBufferRef source=NULL,destination=NULL;
 assert(!CVPixelBufferCreate(NULL,4,6,kCVPixelFormatType_32BGRA,NULL,&source));
 assert(!CVPixelBufferCreate(NULL,6,4,kCVPixelFormatType_32BGRA,NULL,&destination));
 const RendererRect rectangles[]={{{0,0},{1600,1200}},{{.5,-.25},{639.75,479.125}},{{801,601},{0,0}}};
 for(unsigned i=0;i<3;++i)for(unsigned fill=0;fill<2;++fill) {
   transfer_calls=0;transfer_status=i? -12783:0;
   const RendererRect src=rectangles[i],dst=rectangles[(i+1)%3];
   assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,src,destination,dst,fill)==transfer_status);
   assert(transfer_calls==1&&received_source==source&&received_destination==destination&&received_fill==(bool)fill);
   assert(!memcmp(&received_src,&src,sizeof(src))&&!memcmp(&received_dst,&dst,sizeof(dst)));++cases;
 }
 NSDictionary *metadata=@{@"FrameSequence":@7,@"RawSensorWidth":@4096,@"RawSensorHeight":@3072,@"ZoomFactor":@1};
 CVBufferSetAttachment(source,CFSTR("MetadataDictionary"),(CFDictionaryRef)metadata,kCVAttachmentMode_ShouldPropagate);
 const RendererRect empty={{801,601},{0,0}},whole={{0,0},{4,6}},target={{0,378},{1200,1602}};
 transfer_calls=0;transfer_status=-12783;
 assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,empty,destination,target,true)==transfer_status);
 assert(transfer_calls==1&&!memcmp(&received_src,&whole,sizeof(whole))&&!memcmp(&received_dst,&target,sizeof(target))&&received_fill);++cases;
 transfer_calls=0;
 assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,rectangles[1],destination,target,false)==transfer_status);
 assert(transfer_calls==1&&!memcmp(&received_src,&rectangles[1],sizeof(RendererRect)));++cases;
 const NSArray *invalid=@[@{@"FrameSequence":@7,@"RawSensorWidth":@4096,@"RawSensorHeight":@3072,@"ZoomFactor":@2},
    @{@"FrameSequence":@0,@"RawSensorWidth":@4096,@"RawSensorHeight":@3072,@"ZoomFactor":@1},
    @{@"FrameSequence":@7,@"RawSensorWidth":@4096,@"ZoomFactor":@1}];
 for(NSDictionary *value in invalid) {
    CVBufferSetAttachment(source,CFSTR("MetadataDictionary"),(CFDictionaryRef)value,kCVAttachmentMode_ShouldPropagate);
    transfer_calls=0;
    assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,empty,destination,target,false)==transfer_status);
    assert(transfer_calls==1&&!memcmp(&received_src,&empty,sizeof(empty)));++cases;
 }
 CFRelease(source);CFRelease(destination);[renderer release];
 [pool release];printf("PASS %u production renderer forwarding cases: original results, pool, flags, capabilities and exact CGRect arguments\n",cases);
}}
