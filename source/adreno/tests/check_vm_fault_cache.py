#!/usr/bin/env python3
"""Exercise the actual first-fault path and cached job snapshot with read-only fake hardware."""
from pathlib import Path
import argparse, hashlib, json, os, shlex, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--adreno',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
r=a.adreno
def body(path,marker):
    text=path.read_text()
    start=text.index(marker);opening=text.index('{',start);depth=1;at=opening+1
    while depth:
        if text[at]=='{':depth+=1
        elif text[at]=='}':depth-=1
        at+=1
    return text[start:at]+'\n'
def section(path,marker):
    text=path.read_text()
    return text.split('// '+marker+'_BEGIN',1)[1].split('// '+marker+'_END',1)[0]
prefix=r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
using IOReturn=int32_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnIOError=3;
enum ArmSmmu4KStatus { ArmSmmu4KSuccess,ArmSmmu4KFault };
constexpr unsigned GpuTraceVmFailure=81,NativeAdrenoRingStallWords=22;
static bool traceOn=true;
extern "C" {uint32_t native_adreno_vm_fail[4]={};}
static unsigned callbacks,traceEvents;
static std::vector<unsigned> order;
extern "C" bool native_gpu_trace_enabled(void) __attribute__((weak));
extern "C" int proc_selfpid(void){return 77;}
template<typename... T> static void gpuTraceEvent(T...){++traceEvents;order.push_back(4);}
struct Hardware {
 uint32_t fault[4]={0x80000402u,0x60000000u,0,1};
 uint64_t table=0xb0001000;
 unsigned read32s=0,read64s=0;
 bool bad32=false,bad64=false;
};
static int read32(void *cookie,size_t offset,uint32_t *out){
 auto &h=*static_cast<Hardware *>(cookie);++h.read32s;
 if(h.bad32)return 1;
 const uint32_t regs[]={0x58,0x60,0x64,0x68};
 for(unsigned i=0;i<4;++i)if(offset==0x10000+regs[i]){*out=h.fault[i];return 0;}
 assert(false);return 1;
}
static int read64(void *cookie,size_t offset,uint64_t *out){
 auto &h=*static_cast<Hardware *>(cookie);assert(offset==0x10020);++h.read64s;
 if(h.bad64)return 1;
 *out=h.table;return 0;
}
struct Io {void *cookie;int(*read32)(void *,size_t,uint32_t *);int(*read64)(void *,size_t,uint64_t *);};
struct State {uint64_t context_offset=0x10000;unsigned poisoned=0;bool detached=false;};
struct Tables {unsigned quarantines=0;void quarantine(){++quarantines;order.push_back(3);}};
struct Arena {};
struct Region {Tables *tables;Arena *arena;State *state;};
struct Memory {unsigned quarantines=0;void quarantine(){++quarantines;order.push_back(2);}};
struct Mapping {Memory *memory;};
struct Routes {};
using ArmSmmu4KStreamResult=Routes;
static ArmSmmu4KStatus ArmSmmu4KFaultStreams(const Io *,void *,Routes *){return ArmSmmu4KSuccess;}
struct NativeAdrenoVm;
extern "C" void native_adreno_note_vm_failure(NativeAdrenoVm *,const uint32_t *) __attribute__((weak));
struct NativeAdrenoVm {
 bool attached_=true,poisoned_=false,fault_streams_=true;
 size_t high_=1,objects_=1;
 uint64_t next_handle_=99,table_=0xa0001000;
 Memory memory;
 Mapping mapping[1]{{&memory}};
 Mapping *mappings_=mapping;
 State state;Tables tables;Arena arena;
 Region regions_[2]{{&tables,&arena,&state},{nullptr,nullptr,nullptr}};
 Io io_;
 void *streams_=nullptr;
 struct {ArmSmmu4KStatus operation=ArmSmmu4KSuccess,quarantine=ArmSmmu4KSuccess;bool complete=true;} result_;
 mutable unsigned peeks=0,lookups=0;
 explicit NativeAdrenoVm(Hardware &h):io_{&h,read32,read64}{}
 uint64_t ttbr0()const{return table_;}
 IOReturn faultState(uint32_t out[4])const;
 IOReturn failAt(unsigned,ArmSmmu4KStatus,uint32_t,uint64_t,uint64_t);
 IOReturn peekWordsUncached(uint64_t start,uint32_t *out,size_t count)const{
  assert(!poisoned_ && !memory.quarantines && count==16);
  ++peeks;for(size_t i=0;i<count;++i)out[i]=uint32_t(start/4+i);return 0;
 }
 uint64_t handleAt(uint64_t address)const{
  assert(!poisoned_);++lookups;return address>=0x140000000ull && address<0x140001000ull?33:0;
 }
};
struct NativeAdrenoRingClock {void *cookie;uint64_t(*now_us)(void *);int(*idle)(void *);};
struct Chip {unsigned generation=8;};
struct Ring {
 Chip info;
 bool badFence=false,badState=false;
 unsigned fenceReads=0,stateReads=0;
 const Chip *chip()const{return &info;}
 int fenceValue(uint32_t *out){++fenceReads;if(badFence)return 1;*out=100;return 0;}
 int stallState(uint32_t *out){
  ++stateReads;if(badState)return 1;
  for(unsigned i=0;i<22;++i)out[i]=i+20;
  out[8]=3;out[9]=7;out[10]=0x40000000;out[11]=1;out[12]=8;
  out[16]=3;out[17]=0x40000000;out[18]=8;
  out[20]=0x40000000;out[21]=1;return 0;
 }
 int ibBefore(uint32_t,uint64_t *ib,uint32_t *size){*ib=0x140000000ull;*size=16;return 0;}
 uint32_t lastSubmitted()const{return 101;}
 uint32_t readPointer()const{assert(false);return 0;}
 uint32_t writePointer()const{assert(false);return 0;}
};
using NativeAdrenoRing=Ring;
static IOReturn readTable(void *cookie,uint64_t *out){return read64(cookie,0x10020,out)?kIOReturnIOError:kIOReturnSuccess;}
static uint64_t clockNow(void *cookie){return *static_cast<uint64_t *>(cookie);}
struct NativeAdrenoSubmit {
 struct Job {
  uint32_t fence=101;size_t count=1;uint64_t queue=9;
  uint64_t vm_next=40;NativeAdrenoVm *vm=nullptr;
  uint64_t handles[1]={33};bool active=true;uint32_t context=7;uint64_t submit_us=1000;
  int32_t owner=88;size_t word_count=4;
  uint32_t words[4]={0x70bf8003,0x40000000,1,16};
  unsigned ring=0;int32_t priority=1;
 } job;
 bool attached_=true,poisoned_=false,first_vm_fault_sampled_=false;
 unsigned ring_count_=1,table_mode_=1,stall_timeouts=0;
 struct {unsigned loaded()const{return 0;}bool inFlight()const{return false;}} preempt_;
 bool preemption()const{return false;}
 NativeAdrenoVm *vm_,*table_vm_;
 Ring target;
 Ring *rings_[2]={&target,nullptr};
 Job jobs_[1];
 uint64_t now=100001;
 uint64_t(*clock_now_)(void *)=clockNow;void *clock_cookie_=&now;
 NativeAdrenoSubmit(NativeAdrenoVm &kernel,NativeAdrenoVm &owner):vm_(&kernel),table_vm_(&owner){job.vm=&owner;}
 Ring *ring(unsigned=0){return &target;}
 Ring *ringOfFence(uint32_t fence){assert(fence==101);return &target;}
 Job *find(uint32_t fence){return fence==job.fence?&job:nullptr;}
 unsigned outstanding()const{return 1;}
 bool cacheStall(uint32_t,uint64_t,uint64_t,uint32_t,uint32_t,const NativeAdrenoVm *,const uint32_t *,uint32_t,uint64_t,IOReturn,uint32_t=0,NativeAdrenoRing * = nullptr);
 bool noteVmFault(const NativeAdrenoVm *,const uint32_t *,uint32_t,IOReturn(*)(void *,uint64_t *),void *);
};
static uint32_t native_adreno_stall[24],native_adreno_stall_mem[59];
static uint32_t native_adreno_stall_hang,native_adreno_stall_capture_status,native_adreno_stall_log_count,native_adreno_stall_fault[32],native_adreno_stall_ring[10];
static uint64_t native_adreno_stall_log[32][8];
static void findIb2(const NativeAdrenoVm *,const uint32_t *,size_t,uint32_t){assert(false);}
static void findFault(const NativeAdrenoVm *,const uint32_t *,size_t,uint32_t){assert(false);}

