"""Exercise the actual read-only state reader, serializer, and batch CPU fields."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--cxx',default='clang++');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
front=a.source.read_text();folder=a.source.parent
header=(folder/'NativeDisplayWaitState.h').read_text();inc=(folder/'NativeFrameObservers.inc').read_text()
record=front[front.index('struct TraceRec {'):front.index('static TraceRec gTrace[')]
fragment=front[front.index('static void traceWaitBytes('):front.index('static void registerRenderWaitThread(')]
batch=front[front.index('static void batchStage('):front.index('extern "C" bool NBTraceSwapTimingOn(void);')]
reader=inc[inc.index('static int readObserverMemory('):inc.index('static id gObservedWindowDisplay;')]
prefix=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using mach_vm_size_t=uint64_t;using mach_vm_address_t=uint64_t;using kern_return_t=int;
static constexpr int KERN_SUCCESS=0,KERN_INVALID_ARGUMENT=4;
static unsigned mach_task_self(){return 1;}
static std::map<uint64_t,std::vector<uint8_t>> memory;
static unsigned reads=0;static uint64_t failAddress=0,shortAddress=0;
static int mach_vm_read_overwrite(unsigned task,uint64_t address,unsigned bytes,uint64_t target,uint64_t *copied){
 assert(task==1);++reads;
 if(address==failAddress)return 55;
 auto found=memory.find(address);assert(found!=memory.end());assert(found->second.size()==bytes);
 std::memcpy((void *)target,found->second.data(),bytes);*copied=address==shortAddress?bytes-1:bytes;return 0;
}
static uint64_t clockUs=1000;static bool enabled=true,cpuSuccess=true;static unsigned cpuQueries=0;
static uint64_t nowUs(){return clockUs++;}
static bool traceWanted(){return enabled;}static bool diagOn(){return enabled;}
static uint32_t threadIdLow(){return 101;}
static bool detailCpuNow(uint64_t &out){++cpuQueries;clockUs+=7;out=900000+cpuQueries*1000;return cpuSuccess;}
'''
sink=r'''
static std::vector<TraceRec> output;static unsigned renderRecords=0,clientRecords=0;
static void tracePush(const TraceRec &r){++renderRecords;output.push_back(r);}
static void traceSamplePush(const TraceRec &r){++clientRecords;output.push_back(r);}
template<class T> static void put(uint64_t at,const T &v){memory[at]=std::vector<uint8_t>((const uint8_t *)&v,(const uint8_t *)&v+sizeof(v));}
static void init(){
 memory.clear();reads=0;failAddress=shortAddress=0;clockUs=1000;
 put(0x1008,uint64_t(0x2000));put(0x2040,uint64_t(0x3000));put(0x3000,nbdisplaywait::kServerVtable);
 put(0x3060,uint64_t(0x4000));put(0x4000,nbdisplaywait::kDisplayVtable);
 uint64_t tail[52]{};tail[6]=0x5000;tail[16]=0x123456789abcdef0;put(0x3480,tail);
 uint64_t timer[19]{};timer[3]=nbdisplaywait::kTimerCallback;timer[4]=0x3000;timer[11]=1234;put(0x5078,timer);
 uint64_t ss[4]{};put(0x3600,ss);uint64_t ts[8]{};ts[0]=1234;put(0x50d0,ts);
}
static nbdisplaywait::State take(){nbdisplaywait::State s{};s.object=0x1000;nbdisplaywait::capture(s,0,readObserverMemory,nowUs);return s;}
'''
cases=r'''
int main(int argc,char **argv){assert(argc==2);unsigned cases=0;
 assert(sizeof(nbdisplaywait::kQuartzUUID)==16 && nbdisplaywait::kQuartzUUID[0]==0x76);
 assert(nbdisplaywait::kServerTailBytes==sizeof(nbdisplaywait::State{}.serverTail));
 assert(nbdisplaywait::kTimerTailBytes==sizeof(nbdisplaywait::State{}.timerTail));
 init();auto s=take();assert(s.stop==0 && s.flags==31 && reads==9 && s.endedUs>s.beganUs);
 assert(s.serverTail[16]==0x123456789abcdef0 && s.timerTail[11]==1234);++cases;
 output.clear();renderRecords=clientRecords=0;traceWaitBytes(29,12,&s,sizeof(s));assert(output.size()==17 && renderRecords==17 && clientRecords==0);
 nbdisplaywait::Identity identity{};identity.pid=116;identity.role=2;std::strcpy(identity.bootUUID,"12345678-1234-1234-1234-123456789abc");
 traceWaitBytes(30,2,&identity,sizeof(identity),true);assert(output.size()==19 && clientRecords==2);
 FILE *f=std::fopen(argv[1],"wb");assert(f);assert(std::fwrite(output.data(),72,output.size(),f)==output.size());std::fclose(f);++cases;
 init();put(0x3000,uint64_t(nbdisplaywait::kServerVtable+8));s=take();assert(s.stop==nbdisplaywait::WrongServerVtable && reads==3 && !s.flags);++cases;
 init();put(0x4000,uint64_t(nbdisplaywait::kDisplayVtable+8));s=take();assert(s.stop==nbdisplaywait::WrongDisplayVtable && reads==5 && !s.flags);++cases;
 init();failAddress=0x3480;s=take();assert(s.stop==nbdisplaywait::ReadFailed && s.reads[5]==55 && reads==6 && s.flags==1);++cases;
 init();shortAddress=0x5078;s=take();assert(s.stop==nbdisplaywait::ReadFailed && s.reads[6]==KERN_INVALID_ARGUMENT && reads==7 && s.flags==3);++cases;
 init();uint64_t empty[52]{};put(0x3480,empty);s=take();assert(s.stop==nbdisplaywait::NoSoftwareTimer && s.flags==3 && reads==6);++cases;
 init();memory[0x5078][3*8]^=4;s=take();assert(s.stop==nbdisplaywait::WrongTimerIdentity && reads==7 && s.flags==7);++cases;
 init();memory[0x5078][4*8]^=4;s=take();assert(s.stop==nbdisplaywait::WrongTimerIdentity && reads==7);++cases;
 init();memory[0x50d0][0]^=4;s=take();assert(s.stop==0 && s.flags==15 && reads==9);++cases;
 init();memory[0x3600][0]^=4;s=take();assert(s.stop==0 && s.flags==15);++cases;
 init();s={};nbdisplaywait::capture(s,0,readObserverMemory,nowUs);assert(s.stop==nbdisplaywait::NoObject && reads==0);++cases;
 output.clear();cpuQueries=0;enabled=true;batchStage(7,1,1000,64,3);assert(cpuQueries==0 && !output.back().tid2);
 batchStage(7,2,1000,64,3);assert(cpuQueries==1 && output.back().tid2==1 && output.back().t3==901000 && output.back().cpu0==8);
 batchStage(7,3,1000,64,3);assert(cpuQueries==2 && output.back().t3==902000 && output.back().a==3);++cases;
 cpuSuccess=false;batchStage(8,2,1000,3,0);assert(cpuQueries==3 && output.back().tid2==0);cpuSuccess=true;
 enabled=false;batchStage(9,2,1000,3,0);assert(cpuQueries==3);++cases;
 std::cout<<"cases="<<cases<<" actual_observer_reader_and_batch_fields_passed\n";
}
'''
cpp=a.out/'observer-test.cpp';cpp.write_text(prefix+header.replace('#pragma once\n','',1)+record+reader+sink+fragment+batch+cases)
binary=a.out/'observer-test';command=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-fsanitize=address,undefined',str(cpp),'-o',str(binary)]
run=subprocess.run(command,capture_output=True,text=True);(a.out/'build.log').write_text(run.stdout+run.stderr)
assert run.returncode==0,run.stderr
run=subprocess.run([str(binary),str(a.out/'observer.trace')],capture_output=True,text=True,timeout=30)
(a.out/'test.log').write_text(run.stdout+run.stderr);assert run.returncode==0,run.stdout+run.stderr
assert 'cases=14 actual_observer_reader_and_batch_fields_passed' in run.stdout
report=dict(passed=True,actual_source_reader_serializer_batch=True,ASan_UBSan=True,cases=14,
    source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
    display_state_header_sha256=hashlib.sha256((folder/'NativeDisplayWaitState.h').read_bytes()).hexdigest(),
    observer_include_sha256=hashlib.sha256((folder/'NativeFrameObservers.inc').read_bytes()).hexdigest(),
    command=command,phone_operations=False,hardware_verified=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
