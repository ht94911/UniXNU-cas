"""Run the actual client observer against the real Mac main thread with bounded test limits."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import platform
import struct
import subprocess

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--decoder',type=Path,required=True)
p.add_argument('--out',type=Path,required=True);p.add_argument('--cxx',default='clang++');a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True);folder=a.source.parent
front=a.source.read_text();inc=(folder/'NativeFrameObservers.inc').read_text()
header=(folder/'NativeRenderWaitSample.h').read_text();state=(folder/'NativeDisplayWaitState.h').read_text()
fragment=front[front.index('static void traceWaitBytes('):front.index('static void registerRenderWaitThread(')]
record=front[front.index('struct TraceRec {'):front.index('static TraceRec gTrace[')]
identity=inc[inc.index('static bool clientWaitOn()'):inc.index('static int readObserverMemory(')]
client=inc[inc.index('static void startClientWaitSampler()'):]
assert client.count('maximumSamples=90000,maximumDurationUs=1800000000')==1
assert client.count(' }).detach();')==1
client=client.replace('maximumSamples=90000,maximumDurationUs=1800000000','maximumSamples=8,maximumDurationUs=1000000')
client=client.replace(' }).detach();',' }).join();')
assert 'pthread_main_thread_np' in client and 'mach_port_mod_refs' in client and 'mach_port_deallocate' in client
report=dict(source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
    header_sha256=hashlib.sha256((folder/'NativeRenderWaitSample.h').read_bytes()).hexdigest(),
    observer_include_sha256=hashlib.sha256((folder/'NativeFrameObservers.inc').read_bytes()).hexdigest(),
    display_state_header_sha256=hashlib.sha256((folder/'NativeDisplayWaitState.h').read_bytes()).hexdigest(),
    phone_operations=False,real_Mac_main_thread=dict(executed=False,platform=platform.system()),
    controlled_host_changes=['getprogname selects SpringBoard in the test process','sample limit8/duration1second','join instead of detach for fixture ownership'],
    separate_client_filename='springboard-waits.trace',images_emission_stubbed_in_this_Mach_test=True,production_maximum_samples=90000,production_duration_seconds=1800,required_read_bound_bytes=83886080)
assert front.count('/m2v/springboard-waits.trace')==2
assert 'const char *const *paths=render?renderPaths:clientPaths;' in front
assert 'if(!diagOn() || (!render && !client))return;' in front
assert 'if(clientWaitOn())traceStartWriter();' in front
if platform.system()=='Darwin':
    prefix=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
'''
    helpers=r'''
static std::mutex recordLock;static std::vector<TraceRec> records;
static unsigned clientNotes=0;
extern "C" void native_bridge_metal_note(const char *s){assert(!strncmp(s,"ui-wait ",8));++clientNotes;}

static void tracePush(const TraceRec &r){std::lock_guard<std::mutex> l(recordLock);records.push_back(r);}
static void traceSamplePush(const TraceRec &r){tracePush(r);}
static bool diagOn(){return true;}static bool NBTraceSwapTimingOn(){return true;}
static const char *test_getprogname(){return "SpringBoard";}
#define getprogname test_getprogname
static uint64_t nowUs(){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
static void traceRenderWaitImages(bool client){assert(client);}
'''
    main=r'''
int main(int argc,char **argv){assert(argc==2);startClientWaitSampler();assert(clientNotes==2);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(records.data(),72,records.size(),f)==records.size());fclose(f);
 uint64_t tid=0;assert(!pthread_threadid_np(nullptr,&tid));
 printf("{\"main_tid\":%llu,\"pid\":%d,\"records\":%zu}\n",(unsigned long long)tid,getpid(),records.size());
}
'''
    cpp=a.out/'client-main.cpp';cpp.write_text(prefix+header+'\n'+state.replace('#pragma once\n','',1)+record+helpers+fragment+identity+client+main)
    binary=a.out/'client-main';command=[a.cxx,'-std=c++17','-O1','-g','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-unused-const-variable',str(cpp),'-o',str(binary)]
    run=subprocess.run(command,capture_output=True,text=True);(a.out/'build.log').write_text(run.stdout+run.stderr);assert not run.returncode,run.stderr
    run=subprocess.run([str(binary),str(a.out/'client-main.trace')],capture_output=True,text=True,timeout=10)
    (a.out/'run.log').write_text(run.stdout+run.stderr);assert not run.returncode,run.stdout+run.stderr
    info=json.loads(run.stdout)
    spec=importlib.util.spec_from_file_location('wait038',a.decoder);decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    samples=decoder.read_frames(a.out/'client-main.trace',require_header=False)
    actual=[s for s in samples['samples'] if s['stop']!=9]
    assert len(actual)==8 and not samples['quality'],samples['quality']
    assert all(s['thread']==info['main_tid'] and s['basic_kr']==0 and s['state_kr']==0 for s in actual)
    valid=[s for s in actual if s['state']=='waiting' and not s['observation_rejection_reasons'] and s['depth']>0 and s['read_kr']==0]
    assert valid,'No actual main-thread waiting stack'
    fragments={}
    for row in struct.iter_unpack('<II4Q8I',(a.out/'client-main.trace').read_bytes()):
        if row[0]==30:
            assert row[2]==2 and row[7]==96 and row[6] not in fragments
            fragments[row[6]]=struct.pack('<3Q6I',row[3],row[4],row[5],*row[8:])
    assert sorted(fragments)==[0,1]
    identity_bytes=fragments[0]+fragments[1]
    numerator,denominator,timebase_status,_=struct.unpack_from('<IIiI',identity_bytes,80)
    boot_status,boot_bytes=struct.unpack_from('<iI',identity_bytes,32)
    boot_uuid=identity_bytes[40:80].split(b'\0',1)[0].decode()
    assert timebase_status==0 and numerator>0 and denominator>0
    assert boot_status==0 and boot_bytes>=37 and len(boot_uuid)==36
    report['real_Mac_main_thread']=dict(executed=True,passed=True,main_tid=info['main_tid'],pid=info['pid'],
        samples=len(actual),valid_waiting_samples=len(valid),maximum_capture_us=max(s['capture_wall_us'] for s in actual),command=command,
        timebase_numerator=numerator,timebase_denominator=denominator,boot_uuid_read=True)
report['passed']=True
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
