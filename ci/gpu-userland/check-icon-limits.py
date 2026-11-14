"""Exercise the real limits fill loop and ARC pressure-source ownership."""
from pathlib import Path
import argparse
import hashlib
import json
import platform
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--cxx", default="clang++")
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
source = args.source.read_text()
start = source.index("static void fillBaseLimits(id device) {")
end = source.index("static void installAnswers(Class cls)", start)
helper = source[start:end]
assert "- (void)initLimits {fillBaseLimits(self);}" in source
pressure_start = source.index("static void watchImportPressure() {")
pressure_end = source.index("\n// The entry records", pressure_start)
pressure = source[pressure_start:pressure_end]
assert pressure.count("static dispatch_source_t source;") == 1
assert "dispatch_source_t source=dispatch_source_create" not in pressure

prefix = r"""
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <string>
typedef uintptr_t NSUInteger;
typedef const char *SEL;
struct FakeClass;
typedef FakeClass *Class;
typedef void *id;
struct FakeMethod {const char *types; unsigned argc; void *implementation;};
typedef FakeMethod *Method;
struct FakeIvar {ptrdiff_t offset;};
typedef FakeIvar *Ivar;
struct FakeClass {
 size_t instance;
 std::vector<FakeIvar> ivars;
 std::unordered_map<std::string,FakeMethod> methods;
};
static FakeClass baseClass,deviceClass;
static bool present=true,compatible=true,limitsPresent=true;
static ptrdiff_t baseOffset=8;
static unsigned notes=0;
static std::unordered_map<SEL,uint64_t> gAnswers;
static Class objc_getClass(const char *) {return present?&baseClass:nullptr;}
static SEL sel_registerName(const char *name) {return name;}
static const char *sel_getName(SEL sel) {return sel;}
static Class object_getClass(id) {return &deviceClass;}
static Method class_getInstanceMethod(Class cls,SEL sel) {
 auto i=cls->methods.find(sel);return i==cls->methods.end()?nullptr:&i->second;
}
static size_t class_getInstanceSize(Class cls) {return cls->instance;}
static const char *method_getTypeEncoding(Method m) {return m->types;}
static unsigned method_getNumberOfArguments(Method m) {return m->argc;}
static void *method_getImplementation(Method m) {return m->implementation;}
static Ivar *class_copyIvarList(Class cls,unsigned *count) {
 *count=(unsigned)cls->ivars.size();
 auto p=(Ivar *)malloc(sizeof(Ivar)*(*count));
 for(unsigned i=0;i<*count;++i)p[i]=&cls->ivars[i];
 return p;
}
static ptrdiff_t ivar_getOffset(Ivar i) {return i->offset;}
static uint8_t *objc_msgSend(id obj,SEL) {return limitsPresent?(uint8_t *)obj+baseOffset:nullptr;}
static bool fixtureKind(id,Class) {return compatible;}
#define NBLOG(...) do{++notes;(void)filled;}while(0)
template<typename T> static T get(id obj,size_t offset) {
 T v;memcpy(&v,(uint8_t *)obj+baseOffset+offset,sizeof v);return v;
}
static NSUInteger width(id obj,SEL){return get<uint32_t>(obj,0x84);}
static NSUInteger height(id obj,SEL){return get<uint32_t>(obj,0x88);}
static NSUInteger wide(id obj,SEL){return get<uint64_t>(obj,0x98);}
static NSUInteger narrow(id obj,SEL){return get<uint32_t>(obj,0xa0);}
static NSUInteger dependent(id obj,SEL){return 2*get<uint32_t>(obj,0xa8);}
static NSUInteger constant(id,SEL){return 7;}
static void add(const char *name,NSUInteger (*fn)(id,SEL),uint64_t value) {
 baseClass.methods[name]={"Q16@0:8",2,(void *)fn};
 deviceClass.methods[name]={"Q16@0:8",2,(void *)fn};
 gAnswers[name]=value;
}
"""
body = helper.replace("[device isKindOfClass:base]", "fixtureKind(device,base)")
body = body.replace("(__bridge void *)device", "(void *)device")
fixture = r"""
int main() {
 alignas(16) uint8_t memory[1024];unsigned cases=0;
 for(ptrdiff_t offset: {ptrdiff_t(8),ptrdiff_t(40)}) {
  baseOffset=offset;baseClass={};deviceClass={};gAnswers.clear();notes=0;
  baseClass.instance=sizeof(memory);
  baseClass.ivars={{baseOffset},{baseOffset+0x180},{baseOffset+0x200}};
  baseClass.methods["limits"]={"^v16@0:8",2,(void *)constant};
  add("maxTextureWidth2D",width,16384);add("maxTextureHeight2D",height,16384);
  add("maxBufferLength",wide,0x123456789ULL);
  add("maxWideAnswerOnNarrowField",narrow,0x100000007ULL);
  add("maxDependent",dependent,99);add("maxConstant",constant,18);
  add("unrelated",constant,31);
  memset(memory,0x5a,sizeof memory);memset(memory+baseOffset,0,0x180);
  const auto before=std::vector<uint8_t>(memory,memory+sizeof memory);
  // The actual iPhone IconRendering consumer rejects a 120x120 icon while
  // the original empty structure says 0x0.
  assert(120>width(memory,nullptr) && 120>height(memory,nullptr));++cases;
  fillBaseLimits(memory);
  assert(width(memory,nullptr)==16384 && height(memory,nullptr)==16384);
  assert(120<=width(memory,nullptr) && 120<=height(memory,nullptr));++cases;
  assert(get<uint64_t>(memory,0x98)==0x123456789ULL);++cases;
  assert(get<uint32_t>(memory,0xa0)==UINT32_MAX);++cases;
  assert(get<uint32_t>(memory,0xa8)==0);++cases;
  for(size_t at=0;at<sizeof memory;++at) {
   const auto relative=(ptrdiff_t)at-baseOffset;
   if((relative>=0x84 && relative<0x8c) || (relative>=0x98 && relative<0xa4))continue;
   assert(memory[at]==before[at]);
  }
  ++cases;
  auto filled=std::vector<uint8_t>(memory,memory+sizeof memory);
  fillBaseLimits(memory);assert(!memcmp(memory,filled.data(),sizeof memory));++cases;
  for(unsigned mode=0;mode<4;++mode) {
   present=mode!=0;compatible=mode!=1;auto saved=baseClass.ivars;
   if(mode==2)baseClass.ivars={{baseOffset+4}};
   if(mode==3)baseClass.methods.erase("limits");
   fillBaseLimits(memory);
   assert(!memcmp(memory,filled.data(),sizeof memory));++cases;
   baseClass.ivars=saved;
   baseClass.methods["limits"]={"^v16@0:8",2,(void *)constant};
  }
  present=compatible=true;
 }
 fillBaseLimits(nullptr);++cases;
 assert(cases==23);printf("{\"cases\":%u,\"old_120px_icon_rejected\":true,\"new_120px_icon_accepted\":true}\n",cases);
}
"""
path = args.out / "actual-limits.cpp"
path.write_text(prefix + body + fixture)
binary = args.out / "actual-limits"
command = [args.cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-Wno-unknown-pragmas", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           str(path), "-o", str(binary)]
subprocess.run(command, check=True)
execution = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
(args.out / "limits-run.log").write_text(execution.stdout + execution.stderr)
actual = json.loads(execution.stdout)
report = dict(passed=True, source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
    helper_sha256=hashlib.sha256(helper.encode()).hexdigest(),
    pressure_sha256=hashlib.sha256(pressure.encode()).hexdigest(),
    actual_upstream_fill_loop_executed=True, ObjectiveC_runtime_replaced_for_portable_loop=True,
    target_iPhone_getter_offsets_checked_separately=True, actual=actual,
    ASan=True, UBSan=True, phone_operations=False, real_ARC_pressure_lifetime_test=False)

if platform.system() == "Darwin":
    metal_prefix = r"""
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
static std::unordered_map<SEL,uint64_t> gAnswers;
static unsigned notes=0;
#define NBLOG(...) do{++notes;(void)filled;}while(0)
static NSUInteger answer(id,SEL sel){return gAnswers.at(sel);}
"""
    metal_main = r"""
int main() {@autoreleasepool {
 (void)[MTLCompileOptions new];
 Class base=objc_getClass("_MTLDevice");assert(base);
 Class cls=objc_allocateClassPair(base,"NBBaseLimitsCheck045",0);assert(cls);
 const char *names[]={"maxTextureWidth2D","maxTextureHeight2D"};
 for(const char *name:names) {
  SEL s=sel_registerName(name);assert(class_getInstanceMethod(base,s));
  gAnswers[s]=16384;assert(class_addMethod(cls,s,(IMP)answer,"Q16@0:8"));
 }
 objc_registerClassPair(cls);
 id device=class_createInstance(cls,0);assert(device);
 SEL limitsSel=sel_registerName("limits");
 auto *limits=((uint8_t *(*)(id,SEL))objc_msgSend)(device,limitsSel);assert(limits);
 auto original=[&](const char *name){
  SEL s=sel_registerName(name);
  return ((NSUInteger (*)(id,SEL))method_getImplementation(class_getInstanceMethod(base,s)))(device,s);
 };
 assert(original(names[0])==0 && original(names[1])==0);
 fillBaseLimits(device);
 assert(original(names[0])==16384 && original(names[1])==16384 && notes==1);
 const ptrdiff_t limitsOffset=limits-(uint8_t *)(__bridge void *)device;
 printf("{\"real_Mac_MTLDevice\":true,\"original_getters\":true,\"width\":%lu,\"height\":%lu,\"limits_offset\":%td}\n",
  (unsigned long)original(names[0]),(unsigned long)original(names[1]),limitsOffset);
 }}
"""
    metal = args.out / "actual-metal-limits.mm"
    metal.write_text(metal_prefix + helper + metal_main)
    metal_binary = args.out / "actual-metal-limits"
    subprocess.run([args.cxx, "-std=c++17", "-fobjc-arc", "-fblocks", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(metal), "-framework", "Foundation", "-framework", "Metal",
                    "-o", str(metal_binary)], check=True)
    result = subprocess.run([str(metal_binary)], check=True, capture_output=True, text=True)
    (args.out / "original-Metal-run.log").write_text(result.stdout + result.stderr)
    report.update(real_Mac_MTLDevice_getters=True, original_Metal_result=json.loads(result.stdout))
    # Foundation owns these objects with the same ARC rules as the production
    # dispatch source. Only event delivery is replaced, to inject a warning.
    arc_prefix = r"""
#import <Foundation/Foundation.h>
#include <cassert>
#include <atomic>
#include <cstdio>
#include <mutex>
@interface PressureSource : NSObject
@property(copy) void (^handler)(void);
@end
static std::atomic<unsigned> destroyed{0},created{0},trimmed{0},released{0};
@implementation PressureSource
- (void)dealloc {destroyed++;}
@end
#undef DISPATCH_SOURCE_TYPE_MEMORYPRESSURE
#undef DISPATCH_MEMORYPRESSURE_WARN
#undef DISPATCH_MEMORYPRESSURE_CRITICAL
namespace retained {
using dispatch_source_t=PressureSource *;
using dispatch_once_t=bool;
static __weak PressureSource *observed;
static void *gDevice=(void *)1;
static const int DISPATCH_SOURCE_TYPE_MEMORYPRESSURE=1,DISPATCH_MEMORYPRESSURE_WARN=2,
 DISPATCH_MEMORYPRESSURE_CRITICAL=4,QOS_CLASS_UTILITY=1;
static void dispatch_once(dispatch_once_t *once,void (^f)(void)){if(!*once){*once=true;f();}}
static void *dispatch_get_global_queue(int,int){return nullptr;}
static dispatch_source_t dispatch_source_create(int,int,int,void *) {
 PressureSource *source=[PressureSource new];created++;observed=source;return source;
}
static void dispatch_source_set_event_handler(dispatch_source_t s,void (^f)(void)){s.handler=f;}
static void dispatch_activate(dispatch_source_t) {}
static void trimHeldImports(bool all){assert(all);trimmed++;}
static unsigned nmb_device_release_kept_memory(void *){released++;return 1;}
"""
    arc_main = r"""
}
int main() {
 @autoreleasepool {
  @autoreleasepool {retained::watchImportPressure();}
  assert(created==1 && destroyed==0 && retained::observed);
  retained::watchImportPressure();assert(created==1);
  retained::observed.handler();
  assert(trimmed==1 && released==1);
 }
 assert(destroyed==0);
 puts("{\"real_ARC\":true,\"source_survives_initialization\":true,\"one_source\":true,\"warning_releases_resources\":true}");
}
"""
    arc = args.out / "actual-pressure.mm"
    arc.write_text(arc_prefix + pressure + arc_main)
    arc_binary = args.out / "actual-pressure"
    subprocess.run([args.cxx, "-std=c++17", "-fobjc-arc", "-fblocks", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(arc), "-framework", "Foundation", "-o", str(arc_binary)], check=True)
    result = subprocess.run([str(arc_binary)], check=True, capture_output=True, text=True)
    (args.out / "pressure-run.log").write_text(result.stdout + result.stderr)
    report.update(real_ARC_pressure_lifetime_test=True, pressure_result=json.loads(result.stdout),
                  real_system_memory_pressure_delivered=False)
    old_pressure = pressure.replace(" static dispatch_source_t source;\n", "")
    old_pressure = old_pressure.replace("  source=dispatch_source_create",
                                        "  dispatch_source_t source=dispatch_source_create")
    old_main = r"""
}
int main() {
 @autoreleasepool { @autoreleasepool {retained::watchImportPressure();} }
 assert(created==1 && destroyed==1 && !retained::observed);
 assert(!trimmed && !released);
 puts("{\"old_source_disposed\":true,\"warning_handler_unavailable\":true}");
}
"""
    old = args.out / "old-pressure.mm"
    old.write_text(arc_prefix + old_pressure + old_main)
    old_binary = args.out / "old-pressure"
    subprocess.run([args.cxx, "-std=c++17", "-fobjc-arc", "-fblocks", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(old), "-framework", "Foundation", "-o", str(old_binary)], check=True)
    result = subprocess.run([str(old_binary)], check=True, capture_output=True, text=True)
    (args.out / "old-pressure-run.log").write_text(result.stdout + result.stderr)
    report["old_ARC_disposal_reproduced"] = json.loads(result.stdout)["old_source_disposed"]
(args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
