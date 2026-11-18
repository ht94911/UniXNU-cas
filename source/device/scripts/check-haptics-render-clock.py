#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production original-image admission for the haptic render clock."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def function(source,name):
    start=source.index('static IOReturn '+name+'(')
    at=source.index('{',start);end=at+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=False)
    source=Path(__file__).resolve().parents[1]/'package/haptics27/NezhaHapticsClock.cpp'
    text=source.read_text();constants=text[text.index('constexpr uint64_t appleClock'):text.index('constexpr uint64_t convertersAddress')]
    prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <atomic>
typedef int IOReturn;typedef uintptr_t task_t;
enum { kIOReturnSuccess=0,kIOReturnNotReadable=1,kIOReturnUnsupported=2,kIOReturnIOError=3 };
enum { MH_MAGIC_64=0xfeedfacf,CPU_TYPE_ARM64=0x100000c,LC_UUID=0x1b };
struct mach_header_64 {uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct load_command {uint32_t cmd,cmdsize;};
struct uuid_command {uint32_t cmd,cmdsize;uint8_t uuid[16];};
struct Memory {uint64_t address;std::vector<uint8_t> bytes;};
static std::vector<Memory> memory;static uint32_t actual_slide=0x4000;static bool pin_denied,slide_changed,cas_mapped=true;
static unsigned pin_calls;
static void *find(uint64_t address,size_t n){for(auto &m:memory)if(address>=m.address&&address-m.address<=m.bytes.size()&&n<=m.bytes.size()-(address-m.address))return m.bytes.data()+address-m.address;return NULL;}
static int copyin(uint64_t address,void *out,size_t n){void *p=find(address,n);if(!p)return 1;memcpy(out,p,n);return 0;}
static int nezha_haptics_current_slide(uint32_t *out){*out=actual_slide+(slide_changed?4096:0);return 1;}
struct Pin {bool set(task_t task,uint64_t at,size_t n){++pin_calls;return task==7&&!pin_denied&&find(at,n);}};
static task_t get_task_pmap(task_t task){return task;}
static uint64_t pmap_find_pa(task_t task,uint64_t at){void *p=find(at,8);return task==7&&cas_mapped?(uint64_t)(uintptr_t)p:0;}
static uintptr_t phystokv(uint64_t pa){return (uintptr_t)pa;}
'''
    # The original memory barrier is target-only assembly, already checked in
    # the native object. This host fixture executes the identical CAS/readback.
    publish=function(text,'publish_haptic_literal').replace('__asm__ volatile("dsb sy":::"memory");','std::atomic_thread_fence(std::memory_order_seq_cst);')
    tests=r'''
static void put(uint64_t at,const void *p,size_t n){void *d=find(at,n);assert(d);memcpy(d,p,n);}
static void reset(){
memory.clear();pin_denied=slide_changed=false;cas_mapped=true;pin_calls=0;
memory.push_back({hapticImageBase+actual_slide,std::vector<uint8_t>(16384)});
memory.push_back({hapticRender+actual_slide,std::vector<uint8_t>(sizeof hapticRenderWords)});
memory.push_back({hapticFrequency+actual_slide,std::vector<uint8_t>(8)});
memory.push_back({hapticSeconds+actual_slide,std::vector<uint8_t>(8)});
mach_header_64 h={MH_MAGIC_64,CPU_TYPE_ARM64,0x80000002u,6,1,sizeof(uuid_command),0,0};
uuid_command u={LC_UUID,sizeof(uuid_command),{}};memcpy(u.uuid,hapticUuid,16);
put(hapticImageBase+actual_slide,&h,sizeof h);put(hapticImageBase+actual_slide+sizeof h,&u,sizeof u);
put(hapticRender+actual_slide,hapticRenderWords,sizeof hapticRenderWords);
put(hapticFrequency+actual_slide,&appleClock,8);put(hapticSeconds+actual_slide,&appleHostSeconds,8);
}
static int admit(uint64_t hz=19200000){Pin a,b,c,d;uint64_t f=0,s=0;return prepare_haptic_clock(7,actual_slide,hz,a,b,c,d,&f,&s);}
int main(){
reset();assert(admit()==0&&pin_calls==4);
pin_denied=true;assert(admit()==kIOReturnNotReadable);
reset();slide_changed=true;assert(admit()==kIOReturnUnsupported);
reset();((mach_header_64*)find(hapticImageBase+actual_slide,32))->cpusubtype=2;assert(admit()==kIOReturnUnsupported);
reset();((uuid_command*)find(hapticImageBase+actual_slide+32,24))->uuid[3]^=1;assert(admit()==kIOReturnUnsupported);
reset();((uint32_t*)find(hapticRender+actual_slide,20))[0]^=1;assert(admit()==kIOReturnUnsupported);
reset();uint64_t bad=0;put(hapticSeconds+actual_slide,&bad,8);assert(admit()==kIOReturnUnsupported);
reset();bad=0x41724f8000000000ULL;put(hapticFrequency+actual_slide,&bad,8);assert(admit()==0);
put(hapticSeconds+actual_slide,&nezhaHostSeconds,8);assert(admit()==0);assert(admit(24000000)==kIOReturnUnsupported);
reset();assert(admit(24000000)==0);
assert(publish_haptic_literal(7,hapticSeconds+actual_slide,appleHostSeconds,nezhaHostSeconds)==0);
uint64_t actual=0;assert(!copyin(hapticSeconds+actual_slide,&actual,8)&&actual==nezhaHostSeconds);
assert(publish_haptic_literal(7,hapticSeconds+actual_slide,nezhaHostSeconds,nezhaHostSeconds)==0);
assert(publish_haptic_literal(7,hapticSeconds+actual_slide,appleHostSeconds,nezhaHostSeconds)==0);
reset();cas_mapped=false;assert(publish_haptic_literal(7,hapticSeconds+actual_slide,appleHostSeconds,nezhaHostSeconds)==kIOReturnNotReadable);
reset();put(hapticSeconds+actual_slide,&bad,8);assert(publish_haptic_literal(7,hapticSeconds+actual_slide,appleHostSeconds,nezhaHostSeconds)==kIOReturnUnsupported);
puts("PASS production haptic renderer clock: exact original UUID/instructions, pin/slide/24MHz/corrected/mixed values, atomic publication and readback");
}
'''
    program=a.out/'render-clock.cpp';program.write_text(prefix+constants+function(text,'prepare_haptic_clock')+publish+tests)
    command=[os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all',program,'-o',a.out/'render-clock']
    subprocess.run(list(map(str,command)),check=True)
    result=subprocess.run([str(a.out/'render-clock')],capture_output=True,text=True,check=True)
    (a.out/'execution.log').write_text(result.stdout+result.stderr)
    (a.out/'qualified.json').write_text(json.dumps(dict(passed=True,production_prepare_and_publish_executed=True,
        kernel_source_sha256=hashlib.sha256(text.encode()).hexdigest(),native_cache_barriers_tested_separately=True,
        original_render_ARM_instructions_replay_separate=True,phone_operations=False),indent=2)+'\n')
    print(result.stdout,end='')
if __name__=='__main__':main()
