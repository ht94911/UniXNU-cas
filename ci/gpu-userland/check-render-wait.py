"""Run actual capture/fragment code with controlled Mach and real Mac waiting threads."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import platform
import subprocess

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--header',type=Path,required=True)
p.add_argument('--decoder',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--cxx',default='clang++')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
s=a.source.read_text();header=a.header.read_text()
record=s[s.index('struct TraceRec {'):s.index('static TraceRec gTrace[')]
fragment=s[s.index('static void traceWaitBytes('):s.index('static void registerRenderWaitThread(')]
prefix=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
using thread_t=unsigned;using kern_return_t=int;using mach_msg_type_number_t=unsigned;
using thread_state_t=unsigned *;using thread_info_t=int *;
using mach_vm_size_t=uint64_t;using mach_vm_address_t=uint64_t;
static constexpr int KERN_SUCCESS=0,KERN_INVALID_ARGUMENT=4;
static constexpr unsigned THREAD_BASIC_INFO=3,THREAD_BASIC_INFO_COUNT=10,x86_THREAD_STATE64=4,x86_THREAD_STATE64_COUNT=42;
struct Time {int seconds=0,microseconds=0;};
struct thread_basic_info_data_t {Time user_time,system_time;int run_state=0,suspend_count=0,policy=0;};
struct x86_thread_state64_t {uint64_t __rip=0,__rbp=0,__rsp=0;};
static uint64_t clockUs=1000;
static unsigned infoCalls=0,stateCalls=0,readCalls=0;
static int infoError=0,stateError=0,readError=0;
static bool shortRead=false,changeState=false,loopFrame=false,invalidSp=false,shortCount=false;
static uint64_t frameStart=0x1100;
static uint64_t now(){return clockUs++;}
static unsigned mach_task_self(){return 1;}
static int thread_info(unsigned thread,unsigned flavor,thread_info_t p,mach_msg_type_number_t *count){
 assert(thread==55 && flavor==THREAD_BASIC_INFO && *count==THREAD_BASIC_INFO_COUNT);++infoCalls;
 auto &v=*(thread_basic_info_data_t *)p;v.user_time={9,33};v.system_time={2,44};v.run_state=3;v.policy=1;
 return infoError;
}
static int thread_get_state(unsigned thread,unsigned flavor,thread_state_t p,mach_msg_type_number_t *count){
 assert(thread==55 && flavor==x86_THREAD_STATE64 && *count==x86_THREAD_STATE64_COUNT);++stateCalls;
 auto &v=*(x86_thread_state64_t *)p;v.__rip=0x187f01000+(changeState && stateCalls>1?4:0);
 v.__rbp=frameStart;v.__rsp=invalidSp?0:0x1000;if(shortCount)--*count;return stateError;
}
static int mach_vm_read_overwrite(unsigned task,uint64_t address,uint64_t bytes,uint64_t dest,uint64_t *copied){
 assert(task==1 && bytes==16 && address>=0x1000 && address<=0x1ff0);++readCalls;
 auto *v=(uint64_t *)dest;v[0]=loopFrame?address:address+0x10;v[1]=0x187f02000+readCalls*4;
 *copied=shortRead?8:16;return readError;
}
'''
sink='''static std::vector<TraceRec> records;
static void tracePush(const TraceRec &row){records.push_back(row);}
static void traceSamplePush(const TraceRec &row){tracePush(row);}
'''
cases=r'''
static void reset(){clockUs=1000;infoCalls=stateCalls=readCalls=0;infoError=stateError=readError=0;
 shortRead=changeState=loopFrame=invalidSp=shortCount=false;frameStart=0x1100;records.clear();}
static nbwait::Sample take(){nbwait::Sample s{};s.id=1;s.tid=999;s.stackLow=0x1000;s.stackHigh=0x2000;
 nbwait::capture(55,s,now);return s;}
int main(int argc,char **argv){assert(argc==2);assert(nbwait::kSampleBudget==6144);unsigned cases=0;
 reset();auto s=take();assert(s.depth==16 && readCalls==16 && stateCalls==2 && infoCalls==1);
 assert(s.cpuUs==11000077 && s.flags==31 && s.stop==nbwait::FrameLimit && s.stateKr==0);
 assert(s.beganUs<=s.infoEndedUs && s.infoEndedUs<=s.stateEndedUs && s.stateEndedUs<s.endedUs);
 traceWaitBytes(27,s.id,&s,sizeof(s));assert(records.size()==12);
 nbwait::Image image{};image.header=0x187f00000;image.slide=0x1230000;strcpy(image.path,"/system/Example");
 traceWaitBytes(28,1,&image,sizeof(image));assert(records.size()==16);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(records.data(),72,records.size(),f)==records.size());fclose(f);++cases;
 reset();stateError=5;s=take();assert(s.stop==nbwait::NoRegisters && !readCalls && stateCalls==1 && !(s.flags&2));++cases;
 reset();frameStart=0x2000;s=take();assert(!readCalls && !s.depth && s.stop==nbwait::OutsideStack);++cases;
 reset();frameStart=0x1101;s=take();assert(!readCalls && s.stop==nbwait::UnalignedFrame);++cases;
 reset();loopFrame=true;s=take();assert(s.depth==1 && readCalls==1 && s.stop==nbwait::NonIncreasingFrame);++cases;
 reset();readError=1;s=take();assert(!s.depth && readCalls==1 && s.stop==nbwait::ReadFailed);++cases;
 reset();shortRead=true;s=take();assert(!s.depth && readCalls==1 && s.stop==nbwait::ReadFailed);++cases;
 reset();changeState=true;s=take();assert(s.flags&2 && !(s.flags&16) && s.first.pc!=s.last.pc);++cases;
 reset();invalidSp=true;s=take();assert(!readCalls && !(s.flags&4));++cases;
 reset();infoError=5;s=take();assert(!(s.flags&1) && s.flags&2 && s.cpuUs==0);++cases;
 reset();shortCount=true;s=take();assert(s.stateKr==KERN_INVALID_ARGUMENT && !readCalls);++cases;
 assert(nbwait::dueThreshold(24999,1,0)==-1 && nbwait::dueThreshold(25001,1,0)==0);
 assert(nbwait::dueThreshold(120001,1,0)==2 && nbwait::dueThreshold(120001,1,3)==-1);
 assert(nbwait::dueThreshold(180001,1,3)==3 && nbwait::dueThreshold(999999,1,4)==-1);
 assert(nbwait::dueThreshold(100,101,0)==-1 && nbwait::dueThreshold(100,0,0)==-1);++cases;
 std::cout<<"cases="<<cases<<" actual_capture_and_fragment_code_passed\n";
}
'''
assert header.count('#if defined(__APPLE__)')==1
controlled_header=header.replace('#if defined(__APPLE__)','#if 0',1)  # mock Mach declarations only
fixture=a.out/'capture-controlled.cpp';fixture.write_text(prefix+controlled_header+record+sink+fragment+cases)
binary=a.out/'capture-controlled';command=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
 '-Wno-unused-function','-fsanitize=address,undefined',str(fixture),'-o',str(binary)]
subprocess.run(command,check=True)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
run=subprocess.run([str(binary),str(a.out/'actual-producer.trace')],env=env,capture_output=True,text=True,timeout=30)
(a.out/'controlled.log').write_text(run.stdout+run.stderr);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
spec=importlib.util.spec_from_file_location('render_wait_decoder',a.decoder);decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
decoded=decoder.read_frames(a.out/'actual-producer.trace',require_header=False)
assert decoded['sample_count']==1 and decoded['image_count']==1 and not decoded['quality']
row=decoded['samples'][0];assert row['thread']==999 and row['state']=='waiting' and row['cpu_us']==11000077
assert row['depth']==16 and row['flag_meaning']['registers_matched_after_read'] and not row['atomic_stack_snapshot']
assert decoded['images'][0]['slide']==0x1230000 and decoded['images'][0]['path']=='/system/Example'
raw=(a.out/'actual-producer.trace').read_bytes()
for name,value,reason in [('missing',raw[72:],'missing_fragments'),('duplicate',raw[:72]+raw,'duplicate_fragment')]:
    path=a.out/(name+'.trace');path.write_bytes(value);bad=decoder.read_frames(path,require_header=False)
    assert not bad['samples'] and bad['quality'].get(reason),bad

# Force only the existing header's ARM64 branch under controlled Mach types.
# No ARM program is executed on this host; the production field copy is run.
arm_declarations=r"""
static constexpr unsigned ARM_THREAD_STATE64=6,ARM_THREAD_STATE64_COUNT=68;
struct arm_thread_state64_t {
 uint64_t __x[29]{},__fp=0,__lr=0,__sp=0,__pc=0;uint32_t __cpsr=0,__pad=0;
};
static void *__darwin_arm_thread_state64_get_pc_fptr(const arm_thread_state64_t &s){return (void *)(uintptr_t)s.__pc;}
static void *__darwin_arm_thread_state64_get_lr_fptr(const arm_thread_state64_t &s){return (void *)(uintptr_t)s.__lr;}
static uint64_t __darwin_arm_thread_state64_get_fp(const arm_thread_state64_t &s){return s.__fp;}
static uint64_t __darwin_arm_thread_state64_get_sp(const arm_thread_state64_t &s){return s.__sp;}
static int arm_thread_get_state(unsigned thread,unsigned flavor,thread_state_t p,mach_msg_type_number_t *count){
 assert(thread==55 && flavor==ARM_THREAD_STATE64 && *count==ARM_THREAD_STATE64_COUNT);++stateCalls;
 auto &s=*(arm_thread_state64_t *)p;s.__pc=0x187f01000+(changeState && stateCalls>1?4:0);
 s.__lr=0x187f02000;s.__fp=frameStart;s.__sp=invalidSp?0:0x1000;
 for(unsigned i=0;i<29;++i)s.__x[i]=0xf00d000000000000ull+((uint64_t)stateCalls<<32)+i;
 if(shortCount){--*count;}
 return stateError;
}
#define thread_get_state arm_thread_get_state
"""
arm_header=controlled_header.replace('#if defined(__arm64__) || defined(__aarch64__)','#if 1 /* controlled ARM64 state */',1)
assert arm_header!=controlled_header
arm_cases=cases.replace('s.flags==31','s.flags==95')
needle='traceWaitBytes(27,s.id,&s,sizeof(s));assert(records.size()==12);'
assert arm_cases.count(needle)==1
arm_cases=arm_cases.replace(needle,r"""
 assert(s.version==2 && s.bytes==576 && s.architecture==1 && (s.flags&nbwait::IntegerRegistersValid));
 for(unsigned i=0;i<14;++i)assert(s.integers[i]==0xf00d000100000000ull+(i<4?i:i+15));
