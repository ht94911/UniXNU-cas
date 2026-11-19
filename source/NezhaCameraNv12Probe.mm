// SPDX-License-Identifier: MIT
// Exercise the existing Metal/IOSurface path with known NV12 plane codes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

static id number(float value) {return std::isfinite(value)?@(value):(id)[NSNull null];}

static void report(NSDictionary *value) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if(data){fwrite(data.bytes,1,data.length,stdout);fputc('\n',stdout);fflush(stdout);}
}

int main(int argc,char **argv) {
    @autoreleasepool {
        if(argc!=2){report(@{@"phase":@"arguments",@"passed":@NO});return 2;}
        // Match the ordinary camera startup without opening a capture device.
        sleep(150);
        char boot[128]={};size_t bytes=sizeof boot;
        bool identified=sysctlbyname("kern.bootsessionuuid",boot,&bytes,nullptr,0)==0;
        report(@{@"phase":@"start",@"boot":identified?@(boot):@"unknown",@"pid":@(getpid()),@"uid":@(geteuid()),@"capture_started":@NO});
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device){report(@{@"phase":@"device",@"passed":@NO});return 3;}
        NSError *error=nil;
        id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        id<MTLFunction> function=[library newFunctionWithName:@"sample_nv12"];
        id<MTLComputePipelineState> pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
        if(!pipeline){report(@{@"phase":@"pipeline",@"passed":@NO,@"error":error.description?:@"unavailable"});return 4;}
        NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        CVPixelBufferRef pixel=nullptr;
        if(CVPixelBufferCreate(kCFAllocatorDefault,64,16,kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            (__bridge CFDictionaryRef)attributes,&pixel)!=kCVReturnSuccess)return 5;
        IOSurfaceRef surface=CVPixelBufferGetIOSurface(pixel);
        if(!surface || IOSurfaceGetPlaneCount(surface)!=2 || IOSurfaceLock(surface,0,nullptr))return 6;
        const uint8_t values[4][3]={{128,128,128},{76,85,255},{150,44,21},{29,255,107}};
        const size_t yrow=IOSurfaceGetBytesPerRowOfPlane(surface,0),uvrow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
        auto *y=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0),*uv=(uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
        for(unsigned r=0;r<16;++r)for(unsigned x=0;x<64;++x)y[r*yrow+x]=values[x/16][0];
        for(unsigned r=0;r<8;++r)for(unsigned x=0;x<64;x+=2){uv[r*uvrow+x]=values[x/16][1];uv[r*uvrow+x+1]=values[x/16][2];}
        if(IOSurfaceUnlock(surface,0,nullptr))return 7;
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)500 width:64 height:16 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        id<MTLBuffer> output=[device newBufferWithLength:64*16*4*sizeof(float) options:MTLResourceStorageModeShared];
        if(!texture || !output){report(@{@"phase":@"resources",@"passed":@NO});CVPixelBufferRelease(pixel);return 8;}
        std::memset(output.contents,0,output.length);
        id<MTLCommandQueue> queue=[device newCommandQueue];
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];[encoder setTexture:texture atIndex:0];[encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(64,16,1) threadsPerThreadgroup:MTLSizeMake(8,4,1)];[encoder endEncoding];[command commit];
        auto deadline=[NSDate dateWithTimeIntervalSinceNow:15];
        while(command.status!=MTLCommandBufferStatusCompleted && command.status!=MTLCommandBufferStatusError && [deadline timeIntervalSinceNow]>0)usleep(1000);
        if(command.status!=MTLCommandBufferStatusCompleted || command.error){report(@{@"phase":@"completion",@"passed":@NO,@"status":@(command.status),@"error":command.error.description?:@"timeout"});return 9;}
        const auto *data=(const float *)output.contents;unsigned bad=0;NSMutableArray *samples=[NSMutableArray array];
        for(unsigned r=0;r<16;++r)for(unsigned x=0;x<64;++x){
            const auto *codes=values[x/16];float Y=codes[0]/255.f,U=codes[1]/255.f-0.5f,V=codes[2]/255.f-0.5f;
            const float expected[4]={Y+1.402f*V,Y-0.344136f*U-0.714136f*V,Y+1.772f*U,1.f};
            const float *actual=data+(r*64+x)*4;
            // Check centres, away from the two-plane reconstruction at band edges.
            if(x%16>=4 && x%16<12)for(unsigned c=0;c<4;++c)if(!std::isfinite(actual[c])||std::fabs(actual[c]-expected[c])>0.06f)++bad;
            if(r==8 && x%16==8)[samples addObject:@[@(x),@(codes[0]),@(codes[1]),@(codes[2]),number(actual[0]),number(actual[1]),number(actual[2]),number(actual[3])]];
        }
        report(@{@"phase":@"pixels",@"boot":identified?@(boot):@"unknown",@"pid":@(getpid()),@"passed":@(bad==0),@"bad_channels":@(bad),@"samples":samples,@"source_format":@500,@"surface":@(IOSurfaceGetID(surface)),@"yrow":@(yrow),@"uvrow":@(uvrow),@"device":device.name,@"capture_started":@NO});
        CVPixelBufferRelease(pixel);return bad?10:0;
    }
}