static NativeAdrenoSubmit *published;
'''
callbacks_code=r'''
extern "C" bool native_gpu_trace_enabled(void){return traceOn;}
extern "C" void native_adreno_note_vm_failure(NativeAdrenoVm *detector,const uint32_t *fault){
 ++callbacks;order.push_back(1);
 assert(!detector->poisoned_ && !detector->memory.quarantines && !detector->tables.quarantines);
 assert(published);
 (void)published->noteVmFault(detector,fault,77,readTable,published->vm_->io_.cookie);
}
struct Test {
 Hardware kh,dh,jh;
 NativeAdrenoVm kernel{kh},detector{dh},owner{jh};
 NativeAdrenoSubmit submit{kernel,owner};
 Test(){
  callbacks=traceEvents=0;traceOn=true;order.clear();published=&submit;
  owner.table_=0xc0001000;
  __atomic_store_n(&cachedStallSequence,0,__ATOMIC_RELEASE);
 }
};
int main(){
 {
  Test t;
  const auto status=t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192);
  assert(status==kIOReturnIOError && callbacks==1 && traceEvents==1);
  assert(t.detector.poisoned_ && t.detector.memory.quarantines==1 && t.detector.tables.quarantines==1);
  assert(order.front()==1 && order.back()==4);
  assert(!t.owner.poisoned_ && !t.owner.memory.quarantines);
  assert(t.submit.job.count==1 && t.submit.job.fence==101 && t.submit.outstanding()==1);
  NativeAdrenoStallSample s{};
  assert(native_adreno_copy_stall(&s));
  assert(s.serial==1 && s.state[0]==101 && s.state[1]==0);
  assert(s.job[1]==0 && s.job[2]==1000 && s.job[7]==88 && s.job[4]==t.owner.table_);
  assert(s.source[0]==1 && s.source[1]==77 && s.source[2]==t.detector.table_);
  assert(s.source[3]==t.kh.table && s.source[4]==kIOReturnSuccess && s.source[5]==0);
  assert(s.memory[0]==0x80000402u && s.memory[1]==0x60000000u);
  assert(t.kh.read64s==1 && t.dh.read64s==0 && t.owner.peeks==3 && t.detector.peeks==0);
  NativeAdrenoVm another(t.dh);
  (void)another.failAt(123,ArmSmmu4KFault,1,0x180002000ull,8192);
  assert(native_adreno_copy_stall(&s) && s.serial==1);
  assert(t.kh.read64s==1);
 }
 {
  Test t;t.dh.fault[0]=0x400; // AArch64 format, not a latched fault.
  assert(t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192)==kIOReturnIOError);
  assert(callbacks==0 && traceEvents==1 && t.detector.poisoned_);
  NativeAdrenoStallSample s{};assert(!native_adreno_copy_stall(&s));
  assert(t.kh.read64s==0 && t.submit.target.fenceReads==0);
 }
 {
  Test t;t.dh.bad32=true;
  (void)t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192);
  assert(callbacks==0 && traceEvents==1 && t.detector.poisoned_ && t.kh.read64s==0);
 }
 {
  Test t;traceOn=false;
  (void)t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192);
  assert(callbacks==0 && traceEvents==0 && t.detector.poisoned_ && t.dh.read32s==0);
 }
 {
  Test t;t.kh.bad64=true;
  (void)t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192);
  NativeAdrenoStallSample s{};assert(native_adreno_copy_stall(&s));
  assert(s.source[4]==kIOReturnIOError && t.detector.poisoned_);
 }
 {
  Test t;t.submit.target.badState=true;
  (void)t.detector.failAt(123,ArmSmmu4KFault,1,0x180000000ull,8192);
  NativeAdrenoStallSample s{};assert(!native_adreno_copy_stall(&s));
  assert(t.detector.poisoned_ && traceEvents==1 && !t.submit.first_vm_fault_sampled_);
 }
 {
  Test t;
  assert(t.submit.cacheStall(100,t.submit.now,50000,10,1,nullptr,nullptr,0,0,kIOReturnNotReady));
  NativeAdrenoStallSample s{};assert(native_adreno_copy_stall(&s));
  for(auto word:s.source)assert(word==0);
  assert(s.job[1]==50000 && s.state[1]==10 && t.kh.read64s==0 && callbacks==0);
 }
 {
  Test t;t.submit.target.info.generation=7;
  assert(!t.submit.noteVmFault(&t.detector,t.dh.fault,77,readTable,t.kernel.io_.cookie));
  assert(t.submit.target.fenceReads==0 && t.kh.read64s==0);
 }
 puts("PASS 8 actual first-fault/cached-snapshot cases: capture before quarantine, detector distinct from owner, actual bank and error status, immutable first snapshot, original error/quarantine and timed path retained.");
}
'''
source=prefix
source+=section(r/'NativeAdrenoSubmit.h','NATIVE_ADRENO_CACHED_STALL_TYPE')
source+=section(r/'NativeAdrenoSubmit.cpp','NATIVE_ADRENO_CACHED_STALL_COPY')
for marker in ('IOReturn NativeAdrenoVm::faultState(', 'IOReturn NativeAdrenoVm::failAt('):
 source+=body(r/'NativeAdrenoVm.cpp',marker)
for marker in ('static bool activeJobIb(', 'static void peekIb(', 'static void peekFetch(', 'bool NativeAdrenoSubmit::cacheStall(', 'bool NativeAdrenoSubmit::noteVmFault('):
 source+=body(r/'NativeAdrenoSubmit.cpp',marker)
source+=callbacks_code
cpp=a.out/'actual.cpp';binary=a.out/'actual';cpp.write_text(source)
cmd=shlex.split(os.environ.get('CXX','clang++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(binary)]
result=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
(a.out/'build.log').write_text(result.stdout+result.stderr)
assert result.returncode==0,result.stderr
result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
(a.out/'run.log').write_text(result.stdout+result.stderr)
assert result.returncode==0,result.stderr
report=dict(passed=True,cases=8,actual_source_bodies=True,read_only_fake_hardware=True,ASan=True,UBSan=True,source_hashes={n:hashlib.sha256((r/n).read_bytes()).hexdigest() for n in ('NativeAdrenoVm.cpp','NativeAdrenoSubmit.h','NativeAdrenoSubmit.cpp')},command=cmd,phone_operations=False,hardware_cause_or_frame_improvement_verified=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(result.stdout,end='')

