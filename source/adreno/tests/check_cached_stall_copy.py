"""Exercise the actual cached-stall handoff under concurrent publication."""
from pathlib import Path
import hashlib
import json
import subprocess

import argparse,os,shlex
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--adreno',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();G=a.adreno;O=a.out;O.mkdir(parents=True,exist_ok=False)

def section(path,name):
    text=path.read_text()
    a='// '+name+'_BEGIN'
    b='// '+name+'_END'
    assert text.count(a)==1 and text.count(b)==1
    return text.split(a,1)[1].split(b,1)[0]

source='#include <cstdint>\n#include <cassert>\n#include <atomic>\n#include <thread>\n#include <cstdio>\n'
source+=section(G/'NativeAdrenoSubmit.h','NATIVE_ADRENO_CACHED_STALL_TYPE')
source+=section(G/'NativeAdrenoSubmit.cpp','NATIVE_ADRENO_CACHED_STALL_COPY')
source+=r'''
static NativeAdrenoStallSample value(uint32_t n) {
    NativeAdrenoStallSample s{};
    for(unsigned i=0;i<24;++i)s.state[i]=n;
    for(unsigned i=0;i<59;++i)s.memory[i]=n^0x51a84012u;
    for(unsigned i=0;i<20;++i)s.job[i]=(uint64_t(n)<<32)|0xabcdef09u;
    for(unsigned i=0;i<32;++i)s.ibs[i]=n+0x128u;
    for(unsigned i=0;i<6;++i)s.source[i]=(uint64_t(n)<<32)|uint64_t(i+1);
    return s;
}
static void verify(const NativeAdrenoStallSample &s) {
    const uint32_t n=s.state[0];
    for(unsigned i=0;i<24;++i)assert(s.state[i]==n);
    for(unsigned i=0;i<59;++i)assert(s.memory[i]==(n^0x51a84012u));
    for(unsigned i=0;i<20;++i)assert(s.job[i]==((uint64_t(n)<<32)|0xabcdef09u));
    for(unsigned i=0;i<32;++i)assert(s.ibs[i]==n+0x128u);
    for(unsigned i=0;i<6;++i)assert(s.source[i]==((uint64_t(n)<<32)|uint64_t(i+1)));
    assert(s.serial!=0);
}
int main() {
    NativeAdrenoStallSample s{};
    assert(!native_adreno_copy_stall(nullptr));
    assert(!native_adreno_copy_stall(&s));
    publishCachedStall(value(1));
    assert(native_adreno_copy_stall(&s));verify(s);assert(s.serial==1);
    // The exact reader must return immediately during an unfinished writer.
    __atomic_store_n(&cachedStallSequence,3u,__ATOMIC_RELEASE);
    for(unsigned i=0;i<10000;++i)assert(!native_adreno_copy_stall(&s));
    __atomic_store_n(&cachedStallSequence,2u,__ATOMIC_RELEASE);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for(uint32_t n=2;n<=40000;++n)publishCachedStall(value(n));
        done.store(true,std::memory_order_release);
    });
    uint64_t accepted=0,rejected=0;
    do {
        if(native_adreno_copy_stall(&s)){verify(s);++accepted;}
        else ++rejected;
    } while(!done.load(std::memory_order_acquire));
    writer.join();
    assert(native_adreno_copy_stall(&s));verify(s);
    assert(s.serial==40000 && s.state[0]==40000);
    std::printf("PASS actual cached copy: 40000 publications; accepted=%llu rejected=%llu; odd writer never blocks\n",
        (unsigned long long)accepted,(unsigned long long)rejected);
}
'''
(O/'actual-copy.cpp').write_text(source)
command=shlex.split(os.environ.get('CXX','clang++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread',str(O/'actual-copy.cpp'),'-o',str(O/'actual-copy')]
build=subprocess.run(command,capture_output=True,text=True)
(O/'compile.log').write_text(build.stdout+build.stderr)
assert build.returncode==0,build.stderr
run=subprocess.run([str(O/'actual-copy')],capture_output=True,text=True,timeout=60)
(O/'run.log').write_text(run.stdout+run.stderr)
report=dict(passed=run.returncode==0,scope='actual cache publisher/reader; no GPU or phone',
            command=command,output=run.stdout,error=run.stderr,
            sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (G/'NativeAdrenoSubmit.h',G/'NativeAdrenoSubmit.cpp')})
(O/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
assert report['passed']
