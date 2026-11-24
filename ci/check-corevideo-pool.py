#!/usr/bin/env python3
"""Run actual ownership methods with real CoreVideo pools; no GPU completion is claimed."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ['RUNNER_TEMP'])/'corevideo-results'
OUT.mkdir(exist_ok=False)
current=(ROOT/'source/NativeBridgeMetalFrontend.mm').read_text()
legacy=(ROOT/'source/frontend-before.mm').read_text()
def body(text,marker):
 start=text.index(marker);opening=text.index('{',start);depth=0
 for i in range(opening,len(text)):
  depth+=text[i]=='{';depth-=text[i]=='}'
  if not depth:return text[start:i+1]+'\n'
 raise ValueError(marker)
start=current.index('struct NBSurfaceUse {');end=current.index('\n};',start)+3
owner=current[start:end]
retain=body(current,'- (void)retainResource:(id)resource {')
prefix=r'''
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <unordered_map>
struct DetailSpan {template<class... T> DetailSpan(T...) {}};
struct NmbTexture {};
static void nmb_texture_release(NmbTexture *) {}
static uint64_t releasedUses,retainedUses;
'''
classes=r'''
@interface NBBuffer : NSObject
@property(nonatomic) IOSurfaceRef importedSurface;
@end
@implementation NBBuffer @end
@interface NBTexture : NSObject
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NmbTexture *handle;
@property(nonatomic,strong) NBTexture *parent;
@property(nonatomic) BOOL counted;
- (void)countUse;
@end
'''
command=r'''
@interface NBCommandBuffer : NSObject {
@public
 std::unordered_map<IOSurfaceRef,std::unique_ptr<NBSurfaceUse>> surfaceUses;
}
@property(nonatomic,strong) NSMutableSet *resourceOwners;
- (void)retainResource:(id)resource;
- (void)finish;
@end
@implementation NBCommandBuffer
- (instancetype)init {if((self=[super init]))_resourceOwners=[NSMutableSet set];return self;}
'''
pool=r'''
static CVPixelBufferPoolRef makePool(unsigned capacity) {
 CVPixelBufferPoolRef p=nullptr;
 NSDictionary *options=@{(id)kCVPixelBufferPoolMinimumBufferCountKey:@(capacity)};
 NSDictionary *pixels=@{(id)kCVPixelBufferWidthKey:@64,(id)kCVPixelBufferHeightKey:@16,
  (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
  (id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES};
 assert(CVPixelBufferPoolCreate(nullptr,(__bridge CFDictionaryRef)options,(__bridge CFDictionaryRef)pixels,&p)==kCVReturnSuccess);return p;
}
static CVReturn take(CVPixelBufferPoolRef pool,unsigned n,CVPixelBufferRef *p) {
 NSDictionary *limit=@{(id)kCVPixelBufferPoolAllocationThresholdKey:@(n)};*p=nullptr;
 return CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr,pool,(__bridge CFDictionaryRef)limit,p);
}
static NBTexture *texture(IOSurfaceRef surface) {
 NBTexture *t=[NBTexture new];t.surface=(IOSurfaceRef)CFRetain(surface);[t countUse];return t;
}
static void blocked(CVPixelBufferPoolRef pool) {
 CVPixelBufferRef p=nullptr;assert(take(pool,1,&p)==kCVReturnWouldExceedAllocationThreshold && !p);
}
int main(int argc,char **argv){assert(argc==2);@autoreleasepool{
 const bool old=!strcmp(argv[1],"legacy");
 NSMutableDictionary *cache=[NSMutableDictionary dictionary];CVPixelBufferPoolRef pool=makePool(13);unsigned frame=0;
 for(;frame<600;++frame){@autoreleasepool{
  CVPixelBufferRef p=nullptr;CVReturn status=take(pool,13,&p);
  if(status!=kCVReturnSuccess){assert(old && frame==13 && status==kCVReturnWouldExceedAllocationThreshold && !p);break;}
  IOSurfaceRef surface=CVPixelBufferGetIOSurface(p);NSNumber *key=@(IOSurfaceGetID(surface));NBTexture *t=cache[key];
  if(!t){t=texture(surface);cache[key]=t;}
  if(!old){
   assert(!IOSurfaceIsInUse(surface));NBCommandBuffer *c=[NBCommandBuffer new];[c retainResource:t];
   assert(IOSurfaceGetUseCount(surface)==1);[c finish];assert(!IOSurfaceIsInUse(surface));
  }else assert(IOSurfaceIsInUse(surface));
  CVPixelBufferRelease(p);
 }}
 if(old){assert(frame==13 && cache.count==13);[cache removeAllObjects];CFRelease(pool);puts("PASS legacy: original CoreVideo capacity13 exhausts with cached texture object uses");return 0;}
 assert(frame==600 && cache.count<=13);[cache removeAllObjects];CFRelease(pool);
 pool=makePool(1);CVPixelBufferRef p=nullptr;assert(take(pool,1,&p)==kCVReturnSuccess);
 IOSurfaceRef surface=CVPixelBufferGetIOSurface(p);NBTexture *root=texture(surface),*view=texture(surface),*nested=texture(surface);view.parent=root;nested.parent=view;
 NBBuffer *buffer=[NBBuffer new];buffer.importedSurface=surface;assert(!IOSurfaceIsInUse(surface));
 NBCommandBuffer *a=[NBCommandBuffer new],*b=[NBCommandBuffer new];
 for(unsigned i=0;i<1000;++i){[a retainResource:root];[a retainResource:view];[a retainResource:nested];[a retainResource:buffer];}
 assert(IOSurfaceGetUseCount(surface)==1);[b retainResource:nested];assert(IOSurfaceGetUseCount(surface)==2);
 CVPixelBufferRelease(p);blocked(pool);[a finish];assert(IOSurfaceGetUseCount(surface)==1);blocked(pool);
 [b finish];assert(!IOSurfaceIsInUse(surface));assert(take(pool,1,&p)==kCVReturnSuccess);CVPixelBufferRelease(p);
 // These are controlled lifecycle transitions, not simulated GPU callbacks.
 @autoreleasepool{NBCommandBuffer *discard=[NBCommandBuffer new];[discard retainResource:nested];blocked(pool);discard=nil;}
 assert(!IOSurfaceIsInUse(surface));assert(take(pool,1,&p)==kCVReturnSuccess);CVPixelBufferRelease(p);
 NBCommandBuffer *refused=[NBCommandBuffer new];[refused retainResource:root];blocked(pool);[refused finish];assert(!IOSurfaceIsInUse(surface));
 [a finish];[b finish];assert(!IOSurfaceIsInUse(surface));
 assert(take(pool,1,&p)==kCVReturnSuccess);CVPixelBufferRelease(p);CFRelease(pool);
 puts("PASS current: 600 real CoreVideo pool reuses; independent uses, cached views, imported buffers, controlled finish and discard");
}}
'''
reports={}
for label,text in (('legacy',legacy),('current',current)):
 texture_part=text[text.index('@implementation NBTexture\n'):]
 dealloc=body(texture_part,'- (void)dealloc {')
 if label=='legacy':
  count=body(texture_part,'- (void)countUse {');policy='static const bool gCountsSurfaceUse=true;\n'
  # The real regression's commands are unused on the legacy path. Compile the
  # current owner method independently in both instruments to keep one driver.
 else:count='- (void)countUse {}\n';policy=''
 finish='- (void)finish {[_resourceOwners removeAllObjects];surfaceUses.clear();}\n'
 assert 'surfaceUses.clear();' in body(current,'- (void)finish {')
 code=prefix+owner+'\n'+classes+policy+'@implementation NBTexture\n'+dealloc+count+'@end\n'+command+retain+finish+'@end\n'+pool
 source=OUT/(label+'.mm');source.write_text(code);exe=OUT/label
 build=subprocess.run(['xcrun','clang++','-std=c++17','-fobjc-arc','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',
  '-fsanitize=address,undefined',str(source),'-framework','Foundation','-framework','CoreVideo','-framework','IOSurface','-o',str(exe)],capture_output=True,text=True,timeout=120)
 (OUT/(label+'-build.log')).write_text(build.stdout+build.stderr);assert build.returncode==0,(label,build.stderr)
 run=subprocess.run([str(exe),label],capture_output=True,text=True,timeout=30);(OUT/(label+'-run.log')).write_text(run.stdout+run.stderr)
 assert run.returncode==0 and 'PASS '+label+':' in run.stdout,(label,run.returncode,run.stdout,run.stderr)
 reports[label]=dict(exit_code=run.returncode,output=run.stdout);print(run.stdout,end='')
report=dict(passed=True,original_CoreVideo_pool=True,original_IOSurface_counts=True,actual_source_owner_and_retain_method=True,
 source_sha256=hashlib.sha256(current.encode()).hexdigest(),owner_sha256=hashlib.sha256(owner.encode()).hexdigest(),
 old_pool_exhaustion_reproduced=True,capacity_unchanged=13,real_CoreVideo_reuses=600,independent_holders=True,views_and_imported_buffer_deduplicated=True,
 unsubmitted_discard_releases=True,controlled_lifecycle_finish=True,ASan_UBSan=True,GPU_execution=False,GPU_completion_not_simulated=True,
 full_finish_executed=False,phone_operations=False,results=reports)
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
