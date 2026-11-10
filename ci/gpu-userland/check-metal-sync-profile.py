"""Exercise the actual compact sync producer with controlled clocks and a record sink."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--decoder',type=Path)
p.add_argument('--cxx',default='clang++')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
s=a.source.read_text()
record=s[s.index('struct TraceRec {'):s.index('static TraceRec gTrace[')]
globals_=s[s.index('static thread_local uint64_t tDetailCommand;'):s.index('static uint64_t blockInvoke(id block)')]
producer=s[s.index('static void detailDefinition('):s.index('static uint64_t encodeCommandOf(id);')]
assert 'Type18 c/d hold thread CPU nanoseconds' in producer
prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>
static uint64_t wall=1000,cpu=100;
static bool enabled=true,cpu_ok=true;
static unsigned cpu_queries=0;
static bool diagOn(){return enabled;}
static uint64_t nowUs(){return wall;}
static int test_clock_gettime(clockid_t id,timespec *out){
 assert(id==CLOCK_THREAD_CPUTIME_ID);++cpu_queries;
 if(!cpu_ok)return -1;
 out->tv_sec=cpu/1000000000ull;out->tv_nsec=cpu%1000000000ull;return 0;
}
#define clock_gettime test_clock_gettime
static int test_threadid_np(void *,uint64_t *out){*out=77;return 0;}
#define pthread_threadid_np test_threadid_np
'''
sink='''static std::vector<TraceRec> records;
static void tracePush(const TraceRec &row){records.push_back(row);}
'''
cases=r'''
static void reset(){records.clear();wall=1000;cpu=100;enabled=true;cpu_ok=true;cpu_queries=0;assert(tDetailParent==0);}
static const TraceRec &link(uint64_t id){for(auto &r:records)if(r.type==18 && r.t1==id)return r;assert(false);return records[0];}
static unsigned details(){unsigned n=0;for(auto &r:records)n+=r.type==17;return n;}
static int early(){DetailSpan api(22,1,(uintptr_t)"early");wall+=500;cpu+=8000;return 42;}
int main(int argc,char **argv){
 assert(argc==2);unsigned passed=0;
 reset();enabled=false;unsigned called=0;
 assert(syncDetail(26,"disabled",[&]{++called;return 7;})==7);
 assert(called==1 && records.empty() && cpu_queries==0 && tDetailParent==0);++passed;

 reset();{DetailSpan fast(22,1,(uintptr_t)"fast");wall+=249;}
 assert(records.empty() && cpu_queries==1 && tDetailParent==0);++passed;

 reset();uint64_t outerId,childId;
 {DetailCommandScope command(314);DetailSpan outer(22,123,(uintptr_t)"API");outerId=outer.id;
  wall+=50;{DetailSpan child(26,456,(uintptr_t)"runtime");childId=child.id;wall+=300;}
  assert(tDetailParent==outerId);wall+=150;cpu+=(1ull<<33)+17;
 }
 assert(details()==2 && cpu_queries==2 && tDetailParent==0 && tDetailCommand==0);
 assert(link(childId).t2==outerId && link(childId).tid==0);
 const auto &l=link(outerId);assert(l.t2==0 && l.tid==1 && (l.c|uint64_t(l.d)<<32)==(1ull<<33)+17);
 for(auto &r:records)if(r.type==17)assert(r.b==77 && r.t2==314);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(records.data(),sizeof(TraceRec),records.size(),f)==records.size());fclose(f);++passed;

 reset();assert(early()==42);assert(details()==1 && tDetailParent==0);++passed;

 reset();cpu_ok=false;uint64_t bad;
 {DetailSpan failed(22,1,(uintptr_t)"bad-clock");bad=failed.id;wall+=500;}
 assert(link(bad).tid==0 && link(bad).c==0 && link(bad).d==0 && cpu_queries==1);++passed;

 reset();uint64_t backwards;
 {DetailSpan failed(22,1,(uintptr_t)"regressing-clock");backwards=failed.id;wall+=500;cpu=99;}
 assert(link(backwards).tid==0 && link(backwards).c==0 && link(backwards).d==0);++passed;

 reset();uint64_t legacy;
 {DetailSpan callback(3,1,0xdeadbeef,true);legacy=callback.id;wall+=1;}
 assert(details()==2 && cpu_queries==0 && link(legacy).tid==0 && tDetailParent==0);++passed;

 reset();uint64_t edge;
 {DetailSpan threshold(22,1,(uintptr_t)"250us");edge=threshold.id;wall+=250;cpu+=1200;}
 assert(details()==1 && link(edge).tid==1 && link(edge).c==1200);++passed;
 std::cout<<"cases="<<passed<<" records="<<sizeof(TraceRec)<<" nested/early/disabled/CPU-validity/legacy passed\n";
}
'''
fixture=a.out/'sync-producer.cpp';fixture.write_text(prefix+record+sink+globals_+producer+cases)
binary=a.out/'sync-producer'
command=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
         '-fsanitize=address,undefined',str(fixture),'-o',str(binary)]
subprocess.run(command,check=True)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
run=subprocess.run([str(binary),str(a.out/'actual-producer.trace')],env=env,text=True,capture_output=True,timeout=30)
(a.out/'run.log').write_text(run.stdout+run.stderr);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
decoded=False
if a.decoder:
    spec=importlib.util.spec_from_file_location('sync_decoder',a.decoder);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    rows=m.frontend_details((a.out/'actual-producer.trace').read_bytes())
    spans=[r for r in rows if r['name']=='cpu_detail'];links={r['operation']:r for r in rows if r['name']=='cpu_detail_link'}
    outer=next(r for r in spans if r['kind']==22);child=next(r for r in spans if r['kind']==26)
    assert outer['elapsed_us']==500 and child['elapsed_us']==300
    assert links[child['operation']]['parent']==outer['operation']
    assert links[outer['operation']]['thread_cpu_ns']==(1<<33)+17
    assert links[child['operation']]['thread_cpu_ns'] is None
    decoded=True
report=dict(passed=True,actual_source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
    producer_sha256=hashlib.sha256((record+globals_+producer).encode()).hexdigest(),
    fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),ASan_UBSan=True,cases=8,
    command=command,result=run.stdout,actual_producer_to_decoder_checked=decoded,
    mocked_clock_thread_ID_and_trace_sink=True,Metal_calls_not_emulated=True,phone_operations=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
