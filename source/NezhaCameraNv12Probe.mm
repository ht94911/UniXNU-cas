// SPDX-License-Identifier: MIT
// Exercise the existing Metal/IOSurface path with known NV12 plane codes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#include <cmath>
#include <initializer_list>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <time.h>
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

static bool complete(id<MTLCommandBuffer> command,NSString *name) {
    [command commit];
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:15];
    while(command.status!=MTLCommandBufferStatusCompleted && command.status!=MTLCommandBufferStatusError && [deadline timeIntervalSinceNow]>0)usleep(1000);
    const bool passed=command.status==MTLCommandBufferStatusCompleted && !command.error;
    report(@{@"phase":@"completion",@"case":name,@"status":@(command.status),@"passed":@(passed),@"error":command.error.description?:(passed?@"":@"timeout")});
    return passed;
}

static CVPixelBufferRef pixelBuffer(unsigned width,unsigned height) {
    CVPixelBufferRef pixel=nullptr;
    NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    const CVReturn result=CVPixelBufferCreate(kCFAllocatorDefault,width,height,kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
        (__bridge CFDictionaryRef)attributes,&pixel);
    report(@{@"phase":@"pixel-buffer",@"width":@(width),@"height":@(height),@"status":@(result)});
    return result==kCVReturnSuccess?pixel:nullptr;
}

static id<MTLTexture> plane(id<MTLDevice> device,IOSurfaceRef surface,unsigned index,unsigned width,unsigned height,MTLPixelFormat format) {
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderWrite;
    return [device newTextureWithDescriptor:descriptor iosurface:surface plane:index];
}

static uint8_t expectedY(unsigned x,unsigned y) {return (x*7+y*13+17)&255;}
static uint8_t expectedU(unsigned x,unsigned y) {return (x*5+y*11+101)&255;}
static uint8_t expectedV(unsigned x,unsigned y) {return (x*3+y*17+67)&255;}

static bool pixels(IOSurfaceRef surface,unsigned width,unsigned height,bool pattern,NSString *name) {
    if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr))return false;
    const size_t yrow=IOSurfaceGetBytesPerRowOfPlane(surface,0),uvrow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
    const auto *y=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0),*uv=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
    uint64_t badY=0,badUV=0;
    NSMutableArray *first=[NSMutableArray array];
    uint64_t wrongRows=0,wrongColumns=0;unsigned firstX=width,firstY=height,lastX=0,lastY=0;
    bool columnBad[1200]={};
    for(unsigned r=0;r<height;++r) {
        bool rowBad=false;
        for(unsigned x=0;x<width;++x) {
            const unsigned wanted=pattern?expectedY(x,r):107,actual=y[r*yrow+x];
            if(actual!=wanted) {
                ++badY;rowBad=true;columnBad[x]=true;
                firstX=std::min(firstX,x);firstY=std::min(firstY,r);lastX=std::max(lastX,x);lastY=std::max(lastY,r);
                if(first.count<24)[first addObject:@[@(x),@(r),@(actual),@(wanted)]];
            }
        }
        wrongRows+=rowBad;
    }
    for(unsigned x=0;x<width;++x)wrongColumns+=columnBad[x];
    for(unsigned r=0;r<height/2;++r)for(unsigned x=0;x<width/2;++x) {
        badUV+=uv[r*uvrow+x*2]!=(pattern?expectedU(x,r):128);
        badUV+=uv[r*uvrow+x*2+1]!=(pattern?expectedV(x,r):121);
    }
    const unsigned x=width/2,r=height/2;
    report(@{@"phase":@"pixels",@"case":name,@"width":@(width),@"height":@(height),@"surface":@(IOSurfaceGetID(surface)),
        @"formats":@[@13,@23],@"UV_format":@"R16Uint",@"rows":@[@(yrow),@(uvrow)],@"offsets":@[@((uintptr_t)y-(uintptr_t)IOSurfaceGetBaseAddress(surface)),@((uintptr_t)uv-(uintptr_t)IOSurfaceGetBaseAddress(surface))],
        @"bad_Y":@(badY),@"bad_UV":@(badUV),@"first_bad_Y":first,@"bad_rows":@(wrongRows),@"bad_columns":@(wrongColumns),@"bad_Y_bounds":badY?@[@(firstX),@(firstY),@(lastX),@(lastY)]:@[],@"center":@[@(y[r*yrow+x]),@(uv[(r/2)*uvrow+(x&~1u)]),@(uv[(r/2)*uvrow+(x&~1u)+1])],
        @"checked_Y":@((uint64_t)width*height),@"checked_UV":@((uint64_t)width*height/2),@"passed":@(!badY&&!badUV),@"capture_started":@NO});
    return !IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr)&&!badY&&!badUV;
}

