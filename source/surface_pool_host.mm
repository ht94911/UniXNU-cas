// SPDX-License-Identifier: MIT
// Real CoreVideo pool recycling with cached production textures and GPU reads
// held behind unsignaled shared events. No buffer or completion is synthesized.
#import "../NativeBridgeMetalFrontend.mm"
#import <CoreVideo/CoreVideo.h>
#include <cassert>
#include <cstdio>

static CVPixelBufferPoolRef makePool(unsigned capacity) {
 CVPixelBufferPoolRef pool=nullptr;
 NSDictionary *options=@{(id)kCVPixelBufferPoolMinimumBufferCountKey:@(capacity)};
 NSDictionary *pixels=@{(id)kCVPixelBufferWidthKey:@64,(id)kCVPixelBufferHeightKey:@16,
  (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
  (id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES};
 assert(CVPixelBufferPoolCreate(nullptr,(__bridge CFDictionaryRef)options,(__bridge CFDictionaryRef)pixels,&pool)==kCVReturnSuccess);
 return pool;
}
static CVReturn takePixel(CVPixelBufferPoolRef pool,unsigned capacity,CVPixelBufferRef *pixel) {
 NSDictionary *limit=@{(id)kCVPixelBufferPoolAllocationThresholdKey:@(capacity)};
 *pixel=nullptr;
 return CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr,pool,(__bridge CFDictionaryRef)limit,pixel);
}
static void expectBlocked(CVPixelBufferPoolRef pool) {
 CVPixelBufferRef pixel=nullptr;
 assert(takePixel(pool,1,&pixel)==kCVReturnWouldExceedAllocationThreshold && !pixel);
}
static void pattern(CVPixelBufferRef pixel,unsigned seed) {
 assert(CVPixelBufferLockBaseAddress(pixel,0)==kCVReturnSuccess);
 auto *bytes=(unsigned char *)CVPixelBufferGetBaseAddress(pixel);
 const size_t row=CVPixelBufferGetBytesPerRow(pixel);
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<256;++x)bytes[y*row+x]=(unsigned char)(seed+x*3+y*7);
 assert(CVPixelBufferUnlockBaseAddress(pixel,0)==kCVReturnSuccess);
}
static NBTexture *importPixel(id<MTLDevice> device,CVPixelBufferRef pixel) {
 MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:16 mipmapped:NO];
 descriptor.storageMode=MTLStorageModeShared;
 descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
 NBTexture *texture=(NBTexture *)[device newTextureWithDescriptor:descriptor iosurface:CVPixelBufferGetIOSurface(pixel) plane:0];
 assert(texture);
 return texture;
}
static void readTexture(id<MTLBlitCommandEncoder> blit,id<MTLTexture> texture,id<MTLBuffer> output,NSUInteger offset=0) {
 [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,16,1)
  toBuffer:output destinationOffset:offset destinationBytesPerRow:256 destinationBytesPerImage:4096];
}
static void verifyBytes(id<MTLBuffer> output,unsigned seed,NSUInteger offset=0) {
 auto *bytes=(const unsigned char *)output.contents+offset;
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<256;++x)assert(bytes[y*256+x]==(unsigned char)(seed+x*3+y*7));
}
static void finishBounded(id<MTLCommandBuffer> command) {
 auto finished=dispatch_semaphore_create(0);
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool{[command waitUntilCompleted];}dispatch_semaphore_signal(finished);});
 assert(dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))==0);
}

