// SPDX-License-Identifier: MIT
// Exercise the original system transfer API with known NV12 surfaces.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#import <CoreGraphics/CoreGraphics.h>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void report(NSDictionary *value) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if(data){fwrite(data.bytes,1,data.length,stdout);fputc('\n',stdout);fflush(stdout);}
}

static bool ownMemoryLimit() {
    // XNU12377 kern_memorystatus.h: commands 7/8 use this 16-byte record.
    struct Limits {int32_t active;uint32_t activeAttributes;int32_t inactive;uint32_t inactiveAttributes;};
    static_assert(sizeof(Limits)==16);
    using Control=int (*)(uint32_t,int32_t,uint32_t,void *,size_t);
    auto control=(Control)dlsym(RTLD_DEFAULT,"memorystatus_control");
    const int32_t pid=getpid();
    if(!control){report(@{@"phase":@"memory-limit",@"passed":@NO,@"error":@"system interface unavailable"});return false;}
    Limits before={};errno=0;
    const int queried=control(8,pid,0,&before,sizeof before),queryError=errno;
    report(@{@"phase":@"memory-limit",@"state":@"before",@"pid":@(pid),@"status":@(queried),@"errno":@(queryError),@"active_MB":@(before.active),@"inactive_MB":@(before.inactive)});
    if(queried)return false;
    const Limits wanted={768,1,768,1}; // Finite hard limits, for this test process only.
    if(std::memcmp(&before,&wanted,sizeof wanted)) {
        Limits requested=wanted;errno=0;
        const int changed=control(7,pid,0,&requested,sizeof requested),changeError=errno;
        report(@{@"phase":@"memory-limit",@"state":@"set",@"pid":@(pid),@"status":@(changed),@"errno":@(changeError)});
        if(changed)return false;
    }
    Limits after={};errno=0;
    const int checked=control(8,pid,0,&after,sizeof after),checkError=errno;
    const bool passed=!checked && !std::memcmp(&after,&wanted,sizeof wanted);
    report(@{@"phase":@"memory-limit",@"state":@"verified",@"pid":@(pid),@"status":@(checked),@"errno":@(checkError),@"active_MB":@(after.active),@"inactive_MB":@(after.inactive),@"active_attributes":@(after.activeAttributes),@"inactive_attributes":@(after.inactiveAttributes),@"passed":@(passed)});
    return passed;
}

static CVPixelBufferRef pixel(unsigned width,unsigned height,bool full) {
    CVPixelBufferRef image=nullptr;
    NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    const auto format=full?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    return CVPixelBufferCreate(kCFAllocatorDefault,width,height,format,(__bridge CFDictionaryRef)attributes,&image)==0?image:nullptr;
}

