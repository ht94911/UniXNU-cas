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


// Declared from the retained 24A446 ObjC method metadata. The classes and
// methods remain supplied by CMImaging; this helper does not replace them.
@interface NSObject (NezhaController118)
+ (CGSize)maxDownscale;
+ (CGSize)maxUpscale;
+ (int)minSourceWidth;
+ (int)minSourceHeight;
+ (int)minDestinationWidth;
+ (int)minDestinationHeight;
- (instancetype)initWithTransformPriority:(int)priority;
- (int)transform:(CVPixelBufferRef)source srcRect:(CGRect)sourceRect
    dst:(CVPixelBufferRef)destination dstRect:(CGRect)destinationRect
    rotate:(int)rotation sync_m2m:(BOOL)synchronous;
- (int)downsample:(CVPixelBufferRef)source dst:(CVPixelBufferRef)destination
    sync_m2m:(BOOL)synchronous;
- (int)blackFillDst:(CVPixelBufferRef)destination dstRect:(CGRect)rectangle
    sync_m2m:(BOOL)synchronous;
@end

static bool checkMethod(Class cls,bool classMethod,const char *name,const char *expected) {
    SEL selector=sel_registerName(name);
    Method method=classMethod?class_getClassMethod(cls,selector):class_getInstanceMethod(cls,selector);
    const char *actual=method?method_getTypeEncoding(method):nullptr;
    const bool passed=actual&&!std::strcmp(actual,expected);
    report(@{@"phase":@"original-controller-ABI",@"selector":@(name),
        @"class_method":@(classMethod),@"present":@(method!=nullptr),
        @"encoding":actual?@(actual):@"",@"passed":@(passed)});
    return passed;
}

static bool controllerABI(Class cls) {
    bool ok=checkMethod(cls,true,"maxDownscale","{CGSize=dd}16@0:8");
    ok=checkMethod(cls,true,"maxUpscale","{CGSize=dd}16@0:8")&&ok;
    for(const char *name:{"minSourceWidth","minSourceHeight","minDestinationWidth","minDestinationHeight"})
        ok=checkMethod(cls,true,name,"i16@0:8")&&ok;
    ok=checkMethod(cls,false,"initWithTransformPriority:","@20@0:8i16")&&ok;
    ok=checkMethod(cls,false,"transform:srcRect:dst:dstRect:rotate:sync_m2m:",
        "i104@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24^{__CVBuffer=}56{CGRect={CGPoint=dd}{CGSize=dd}}64i96B100")&&ok;
    ok=checkMethod(cls,false,"downsample:dst:sync_m2m:","i36@0:8^{__CVBuffer=}16^{__CVBuffer=}24B32")&&ok;
    ok=checkMethod(cls,false,"blackFillDst:dstRect:sync_m2m:",
        "i60@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24B56")&&ok;
    return ok;
}

static bool inspect(CVPixelBufferRef image,NSString *name,int expectedCenter) {
    const CVReturn locked=CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    if(locked){report(@{@"phase":@"original-controller-pixels",@"case":name,@"lock_status":@(locked),@"passed":@NO});return false;}
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
    report(@{@"phase":@"original-controller-pixels",@"case":name,@"width":@(width),@"height":@(height),
        @"center_YUV":@[@(center),@(centerU),@(centerV)],@"expected_center_Y":@(expectedCenter),
        @"comparison_performed":@(compared),@"range_Y":@[@(low),@(high)],@"nonzero_Y":@(nonzero),
        @"unchanged_Y":@(unchanged),@"total_Y":@(total),@"bad_UV":@(badUV),@"rows":rows,
        @"unlock_status":@(unlocked),@"passed":@(passed),@"capture_started":@NO});
    return passed;
}

static bool callResult(NSString *name,int status,CVPixelBufferRef image,int expected) {
    report(@{@"phase":@"original-controller-call",@"case":name,@"status":@(status),@"capture_started":@NO});
    const bool pixels=inspect(image,name,status==0?expected:-1);
    return status==0&&pixels;
}

int main(int argc,char **argv) {
    @autoreleasepool {
        (void)argc;(void)argv;
        if(!ownMemoryLimit())return 11;
        sleep(150);
        char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
        report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"capture_started":@NO,
            @"original_controller":@YES,@"system_generates_parameters":@YES});
        void *framework=dlopen("/System/Library/PrivateFrameworks/CMImaging.framework/CMImaging",RTLD_NOW|RTLD_GLOBAL);
        Class cls=framework?objc_getClass("FigM2MController"):Nil;
        report(@{@"phase":@"original-controller-class",@"framework_loaded":@(framework!=nullptr),@"present":@(cls!=Nil)});
        if(!cls||!controllerABI(cls))return 12;
        const CGSize down=[cls maxDownscale],up=[cls maxUpscale];
        const int minSW=[cls minSourceWidth],minSH=[cls minSourceHeight];
        const int minDW=[cls minDestinationWidth],minDH=[cls minDestinationHeight];
        const bool finite=std::isfinite(down.width)&&std::isfinite(down.height)&&
            std::isfinite(up.width)&&std::isfinite(up.height);
        report(@{@"phase":@"original-controller-capabilities",@"max_downscale":@[@(down.width),@(down.height)],
            @"max_upscale":@[@(up.width),@(up.height)],@"min_source":@[@(minSW),@(minSH)],
            @"min_destination":@[@(minDW),@(minDH)],@"finite":@(finite),
            @"positive_scale_limits":@(finite&&down.width>0&&down.height>0&&up.width>0&&up.height>0)});
        id controller=[[cls alloc] initWithTransformPriority:4];
        report(@{@"phase":@"original-controller-create",@"present":@(controller!=nil)});
        if(!controller)return 13;
        CVPixelBufferRef source=pixel(1600,1200,true),portrait=pixel(1200,2608,true);
        CVPixelBufferRef small=pixel(16,16,true),output=pixel(1200,2608,true);
        if(!source||!portrait||!small||!output)return 3;
        if(!fill(source,false,107,128,128)||!fill(portrait,false,165,165,165)||
           !fill(small,false,165,165,165)||!fill(output,false,165,165,165))return 4;
        bool passed=true;
        const CGRect src=CGRectMake(0,0,1600,1200),full=CGRectMake(0,0,1200,2608);
        const CGRect active=CGRectMake(0,854,1200,900),tiny=CGRectMake(0,0,16,16);
        int status=[controller blackFillDst:portrait dstRect:full sync_m2m:YES];
        passed=callResult(@"controller-black-fill",status,portrait,0)&&passed;
        status=[controller transform:source srcRect:src dst:portrait dstRect:active rotate:1 sync_m2m:YES];
        passed=callResult(@"controller-centred-transfer",status,portrait,107)&&passed;
        status=[controller downsample:portrait dst:small sync_m2m:YES];
        passed=callResult(@"controller-original-downsample",status,small,107)&&passed;
        status=[controller transform:small srcRect:tiny dst:output dstRect:full rotate:1 sync_m2m:YES];
        passed=callResult(@"controller-original-expand",status,output,107)&&passed;
        CVPixelBufferRelease(source);CVPixelBufferRelease(portrait);
        CVPixelBufferRelease(small);CVPixelBufferRelease(output);
        report(@{@"phase":@"result",@"passed":@(passed),@"original_controller":@YES,@"capture_started":@NO});
        return passed?0:10;
    }
}
