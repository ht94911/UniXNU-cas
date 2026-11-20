// SPDX-License-Identifier: MIT
// Replay the original transfer kernel with fixture files retained on this host.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#include <algorithm>
#include <initializer_list>
#include <cerrno>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
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

static bool complete(id<MTLCommandBuffer> command,NSString *name) {
    [command commit];
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:15];
    while(command.status!=MTLCommandBufferStatusCompleted && command.status!=MTLCommandBufferStatusError &&
          deadline.timeIntervalSinceNow>0)usleep(1000);
    const bool passed=command.status==MTLCommandBufferStatusCompleted && !command.error;
    report(@{@"phase":@"completion",@"case":name,@"status":@(command.status),@"passed":@(passed),
             @"error":command.error.description?:(passed?@"":@"timeout")});
    return passed;
}

static CVPixelBufferRef pixelBuffer(unsigned width,unsigned height) {
    CVPixelBufferRef pixel=nullptr;
    NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    const CVReturn status=CVPixelBufferCreate(kCFAllocatorDefault,width,height,
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,(__bridge CFDictionaryRef)attributes,&pixel);
    return status==kCVReturnSuccess?pixel:nullptr;
}

static id<MTLTexture> texture(id<MTLDevice> device,IOSurfaceRef surface,unsigned plane,
    unsigned width,unsigned height,MTLPixelFormat format,MTLTextureUsage usage) {
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=usage;
    return [device newTextureWithDescriptor:descriptor iosurface:surface plane:plane];
}

static bool codes(IOSurfaceRef surface,unsigned y,unsigned u,unsigned v) {
    if(IOSurfaceLock(surface,0,nullptr))return false;
    auto *luma=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0);
    auto *chroma=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
    const size_t width=IOSurfaceGetWidthOfPlane(surface,0),height=IOSurfaceGetHeightOfPlane(surface,0);
    const size_t yr=IOSurfaceGetBytesPerRowOfPlane(surface,0),ur=IOSurfaceGetBytesPerRowOfPlane(surface,1);
    for(size_t r=0;r<height;++r)std::memset(luma+r*yr,y,width);
    for(size_t r=0;r<height/2;++r)for(size_t x=0;x<width;x+=2){chroma[r*ur+x]=u;chroma[r*ur+x+1]=v;}
    return !IOSurfaceUnlock(surface,0,nullptr);
}

static bool pixels(IOSurfaceRef surface,NSString *name,bool compare) {
    if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr))return false;
    const auto *y=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0);
    const auto *uv=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
    const size_t width=IOSurfaceGetWidthOfPlane(surface,0),height=IOSurfaceGetHeightOfPlane(surface,0);
    const size_t yr=IOSurfaceGetBytesPerRowOfPlane(surface,0),ur=IOSurfaceGetBytesPerRowOfPlane(surface,1);
    uint64_t badY=0,badUV=0,nonzeroY=0,nonzeroUV=0;
    unsigned loY=255,hiY=0,loUV=255,hiUV=0;
    for(size_t r=0;r<height;++r)for(size_t x=0;x<width;++x){
        const unsigned value=y[r*yr+x];badY+=value!=0;nonzeroY+=value!=0;loY=std::min(loY,value);hiY=std::max(hiY,value);
    }
    for(size_t r=0;r<height/2;++r)for(size_t x=0;x<width;++x){
        const unsigned value=uv[r*ur+x];badUV+=value!=128;nonzeroUV+=value!=0;loUV=std::min(loUV,value);hiUV=std::max(hiUV,value);
    }
    report(@{@"phase":@"original-pixels",@"case":name,@"width":@(width),@"height":@(height),
        @"compare_known_black":@(compare),@"bad_Y":@(badY),@"bad_UV":@(badUV),@"nonzero_Y":@(nonzeroY),@"nonzero_UV":@(nonzeroUV),
        @"range_Y":@[@(loY),@(hiY)],@"range_UV":@[@(loUV),@(hiUV)],
        @"center":@[@(y[(height/2)*yr+width/2]),@(uv[(height/4)*ur+(width/2&~size_t(1))]),@(uv[(height/4)*ur+(width/2&~size_t(1))+1])],
        @"passed":@(!badY&&!badUV),@"capture_started":@NO});
    return !IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr)&&(!compare||(!badY&&!badUV));
}