static bool clear(IOSurfaceRef surface) {
    if(IOSurfaceLock(surface,0,nullptr))return false;
    std::memset(IOSurfaceGetBaseAddress(surface),0xa5,IOSurfaceGetAllocSize(surface));
    return !IOSurfaceUnlock(surface,0,nullptr);
}

static bool exercise(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLComputePipelineState> direct,id<MTLComputePipelineState> shared,id<MTLComputePipelineState> constant,id<MTLComputePipelineState> sampled,unsigned width,unsigned height) {
    CVPixelBufferRef output=pixelBuffer(width,height),input=pixelBuffer(width,height);
    if(!output||!input){if(output)CVPixelBufferRelease(output);if(input)CVPixelBufferRelease(input);return false;}
    IOSurfaceRef dst=CVPixelBufferGetIOSurface(output),src=CVPixelBufferGetIOSurface(input);
    bool ok=dst&&src&&IOSurfaceGetPlaneCount(dst)==2&&IOSurfaceGetPlaneCount(src)==2;
    bool passed=true;
    id<MTLTexture> y=ok?plane(device,dst,0,width,height,MTLPixelFormatR8Uint):nil;
    id<MTLTexture> uv=ok?plane(device,dst,1,width/2,height/2,MTLPixelFormatR16Uint):nil;
    ok=ok&&y&&uv;
    if(ok) {
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)540 width:width height:height mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor iosurface:src plane:0];
        ok=texture&&!IOSurfaceLock(src,0,nullptr);
        if(ok) {
            const size_t yr=IOSurfaceGetBytesPerRowOfPlane(src,0),ur=IOSurfaceGetBytesPerRowOfPlane(src,1);
            auto *yp=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(src,0),*up=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(src,1);
            for(unsigned r=0;r<height;++r)std::memset(yp+r*yr,107,width);
            for(unsigned r=0;r<height/2;++r)for(unsigned x=0;x<width;x+=2){up[r*ur+x]=128;up[r*ur+x+1]=121;}
            ok=!IOSurfaceUnlock(src,0,nullptr);
        }
        // Plain integer storage writes isolate the destination planes from sampling and shared memory.
        if(ok&&clear(dst)) {
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:direct];[encoder setTexture:y atIndex:0];[encoder setTexture:uv atIndex:1];
            [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(32,8,1)];[encoder endEncoding];
            const bool finished=complete(command,@"integer-store");
            passed=(finished&&pixels(dst,width,height,true,@"integer-store"))&&passed;
        } else ok=false;
        if(ok) {
            id<MTLBuffer> result=[device newBufferWithLength:(NSUInteger)width*height*4*sizeof(float) options:MTLResourceStorageModeShared];
            if(!result)ok=false;
            else {
                std::memset(result.contents,0xa5,result.length);
                id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
                [encoder setComputePipelineState:sampled];[encoder setTexture:texture atIndex:0];[encoder setBuffer:result offset:0 atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(32,8,1)];[encoder endEncoding];
                const bool done=complete(command,@"sample-to-buffer");uint64_t bad=0;NSMutableArray *first=[NSMutableArray array];
                if(done) {
                    const float *actual=(const float *)result.contents;const float want[4]={121/255.f,107/255.f,128/255.f,1.f};
                    for(unsigned r=0;r<height;++r)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<4;++c) {
                        const float value=actual[((NSUInteger)r*width+x)*4+c];
                        if(!std::isfinite(value)||std::fabs(value-want[c])>0.002f) {
                            ++bad;if(first.count<24)[first addObject:@[@(x),@(r),@(c),std::isfinite(value)?@(value):(id)[NSNull null]]];
                        }
                    }
                }
                report(@{@"phase":@"sample-buffer",@"width":@(width),@"height":@(height),@"bad_channels":@(bad),@"first_bad":first,@"passed":@(done&&!bad),@"capture_started":@NO});
                passed=(done&&!bad)&&passed;
            }
        }
        if(ok&&clear(dst)) {
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:constant];[encoder setTexture:y atIndex:1];[encoder setTexture:uv atIndex:2];[encoder setThreadgroupMemoryLength:16384 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake((width+1023)/1024,height,1) threadsPerThreadgroup:MTLSizeMake(1024,1,1)];[encoder endEncoding];
            const bool done=complete(command,@"constant-threadgroup");passed=(done&&pixels(dst,width,height,false,@"constant-threadgroup"))&&passed;
        } else ok=false;
        // Two explicit dynamic lengths share the pipeline; both runs depend on another lane's write.
        for(unsigned length:{16384u,32768u}) {
            if(!ok||!clear(dst)){ok=false;break;}
            NSString *name=[NSString stringWithFormat:@"combined-sample-threadgroup-%u",length];
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:shared];[encoder setTexture:texture atIndex:0];[encoder setTexture:y atIndex:1];[encoder setTexture:uv atIndex:2];
            [encoder setThreadgroupMemoryLength:length atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake((width+1023)/1024,height,1) threadsPerThreadgroup:MTLSizeMake(1024,1,1)];[encoder endEncoding];
            const bool finished=complete(command,name);
            passed=(finished&&pixels(dst,width,height,false,name))&&passed;
        }
    }
    CVPixelBufferRelease(input);CVPixelBufferRelease(output);return ok&&passed;
}