int main(int argc,char **argv) {
 if(argc!=4)return 2;
 @autoreleasepool {
  const bool legacy=!strcmp(argv[3],"legacy");
  id<MTLDevice> device=native_bridge_metal_open(argv[1],argv[2],0x6e6d62,nullptr);assert(device);
  id<MTLCommandQueue> queue=[device newCommandQueue];assert(queue);
  NSMutableDictionary *cached=[NSMutableDictionary dictionary];
  CVPixelBufferPoolRef pool=makePool(13);
  unsigned frames=0;
  for(;frames<64;++frames) {
   @autoreleasepool {
    CVPixelBufferRef pixel=nullptr;
    CVReturn result=takePixel(pool,13,&pixel);
    if(result!=kCVReturnSuccess) {
     assert(legacy && frames==13 && result==kCVReturnWouldExceedAllocationThreshold && !pixel);
     break;
    }
    pattern(pixel,frames+1);
    IOSurfaceRef surface=CVPixelBufferGetIOSurface(pixel);NSNumber *key=@(IOSurfaceGetID(surface));
    NSArray *textures=cached[key];
    if(!textures) {
     NBTexture *root=importPixel(device,pixel);
     id<MTLTexture> view=[root newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
     id<MTLTexture> nested=[view newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
     assert(view && nested);textures=@[root,view,nested];cached[key]=textures;
    }
    assert(legacy?IOSurfaceIsInUse(surface):!IOSurfaceIsInUse(surface));
    id<MTLBuffer> output=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    readTexture(blit,textures.lastObject,output);[blit endEncoding];[command commit];finishBounded(command);
    assert(command.status==MTLCommandBufferStatusCompleted);verifyBytes(output,frames+1);
    assert(legacy?IOSurfaceIsInUse(surface):!IOSurfaceIsInUse(surface));
    CVPixelBufferRelease(pixel);
   }
  }
  if(legacy) {
   assert(frames==13 && cached.count==13);
   puts("PASS legacy: real CoreVideo pool exhausted at 13 cached surfaces after completed GPU reads");
   [cached removeAllObjects];CFRelease(pool);return 0;
  }
  assert(frames==64 && cached.count<=13);[cached removeAllObjects];CFRelease(pool);

  // Independent commands hold independent uses. Multiple roots/views/bindings
  // in one command hold a single use until the actual GPU read completes.
  pool=makePool(1);CVPixelBufferRef pixel=nullptr;assert(takePixel(pool,1,&pixel)==kCVReturnSuccess);
  pattern(pixel,97);NBTexture *root=importPixel(device,pixel);
  id<MTLTexture> view=[root newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
  id<MTLTexture> nested=[view newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
  IOSurfaceRef surface=root.surface;const uint32_t surfaceID=IOSurfaceGetID(surface);
  assert(!IOSurfaceIsInUse(surface));
  id<MTLSharedEvent> event=[device newSharedEvent];
  id<MTLBuffer> a=[device newBufferWithLength:12288 options:MTLResourceStorageModeShared];
  id<MTLBuffer> b=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> first=[queue commandBuffer],second=[queue commandBuffer];
  [first encodeWaitForEvent:event value:1];id<MTLBlitCommandEncoder> blit=[first blitCommandEncoder];
  readTexture(blit,root,a);readTexture(blit,view,a,4096);readTexture(blit,nested,a,8192);[blit endEncoding];blit=nil;
  assert(IOSurfaceGetUseCount(surface)==1);
  [second encodeWaitForEvent:event value:2];blit=[second blitCommandEncoder];readTexture(blit,nested,b);[blit endEncoding];blit=nil;
  assert(IOSurfaceGetUseCount(surface)==2);CVPixelBufferRelease(pixel);expectBlocked(pool);
  [first commit];[second commit];usleep(100000);
  assert(first.status!=MTLCommandBufferStatusCompleted && second.status!=MTLCommandBufferStatusCompleted);
  expectBlocked(pool);assert(IOSurfaceGetUseCount(surface)==2);
  event.signaledValue=1;finishBounded(first);assert(first.status==MTLCommandBufferStatusCompleted);
  verifyBytes(a,97);verifyBytes(a,97,4096);verifyBytes(a,97,8192);
  assert(IOSurfaceGetUseCount(surface)==1);expectBlocked(pool);
  event.signaledValue=2;finishBounded(second);assert(second.status==MTLCommandBufferStatusCompleted);verifyBytes(b,97);
  assert(!IOSurfaceIsInUse(surface));assert(takePixel(pool,1,&pixel)==kCVReturnSuccess);
  assert(IOSurfaceGetID(CVPixelBufferGetIOSurface(pixel))==surfaceID);CVPixelBufferRelease(pixel);

  // An unsubmitted encoder keeps the surface active, then releases on discard.
  @autoreleasepool {
   id<MTLCommandBuffer> discarded=[queue commandBuffer];blit=[discarded blitCommandEncoder];
   readTexture(blit,nested,b);[blit endEncoding];blit=nil;encodeDrain();
   assert(IOSurfaceGetUseCount(surface)==1);expectBlocked(pool);discarded=nil;
  }
  for(unsigned i=0;i<500 && IOSurfaceIsInUse(surface);++i)usleep(1000);
  assert(!IOSurfaceIsInUse(surface));assert(takePixel(pool,1,&pixel)==kCVReturnSuccess);CVPixelBufferRelease(pixel);

  id<MTLCommandBuffer> refused=[queue commandBuffer];blit=[refused blitCommandEncoder];
  readTexture(blit,nested,b);[blit fillBuffer:nil range:NSMakeRange(0,16) value:0];[blit endEncoding];blit=nil;
  assert(IOSurfaceGetUseCount(surface)==1);[refused commit];finishBounded(refused);
  assert(refused.status==MTLCommandBufferStatusError && !IOSurfaceIsInUse(surface));
  assert(takePixel(pool,1,&pixel)==kCVReturnSuccess);CVPixelBufferRelease(pixel);

  // Compute texture binding and render attachments use the same protection.
  id<MTLCommandBuffer> bindings=[queue commandBuffer];
  id<MTLComputeCommandEncoder> compute=[bindings computeCommandEncoder];[compute setTexture:nested atIndex:0];[compute endEncoding];compute=nil;
  auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=root;
  pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  [[bindings renderCommandEncoderWithDescriptor:pass] endEncoding];
  blit=[bindings blitCommandEncoder];readTexture(blit,nested,b);[blit endEncoding];blit=nil;
  assert(IOSurfaceGetUseCount(surface)==1);[bindings commit];finishBounded(bindings);
  assert(bindings.status==MTLCommandBufferStatusCompleted && !IOSurfaceIsInUse(surface));verifyBytes(b,97);
  // A buffer imported through an IOSurface retains its own surface identity.
  id<MTLCommandBuffer> bufferUse=[queue commandBuffer];blit=[bufferUse blitCommandEncoder];
  [blit copyFromBuffer:root.buffer sourceOffset:0 toBuffer:b destinationOffset:0 size:16];[blit endEncoding];blit=nil;
  assert(IOSurfaceGetUseCount(surface)==1);[bufferUse commit];finishBounded(bufferUse);
  assert(bufferUse.status==MTLCommandBufferStatusCompleted && !IOSurfaceIsInUse(surface));
  CFRelease(pool);
  puts("PASS current: 64 recycled GPU frames; gated independent commands, cached views, discard, refusal, compute/render and imported-buffer uses");
 }
 return 0;
}
