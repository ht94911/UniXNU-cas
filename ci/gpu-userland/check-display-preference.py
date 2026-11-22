"""Run the original display reader and preference rules against independent memory."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
folder = args.source.parent
header = (folder / 'NativeDisplayWaitState.h').read_text().replace('#pragma once\n', '', 1)
observer = (folder / 'NativeFrameObservers.inc').read_text()
assert 'nbdisplaywait::displayPreference(state,gObservedQuartzSlide)' in observer
assert 'if(rank>preference)' in observer
prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
static std::map<uint64_t,std::vector<uint8_t>> memory;
static uint64_t ticks=1000;
static unsigned readCount=0;
static bool failTimer=false;
template<class T> static void put(uint64_t at,const T &value){
 memory[at]=std::vector<uint8_t>((const uint8_t *)&value,(const uint8_t *)&value+sizeof(value));
}
static int read(uint64_t at,void *out,unsigned bytes){
 ++readCount;if(failTimer && at==0x5078)return 4;
 auto it=memory.find(at);assert(it!=memory.end() && it->second.size()==bytes);
 memcpy(out,it->second.data(),bytes);return 0;
}
static uint64_t now(){return ticks++;}
'''
cases = r'''
static void reset(uint64_t display,bool timer,int64_t slide=0){
 memory.clear();ticks=1000;readCount=0;failTimer=false;
 put(0x1008,uint64_t(0x2000));put(0x2040,uint64_t(0x3000));
 put(0x3000,uint64_t(nbdisplaywait::kAccelServerVtable+slide));
 put(0x3060,uint64_t(0x4000));put(0x4000,uint64_t(display+slide));
 uint64_t tail[52]{};tail[6]=timer?0x5000:0;put(0x3480,tail);
 uint64_t t[19]{};t[3]=nbdisplaywait::kTimerCallback+slide;t[4]=0x3000;
 put(0x5078,t);uint64_t s[4]{};put(0x3600,s);uint64_t confirm[8]{};put(0x50d0,confirm);
}
static nbdisplaywait::State take(int64_t slide=0){
 nbdisplaywait::State state{};state.object=0x1000;
 nbdisplaywait::capture(state,slide,read,now);return state;
}
int main(){unsigned cases=0;
 assert(nbdisplaywait::kQuartzUUID[0]==0x76);
 assert(sizeof(nbdisplaywait::State{}.serverTail)==nbdisplaywait::kServerTailBytes);
 assert(sizeof(nbdisplaywait::State{}.timerTail)==nbdisplaywait::kTimerTailBytes);
 for(uint64_t table:{nbdisplaywait::kDisplayVtable,nbdisplaywait::kInternalDisplayVtable,
                    nbdisplaywait::kExternalDisplayVtable,nbdisplaywait::kWirelessDisplayVtable}){
  reset(table,true);auto s=take();assert(s.stop==0 && s.flags==31 && readCount==9);
  assert(nbdisplaywait::displayPreference(s,0)>=5);++cases;
 }
 reset(nbdisplaywait::kWirelessDisplayVtable,false);auto wireless=take();
 assert(wireless.stop==nbdisplaywait::NoSoftwareTimer && nbdisplaywait::displayPreference(wireless,0)==1);++cases;
 reset(nbdisplaywait::kInternalDisplayVtable,true);auto internal=take();
 assert(nbdisplaywait::displayPreference(internal,0)>nbdisplaywait::displayPreference(wireless,0));++cases;
 reset(nbdisplaywait::kDisplayVtable,false);auto generic=take();
 assert(nbdisplaywait::displayPreference(internal,0)>nbdisplaywait::displayPreference(generic,0));++cases;
 reset(nbdisplaywait::kExternalDisplayVtable,true);auto external=take();
 assert(nbdisplaywait::displayPreference(internal,0)>nbdisplaywait::displayPreference(external,0));++cases;
 reset(nbdisplaywait::kInternalDisplayVtable,true,0xf50000);auto shifted=take(0xf50000);
 assert(shifted.stop==0 && nbdisplaywait::displayPreference(shifted,0xf50000)==7);++cases;
 reset(nbdisplaywait::kInternalDisplayVtable+8,true);auto wrong=take();
 assert(wrong.stop==nbdisplaywait::WrongDisplayVtable && readCount==5 && !nbdisplaywait::displayPreference(wrong,0));++cases;
 reset(nbdisplaywait::kInternalDisplayVtable,true);failTimer=true;auto failed=take();
 assert(failed.stop==nbdisplaywait::ReadFailed && !(failed.flags&nbdisplaywait::TimerRead));++cases;
 reset(nbdisplaywait::kInternalDisplayVtable,true);memory[0x5078][24]^=4;auto callback=take();
 assert(callback.stop==nbdisplaywait::WrongTimerIdentity && !(callback.flags&nbdisplaywait::TimerIdentityMatched));++cases;
 unsigned best=0;uint64_t chosen=0;
 for(const auto &s:{wireless,generic,external,internal}){
  unsigned rank=nbdisplaywait::displayPreference(s,0);
  if(rank>best){best=rank;chosen=s.displayVtable;}
 }
 assert(chosen==nbdisplaywait::kInternalDisplayVtable);++cases;
 reset(nbdisplaywait::kInternalDisplayVtable,false);auto vsync=take();vsync.serverTail[4]=0x123000;
 assert(nbdisplaywait::displayPreference(vsync,0)==7);++cases;
 printf("PASS %u actual display reader/preference cases; wireless fallback loses to verified internal timer\n",cases);

 unsigned timingCases=0;
 auto timingMemory=[] {
  reset(nbdisplaywait::kInternalDisplayVtable,false);
  nbdisplaywait::State base=take();base.id=77;
  const uint64_t shared=0x10000;
  put(base.display+nbdisplaywait::kTimingSharedOffset,shared);
  uint64_t words[11]{};words[0]=1;words[6]=uint64_t(2)<<32;words[7]=8;
  memory[shared]=std::vector<uint8_t>((uint8_t *)words,(uint8_t *)words+sizeof(words));
  return base;
 };
 auto timingSample=[](const nbdisplaywait::State &base,auto reader) {
  uint64_t absolute=1000;
  nbdisplaywait::TimingState t{};
  nbdisplaywait::captureTiming(t,base,0,reader,[]{return ++ticks;},[&]{return absolute++;});
  return t;
 };
 auto base=timingMemory();auto t=timingSample(base,read);
 assert(t.id==77 && t.flags==31 && t.reads[4]==0 && t.timing==0x10000);
 assert(t.data[6]==uint64_t(2)<<32 && t.data[7]==8 && t.beganTicks<=t.endedTicks);++timingCases;
 base=timingMemory();put(base.display+nbdisplaywait::kTimingSharedOffset,uint64_t(0));
 t=timingSample(base,read);assert(t.timing==0 && !(t.flags&nbdisplaywait::TimingDataRead) && t.reads[2]==-1);++timingCases;
 base=timingMemory();put(base.display,nbdisplaywait::kExternalDisplayVtable);
 t=timingSample(base,read);assert(!(t.flags&nbdisplaywait::TimingDisplayMatched) && t.reads[1]==-1);++timingCases;
 base=timingMemory();base.displayVtable=nbdisplaywait::kWirelessDisplayVtable;
 unsigned oldReads=readCount;t=timingSample(base,read);
 assert(readCount==oldReads && !(t.flags&nbdisplaywait::TimingDataRead));++timingCases;
 base=timingMemory();t=timingSample(base,[](uint64_t at,void *out,unsigned bytes){
  return at==0x10000?5:read(at,out,bytes);});
 assert(t.reads[2]==5 && !(t.flags&nbdisplaywait::TimingDataRead));++timingCases;
 base=timingMemory();unsigned pointerReads=0;
 t=timingSample(base,[&](uint64_t at,void *out,unsigned bytes){
  if(at==base.display+nbdisplaywait::kTimingSharedOffset && ++pointerReads==2){uint64_t other=0x11000;std::memcpy(out,&other,8);return 0;}
  return read(at,out,bytes);});
 assert(t.timing!=t.confirmTiming && !(t.flags&nbdisplaywait::TimingPointerMatched) && t.reads[4]==-1);++timingCases;
 base=timingMemory();unsigned dataReads=0;
 t=timingSample(base,[&](uint64_t at,void *out,unsigned bytes){
  int result=read(at,out,bytes);
  if(at==0x10000 && ++dataReads==2)((uint64_t *)out)[6]=uint64_t(4)<<32;
  return result;});
 assert((t.flags&nbdisplaywait::TimingPointerMatched) && !(t.flags&nbdisplaywait::TimingFieldsMatched));++timingCases;
 base=timingMemory();uint64_t clockCalls=0;t={};
 nbdisplaywait::captureTiming(t,base,0,read,[]{return ++ticks;},[&]{return clockCalls++?999ULL:1000ULL;});
 assert(t.beganTicks>t.endedTicks && !(t.flags&nbdisplaywait::TimingClockRecorded));++timingCases;
 base=timingMemory();((uint64_t *)memory[0x10000].data())[0]=2;
 t=timingSample(base,read);assert(t.flags==31 && (t.data[0]&255)==2);++timingCases;
 assert(timingCases==9);
 printf("PASS %u actual timing-reader cases; rejected data is not an interval or a frame rate\n",timingCases);
}
'''
cpp = args.out / 'display.cpp'
cpp.write_text(prefix + header + cases)
binary = args.out / 'display'
command = [args.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(cpp), '-o', str(binary)]
build = subprocess.run(command, capture_output=True, text=True, timeout=120)
(args.out / 'build.log').write_text(build.stdout + build.stderr)
assert build.returncode == 0, build.stderr
run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
(args.out / 'run.log').write_text(run.stdout + run.stderr)
assert run.returncode == 0, run.stdout + run.stderr
report = dict(passed=True, actual_reader_and_preference_executed=True, cases=14,
              actual_timing_reader_executed=True, timing_cases=9,
              ASan_UBSan=True, fixture_self_memory=True, command=command, output=run.stdout,
              header_sha256=hashlib.sha256((folder/'NativeDisplayWaitState.h').read_bytes()).hexdigest(),
              observer_sha256=hashlib.sha256((folder/'NativeFrameObservers.inc').read_bytes()).hexdigest(),
              original_Quartz_class_evidence_required=True, phone_operations=False,
              live_internal_display_verified=False, physical_frame_improvement_verified=False)
(args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