int main(int argc,char **argv) {
    @autoreleasepool {
        if(argc!=2){report(@{@"phase":@"arguments",@"passed":@NO});return 2;}
        if(!ownMemoryLimit())return 11;
        sleep(150);
        char boot[128]={};size_t bytes=sizeof boot;
        bool identified=sysctlbyname("kern.bootsessionuuid",boot,&bytes,nullptr,0)==0;
        report(@{@"phase":@"start",@"boot":identified?@(boot):@"unknown",@"pid":@(getpid()),@"uid":@(geteuid()),@"capture_started":@NO});
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();NSError *error=nil;
        report(@{@"phase":@"device",@"present":@(device!=nil)});
        if(!device)return 3;
        id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        id<MTLFunction> direct=[library newFunctionWithName:@"write_planar_codes"],shared=[library newFunctionWithName:@"copy_planar_threadgroup"];
        id<MTLComputePipelineState> directState=direct?[device newComputePipelineStateWithFunction:direct error:&error]:nil;
        id<MTLComputePipelineState> sharedState=shared?[device newComputePipelineStateWithFunction:shared error:&error]:nil;
        id<MTLFunction> cf=[library newFunctionWithName:@"constant_planar_threadgroup"],sf=[library newFunctionWithName:@"sample_planar_buffer"];
        id<MTLComputePipelineState> constant=cf?[device newComputePipelineStateWithFunction:cf error:&error]:nil;
        id<MTLComputePipelineState> sampled=sf?[device newComputePipelineStateWithFunction:sf error:&error]:nil;
        report(@{@"phase":@"pipeline",@"direct":@(directState!=nil),@"shared":@(sharedState!=nil),@"constant":@(constant!=nil),@"sampled":@(sampled!=nil),@"error":error.description?:@""});
        if(!directState||!sharedState||!constant||!sampled)return 4;
        id<MTLCommandQueue> queue=[device newCommandQueue];
        const bool small=queue&&exercise(device,queue,directState,sharedState,constant,sampled,64,16);
        const bool large=queue&&exercise(device,queue,directState,sharedState,constant,sampled,1200,2608);
        const bool passed=small&&large;
        report(@{@"phase":@"result",@"boot":identified?@(boot):@"unknown",@"pid":@(getpid()),@"passed":@(passed),@"capture_started":@NO,@"device":device.name});
        return passed?0:10;
    }
}
