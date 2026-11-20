// SPDX-License-Identifier: MIT
// Exercise the original system transfer API with known NV12 surfaces.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <IOSurface/IOSurfaceRef.h>
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

static bool inspect(CVPixelBufferRef image,NSString *name,unsigned expected,bool bars) {
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return false;
    const auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
    const auto *uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const size_t yr=CVPixelBufferGetBytesPerRowOfPlane(image,0),ur=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    uint64_t nonzero=0,unchanged=0,total=0;unsigned low=255,high=0;
    NSMutableArray *rows=[NSMutableArray array];
    for(size_t row=0;row<height;++row)for(size_t x=0;x<width;++x){
        const unsigned v=y[row*yr+x];nonzero+=v!=0;unchanged+=v==165;low=std::min(low,v);high=std::max(high,v);++total;
    }
    for(unsigned r=0;r<8;++r){NSMutableArray *values=[NSMutableArray array];
        for(unsigned x=0;x<8;++x)[values addObject:@(y[(height-1)*r/7*yr+(width-1)*x/7])];
        [rows addObject:values];
    }
    const unsigned center=y[height/2*yr+width/2];
    const bool passed=nonzero>total/8&&unchanged<total/8&&std::abs(int(center)-int(expected))<=2;
    report(@{@"phase":@"system-transfer-pixels",@"case":name,@"passed":@(passed),@"expected_center_Y":@(expected),
        @"source_has_black_bars":@(bars),@"width":@(width),@"height":@(height),@"range_Y":@[@(low),@(high)],
        @"nonzero_Y":@(nonzero),@"unchanged_Y":@(unchanged),@"total_Y":@(total),@"rows":rows,
        @"center_YUV":@[@(center),@(uv[height/4*ur+(width/2&~size_t(1))]),@(uv[height/4*ur+(width/2&~size_t(1))+1])],
        @"capture_started":@NO});
    return !CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly)&&passed;
}

static bool transfer(CVPixelBufferRef source,CVPixelBufferRef output,NSString *name,bool letterbox,bool bars) {
    VTPixelTransferSessionRef session=nullptr;
    const OSStatus created=VTPixelTransferSessionCreate(kCFAllocatorDefault,&session);
    report(@{@"phase":@"system-transfer-create",@"case":name,@"status":@(created),@"present":@(session!=nullptr)});
    if(created||!session)return false;
    const OSStatus configured=VTSessionSetProperty(session,kVTPixelTransferPropertyKey_ScalingMode,
        letterbox?kVTScalingMode_Letterbox:kVTScalingMode_Normal);
    report(@{@"phase":@"system-transfer-configure",@"case":name,@"status":@(configured),@"letterbox":@(letterbox)});
    const OSStatus status=configured?configured:VTPixelTransferSessionTransferImage(session,source,output);
    report(@{@"phase":@"system-transfer-call",@"case":name,@"status":@(status),@"passed":@(status==0),@"capture_started":@NO});
    const bool passed=status==0&&inspect(output,name,107,bars);
    VTPixelTransferSessionInvalidate(session);CFRelease(session);return passed;
}

int main(int argc,char **argv) {
    @autoreleasepool {
        (void)argc;(void)argv;
        if(!ownMemoryLimit())return 11;
        sleep(150);
        char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
        report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"capture_started":@NO,@"system_generates_parameters":@YES});
        bool passed=true;
        for(unsigned scenario=0;scenario<4;++scenario){
            const bool bars=scenario>=2,letterbox=scenario&1;
            CVPixelBufferRef source=pixel(1600,1200,true),output=pixel(1200,2608,true);
            if(!source||!output)return 3;
            if(!fill(source,bars,107,128,128)||!fill(output,false,165,165,165))return 4;
            NSString *name=[NSString stringWithFormat:@"originalVT-%@-%@",bars?@"black-bars":@"uniform",letterbox?@"letterbox":@"normal"];
            passed=transfer(source,output,name,letterbox,bars)&&passed;
            CVPixelBufferRelease(source);CVPixelBufferRelease(output);
        }
        report(@{@"phase":@"result",@"passed":@(passed),@"capture_started":@NO});return passed?0:10;
    }
}
