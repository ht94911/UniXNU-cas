"""Exercise actual discovery with Foundation classes and owned fixture memory on Mac."""
from pathlib import Path
import argparse
import hashlib
import json
import platform
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--cxx',default='clang++');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
folder=a.source.parent;inc=(folder/'NativeFrameObservers.inc').read_text()
header=(folder/'NativeDisplayWaitState.h').read_text().replace('#pragma once\n','',1)
reader=inc[inc.index('static int readObserverMemory('):inc.index('extern "C" void native_bridge_metal_note')]
fragment=inc[inc.index('extern "C" void native_bridge_metal_note'):inc.index('static void traceDisplayObserver')]
assert fragment.count('#if defined(__arm64e__)')==1
fragment=fragment.replace('#if defined(__arm64e__)','#if 1 /* fixture owns the pointer-memory contract */')
prefix=r'''
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
'''
fixtures=r'''
static Class fixtureServerClass,fixtureDisplayClass;
static id fixtureServer;
static NSArray *fixtureDisplays;
static bool published=false,imagePresent=true;
static uint64_t clockNow=100;
static uint64_t nowUs(){return clockNow++;}
struct FixtureImage {mach_header_64 header;uuid_command uuid;};
static FixtureImage fixtureImage;
static unsigned noteCount;
extern "C" void native_bridge_metal_note(const char *s){assert(!strncmp(s,"display-wait discovery=",23));++noteCount;}
static id fixtureServerGetter(id,SEL){return published?fixtureServer:nil;}
static id fixtureDisplaysGetter(id,SEL){return fixtureDisplays;}
static Class fixtureClass(const char *name){
 if(!strcmp(name,"CAWindowServer"))return fixtureServerClass;
 if(!strcmp(name,"CAWindowServerDisplay"))return fixtureDisplayClass;
 return objc_getClass(name);
}
static uint32_t fixtureImageCount(){return imagePresent?1:0;}
static const char *fixtureImageName(uint32_t i){assert(!i);return "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";}
static const mach_header *fixtureImageHeader(uint32_t i){assert(!i);return (const mach_header *)&fixtureImage.header;}
static intptr_t fixtureImageSlide(uint32_t i){assert(!i);return 0;}
#define objc_getClass fixtureClass
#define _dyld_image_count fixtureImageCount
#define _dyld_get_image_name fixtureImageName
#define _dyld_get_image_header fixtureImageHeader
#define _dyld_get_image_vmaddr_slide fixtureImageSlide
'''
main=r'''
static void resetDiscovery(){
 gObservedWindowDisplay=nil;gObservedWindowCandidate=nil;gObservedQuartzRejected=false;
 gDisplayDiscovery={};gDiscoveryClassSize=0;gDiscoveryImplOffset=-1;gDiscoveryDisplayCount=0;
}
static bool discover(uint64_t now){
 assert(gDisplayDiscovery.due(now,gObservedWindowDisplay!=nil,gObservedQuartzRejected));
 gDisplayDiscovery.attempted(now);bool result=prepareDisplayObserver();reportDisplayDiscovery();return result;
}
int main(){@autoreleasepool {
 static_assert(sizeof(nbdisplaywait::State{}.serverTail)==nbdisplaywait::kServerTailBytes);
 static_assert(sizeof(nbdisplaywait::State{}.timerTail)==nbdisplaywait::kTimerTailBytes);
 fixtureImage.header.magic=MH_MAGIC_64;fixtureImage.header.ncmds=1;fixtureImage.header.sizeofcmds=sizeof(uuid_command);
 fixtureImage.uuid.cmd=LC_UUID;fixtureImage.uuid.cmdsize=sizeof(uuid_command);
 memcpy(fixtureImage.uuid.uuid,nbdisplaywait::kQuartzUUID,16);
 fixtureServerClass=objc_allocateClassPair([NSObject class],"NBDiscoveryFixtureServer",0);assert(fixtureServerClass);
 assert(class_addMethod(object_getClass(fixtureServerClass),sel_registerName("serverIfRunning"),(IMP)fixtureServerGetter,"@@:"));
 assert(class_addMethod(fixtureServerClass,sel_registerName("displays"),(IMP)fixtureDisplaysGetter,"@@:"));
 objc_registerClassPair(fixtureServerClass);fixtureServer=[fixtureServerClass new];
 fixtureDisplayClass=objc_allocateClassPair([NSObject class],"NBDiscoveryFixtureDisplay",0);assert(fixtureDisplayClass);
 assert(class_addIvar(fixtureDisplayClass,"_impl",8,3,"^v"));
 assert(class_addIvar(fixtureDisplayClass,"_padding",56,3,"[56c]"));objc_registerClassPair(fixtureDisplayClass);
 assert(class_getInstanceSize(fixtureDisplayClass)==72);
 id display=[fixtureDisplayClass new];
 auto *impl=(unsigned char *)calloc(1,0x100);auto *server=(unsigned char *)calloc(1,0x620);
 auto *cppDisplay=(unsigned char *)calloc(1,0x100);assert(impl&&server&&cppDisplay);
 *(uintptr_t *)((unsigned char *)(__bridge void *)display+8)=(uintptr_t)impl;
 *(uintptr_t *)(impl+0x40)=(uintptr_t)server;
 *(uint64_t *)server=nbdisplaywait::kServerVtable;*(uintptr_t *)(server+0x60)=(uintptr_t)cppDisplay;
 *(uint64_t *)cppDisplay=nbdisplaywait::kDisplayVtable;
 fixtureDisplays=@[display];unsigned cases=0;
 for(unsigned i=0;i<12;++i){assert(!discover(i*1000000ull));assert(gDisplayDiscovery.status==nbdisplaywait::WaitingServer);}
 assert(gDisplayDiscovery.attempts==12 && !gObservedQuartzRejected);++cases;
 published=true;assert(discover(12000000));assert(gObservedWindowDisplay==display && gDisplayDiscovery.status==0);++cases;
 resetDiscovery();*(uint64_t *)server=nbdisplaywait::kServerVtable+8;
 assert(!discover(0));assert(gDisplayDiscovery.status==nbdisplaywait::CandidateLayoutRejected && gObservedWindowCandidate==display);
 nbdisplaywait::State state{};state.object=(uintptr_t)(__bridge void *)gObservedWindowCandidate;
 nbdisplaywait::capture(state,0,readObserverMemory,nowUs);
 assert(state.stop==nbdisplaywait::WrongServerVtable && state.serverVtable==nbdisplaywait::kServerVtable+8);++cases;
 *(uint64_t *)server=nbdisplaywait::kServerVtable;assert(discover(1000000));++cases;
 resetDiscovery();fixtureImage.uuid.uuid[0]^=1;assert(!discover(0));
 assert(gObservedQuartzRejected && gDisplayDiscovery.status==nbdisplaywait::WrongQuartz);
 assert(!gDisplayDiscovery.due(2000000,false,true));fixtureImage.uuid.uuid[0]^=1;++cases;
 resetDiscovery();fixtureDisplays=@[];assert(!discover(0));assert(gDisplayDiscovery.status==nbdisplaywait::EmptyDisplayCollection);
 fixtureDisplays=@[display];assert(discover(1000000));++cases;
 resetDiscovery();imagePresent=false;assert(!discover(0));assert(gDisplayDiscovery.status==nbdisplaywait::WaitingQuartz);
 imagePresent=true;assert(discover(1000000));++cases;
 assert(noteCount>=2 && noteCount<=16);
 resetDiscovery();fixtureDisplays=nil;*(uintptr_t *)((unsigned char *)(__bridge void *)display+8)=0;
 free(cppDisplay);free(server);free(impl);
 printf("{\"cases\":%u,\"notes\":%u,\"delayed_discovery\":true,\"actual_ObjC_runtime\":true,\"actual_Mach_self_reads\":true}\n",cases,noteCount);
}return 0;}
'''
cpp=a.out/'discovery.mm';cpp.write_text(prefix+header+fixtures+reader+fragment+main)
report=dict(source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
    observer_include_sha256=hashlib.sha256((folder/'NativeFrameObservers.inc').read_bytes()).hexdigest(),
    header_sha256=hashlib.sha256((folder/'NativeDisplayWaitState.h').read_bytes()).hexdigest(),
    fixture_sha256=hashlib.sha256(cpp.read_bytes()).hexdigest(),phone_operations=False,
    fixture_only=True,real_Mac_executed=False,
    controlled_substitutions=['dyld image inventory owns one UUID-bound fixture header',
        'class lookup names map to owned Foundation subclasses','arm64e discovery branch exercised on host architecture'])
if platform.system()=='Darwin':
    binary=a.out/'discovery';command=[a.cxx,'-std=c++17','-fobjc-arc','-O1','-g','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-framework','Foundation',str(cpp),'-o',str(binary)]
    result=subprocess.run(command,capture_output=True,text=True);(a.out/'build.log').write_text(result.stdout+result.stderr)
    assert not result.returncode,result.stderr
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20);(a.out/'run.log').write_text(result.stdout+result.stderr)
    assert not result.returncode,result.stdout+result.stderr
    observed=json.loads(result.stdout);assert observed['cases']==7
    report.update(real_Mac_executed=True,actual_result=observed,command=command)
report['passed']=True
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
