"""Exercise the actual display reader and actual frame producer with the decoder."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess

R=Path('/home/hua/nezha-work/ios27/gpu27-astra')
E=Path('/mnt/i/Code/UniXNU-nezha/evidence/gpu27-main078-20261104')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
p.add_argument('--source',type=Path,default=R/'userland/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm')
p.add_argument('--gop',type=Path,default=R/'gop-integration')
p.add_argument('--core',type=Path,default=R/'device-main078/package/gpu')
p.add_argument('--reader-test',type=Path,default=E/'display-timing/check-dpu-timing.c')
p.add_argument('--decoder',type=Path,default=E/'decode-display-timing.py');a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
front=a.source
source=front.read_text();producer=source[source.index('extern "C" void NBTraceSwapTiming('):source.index('static void traceNote(',source.index('extern "C" void NBTraceSwapTiming('))]
record=re.search(r'struct TraceRec \{[^\n]+\};',source).group()
gop=a.gop;device=a.core

def run(cmd,name):
    result=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=60)
    (a.out/(name+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(name+': '+result.stdout+result.stderr)
    return result.stdout

run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',
     '-I',device,'-I',gop,a.reader_test,'-o',a.out/'reader'],'reader-build')
run([a.out/'reader'],'reader-run')
cpp='''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include "NativeGOPTiming.h"
'''+record+'''
static bool on=true;
extern "C" bool NBTraceSwapTimingOn(){return on;}
static std::vector<TraceRec> records;
static void tracePush(TraceRec r){r.idx=(uint32_t)records.size();records.push_back(r);}
'''+producer+'''
static NGPTimingReply sample(uint64_t seq,uint32_t phase,uint64_t us,uint32_t count){
 NGPTimingReply r{};r.version=1;r.flags=7;r.hardware_version=0xd0000000;r.attempts=1;
 r.sequence=seq;r.phase=phase;r.surface_handle=417;r.mapping=3;r.source_cookie=0xabcdef1234567890ull;
 r.cpu_before_us=us;r.cpu_after_us=us+4;r.counter_before=us*192/10+20;r.counter_after=us*192/10+60;
 auto *v=r.interfaces[0];v[5]=1;v[6]=1;v[9]=0x80000001u;v[11]=count;v[12]=77;
 v[13]=(count<<16)|123;v[15]=120;v[24]=(count<<16)|100;
 uint64_t stamp=(us-10000)*192/10;v[20]=(uint32_t)stamp;v[21]=(uint32_t)(stamp>>32);
 return r;
}
int main(int argc,char **argv){
 assert(argc==2);NBTraceSwapTiming(nullptr);assert(records.empty());
 NGPTimingReply r=sample(1,2,1000000,100);on=false;NBTraceSwapTiming(&r);assert(records.empty());on=true;
 TraceRec h{};h.type=0;h.b=63;h.d=2;tracePush(h);
 NBTraceSwapTiming(&r);assert(records.size()==8);
 TraceRec c{};c.type=5;c.a=1;c.t0=999000;c.t1=1000005;c.t2=1000010;tracePush(c);
 r=sample(2,1,1100000,112);NBTraceSwapTiming(&r);
 c.a=2;c.t0=1099999;c.t1=1101000;c.t2=1101050;tracePush(c);
 // All 32-bit words, high addresses and six 64-bit fields travel unmodified.
 NGPTimingReply wide{};uint32_t words[sizeof(wide)/4];
 for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i)words[i]=0x89765400u+i;
 memcpy(&wide,words,sizeof wide);wide.sequence=0x123456789aull;wide.phase=2;
 NBTraceSwapTiming(&wide);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(records.data(),sizeof(TraceRec),records.size(),f)==records.size());fclose(f);
 puts("PASS actual frame producer: null/off, seven fragments, high addresses and all reply words");return 0;
}
'''
(a.out/'producer.cpp').write_text(cpp)
run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',
     '-I',gop,a.out/'producer.cpp','-o',a.out/'producer'],'producer-build')
frames=a.out/'fake-display-frames.trace';run([a.out/'producer',frames],'producer-run')
spec=importlib.util.spec_from_file_location('display_decoder',a.decoder)
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
decoded=decoder.read_frames(frames);assert decoded['quality']=={} and len(decoded['rows'])==3
first,second,wide=decoded['rows']
assert first['source_cookie']==0xabcdef1234567890 and first['surface_handle']==417
assert first['interfaces'][0]['mdp_frame_count']==100 and first['interfaces'][0]['video_frame_count']==77
assert second['interfaces'][0]['mdp_frame_count']==112
assert wide['sequence']==0x123456789a and wide['version']==0x89765400 and wide['interfaces'][1]['write_line_high']==0x89765449
pairs=decoder.unchanged_scanout_intervals(decoded['rows'],decoded['copies'])
assert len(pairs)==1 and pairs[0]['hardware_clock_corroborated'],pairs
assert pairs[0]['mdp_cycles']==12 and pairs[0]['interior_MDP_cycles_lower_bound']==11
assert abs(pairs[0]['unchanged_scanout_ms']-99.996)<.000001

blob=frames.read_bytes();parts=[blob[i:i+decoder.FRAME.size] for i in range(0,len(blob),decoder.FRAME.size)]
for name,payload,reason in [
    ('missing',b''.join(parts[:7]+parts[8:]),'incomplete_fragment_group'),
    ('duplicate',b''.join(parts[:2]+[parts[1]]+parts[2:]),'duplicate_fragment_group')]:
    path=a.out/(name+'.trace');path.write_bytes(payload);report=decoder.read_frames(path)
    assert report['quality'][reason]==1
    assert not decoder.unchanged_scanout_intervals(report['rows'],report['copies'])

def rejected(mutator,expected):
    rows=json.loads(json.dumps(decoded['rows']));mutator(rows)
    pair=decoder.unchanged_scanout_intervals(rows,decoded['copies'])[0]
    assert not pair['hardware_clock_corroborated'] and expected in pair['rejection_reasons'],pair

rejected(lambda rows:rows[1].update(cpu_frames=1),'non_GPU_writer_or_fallback_changed')
rejected(lambda rows:rows[1].update(active_interface_indices=[]),'no_unique_stable_active_interface')
rejected(lambda rows:rows[1]['interfaces'][0].update(mdp_frame_count=100),
         'hardware_refresh_did_not_demonstrably_advance')
rejected(lambda rows:rows[1]['interfaces'][0].update(autorefresh_config=0x80000002),
         'display_configuration_changed')
rejected(lambda rows:rows[1]['interfaces'][0].update(underruns=1),'display_underruns_changed')
for field,value in [('status',3),('flags',15),('hardware_version',0x90000000),('counter_after',0)]:
    sample=dict(first);sample[field]=value;assert decoder.clock_sample_reasons(sample)

report=dict(passed=True,phone_operations=False,physical_display_measured=False,
    actual_reader=True,actual_frame_producer=True,actual_decoder=True,
    cases=['reader raw counters and inactive data','reader crossing/torn timestamp/IO/clocks',
           'actual producer and decoder full-width roundtrip','missing/duplicate groups',
           'hardware-clock corroboration under a known synthetic timeline',
           'non-GPU write/inactive/stalled clock/configuration/underrun rejection'],
    source_sha256={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in
        [front,gop/'NativeGOPTiming.h',device/'NezhaDpuTiming.h',a.decoder]},
    sample_result=pairs[0])
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
