#!/usr/bin/env python3
"""Exercise production compute-dump forwarding and planar CPU sample bounds."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def body(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for offset in range(brace, len(source)):
        depth += source[offset] == '{'
        depth -= source[offset] == '}'
        if not depth:
            return source[start:offset + 1] + '\n'
    raise ValueError('Incomplete method ' + signature)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
text = args.source.read_text()
source = body(text, 'static NSString *dumpSurfaceCodes(')
compute = text[text.index('@implementation NBComputeEncoder\n'):]
state = body(compute, '- (void)setComputePipelineState:')
dispatch = body(compute, '- (void)dispatch:(MTLSize)')
selection = body(text, 'static void dumpSelectCompute(')
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
prefix = r'''
#import <Foundation/Foundation.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
struct MTLSize {NSUInteger width,height,depth;};
static MTLSize MTLSizeMake(NSUInteger w,NSUInteger h,NSUInteger d) {return {w,h,d};}
@protocol MTLComputePipelineState @end
struct __IOSurface {uint32_t format;size_t planes,width,height,size,offset[2],row[2];uint8_t *base;};
static uint32_t IOSurfaceGetID(IOSurfaceRef) {return 7;}
static uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s) {return s->format;}
static size_t IOSurfaceGetPlaneCount(IOSurfaceRef s) {return s->planes;}
static size_t IOSurfaceGetWidth(IOSurfaceRef s) {return s->width;}
static size_t IOSurfaceGetHeight(IOSurfaceRef s) {return s->height;}
static size_t IOSurfaceGetAllocSize(IOSurfaceRef s) {return s->size;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s) {return s->base;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,unsigned p) {return s->base+s->offset[p];}
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,unsigned p) {return s->row[p];}
struct Work {unsigned dispatches;};
static bool enabled;
static NSString *filterText;
static unsigned dispatchCalls,pipelineCalls,bindingCalls;
static bool askedThreads;static uint64_t askedSize[3],askedGroup[3];
static uint64_t gDispatches;
static unsigned nativeEncoder,nativePipeline;
@interface NBTexture : NSObject
@property(nonatomic) IOSurfaceRef surface;
@end
@implementation NBTexture @end
@interface NBCommandBuffer : NSObject {@public Work encodedWork;}
@property(nonatomic) unsigned dumpKernels;
@property(nonatomic) BOOL dumpReadback,dumpComputeSelected;
@property(nonatomic,strong) NSMutableArray *dumpLog;
- (void)refuse:(NSString *)text;
@end
@implementation NBCommandBuffer
- (void)refuse:(NSString *)text {assert(text);}
@end
@interface NBComputePipeline : NSObject
@property(nonatomic) void *handle;
@property(nonatomic,copy) NSString *dumpInfo;
@end
@implementation NBComputePipeline @end
@interface NBComputeEncoder : NSObject
@property(nonatomic) void *handle;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@property(nonatomic,strong) id dumpPipeline;
- (void)dumpBindings;
- (void)check:(bool)value what:(const char *)what;
@end
static bool dumpMaybe(NBCommandBuffer *c) {return enabled && !c.dumpReadback && (!filterText.length || c.dumpComputeSelected);}
static std::pair<unsigned,unsigned> dumpRange() {return {enabled?1u:0u,2u};}
static NSString *dumpComputeFilter() {return filterText;}
static void dumpLine(NBCommandBuffer *c,NSString *text) {if(!c.dumpLog)c.dumpLog=[NSMutableArray array];[c.dumpLog addObject:text];}
static bool nmb_compute_set_pipeline(void *e,void *p) {assert(e==&nativeEncoder&&p==&nativePipeline);++pipelineCalls;return true;}
static bool nmb_compute_dispatch(void *e,const uint64_t *s,const uint64_t *g,bool threads) {
 assert(e==&nativeEncoder);++dispatchCalls;askedThreads=threads;
 for(unsigned i=0;i<3;++i){askedSize[i]=s[i];askedGroup[i]=g[i];}return true;
}
#define ENCODE(...) do{__VA_ARGS__;}while(0)
'''
prefix += selection + r'''
@implementation NBComputeEncoder
- (void)dumpBindings {++bindingCalls;}
- (void)check:(bool)value what:(const char *)what {assert(value&&what);}
'''
fixture = r'''
@end
int main(){@autoreleasepool{
 unsigned checks=0;
 std::vector<uint8_t> bytes(8192,0);
 __IOSurface s{0x34323066,2,64,16,bytes.size(),{0,4096},{64,64},bytes.data()};
 NBTexture *texture=[NBTexture new];texture.surface=&s;
 bytes[8*64+32]=55;bytes[4096+4*64+32]=128;bytes[4096+4*64+33]=118;
 assert([dumpSurfaceCodes(texture) containsString:@"center 55/128/118"]);++checks;
 s.format=0x34323076;assert(dumpSurfaceCodes(texture));++checks;
 s.format=0x42475241;assert(!dumpSurfaceCodes(texture));++checks;s.format=0x34323066;
 s.planes=1;assert(!dumpSurfaceCodes(texture));++checks;s.planes=2;
 s.row[1]=0;assert(!dumpSurfaceCodes(texture));++checks;s.row[1]=64;
 s.offset[1]=s.size;assert(!dumpSurfaceCodes(texture));++checks;s.offset[1]=4096;
 s.row[1]=s.size;assert(!dumpSurfaceCodes(texture));++checks;s.row[1]=64;
 s.size=4096+4*64+33;assert(!dumpSurfaceCodes(texture));++checks;s.size=bytes.size();
 NBComputeEncoder *e=[NBComputeEncoder new];e.handle=&nativeEncoder;e.commandBuffer=[NBCommandBuffer new];e.commandBuffer.dumpKernels=3;
 NBComputePipeline *p=[NBComputePipeline new];p.handle=&nativePipeline;p.dumpInfo=@"owned compute k=#1:29:01000000";
 enabled=false;[e setComputePipelineState:(id<MTLComputePipelineState>)p];
 [e dispatch:MTLSizeMake(13,7,1) group:MTLSizeMake(8,4,1) threads:true];
 assert(dispatchCalls==1&&pipelineCalls==1&&bindingCalls==0&&!e.dumpPipeline&&!e.commandBuffer.dumpLog);++checks;
 enabled=true;[e setComputePipelineState:(id<MTLComputePipelineState>)p];
 [e dispatch:MTLSizeMake(5,3,1) group:MTLSizeMake(4,2,1) threads:false];
 assert(dispatchCalls==2&&pipelineCalls==2&&bindingCalls==1&&e.dumpPipeline==p);++checks;
 assert(!askedThreads&&askedSize[0]==5&&askedSize[1]==3&&askedGroup[0]==4&&askedGroup[1]==2);++checks;
 assert([e.commandBuffer.dumpLog.firstObject containsString:@"kernel 3 dispatch groups 5x3x1 group 4x2x1 | owned compute"]);++checks;
 assert(e.commandBuffer->encodedWork.dispatches==2&&gDispatches==2);++checks;
 filterText=@"compute VTMTS";
 e.commandBuffer=[NBCommandBuffer new];e.dumpPipeline=nil;
 NBComputePipeline *startup=[NBComputePipeline new];startup.handle=&nativePipeline;startup.dumpInfo=@"compute lanczosVerticalScale k=";
 p.dumpInfo=@"compute VTMTSComputeFunction1x1 k=#61:29:02000000";
 [e setComputePipelineState:(id<MTLComputePipelineState>)startup];
 [e dispatch:MTLSizeMake(5,3,1) group:MTLSizeMake(4,2,1) threads:false];
 assert(!e.commandBuffer.dumpComputeSelected&&!e.commandBuffer.dumpLog&&bindingCalls==1&&dispatchCalls==3);++checks;
 [e setComputePipelineState:(id<MTLComputePipelineState>)p];
 assert(e.commandBuffer.dumpComputeSelected);++checks;
 [e setComputePipelineState:(id<MTLComputePipelineState>)p];
 [e dispatch:MTLSizeMake(13,7,1) group:MTLSizeMake(8,4,1) threads:true];
 assert(bindingCalls==2&&dispatchCalls==4&&askedThreads&&askedSize[0]==13&&askedGroup[0]==8);++checks;
 NBCommandBuffer *readback=[NBCommandBuffer new];readback.dumpReadback=YES;dumpSelectCompute(readback,p);
 assert(!readback.dumpComputeSelected);++checks;
 NBCommandBuffer *second=[NBCommandBuffer new];dumpSelectCompute(second,p);assert(second.dumpComputeSelected);++checks;
 NBCommandBuffer *third=[NBCommandBuffer new];dumpSelectCompute(third,p);assert(!third.dumpComputeSelected);++checks;
 enabled=false;NBCommandBuffer *disabled=[NBCommandBuffer new];dumpSelectCompute(disabled,p);assert(!disabled.dumpComputeSelected);++checks;
 printf("PASS compute dump: %u production checks; disabled dispatch unchanged, function/bindings and two plane codes\n",checks);
}return 0;}
'''
harness = out / 'consumer.mm'
harness.write_text(prefix + state + dispatch + '@end\n' + source + fixture.replace('@end\n', '', 1))
command = ['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', '-O1', '-g0',
    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', str(harness),
    '-framework', 'Foundation', '-framework', 'Metal', '-o', str(out / 'consumer')]
compiled = subprocess.run(command, capture_output=True)
(out / 'build.log').write_bytes(compiled.stdout + compiled.stderr)
assert compiled.returncode == 0, compiled.stderr.decode(errors='replace')
executed = subprocess.run([str(out / 'consumer')], capture_output=True, timeout=15)
(out / 'run.log').write_bytes(executed.stdout + executed.stderr)
assert executed.returncode == 0 and b'PASS compute dump: 20 ' in executed.stdout
report = dict(passed=True, checks=20, actual_ObjectiveC_bodies_executed=True,
    disabled_dispatch_arguments_unchanged=True, compute_function_and_bindings_logged=True,
    planar_CPU_codes_bounds_checked=True, compute_filter_only_matching_commands=True,
    compute_filter_budget=2, repeated_pipeline_does_not_spend_budget=True,
    readbacks_do_not_spend_budget=True, no_GPU_created=True,
    source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(), phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(executed.stdout.decode().strip())
