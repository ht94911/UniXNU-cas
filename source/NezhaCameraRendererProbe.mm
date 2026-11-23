// SPDX-License-Identifier: MIT
// Exercise the original rectangular scaling API on the real preview geometry.
// No camera, real light, approximate uniforms or new logging facility is used.
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
- (int)borderBlackFillPixelBuffer:(CVPixelBufferRef)buffer validRect:(CGRect)rect;
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst;
- (int)scalePixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src exifOrientation:(int)orientation intoPixelBuffer:(CVPixelBufferRef)destination rect:(CGRect)dst;
- (void)invalidate;
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

static bool fillBandPattern(CVPixelBufferRef image) {
    if(!fill(image,false,0,128,128)||CVPixelBufferLockBaseAddress(image,0))return false;
    auto *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,0);
    const uint8_t values[]={181,51,219,107};
    for(size_t row=0;row<1602;++row)for(size_t x=0;x<1200;++x)
        y[(row+378)*stride+x]=values[(row>=801?2:0)+(x>=600?1:0)];
    return !CVPixelBufferUnlockBaseAddress(image,0);
}
static bool inspectPattern(CVPixelBufferRef image,NSString *name,CGRect destinationRect,bool clockwise,bool checkOutside=true) {
    if(!inspect(image,name,-1)||CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return false;
    const auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const size_t rows=CVPixelBufferGetBytesPerRowOfPlane(image,0);
    const unsigned plain[]={51,107,181,219},rotated[]={181,51,219,107};
    const unsigned *expected=clockwise?rotated:plain;
    NSMutableArray *actual=[NSMutableArray array];bool passed=true;
    for(unsigned quadrant=0;quadrant<4;++quadrant) {
        const size_t x=(size_t)(destinationRect.origin.x+destinationRect.size.width*(quadrant%2?.75:.25));
        const size_t row=(size_t)(destinationRect.origin.y+destinationRect.size.height*(quadrant/2?.75:.25));
        const unsigned value=y[row*rows+x];[actual addObject:@(value)];
        passed=passed&&std::abs(int(value)-int(expected[quadrant]))<=2;
    }
    if(clockwise&&checkOutside)passed=passed&&y[0]==0&&y[(CVPixelBufferGetHeight(image)-1)*rows]==0;
    const CVReturn unlocked=CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    passed=passed&&!unlocked;
    report(@{@"phase":@"original-renderer-pattern",@"case":name,@"clockwise_90":@(clockwise),
        @"actual_quadrants":actual,@"expected_quadrants":@[@(expected[0]),@(expected[1]),@(expected[2]),@(expected[3])],
        @"destination_rect":@[@(destinationRect.origin.x),@(destinationRect.origin.y),@(destinationRect.size.width),@(destinationRect.size.height)],
        @"passed":@(passed),@"capture_started":@NO});
    return passed;
}

static bool inspectScaledBand(CVPixelBufferRef image,NSString *name,bool fullSource) {
    const CGRect active=fullSource?CGRectMake(0,378.0*16/2608,16,1602.0*16/2608):CGRectMake(0,0,16,16);
    // Filtering may blend a boundary with the black band. Compare interior
    // quadrants with the existing +/-2 allowance, and report every row.
    if(!inspectPattern(image,name,active,true,false))return false;
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return false;
    const auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const auto *uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    const size_t yr=CVPixelBufferGetBytesPerRowOfPlane(image,0),ur=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    unsigned nonzero=0,unchanged=0,badUV=0;
    for(size_t row=0;row<16;++row)for(size_t x=0;x<16;++x){nonzero+=y[row*yr+x]!=0;unchanged+=y[row*yr+x]==165;}
    for(size_t row=0;row<8;++row)for(size_t x=0;x<16;++x)badUV+=uv[row*ur+x]!=128;
    const auto unlocked=CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    const bool okay=nonzero>=128&&!unchanged&&!badUV&&!unlocked;
    report(@{@"phase":@"original-scaling-validation",@"case":name,@"passed":@(okay),
        @"nonzero_Y":@(nonzero),@"unchanged_Y":@(unchanged),@"bad_UV":@(badUV),@"capture_started":@NO});
    return okay;
}
int main(int argc,char **argv) {@autoreleasepool {
 (void)argc;(void)argv;if(!ownMemoryLimit())return 11;sleep(150);
 char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
 report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"original_rectangular_scaling":@YES,@"capture_started":@NO});
 void *capture=dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture",RTLD_NOW|RTLD_GLOBAL);
 Class scaling=capture?objc_getClass("BWScalingSession"):Nil;
 if(!scaling||!methodABI(scaling,false,"init","@16@0:8")||
    !methodABI(scaling,false,"scalePixelBuffer:rect:intoPixelBuffer:rect:","i96@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24^{__CVBuffer=}56{CGRect={CGPoint=dd}{CGSize=dd}}64")||
    !methodABI(scaling,false,"scalePixelBuffer:rect:exifOrientation:intoPixelBuffer:rect:","i100@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24i56^{__CVBuffer=}60{CGRect={CGPoint=dd}{CGSize=dd}}68"))return 12;
 void *library=dlopen("/System/Library/MediaCapture/libNezhaCamera27.dylib",RTLD_NOW|RTLD_LOCAL);
 auto observe=library?(void (*)(void))dlsym(library,"nezha_camera_renderer_observe"):nullptr;
 report(@{@"phase":@"fallback-library",@"present":@(library!=nullptr),@"observer":@(observe!=nullptr)});if(!observe)return 14;
 observe();
 CVPixelBufferRef source=pixel(1200,2608,true),small=pixel(16,16,true);if(!source||!small||!fillBandPattern(source))return 3;
 const CGRect full=CGRectMake(0,0,1200,2608),active=CGRectMake(0,378,1200,1602),target=CGRectMake(0,0,16,16);
 bool all=true;
 for(unsigned region=0;region<2;++region) {
    id session=[scaling new];if(!session)return 15;
    if(!fill(small,false,165,165,165))return 4;
    const CGRect rect=region?active:full;
    NSString *name=region?@"BW-active-band-rect":@"BW-full-source-rect";
    report(@{@"phase":@"original-scaling-begin",@"case":name,
        @"source_rect":@[@(rect.origin.x),@(rect.origin.y),@(rect.size.width),@(rect.size.height)],
        @"destination_rect":@[@0,@0,@16,@16],@"capture_started":@NO});
    const int status=[session scalePixelBuffer:source rect:rect intoPixelBuffer:small rect:target];
    report(@{@"phase":@"original-scaling-return",@"case":name,@"status":@(status),@"capture_started":@NO});
    const bool pixels=inspectScaledBand(small,name,!region);all=all&&!status&&pixels;
    [session invalidate];
 }
 // Unlike the previous Normal test, these calls set the same original
 // SourceCropRectangle and DestinationRectangle properties as BWScalingSession.
 for(unsigned normal=0;normal<2;++normal) {
    if(!fill(small,false,165,165,165))return 4;
    VTPixelTransferSessionRef session=nullptr;
    OSStatus status=VTPixelTransferSessionCreate(kCFAllocatorDefault,&session);
    CFDictionaryRef src=CGRectCreateDictionaryRepresentation(full),dst=CGRectCreateDictionaryRepresentation(target);
    if(!src||!dst||status||!session)return 16;
    if(!status)status=VTSessionSetProperty((VTSessionRef)session,CFSTR("SourceCropRectangle"),src);
    if(!status)status=VTSessionSetProperty((VTSessionRef)session,CFSTR("DestinationRectangle"),dst);
    if(!status&&normal)status=VTSessionSetProperty((VTSessionRef)session,kVTPixelTransferPropertyKey_ScalingMode,kVTScalingMode_Normal);
    NSString *name=normal?@"VT-rect-Normal":@"VT-rect-default";
    report(@{@"phase":@"original-VT-rect-configuration",@"case":name,@"status":@(status),@"system_generates_parameters":@YES,@"capture_started":@NO});
    if(!status)status=VTPixelTransferSessionTransferImage(session,source,small);
    report(@{@"phase":@"original-VT-rect-return",@"case":name,@"status":@(status),@"capture_started":@NO});
    const bool pixels=inspectScaledBand(small,name,true);all=all&&!status&&pixels;
    CFRelease(src);CFRelease(dst);VTPixelTransferSessionInvalidate(session);CFRelease(session);
 }
 CVPixelBufferRelease(source);CVPixelBufferRelease(small);
 report(@{@"phase":@"result",@"passed":@(all),@"original_rectangular_scaling":@YES,
    @"BW_cases":@2,@"VT_rect_cases":@2,@"capture_started":@NO});return all?0:10;
}}
