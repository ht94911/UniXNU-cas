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
typedef struct {int32_t width,height;} ProbeDimensions;

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
@end
@protocol NezhaBWStitcher <NSObject>
- (instancetype)initWithCameraInfo:(id)info;
- (instancetype)initWithCameraInfoArray:(id)info multiCameraStitchingEnabled:(bool)multi fractionalSourceRectEnabled:(bool)fractional inputBuffersHaveHorizontalOverscanOnly:(bool)overscan superWideSensorCenterOffsetEnabled:(bool)offset parallaxMitigationBasedOnZoomFactorEnabled:(bool)parallax;
- (void)_getInputRect:(CGRect *)input outputRect:(CGRect *)output inputShiftAppliedInOutputRect:(CGPoint *)applied toTransferPixelBuffer:(CVPixelBufferRef)source rect:(CGRect)src intoPixelBufferDimensions:(ProbeDimensions)dimensions rect:(CGRect)dst withInputShift:(CGPoint)shift shiftOutsideBoundingRectAllowed:(bool)outside ignoreBounds:(bool)ignore scale:(float)scale forFinalOutput:(bool)final;
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

static bool fillPattern(CVPixelBufferRef image) {
    if(!fill(image,false,0,128,128)||CVPixelBufferLockBaseAddress(image,0))return false;
    auto *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const size_t rowBytes=CVPixelBufferGetBytesPerRowOfPlane(image,0);
    const uint8_t values[]={51,107,181,219};
    for(size_t row=0;row<height;++row)for(size_t x=0;x<width;++x)
        y[row*rowBytes+x]=values[(row>=height/2?2:0)+(x>=width/2?1:0)];
    return CVPixelBufferUnlockBaseAddress(image,0)==0;
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
static bool inspectBorder(CVPixelBufferRef image,NSString *name,CGRect rect,unsigned expectedY=0,unsigned expectedUV=128) {
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return false;
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const size_t left=(size_t)rect.origin.x,top=(size_t)rect.origin.y;
    const size_t right=left+(size_t)rect.size.width,bottom=top+(size_t)rect.size.height;
    const auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const auto *uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    const size_t yr=CVPixelBufferGetBytesPerRowOfPlane(image,0),ur=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    uint64_t outside=0,badY=0,badUV=0,unchanged=0;
    for(size_t row=0;row<height;++row)for(size_t x=0;x<width;++x) {
        if(row<top||row>=bottom||x<left||x>=right){++outside;badY+=y[row*yr+x]!=expectedY;}
        unchanged+=y[row*yr+x]==165;
    }
    for(size_t row=0;row<height/2;++row)for(size_t x=0;x<width;x+=2)
        if(row*2+1<top||row*2>=bottom||x+1<left||x>=right)
            badUV+=uv[row*ur+x]!=expectedUV||uv[row*ur+x+1]!=expectedUV;
    const CVReturn unlocked=CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    const bool passed=outside>0&&!badY&&!badUV&&!unlocked;
    report(@{@"phase":@"original-renderer-border",@"case":name,@"outside_Y":@(outside),
        @"bad_outside_Y":@(badY),@"bad_outside_UV_pairs":@(badUV),@"unchanged_Y":@(unchanged),
        @"expected_outside_Y":@(expectedY),@"expected_outside_UV":@(expectedUV),
        @"passed":@(passed),@"capture_started":@NO});return passed;
}
static int stitcherGeometry() {
    char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
    report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"original_renderer":@YES,@"capture_started":@NO,@"actual_configuration":@"nil pool; fractional=false; compressed=false; owned RAW1x"});
    void *capture=dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture",RTLD_NOW|RTLD_GLOBAL);
    Class stitcherClass=capture?objc_getClass("BWPreviewStitcherRenderer"):Nil;
    Class infoClass=capture?objc_getClass("BWPreviewStitcherCameraInfo"):Nil;
    Class rendererClass=capture?objc_getClass("BWPixelBufferTransferRenderer"):Nil;
    if(!stitcherClass||!infoClass||!rendererClass)return 12;
    const char *geometry="_getInputRect:outputRect:inputShiftAppliedInOutputRect:toTransferPixelBuffer:rect:intoPixelBufferDimensions:rect:withInputShift:shiftOutsideBoundingRectAllowed:ignoreBounds:scale:forFinalOutput:";
    if(!methodABI(stitcherClass,false,geometry,"v152@0:8^{CGRect={CGPoint=dd}{CGSize=dd}}16^{CGRect={CGPoint=dd}{CGSize=dd}}24^{CGPoint=dd}32^{__CVBuffer=}40{CGRect={CGPoint=dd}{CGSize=dd}}48{?=ii}80{CGRect={CGPoint=dd}{CGSize=dd}}88{CGPoint=dd}120B136B140f144B148") ||
       !methodABI(stitcherClass,false,"initWithCameraInfoArray:multiCameraStitchingEnabled:fractionalSourceRectEnabled:inputBuffersHaveHorizontalOverscanOnly:superWideSensorCenterOffsetEnabled:parallaxMitigationBasedOnZoomFactorEnabled:","@44@0:8@16B24B28B32B36B40") ||
       !methodABI(infoClass,false,"initWithCameraInfo:","@24@0:8@16") ||
       !methodABI(rendererClass,false,"initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:","@32@0:8@16B24B28"))return 13;
    const SEL selector=sel_registerName(geometry);
    const IMP original=method_getImplementation(class_getInstanceMethod(stitcherClass,selector));
    NSDictionary *cameraInfo=@{@"BaseZoomFactor":@1,@"BaseZoomFactorAfterGDC":@1};
    id info0=[[infoClass alloc] initWithCameraInfo:cameraInfo];
    id info1=[[infoClass alloc] initWithCameraInfo:cameraInfo];
    id info2=[[infoClass alloc] initWithCameraInfo:cameraInfo];
    if(!info0||!info1||!info2)return 15;
    id<NezhaBWStitcher> stitcher=[[stitcherClass alloc] initWithCameraInfoArray:@[info0,info1,info2] multiCameraStitchingEnabled:false fractionalSourceRectEnabled:false inputBuffersHaveHorizontalOverscanOnly:false superWideSensorCenterOffsetEnabled:false parallaxMitigationBasedOnZoomFactorEnabled:false];
    id renderer=[[rendererClass alloc] initWithIntermediateBufferPool:nil fractionalSourceRectEnabled:false compressedFormatEnabled:false];
    if(!stitcher||!renderer)return 15;
    void *library=dlopen("/System/Library/MediaCapture/libNezhaCamera27.dylib",RTLD_NOW|RTLD_LOCAL);
    auto observe=library?(void (*)(void))dlsym(library,"nezha_camera_renderer_observe"):nullptr;
    if(!observe)return 14;
    observe();
    const bool installed=method_getImplementation(class_getInstanceMethod(stitcherClass,selector))!=original;
    report(@{@"phase":@"stitcher-repair-installed",@"passed":@(installed),@"capture_started":@NO});
    if(!installed)return 19;
    CVPixelBufferRef source=pixel(1600,1200,true),output=pixel(1200,2608,true);
    if(!source||!output||!fillPattern(source))return 3;
    CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
    const CGRect target=CGRectMake(0,378,1200,1602);
    const ProbeDimensions dimensions={1200,2608};
    bool passed=true;
    for(unsigned empty=0;empty<2;++empty) {
        const CGRect requested=empty?CGRectMake(801,601,0,0):CGRectMake(0,0,1600,1200);
        CGRect beforeInput={},beforeOutput={},selectedInput={},selectedOutput={};
        CGPoint beforeShift={},selectedShift={};
        using GetRect=void (*)(id,SEL,CGRect *,CGRect *,CGPoint *,CVPixelBufferRef,CGRect,ProbeDimensions,CGRect,CGPoint,bool,bool,float,bool);
        ((GetRect)original)(stitcher,selector,&beforeInput,&beforeOutput,&beforeShift,source,requested,dimensions,target,CGPointZero,false,true,1,false);
        [stitcher _getInputRect:&selectedInput outputRect:&selectedOutput inputShiftAppliedInOutputRect:&selectedShift toTransferPixelBuffer:source rect:requested intoPixelBufferDimensions:dimensions rect:target withInputShift:CGPointZero shiftOutsideBoundingRectAllowed:false ignoreBounds:true scale:1 forFinalOutput:false];
        const bool changed=beforeInput.size.width==0&&beforeInput.size.height==0&&std::isfinite(beforeInput.origin.x)&&std::isfinite(beforeInput.origin.y);
        const bool valid=selectedInput.size.width>0&&selectedInput.size.height>0&&
            std::isfinite(selectedInput.origin.x)&&std::isfinite(selectedInput.origin.y)&&
            !std::memcmp(&beforeOutput,&selectedOutput,sizeof beforeOutput)&&!std::memcmp(&beforeShift,&selectedShift,sizeof beforeShift);
        NSString *name=empty?@"stitcher-owned-empty-input":@"stitcher-owned-full-input";
        report(@{@"phase":@"stitcher-geometry",@"case":name,@"original_input":@[@(beforeInput.origin.x),@(beforeInput.origin.y),@(beforeInput.size.width),@(beforeInput.size.height)],@"selected_input":@[@(selectedInput.origin.x),@(selectedInput.origin.y),@(selectedInput.size.width),@(selectedInput.size.height)],@"selected_output":@[@(selectedOutput.origin.x),@(selectedOutput.origin.y),@(selectedOutput.size.width),@(selectedOutput.size.height)],@"original_empty":@(changed),@"passed":@(valid),@"capture_started":@NO});
        if(!valid){passed=false;continue;}
        VTPixelRotationSessionRef session=nullptr;
        OSStatus status=VTPixelRotationSessionCreate(kCFAllocatorDefault,&session);
        if(!status&&session)status=VTSessionSetProperty((VTSessionRef)session,CFSTR("Rotation"),CFSTR("RotationCW90"));
        if(!status&&session&&!fill(output,false,165,165,165))status=-1;
        if(!status&&session)status=[renderer doPixelRotationTransferWithSession:session sourceBuffer:source sourceRect:selectedInput destinationBuffer:output destinationRect:selectedOutput blackFillUnalignedEdges:true];
        report(@{@"phase":@"stitcher-rotation",@"case":name,@"status":@(status),@"pool_present":@NO,@"compressed_requested":@NO,@"capture_started":@NO});
        const bool pixels=!status&&inspectPattern(output,name,selectedOutput,true,true);
        const bool border=!status&&inspectBorder(output,name,selectedOutput);
        passed=passed&&pixels&&border&&(!empty||changed);
        if(session){VTPixelRotationSessionInvalidate(session);CFRelease(session);}
    }
    CVPixelBufferRelease(source);CVPixelBufferRelease(output);
    report(@{@"phase":@"result",@"passed":@(passed),@"original_geometry_consumer":@YES,@"nil_intermediate_pool":@YES,@"owned_source_crop_export_consistent":@(passed),@"geometry_cases":@2,@"capture_started":@NO});
    return passed?0:10;
}
int main(int argc,char **argv) {@autoreleasepool {
 if(!ownMemoryLimit())return 11;sleep(150);
 if(argc==2&&!std::strcmp(argv[1],"--stitcher-geometry"))return stitcherGeometry();
 char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
 report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"original_renderer":@YES,@"capture_started":@NO});
 void *capture=dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture",RTLD_NOW|RTLD_GLOBAL);
 Class rendererClass=capture?objc_getClass("BWPixelBufferTransferRenderer"):Nil;
 Class formatClass=capture?objc_getClass("BWVideoFormat"):Nil;Class poolClass=capture?objc_getClass("BWPixelBufferPool"):Nil;
 if(!rendererClass||!formatClass||!poolClass)return 12;
 if(!methodABI(rendererClass,true,"supportsAcceleratedBorderFill","B16@0:8")||
    !methodABI(rendererClass,false,"initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:","@32@0:8@16B24B28")||
    !methodABI(rendererClass,false,"doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:","i108@0:8^{OpaqueVTPixelRotationSession=}16^{__CVBuffer=}24{CGRect={CGPoint=dd}{CGSize=dd}}32^{__CVBuffer=}64{CGRect={CGPoint=dd}{CGSize=dd}}72B104")||
    !methodABI(rendererClass,false,"borderBlackFillPixelBuffer:validRect:","i56@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24")||
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
 if(!fillPattern(source))return 4;
 bool passed=!after;
 // Camera130 used this asymmetric, chroma-aligned destination and fill=true.
 // The previous three pattern cases all used fill=false and remain archived.
 const CGRect destinationRect=CGRectMake(0,378,1200,1602);
 for(unsigned rotation=0;rotation<2;++rotation) {
    if(!fill(output,false,165,165,165))return 4;
    VTPixelRotationSessionRef session=nullptr;OSStatus created=VTPixelRotationSessionCreate(kCFAllocatorDefault,&session);
    report(@{@"phase":@"original-rotation-session",@"rotation_case":@(rotation),@"status":@(created),@"present":@(session!=nullptr)});if(created||!session)return 16;
    const OSStatus property=VTSessionSetProperty((VTSessionRef)session,CFSTR("Rotation"),CFSTR("RotationCW90"));
    if(property)return 17;
    if(rotation==1) {
        NSDictionary *metadata=@{@"FrameSequence":@7,@"RawSensorWidth":@4096,@"RawSensorHeight":@3072,@"ZoomFactor":@1};
        CVBufferSetAttachment(source,CFSTR("MetadataDictionary"),(__bridge CFDictionaryRef)metadata,kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),kCFBooleanTrue,kCVAttachmentMode_ShouldPropagate);
        CVBufferRemoveAttachment(source,CFSTR("MetadataDictionary"));
    }
    const CGRect sourceRect=rotation==1?CGRectMake(801,601,0,0):CGRectMake(0,0,1600,1200);
    NSString *name=rotation?@"filled-own-empty-crop-CW90-pattern":@"filled-asymmetric-CW90-pattern";
    report(@{@"phase":@"original-renderer-call-begin",@"case":name,@"black_fill_unaligned_edges":@YES,
        @"source_rect":@[@(sourceRect.origin.x),@(sourceRect.origin.y),@(sourceRect.size.width),@(sourceRect.size.height)],
        @"destination_rect":@[@0,@378,@1200,@1602],@"capture_started":@NO});
    const int status=[renderer doPixelRotationTransferWithSession:session sourceBuffer:source sourceRect:sourceRect destinationBuffer:output destinationRect:destinationRect blackFillUnalignedEdges:true];
    report(@{@"phase":@"original-renderer-call",@"case":name,@"status":@(status),@"capture_started":@NO});
    // Preserve Camera132's unmarked source behavior, and require black
    // outside pixels for the provider-owned completed-frame source.
    const bool pixels=inspectPattern(output,name,destinationRect,true,rotation==1);
    const bool border=inspectBorder(output,name,destinationRect,rotation==0?165:0,rotation==0?165:128);
    passed=passed&&!status&&pixels&&border;
    if(rotation==1) {
        // Follow the corrected original rotation with the system's exact
        // downscale API, so conversion parameters come from VideoToolbox.
        CVPixelBufferRef small=pixel(16,16,true);
        if(!small||!fill(small,false,165,165,165))return 18;
        VTPixelTransferSessionRef transfer=nullptr;
        OSStatus scaled=VTPixelTransferSessionCreate(kCFAllocatorDefault,&transfer);
        if(!scaled&&transfer)scaled=VTSessionSetProperty((VTSessionRef)transfer,
            kVTPixelTransferPropertyKey_ScalingMode,kVTScalingMode_Normal);
        if(!scaled&&transfer)scaled=VTPixelTransferSessionTransferImage(transfer,output,small);
        report(@{@"phase":@"original-system-downscale",@"status":@(scaled),
            @"source_size":@[@1200,@2608],@"destination_size":@[@16,@16],
            @"system_generates_parameters":@YES,@"capture_started":@NO});
        const CGRect smallRect=CGRectMake(0,378.0*16/2608,16,1602.0*16/2608);
        const bool smallPixels=inspectPattern(small,@"owned-filled-rotation-system-downscale",smallRect,true);
        passed=passed&&!scaled&&transfer&&smallPixels;
        if(transfer){VTPixelTransferSessionInvalidate(transfer);CFRelease(transfer);}
        CVPixelBufferRelease(small);
    }
    VTPixelRotationSessionInvalidate(session);CFRelease(session);
 }
 if(!fillPattern(output))return 4;
 const int borderStatus=[renderer borderBlackFillPixelBuffer:output validRect:destinationRect];
 report(@{@"phase":@"standalone-original-border-fill",@"status":@(borderStatus),@"capture_started":@NO});
 const bool border=inspectBorder(output,@"standalone-authored-nonuniform-border",destinationRect);
 const bool inside=inspectPattern(output,@"standalone-authored-nonuniform-border",CGRectMake(0,0,1200,2608),false);
 passed=passed&&!borderStatus&&border&&inside;
 CVPixelBufferRelease(source);CVPixelBufferRelease(output);
 report(@{@"phase":@"result",@"passed":@(passed),@"original_renderer":@YES,@"nonuniform_rotation_cases":@2,
    @"black_fill_enabled":@YES,@"standalone_border_case":@YES,@"system_downscale_case":@YES,
    @"capture_started":@NO});return passed?0:10;
}}
