"""Exercise the production cache/compile critical section with concurrent callers."""
from pathlib import Path
import argparse
import hashlib
import json
import platform
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
source = args.source.read_text()
begin = source.index(' NSString *cachedName=shaderCacheName(compilerUuid,type,request);')
end = source.index('\n}\n// Metal source, request 13:', begin)
tail = source[begin:end]
compile_begin = tail.index('  const uint64_t compileBegan=nowUs();')
compile_end = tail.index('  gTimeCompile.add(compileBegan);', compile_begin)
# Only the external compiler callback and Objective-C spelling are supplied by
# the fixture. The mutex, both lookups, publication and result branches are the
# actual production critical section.
tail = (tail[:compile_begin] + '  const uint64_t compileBegan=1;\n'
        + '  fixtureCompile(metallib,warned,message,called);\n'
        + tail[compile_end:])
tail = tail.replace('__block ', '').replace('@"the Metal compiler did not answer"',
                                           '&noAnswer')
prefix = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
using NSString=std::string;
using NSData=std::vector<uint8_t>;
static NSString noAnswer="the Metal compiler did not answer",compileError="compiler refused";
static NSString fixtureKey="one",warning="warning";
static NSData answer={2,4,6},alternate={3,5,7};
static std::mutex storeLock,barrierLock;
static std::condition_variable barrierCV;
static std::unordered_map<std::string,NSData *> store;
static std::atomic<unsigned> compiled{0},gCompileCached{0};
static unsigned arrived=0,parties=0;
static bool writes=true,callback=true,refusal=false,available=true;
static thread_local unsigned reads=0;
struct Timed {void add(uint64_t){}};
static Timed gTimeCompile;
static NSString *shaderCacheName(const unsigned char *,uint32_t,const std::vector<uint8_t> &request){
 return request[0]==1?&fixtureKey: &warning;
}
static NSString *shaderCachePath(NSString *name){return available?name:nullptr;}
static bool shaderCacheRead(NSString *path,NSString *name,NSData **bytes,NSString **warnings){
 assert(name && (!path || path==name));
 bool hit=false;
 {std::lock_guard<std::mutex> g(storeLock);
  if(path){auto i=store.find(*path);if(i!=store.end()){*bytes=i->second;*warnings=&warning;hit=true;}}
 }
 if(++reads==1 && parties){
  std::unique_lock<std::mutex> g(barrierLock);++arrived;barrierCV.notify_all();
  barrierCV.wait(g,[]{return arrived>=parties;});
 }
 return hit;
}
static void shaderCacheWrite(NSString *path,NSData *bytes,NSString *){
 if(path && bytes && writes){std::lock_guard<std::mutex> g(storeLock);store[*path]=bytes;}
}
static void fixtureCompile(NSData *&bytes,NSString *&warnings,NSString *&failure,bool &called){
 ++compiled;called=callback;warnings=&warning;
 if(refusal){failure=&compileError;return;}
 if(callback)bytes=&answer;
}
static NSData *request(uint32_t type,const std::vector<uint8_t> &request,NSString **warnings,NSString **failure){
 static std::mutex lock;
 static unsigned char compilerUuid[16]{};
'''
suffix = r'''
}
static void reset(){
 store.clear();compiled=0;gCompileCached=0;arrived=0;parties=0;
 writes=callback=available=true;refusal=false;reads=0;
}
static NSData *ask(uint8_t key=1){NSString *warn=nullptr,*failure=nullptr;
 auto *r=request(14,{key},&warn,&failure);
 assert(warn==&warning);
 if(callback && !refusal)assert(r==&answer && !failure);
 else assert(!r && failure==(refusal?&compileError:&noAnswer));
 return r;
}
int main(){unsigned cases=0;
 reset();parties=8;std::vector<std::thread> callers;
 for(unsigned i=0;i<parties;++i)callers.emplace_back([]{ask();});
 for(auto &thread:callers)thread.join();
#if RECHECK
 assert(compiled==1 && gCompileCached==7);
#else
 assert(compiled==8 && gCompileCached==0);
#endif
 ++cases;
 reset();ask();ask();assert(compiled==1 && gCompileCached==1);++cases;
 reset();ask(1);ask(2);assert(compiled==2);++cases;
 reset();refusal=true;ask();ask();assert(compiled==2 && store.empty());++cases;
 reset();callback=false;ask();ask();assert(compiled==2 && store.empty());++cases;
 reset();available=false;ask();ask();assert(compiled==2 && store.empty());++cases;
 reset();writes=false;ask();ask();assert(compiled==2 && store.empty());++cases;
 reset();store[fixtureKey]=&answer;ask();assert(compiled==0 && gCompileCached==1);++cases;
 printf("PASS %u actual critical-section cases; concurrent cold compiler calls=%u; before=%u\n",
        cases,RECHECK?1:8,RECHECK?0:1);
}
'''
recheck = '''  // Another request may have published this reply while this one waited.
  if(shaderCacheRead(cached,cachedName,&kept,&keptWarnings)) {
   gCompileCached++;
   *warnings=keptWarnings;*failure=nil;
   return kept;
  }
'''
assert tail.count(recheck) == 1
assert tail.index('shaderCacheWrite(cached,metallib,warned);') < tail.index('\n }\n *warnings=')
old = tail.replace(recheck, '')
old = old.replace('  // Publish before the next waiter can recheck the same request.\n'
                  '  if(!message)shaderCacheWrite(cached,metallib,warned);\n', '')
old = old.replace(' return message?nil:metallib;',
                  ' if(!message)shaderCacheWrite(cached,metallib,warned);\n return message?nil:metallib;')
results = []
for fixed, body in [(False, old), (True, tail)]:
    name = 'fixed' if fixed else 'old'
    cpp = args.out / (name + '.cpp')
    cpp.write_text(prefix + body.replace('nil', 'nullptr') + suffix)
    binary = args.out / name
    command = [args.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-pthread', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-DRECHECK=' + str(int(fixed)), str(cpp), '-o', str(binary)]
    build = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (args.out / (name + '-build.log')).write_text(build.stdout + build.stderr)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (args.out / (name + '-run.log')).write_text(run.stdout + run.stderr)
    assert run.returncode == 0, run.stdout + run.stderr
    results.append(dict(fixed=fixed, command=command, output=run.stdout,
                        source_sha256=hashlib.sha256(cpp.read_bytes()).hexdigest()))
report = dict(passed=True, actual_cache_compile_critical_section=True, cases=8,
              original_cold_compiles=8, fixed_cold_compiles=1, ASan_UBSan=True,
              platform=platform.system(), source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
              substitutions=['external compiler callback', 'Objective-C object and nil spelling',
                             'cache I/O with counted independent fixture storage'],
              results=results, physical_frame_improvement_verified=False, phone_operations=False)
(args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k:v for k,v in report.items() if k != 'results'}, indent=2))