"""+needle)
arm_fixture=a.out/'capture-arm64-controlled.cpp';arm_fixture.write_text(prefix+arm_declarations+arm_header+record+sink+fragment+arm_cases)
arm_binary=a.out/'capture-arm64-controlled'
arm_command=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(arm_fixture),'-o',str(arm_binary)]
subprocess.run(arm_command,check=True)
arm_run=subprocess.run([str(arm_binary),str(a.out/'arm64-producer.trace')],env=env,capture_output=True,text=True,timeout=30)
(a.out/'arm64-controlled.log').write_text(arm_run.stdout+arm_run.stderr);assert arm_run.returncode==0,(arm_run.stdout,arm_run.stderr)
arm_decoded=decoder.read_frames(a.out/'arm64-producer.trace',require_header=False)
assert arm_decoded['sample_count']==arm_decoded['image_count']==1 and not arm_decoded['quality']
arm_row=arm_decoded['samples'][0]
assert arm_row['version']==2 and arm_row['bytes']==576 and arm_row['depth']==16
assert arm_row['integer_registers_from_first_get_state'] and not arm_row['integer_registers_rechecked']
assert arm_row['arm64_integer_registers']=={'x'+str(i):hex(0xf00d000100000000+i) for i in [0,1,2,3,*range(19,29)]}
assert decoded['samples'][0]['arm64_integer_registers'] is None
# A fragment group cannot mix the two declared sizes admitted by the decoder.
import struct
mixed=bytearray((a.out/'arm64-producer.trace').read_bytes());struct.pack_into('<I',mixed,44,336)
(a.out/'mixed-sizes.trace').write_bytes(mixed);bad=decoder.read_frames(a.out/'mixed-sizes.trace',require_header=False)
assert not bad['samples'] and bad['quality'].get('invalid_fragment_header')
maximum_UI_bytes=90001*12*72+1024*4*72+4*72
assert maximum_UI_bytes<=80<<20

real=dict(executed=False,platform=platform.system(),device_sandbox_verified=False)
if platform.system()=='Darwin':
    registry=s[s.index('struct RenderWaitTarget {'):s.index('static void traceRenderWaitImages(')]
    real_prefix=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
'''
    real_helpers=r'''
static std::vector<TraceRec> records;static std::mutex recordLock;
static void tracePush(const TraceRec &r){std::lock_guard<std::mutex> lock(recordLock);records.push_back(r);}
static void traceSamplePush(const TraceRec &r){tracePush(r);}
static bool traceWanted(){return true;}static bool NBTraceSwapTimingOn(){return true;}
static uint64_t nowUs(){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
'''
    real_cases=r'''
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static std::atomic<unsigned> ready{0};static bool released=false;
__attribute__((noinline)) static void park(){pthread_mutex_lock(&lock);++ready;
 while(!released)pthread_cond_wait(&condition,&lock);pthread_mutex_unlock(&lock);}
__attribute__((noinline)) static void *worker(void *arg){registerRenderWaitThread((uintptr_t)arg);park();return nullptr;}
int main(int argc,char **argv){assert(argc==2);assert(nbwait::kSampleBudget==6144 && gRenderWaitCopyUs.load()==0);pthread_t threads[2];
 for(unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],nullptr,worker,(void *)(uintptr_t)(i+123)));
 const uint64_t deadline=nowUs()+5000000;while(ready.load()!=2 && nowUs()<deadline)usleep(1000);assert(ready.load()==2);
 unsigned count=0;uint64_t previousTid=0,maxUs=0;
 for(auto &t:gRenderWaitTargets){if(t.ready.load()!=2)continue;
  assert(t.tid && t.tid!=previousTid && t.low<t.high && t.lastCommand.load()>=123);previousTid=t.tid;
  nbwait::Sample sample{};
  for(unsigned attempt=0;attempt<12;++attempt){sample={};sample.id=count+1;sample.tid=t.tid;
   sample.lastCommand=t.lastCommand.load();sample.stackLow=t.low;sample.stackHigh=t.high;
   nbwait::capture(t.port,sample,nowUs);
   if(sample.runState==TH_STATE_WAITING && (sample.flags&31)==31 && sample.depth>0)break;usleep(1000);
  }
  assert(sample.basicKr==KERN_SUCCESS && sample.stateKr==KERN_SUCCESS && sample.readKr==KERN_SUCCESS);
  assert(sample.runState==TH_STATE_WAITING && (sample.flags&31)==31 && sample.depth>0 && sample.first.pc);
  maxUs=std::max<uint64_t>(maxUs,sample.endedUs-sample.beganUs);
  traceWaitBytes(27,sample.id,&sample,sizeof(sample));++count;
 }
 assert(count==2);pthread_mutex_lock(&lock);released=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&lock);
 for(auto t:threads)pthread_join(t,nullptr);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(records.data(),72,records.size(),f)==records.size());fclose(f);
 printf("{\"threads\":%u,\"max_capture_us\":%llu,\"waiting_stack_read\":true}\n",count,(unsigned long long)maxUs);
}
'''
    source=a.out/'real-mach-wait.cpp';source.write_text(real_prefix+header+record+real_helpers+registry+real_cases)
    exe=a.out/'real-mach-wait';compile_command=[a.cxx,'-std=c++17','-O1','-g','-fno-omit-frame-pointer',
        '-Wall','-Wextra','-Werror','-Wno-unused-function',str(source),'-o',str(exe)]
    subprocess.run(compile_command,check=True)
    live=subprocess.run([str(exe),str(a.out/'real-mach-wait.trace')],capture_output=True,text=True,timeout=30)
    (a.out/'real-mach-wait.log').write_text(live.stdout+live.stderr);assert live.returncode==0,(live.returncode,live.stdout,live.stderr)
    real_decoded=decoder.read_frames(a.out/'real-mach-wait.trace',require_header=False)
    assert real_decoded['sample_count']==2 and not real_decoded['quality']
    real.update(executed=True,passed=True,result=json.loads(live.stdout),command=compile_command)
report=dict(passed=True,controlled_cases=24,fragment_rejection_cases=3,ASan_UBSan=True,
    ARM64_integer_values_from_actual_reader=True,integer_registers_unmodified_64bit=True,stack_frames=16,wire_version=2,wire_bytes=576,maximum_UI_bytes=maximum_UI_bytes,required_UI_read_bound_bytes=80<<20,
    actual_capture_and_fragment_code=True,actual_registration_code_on_Mac=real['executed'],
    real_Mach_wait=real,source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
    header_sha256=hashlib.sha256(a.header.read_bytes()).hexdigest(),decoder_sha256=hashlib.sha256(a.decoder.read_bytes()).hexdigest(),
    phone_operations=False,performance_improvement_verified=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
