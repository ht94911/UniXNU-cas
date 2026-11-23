/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
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
static int received_orientation;
static unsigned border_calls;
static int border_status;
static CVPixelBufferRef received_border_buffer;
static CGRect received_border_rect;
static unsigned preview_calls;
static CMSampleBufferRef received_sample;
static id received_input;
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
- (int)borderBlackFillPixelBuffer:(CVPixelBufferRef)buffer validRect:(CGRect)rect;
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst;
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src exifOrientation:(int)orientation intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst;
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
- (int)borderBlackFillPixelBuffer:(CVPixelBufferRef)buffer validRect:(CGRect)rect {
 ++border_calls;assert(transfer_calls==1);received_border_buffer=buffer;received_border_rect=rect;return border_status;
}
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst {
 ++transfer_calls;received_source=source;received_destination=destination;
 received_src=src;received_dst=dst;return transfer_status;
}
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src exifOrientation:(int)orientation intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst {
 received_orientation=orientation;
 return [self scalePixelBuffer:source rect:src intoPixelBuffer:destination rect:dst];
}
@end
@interface TestPreviewSink : NSObject
- (void)renderSampleBuffer:(CMSampleBufferRef)sample forInput:(id)input;
@end
@implementation TestPreviewSink
- (void)renderSampleBuffer:(CMSampleBufferRef)sample forInput:(id)input {
 ++preview_calls;received_sample=sample;received_input=input;
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
 original_border=method_getImplementation(class_getInstanceMethod([TestRenderer class],@selector(borderBlackFillPixelBuffer:validRect:)));
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
 CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
 CVBufferRemoveAttachment(source,CFSTR("MetadataDictionary"));
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
    CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanFalse,kCVAttachmentMode_ShouldPropagate);
    transfer_calls=0;
    assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,empty,destination,target,false)==transfer_status);
    assert(transfer_calls==1&&!memcmp(&received_src,&empty,sizeof(empty)));++cases;
 }
 CVPixelBufferRef portrait=NULL;
 assert(!CVPixelBufferCreate(NULL,12,26,kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,NULL,&portrait));
 const RendererRect active={{0,4},{12,16}},full={{0,0},{12,26}};
 CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
 up_limit=down_limit=CGSizeMake(0,0);
 for(unsigned failure=0;failure<2;++failure) {
   transfer_calls=border_calls=0;transfer_status=0;border_status=failure?-12783:0;
   assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,empty,portrait,active,true)==border_status);
   assert(transfer_calls==1&&border_calls==1&&received_border_buffer==portrait&&
       !memcmp(&received_border_rect,&active,sizeof(active))&&!memcmp(&received_src,&whole,sizeof(whole)));++cases;
 }
 transfer_calls=border_calls=0;transfer_status=-12784;border_status=0;
 assert(transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,whole,portrait,active,true)==-12784);
 assert(transfer_calls==1&&!border_calls);++cases;
 const RendererRect excluded[]={full,{{0,4},{0,16}},{{0,4},{12,24}},{{0,3},{12,16}},{{0,4},{11,16}},{{0,NAN},{12,16}}};
 transfer_status=0;
 for(unsigned i=0;i<sizeof(excluded)/sizeof(excluded[0]);++i) {
   transfer_calls=border_calls=0;
   assert(!transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,whole,portrait,excluded[i],true));
   assert(transfer_calls==1&&!border_calls);++cases;
 }
 for(unsigned reason=0;reason<3;++reason) {
   CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),reason==0?kCFBooleanFalse:kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
   up_limit=down_limit=CGSizeMake(reason==1?8:0,reason==1?8:0);
   transfer_calls=border_calls=0;
   assert(!transfer_pixels(renderer,@selector(doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:),NULL,source,whole,portrait,active,reason!=2));
   assert(transfer_calls==1&&!border_calls);++cases;
 }
 original_scale=method_getImplementation(class_getInstanceMethod([TestRenderer class],@selector(scalePixelBuffer:rect:intoPixelBuffer:rect:)));
 original_oriented_scale=method_getImplementation(class_getInstanceMethod([TestRenderer class],@selector(scalePixelBuffer:rect:exifOrientation:intoPixelBuffer:rect:)));
 /* Actual empty/fractional rectangles, null buffers and failures must
  * remain unchanged across the native Objective-C ABI boundary. */
 for(unsigned i=0;i<3;++i)for(unsigned oriented=0;oriented<2;++oriented) {
   RendererRect src=rectangles[i],dst=rectangles[(i+1)%3];
   transfer_calls=0;transfer_status=i?-12783:0;received_orientation=0;
   CVPixelBufferRef input=i==2?NULL:source,output=i==1?NULL:portrait;
   int status=oriented ? scale_oriented_pixels(renderer,@selector(scalePixelBuffer:rect:exifOrientation:intoPixelBuffer:rect:),input,src,6,output,dst) :
     scale_pixels(renderer,@selector(scalePixelBuffer:rect:intoPixelBuffer:rect:),input,src,output,dst);
   assert(status==transfer_status&&transfer_calls==1&&received_source==input&&received_destination==output);
   assert(!memcmp(&received_src,&src,sizeof src)&&!memcmp(&received_dst,&dst,sizeof dst));
   assert(received_orientation==(oriented?6:0));++cases;
 }
 CFRelease(portrait);
 /* The final observer must forward null and real samples once, preserve
  * timestamps/attachments, and read only active pixels of padded planes. */
 original_preview_sink=method_getImplementation(class_getInstanceMethod([TestPreviewSink class],@selector(renderSampleBuffer:forInput:)));
 TestPreviewSink *sink=[TestPreviewSink new];
 const uint32_t formats[]={kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
     kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,kCVPixelFormatType_32BGRA};
 for(unsigned f=0;f<3;++f) {
   CVPixelBufferRef pixel=NULL;
   assert(!CVPixelBufferCreate(NULL,12,26,formats[f],NULL,&pixel));
   assert(!CVPixelBufferLockBaseAddress(pixel,0));
   const bool planar=CVPixelBufferIsPlanar(pixel);
   uint8_t *base=planar?CVPixelBufferGetBaseAddressOfPlane(pixel,0):CVPixelBufferGetBaseAddress(pixel);
   size_t row=planar?CVPixelBufferGetBytesPerRowOfPlane(pixel,0):CVPixelBufferGetBytesPerRow(pixel);
   memset(base,165,row*26);
   for(size_t y=0;y<26;++y)for(size_t x=0;x<12;++x) {
     if(planar)base[y*row+x]=(uint8_t)(x+3*y);
     else {uint8_t *at=base+y*row+x*4;at[0]=at[1]=at[2]=(uint8_t)(x+3*y);at[3]=255;}
   }
   if(planar) {
     uint8_t *uv=CVPixelBufferGetBaseAddressOfPlane(pixel,1);
     size_t uvrow=CVPixelBufferGetBytesPerRowOfPlane(pixel,1);
     memset(uv,165,uvrow*13);
     for(size_t y=0;y<13;++y)for(size_t x=0;x<6;++x) {uv[y*uvrow+2*x]=91;uv[y*uvrow+2*x+1]=173;}
   }
   assert(!CVPixelBufferUnlockBaseAddress(pixel,0));
   struct PreviewPixels observed=preview_pixels(pixel);
   assert(!observed.lock_status&&!observed.unlock_status&&observed.count==25&&observed.nonzero==24&&
       observed.minimum==0&&observed.maximum==86);
   for(unsigned gy=0;gy<5;++gy)for(unsigned gx=0;gx<5;++gx)
     assert(observed.luma[5*gy+gx]==(11*gx/4)+3*(25*gy/4));
   if(planar)assert(observed.u==91&&observed.v==173);
   ++cases;
   CMVideoFormatDescriptionRef description=NULL;
   assert(!CMVideoFormatDescriptionCreateForImageBuffer(NULL,pixel,&description));
   CMSampleTimingInfo timing={.duration=CMTimeMake(1,30),.presentationTimeStamp=CMTimeMake(7,30),.decodeTimeStamp=kCMTimeInvalid};
   CMSampleBufferRef sample=NULL;
   assert(!CMSampleBufferCreateReadyWithImageBuffer(NULL,pixel,description,&timing,&sample));
   CMSetAttachment(sample,CFSTR("FrameIsBlackened"),kCFBooleanTrue,kCMAttachmentMode_ShouldPropagate);
   NSDictionary *info=@{@"FrameSequence":@7};
   CMSetAttachment(sample,CFSTR("MetadataDictionary"),(CFDictionaryRef)info,kCMAttachmentMode_ShouldPropagate);
   CVBufferSetAttachment(pixel,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
   preview_calls=0;
   render_preview_sample(sink,@selector(renderSampleBuffer:forInput:),sample,pool);
   assert(preview_calls==1&&received_sample==sample&&received_input==pool);
   assert(CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample),timing.presentationTimeStamp)==0);
   assert(CMGetAttachment(sample,CFSTR("FrameIsBlackened"),NULL)==kCFBooleanTrue);
   assert(CMGetAttachment(sample,CFSTR("MetadataDictionary"),NULL)==(CFDictionaryRef)info);
   struct PreviewPixels after=preview_pixels(pixel);
   assert(!memcmp(observed.luma,after.luma,sizeof observed.luma));++cases;
   CFRelease(sample);CFRelease(description);CFRelease(pixel);
 }
 for(unsigned i=0;i<10;++i) {
   preview_calls=0;
   render_preview_sample(sink,@selector(renderSampleBuffer:forInput:),NULL,i?pool:nil);
   assert(preview_calls==1&&received_sample==NULL&&received_input==(i?pool:nil));++cases;
 }
 [sink release];
 CFRelease(source);CFRelease(destination);[renderer release];
 [pool release];printf("PASS %u production renderer forwarding cases: original results, pool, flags, capabilities and exact CGRect arguments\n",cases);
}}
