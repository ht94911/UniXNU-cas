"""Invoke the actual heap and imageblock Objective-C bodies with captured C ABI calls."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
kit = a.unixnu.resolve() / 'drivers/AdrenoMetal'
source = kit / 'NativeBridgeMetalFrontend.mm'
text = source.read_text()
heap = text[text.index('- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {'):
            text.index('// A descriptor the runtime refuses answers {0, 0}')]
heap_class = text[text.index('@interface NBHeap : NBObject'):
                  text.index('- (id<MTLBuffer>)bufferWithLength:', text.index('@interface NBHeap : NBObject'))] + '@end\n'
query = text[text.index('- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions {'):
             text.index('@end', text.index('- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions {'))]
size = text[text.index('- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {'):
            text.index('- (void)dispatch:(MTLSize)size', text.index('- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {'))]
out = a.logs / 'camera-forwarding'
out.mkdir(parents=True, exist_ok=False)
prefix = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
struct NmbDevice {}; struct NmbHeap {}; struct NmbEncoder {}; struct NmbComputePipeline {};
static NmbDevice nativeDevice; static NmbDevice *gDevice=&nativeDevice;
static NmbHeap nativeHeap; static NmbEncoder nativeEncoder; static NmbComputePipeline nativePipeline;
static uint64_t askedType,askedSize,askedOptions,askedWidth,askedHeight,askedDimensions[3];
static unsigned heapCalls,sizeCalls,queryCalls,checkCalls;
static bool refused=false,lastCheck=true;
struct DetailSpan {template<class... T> DetailSpan(T...) {}};
template<class F> static auto syncDetail(unsigned,const char *,F body) {return body();}
#define NBLOG(...) do {} while(0)
#define ENCODE(...) do {__VA_ARGS__;} while(0)
static NSString *lastError() {return @"captured refusal";}
static NmbHeap *nmb_heap_new(NmbDevice *d,uint64_t type,uint64_t bytes,uint64_t options) {
 assert(d==gDevice);++heapCalls;askedType=type;askedSize=bytes;askedOptions=options;
 return refused?nullptr:&nativeHeap;
}
static void nmb_heap_release(NmbHeap *h) {assert(h==&nativeHeap);}
static uint64_t nmb_heap_size(const NmbHeap *h) {assert(h==&nativeHeap);return askedSize;}
static uint64_t nmb_heap_used_size(const NmbHeap *) {return 0;}
static uint64_t nmb_heap_allocated_size(const NmbHeap *) {return askedSize;}
static uint64_t nmb_heap_max_available_size(const NmbHeap *,uint64_t) {return askedSize;}
static uint64_t nmb_heap_set_purgeable_state(const NmbHeap *,uint64_t state) {return state;}
static bool nmb_compute_set_imageblock_size(NmbEncoder *e,uint64_t w,uint64_t h) {
 assert(e==&nativeEncoder);++sizeCalls;askedWidth=w;askedHeight=h;return !refused;
}
static uint64_t nmb_compute_pipeline_imageblock_memory_length(const NmbComputePipeline *p,const uint64_t *d) {
 assert(p==&nativePipeline);++queryCalls;for(unsigned i=0;i<3;++i)askedDimensions[i]=d[i];return 8192;
}
@interface NBObject : NSObject @end
@implementation NBObject @end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@interface NBComputeEncoder : NBObject
@property(nonatomic) NmbEncoder *handle;
- (void)check:(bool)value what:(const char *)what;
@end
@interface NBDeviceMethods : NSObject @end
'''
body = (prefix + heap_class + '@implementation NBDeviceMethods\n' + heap + '@end\n' +
        '@implementation NBComputePipeline\n' + query + '@end\n' +
        '@implementation NBComputeEncoder\n'
        '- (void)check:(bool)value what:(const char *)what {assert(what);++checkCalls;lastCheck=value;}\n' + size + '@end\n')
fixture = r'''
@interface Descriptor : NSObject
@property(nonatomic) NSUInteger size;
@property(nonatomic) MTLHeapType type;
@property(nonatomic) MTLResourceOptions resourceOptions;
@end
@implementation Descriptor @end
int main() {@autoreleasepool {
 NBDeviceMethods *device=[NBDeviceMethods new];unsigned checks=0;
 for(uint64_t value:{0x200ull,0x200200ull,0x400200ull,0x600200ull,0x600220ull}) {
  Descriptor *d=[Descriptor new];d.size=65536;d.type=MTLHeapTypePlacement;d.resourceOptions=(MTLResourceOptions)value;
  NBHeap *heap=(NBHeap *)[device newHeapWithDescriptor:(MTLHeapDescriptor *)d];
  assert(heap && askedType==1 && askedSize==65536 && askedOptions==value);
  assert((uint64_t)heap.resourceOptions==(value&~0x600000ull));
  assert((uint64_t)heap.storageMode==((value>>4)&15));
  assert((uint64_t)heap.cpuCacheMode==(value&15));
  assert((uint64_t)heap.hazardTrackingMode==((value>>8)&3));
  assert((uint64_t)d.resourceOptions==value);++checks;
 }
 refused=true;Descriptor *d=[Descriptor new];d.size=65536;
 assert(![device newHeapWithDescriptor:(MTLHeapDescriptor *)d]);++checks;
 unsigned before=heapCalls;assert(![device newHeapWithDescriptor:nil] && heapCalls==before);++checks;
 NBComputeEncoder *encoder=[NBComputeEncoder new];encoder.handle=&nativeEncoder;
 refused=false;[encoder setImageblockWidth:32 height:32];assert(askedWidth==32 && askedHeight==32 && lastCheck);++checks;
 [encoder setImageblockWidth:64 height:16];assert(askedWidth==64 && askedHeight==16);++checks;
 refused=true;[encoder setImageblockWidth:65 height:65];assert(!lastCheck && sizeCalls==checkCalls);++checks;
 NBComputePipeline *pipeline=[NBComputePipeline new];pipeline.handle=&nativePipeline;
 assert([pipeline imageblockMemoryLengthForDimensions:MTLSizeMake(32,32,7)]==8192);
 assert(askedDimensions[0]==32 && askedDimensions[1]==32 && askedDimensions[2]==7 && queryCalls==1);++checks;
 printf("PASS Camera forwarding: %u actual Objective-C heap/imageblock calls\n",checks);
} return 0;}
'''
harness = out / 'consumer.mm'
harness.write_text(body + fixture)
result = subprocess.run(['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', '-O1', '-g0',
                         '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                         '-Wno-deprecated-declarations', str(harness), '-framework', 'Foundation',
                         '-framework', 'Metal', '-o', str(out / 'consumer')], capture_output=True, text=True)
(out / 'build.log').write_text(result.stdout + result.stderr)
assert result.returncode == 0, 'Actual forwarding consumer compilation failed'
result = subprocess.run([str(out / 'consumer')], capture_output=True, text=True, timeout=30)
(out / 'run.log').write_text(result.stdout + result.stderr)
assert result.returncode == 0 and 'PASS Camera forwarding: 11 actual' in result.stdout, result.stdout + result.stderr
report = dict(passed=True, actual_ObjectiveC_body_executed=True, checks=11,
              heap_constructor_forwarding=True, heap_option_getters=True,
              production_imageblock_size_selector=True, production_pipeline_memory_query=True,
              C_ABI_calls_captured=True, host_Vulkan_device_opened=False,
              source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
              source_method_bodies_sha256=hashlib.sha256((heap + heap_class + query + size).encode()).hexdigest(),
              physical_camera_preview_verified=False, phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