static bool diagnostics(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLLibrary> authored,
    id<MTLTexture> source,id<MTLBuffer> config,id<MTLTexture> filter,id<MTLSamplerState> sampler,unsigned black) {
    bool passed=true;
    for(NSString *name in @[@"replay_uniform",@"replay_filter",@"replay_sampler"]){
        NSError *error=nil;id<MTLFunction> function=[authored newFunctionWithName:name];
        id<MTLComputePipelineState> state=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
        id<MTLBuffer> result=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        if(!state||!result){report(@{@"phase":@"diagnostic",@"case":name,@"passed":@NO,@"error":error.description?:@"allocation"});passed=false;continue;}
        std::memset(result.contents,0xa5,result.length);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:state];[encoder setBuffer:config offset:0 atIndex:0];[encoder setBuffer:result offset:0 atIndex:1];
        [encoder setTexture:source atIndex:0];[encoder setTexture:filter atIndex:3];[encoder setSamplerState:sampler atIndex:0];
        [encoder dispatchThreads:MTLSizeMake([name isEqualToString:@"replay_uniform"]?44:64,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
        [encoder endEncoding];const bool done=complete(command,name);bool correct=done;
        NSMutableArray *first=[NSMutableArray array];
        if(done){
            if([name isEqualToString:@"replay_uniform"]){
                correct=!std::memcmp(result.contents,config.contents,176);
                const auto *words=(const uint32_t *)result.contents;for(unsigned i=0;i<12;++i)[first addObject:@(words[i])];
            }else{
                const auto *values=(const float *)result.contents;
                for(unsigned i=0;i<12;++i)[first addObject:std::isfinite(values[i])?@(values[i]):@"nonfinite"];
                if([name isEqualToString:@"replay_filter"]){
                    const auto *expected=(const float *)[(id<MTLBuffer>)filter.buffer contents];
                    if(!expected)correct=false;
                    else for(unsigned i=0;i<64*4;++i)correct=correct&&std::isfinite(values[i])&&std::fabs(values[i]-expected[i])<1e-6f;
                }else{
                    const float expected[4]={128/255.f,black/255.f,128/255.f,1};
                    for(unsigned i=0;i<64*4;++i)correct=correct&&std::isfinite(values[i])&&std::fabs(values[i]-expected[i%4])<0.002f;
                }
            }
        }
        report(@{@"phase":@"diagnostic",@"case":name,@"passed":@(correct),@"first":first,@"capture_started":@NO});
        passed=correct&&passed;
    }
    return passed;
}

int main(int argc,char **argv) {
    @autoreleasepool {
        // No original Apple or captured input bytes are part of this source.
        if(argc!=2)return 2;NSString *folder=@(argv[1]);
        if(!ownMemoryLimit())return 11;
        sleep(150);
        char boot[128]={};size_t length=sizeof boot;sysctlbyname("kern.bootsessionuuid",boot,&length,nullptr,0);
        report(@{@"phase":@"start",@"boot":@(boot),@"pid":@(getpid()),@"capture_started":@NO});
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 3;
        NSError *error=nil;
        id<MTLLibrary> original=[device newLibraryWithURL:[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"VideoToolbox.metallib"]] error:&error];
        id<MTLLibrary> authored=[device newLibraryWithURL:[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"replay-sample.metallib"]] error:&error];
        NSData *uniform=[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"uniform.bin"]];
        NSData *taps=[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"filter.bin"]];
        NSArray *rows=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"constants.json"]] options:0 error:&error];
        if(!original||!authored||uniform.length!=1000||taps.length!=2048||rows.count!=242)return 4;
        MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new];
        for(NSDictionary *row in rows){
            const NSUInteger type=[row[@"type"] unsignedIntegerValue],index=[row[@"index"] unsignedIntegerValue];
            NSArray *bytes=row[@"bytes"];if(!((type==53&&bytes.count==1)||((type==3||type==29||type==33)&&bytes.count==4)))return 5;
            uint8_t value[4]={};for(NSUInteger i=0;i<bytes.count;++i)value[i]=[bytes[i] unsignedCharValue];
            [constants setConstantValue:value type:(MTLDataType)type atIndex:index];
        }
        id<MTLFunction> function=[original newFunctionWithName:@"VTMTSComputeFunction1x1" constantValues:constants error:&error];
        id<MTLComputePipelineState> state=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
        report(@{@"phase":@"pipeline",@"present":@(state!=nil),@"error":error.description?:@"",@"static_threadgroup":@(state.staticThreadgroupMemoryLength),@"max_threads":@(state.maxTotalThreadsPerThreadgroup)});
        if(!state)return 6;
        CVPixelBufferRef input=pixelBuffer(16,16),output=pixelBuffer(1200,2608);if(!input||!output)return 7;
        IOSurfaceRef src=CVPixelBufferGetIOSurface(input),dst=CVPixelBufferGetIOSurface(output);
        id<MTLTexture> sampled=texture(device,src,0,16,16,(MTLPixelFormat)540,MTLTextureUsageShaderRead);
        id<MTLTexture> y=texture(device,dst,0,1200,2608,MTLPixelFormatR8Uint,MTLTextureUsageShaderWrite);
        id<MTLTexture> uv=texture(device,dst,1,600,1304,MTLPixelFormatR16Uint,MTLTextureUsageShaderWrite);
        id<MTLBuffer> config=[device newBufferWithBytes:uniform.bytes length:uniform.length options:MTLResourceStorageModeShared];
        float extents[12]={0,0,15,15,0,0,1199,2607,0,0,599,1303},exposure[4]={10000,0.0001f,1,1};uint8_t more[212]={};
        id<MTLBuffer> extent=[device newBufferWithBytes:extents length:sizeof extents options:MTLResourceStorageModeShared];
        id<MTLBuffer> exp=[device newBufferWithBytes:exposure length:sizeof exposure options:MTLResourceStorageModeShared];
        id<MTLBuffer> auxiliary=[device newBufferWithBytes:more length:sizeof more options:MTLResourceStorageModeShared];
        id<MTLBuffer> filters[2];id<MTLTexture> ft[2];
        auto fd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:2 height:64 mipmapped:NO];
        fd.storageMode=MTLStorageModeShared;fd.usage=MTLTextureUsageShaderRead;
        for(unsigned i=0;i<2;++i){filters[i]=[device newBufferWithBytes:taps.bytes length:taps.length options:MTLResourceStorageModeShared];ft[i]=[filters[i] newTextureWithDescriptor:fd offset:0 bytesPerRow:32];}
        auto sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear;sd.normalizedCoordinates=NO;sd.lodMaxClamp=3.402823466e38f;
        id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];id<MTLCommandQueue> queue=[device newCommandQueue];
        if(!sampled||!y||!uv||!config||!extent||!exp||!auxiliary||!ft[0]||!ft[1]||!sampler||!queue)return 8;
        bool passed=true;
        for(unsigned black:{0u,107u}){
            if(!codes(src,black,128,128))return 9;
            passed=diagnostics(device,queue,authored,sampled,config,ft[0],sampler,black)&&passed;
            for(unsigned tg:{16384u,32768u}){
                NSString *name=[NSString stringWithFormat:@"original-1x1-Y%u-TG%u",black,tg];if(!codes(dst,165,165,165))return 9;
                id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
                [encoder setComputePipelineState:state];
                for(unsigned i=0;i<4;++i)[encoder setBuffer:@[config,extent,exp,auxiliary][i] offset:0 atIndex:i];
                for(unsigned i=0;i<5;++i)[encoder setTexture:@[sampled,y,uv,ft[0],ft[1]][i] atIndex:i];
                [encoder setSamplerState:sampler atIndex:0];[encoder setThreadgroupMemoryLength:tg atIndex:0];
                [encoder dispatchThreadgroups:MTLSizeMake(2,2608,1) threadsPerThreadgroup:MTLSizeMake(1024,1,1)];[encoder endEncoding];
                const bool done=complete(command,name);const bool compared=done&&pixels(dst,name,black==0);passed=compared&&passed;
            }
        }
        CVPixelBufferRelease(input);CVPixelBufferRelease(output);
        report(@{@"phase":@"result",@"passed":@(passed),@"capture_started":@NO});return passed?0:10;
    }
}