static bool fill(CVPixelBufferRef image,bool bars,unsigned yValue,unsigned uValue,unsigned vValue) {
    if(CVPixelBufferLockBaseAddress(image,0))return false;
    auto *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    auto *uv=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const size_t yr=CVPixelBufferGetBytesPerRowOfPlane(image,0),ur=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    for(size_t row=0;row<height;++row)
        std::memset(y+row*yr,bars&&(row<height/4||row>=3*height/4)?0:yValue,width);
    for(size_t row=0;row<height/2;++row)for(size_t x=0;x<width;x+=2){uv[row*ur+x]=uValue;uv[row*ur+x+1]=vValue;}
    CVBufferSetAttachment(image,kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_601_4,kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(image,kCVImageBufferColorPrimariesKey,kCVImageBufferColorPrimaries_ITU_R_709_2,kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(image,kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_sRGB,kCVAttachmentMode_ShouldPropagate);
    return !CVPixelBufferUnlockBaseAddress(image,0);
}



@protocol NezhaBWVideoFormat
- (void)setWidth:(NSUInteger)width;
- (void)setHeight:(NSUInteger)height;
- (void)setExtendedWidth:(NSUInteger)width;
- (void)setExtendedHeight:(NSUInteger)height;
- (void)setPixelFormat:(uint32_t)format;
@end
@interface NSObject (NativeRenderer121)
+ (bool)supportsAcceleratedBorderFill;
- (void)setWidth:(NSUInteger)width;
- (void)setHeight:(NSUInteger)height;
- (void)setExtendedWidth:(NSUInteger)width;
- (void)setExtendedHeight:(NSUInteger)height;
- (void)setPixelFormat:(uint32_t)format;
- (instancetype)initWithVideoFormat:(id)format capacity:(NSUInteger)capacity name:(NSString *)name;
- (instancetype)initWithIntermediateBufferPool:(id)pool fractionalSourceRectEnabled:(bool)fractional compressedFormatEnabled:(bool)compressed;
- (int)doPixelRotationTransferWithSession:(VTPixelRotationSessionRef)session sourceBuffer:(CVPixelBufferRef)source sourceRect:(CGRect)src destinationBuffer:(CVPixelBufferRef)destination destinationRect:(CGRect)dst blackFillUnalignedEdges:(bool)fill;
@end
static bool methodABI(Class cls,bool classMethod,const char *selector,const char *expected) {
 Method method=classMethod?class_getClassMethod(cls,sel_registerName(selector)):class_getInstanceMethod(cls,sel_registerName(selector));
 const char *types=method?method_getTypeEncoding(method):nullptr;bool passed=types&&!strcmp(types,expected);
 report(@{@"phase":@"original-renderer-ABI",@"selector":@(selector),@"encoding":types?@(types):@"",@"passed":@(passed)});return passed;
}
static bool inspect(CVPixelBufferRef image,NSString *name,int expectedCenter) {
    const CVReturn locked=CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    if(locked){report(@{@"phase":@"original-renderer-pixels",@"case":name,@"lock_status":@(locked),@"passed":@NO});return false;}
    const auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const auto *uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const size_t yr=CVPixelBufferGetBytesPerRowOfPlane(image,0),ur=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    uint64_t nonzero=0,unchanged=0,total=0,badUV=0;unsigned low=255,high=0;
    NSMutableArray *rows=[NSMutableArray array];
    for(size_t row=0;row<height;++row)for(size_t x=0;x<width;++x){
        const unsigned value=y[row*yr+x];nonzero+=value!=0;unchanged+=value==165;
        low=std::min(low,value);high=std::max(high,value);++total;
    }
    for(size_t row=0;row<height/2;++row)for(size_t x=0;x<width;++x)
        badUV+=std::abs(int(uv[row*ur+x])-128)>2;
    for(unsigned row=0;row<8;++row){NSMutableArray *values=[NSMutableArray array];
        for(unsigned x=0;x<8;++x)[values addObject:@(y[(height-1)*row/7*yr+(width-1)*x/7])];
        [rows addObject:values];
    }
    const unsigned center=y[height/2*yr+width/2];
    const unsigned centerU=uv[height/4*ur+(width/2&~size_t(1))];
    const unsigned centerV=uv[height/4*ur+(width/2&~size_t(1))+1];
    const bool compared=expectedCenter>=0;
    const bool pixelsCorrect=!compared||
        (std::abs(int(center)-expectedCenter)<=2&&badUV==0&&unchanged==0&&
         (expectedCenter==0?nonzero==0:nonzero>total/8));
    const CVReturn unlocked=CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    const bool passed=pixelsCorrect&&unlocked==0;
    report(@{@"phase":@"original-renderer-pixels",@"case":name,@"width":@(width),@"height":@(height),
        @"center_YUV":@[@(center),@(centerU),@(centerV)],@"expected_center_Y":@(expectedCenter),
        @"comparison_performed":@(compared),@"range_Y":@[@(low),@(high)],@"nonzero_Y":@(nonzero),
        @"unchanged_Y":@(unchanged),@"total_Y":@(total),@"bad_UV":@(badUV),@"rows":rows,
        @"unlock_status":@(unlocked),@"passed":@(passed),@"capture_started":@NO});
    return passed;
}

int main(int argc,char **argv) {@autoreleasepool {
 (void)argc;(void)argv;if(!ownMemoryLimit())return 11;sleep(150);
 char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
 report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"original_renderer":@YES,@"capture_started":@NO});
 void *capture=dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture",RTLD_NOW|RTLD_GLOBAL);
 Class rendererClass=capture?objc_getClass("BWPixelBufferTransferRenderer"):Nil;
 Class formatClass=capture?objc_getClass("BWVideoFormat"):Nil;Class poolClass=capture?objc_getClass("BWPixelBufferPool"):Nil;
 if(!rendererClass||!formatClass||!poolClass)return 12;
 if(!methodABI(rendererClass,true,"supportsAcceleratedBorderFill","B16@0:8")||
    !methodABI(rendererClass,false,"initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:","@32@0:8@16B24B28")||
    !methodABI(rendererClass,false,"doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:","i108@0:8^{OpaqueVTPixelRotationSession=}16^{__CVBuffer=}24{CGRect={CGPoint=dd}{CGSize=dd}}32^{__CVBuffer=}64{CGRect={CGPoint=dd}{CGSize=dd}}72B104")||
    !methodABI(poolClass,false,"initWithVideoFormat:capacity:name:","@40@0:8@16Q24@32"))return 13;
 const bool before=[rendererClass supportsAcceleratedBorderFill];
 void *library=dlopen("/System/Library/MediaCapture/libNezhaCamera27.dylib",RTLD_NOW|RTLD_LOCAL);
 auto observe=library?(void (*)(void))dlsym(library,"nezha_camera_renderer_observe"):nullptr;
 report(@{@"phase":@"fallback-library",@"present":@(library!=nullptr),@"observer":@(observe!=nullptr)});if(!observe)return 14;
 observe();const bool after=[rendererClass supportsAcceleratedBorderFill];
 report(@{@"phase":@"original-renderer-support",@"before":@(before),@"after":@(after)});
 id<NezhaBWVideoFormat> format=(id<NezhaBWVideoFormat>)[formatClass new];[format setWidth:1200];[format setHeight:2608];[format setExtendedWidth:1200];[format setExtendedHeight:2608];[format setPixelFormat:kCVPixelFormatType_420YpCbCr8BiPlanarFullRange];
 id pool=[[poolClass alloc] initWithVideoFormat:format capacity:3 name:@"Nezha authored renderer fixture"];
 id renderer=pool?[[rendererClass alloc] initWithIntermediateBufferPool:pool fractionalSourceRectEnabled:false compressedFormatEnabled:true]:nil;
 report(@{@"phase":@"original-renderer-create",@"format":@(format!=nil),@"pool":@(pool!=nil),@"renderer":@(renderer!=nil)});if(!renderer)return 15;
 CVPixelBufferRef source=pixel(1600,1200,true),output=pixel(1200,2608,true);if(!source||!output)return 3;
 if(!fill(source,false,107,128,128)||!fill(output,false,165,165,165))return 4;
 VTPixelRotationSessionRef session=nullptr;OSStatus created=VTPixelRotationSessionCreate(kCFAllocatorDefault,&session);
 report(@{@"phase":@"original-rotation-session",@"status":@(created),@"present":@(session!=nullptr)});if(created||!session)return 16;
 int status=[renderer doPixelRotationTransferWithSession:session sourceBuffer:source sourceRect:CGRectMake(0,0,1600,1200) destinationBuffer:output destinationRect:CGRectMake(0,0,1200,2608) blackFillUnalignedEdges:false];
 report(@{@"phase":@"original-renderer-call",@"status":@(status),@"capture_started":@NO});
 bool pixels=inspect(output,@"original-BWPixelBufferTransferRenderer",status? -1:107);const bool passed=!status&&pixels&&!after;
 VTPixelRotationSessionInvalidate(session);CFRelease(session);CVPixelBufferRelease(source);CVPixelBufferRelease(output);
 report(@{@"phase":@"result",@"passed":@(passed),@"original_renderer":@YES,@"capture_started":@NO});return passed?0:10;
}}
