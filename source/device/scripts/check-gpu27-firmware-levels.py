#!/usr/bin/env python3
"""Exercise the actual full-table GPU governor, thermal caps and HFI recovery."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
source=a.source or root/'package/gpu/NezhaGpu.c'
code=source.read_text()
policy=source.with_name('NezhaGpuPerfPolicy.h')
recovery=source.with_name('NezhaGpuPerfVotePolicy.h')
tables=re.search(r'static const struct \{ uint32_t hz, level; \} gpu_opps\[\] = \{(.*?)\}, gmu_opps',code,re.S)
assert tables
opps='static const struct { uint32_t hz, level; } gpu_opps[] = {'+tables[1]+'};\n'
bandwidth=re.search(r'static const uint8_t shared_opp_bw\[\] = \{[^}]+\};',code)
assert bandwidth
begin=code.index('static uint32_t shared_level_for(')
end=code.index('\n}',begin)+2
selector=code[begin:end]
actual=code[code.index('static void shared_perf_thread('):code.index('int nezha_gpu_platform_perf_start(')]
assert 'shared_level_for(needed_hz*11/10)' in actual
assert 'nezha_gpu_perf_dynamic_level(&dynamic,cur,NGPU-1' in actual
assert 'thermal,mode,NGPU-1,probe' in actual
assert 'nezha_gpu_perf_dynamic_accepted(&dynamic,cur,wanted)' in actual
assert 'nezha_gpu_safe_vote_due' in actual and 'shared_opp_bw[wanted-1]' in actual
a.out.mkdir(parents=True,exist_ok=False)
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "NezhaGpuPerfPolicy.h"
#include "NezhaGpuPerfVotePolicy.h"
'''+opps+'#define NGPU (1+sizeof(gpu_opps)/sizeof(gpu_opps[0]))\n'+bandwidth[0]+'\n'+selector+r'''
static uint32_t sample(struct nezha_gpu_perf_dynamic_state *s,uint32_t cur,
    unsigned busy,uint64_t age,int pending,int valid,unsigned thermal) {
    uint64_t need=(uint64_t)gpu_opps[cur-1].hz*busy*2/1000;
    unsigned wanted=nezha_gpu_perf_dynamic_level(s,cur,NGPU-1,
        shared_level_for(need*11/10),need,cur>1?gpu_opps[cur-2].hz:0,
        busy,16000,valid,age,pending);
    return nezha_gpu_perf_thermal_cap(wanted,thermal,0,NGPU-1,0);
}
int main(void) {
    unsigned cases=0;
    struct nezha_gpu_perf_dynamic_state s={0};
    assert(NGPU-1==18 && gpu_opps[17].hz==1200000000);
    assert(sizeof(shared_opp_bw)==NGPU-1 && shared_opp_bw[17]==11);++cases;
    assert(shared_level_for(579000000)==11 && shared_level_for(1200000000)==18);
    assert(shared_level_for(UINT64_MAX)==18);++cases;
    assert(sample(&s,3,0,80000,0,1,0)==3);++cases;
    assert(sample(&s,3,0,0,1,1,0)==10);++cases;
    memset(&s,0,sizeof(s));
    assert(sample(&s,10,600,0,1,1,0)==13);++cases;
    assert(sample(&s,10,900,0,1,1,0)==18);++cases;
    memset(&s,0,sizeof(s));
    assert(sample(&s,18,480,0,1,1,0)==18);++cases;
    memset(&s,0,sizeof(s));
    for(unsigned i=0;i<3;++i)assert(sample(&s,18,100,0,1,1,0)==18);
    assert(sample(&s,18,100,0,1,1,0)==17);++cases;
    memset(&s,0,sizeof(s));nezha_gpu_perf_dynamic_accepted(&s,10,18);
    for(unsigned i=0;i<4;++i)assert(sample(&s,18,100,0,1,1,0)==18);
    assert(sample(&s,18,100,0,1,1,0)==17);++cases;
    memset(&s,0,sizeof(s));
    assert(sample(&s,18,0,80000,0,1,0)==3);++cases;
    assert(sample(&s,10,999,0,1,0,0)==10);
    assert(sample(&s,18,999,80000,0,0,0)==3);++cases;
    memset(&s,0,sizeof(s));
    assert(sample(&s,10,900,0,1,1,9)==9);
    assert(sample(&s,9,900,0,1,1,0)==18);++cases;
    assert(nezha_gpu_perf_level(1,0,0,1,0,18)==1);
    assert(nezha_gpu_perf_level(2,900,0,1,0,18)==10);
    assert(nezha_gpu_perf_level(3,900,0,1,0,18)==10);
    assert(nezha_gpu_perf_level(4,900,0,1,0,18)==12);
    assert(nezha_gpu_perf_level(5,900,0,1,0,18)==14);
    assert(nezha_gpu_perf_level(0,900,0,1,1,18)==3);++cases;
    assert(nezha_gpu_perf_thermal_cap(18,0,0,18,0)==18);
    assert(nezha_gpu_perf_thermal_cap(14,0,5,18,0)==10);
    assert(nezha_gpu_perf_thermal_cap(14,0,5,18,1)==14);
    assert(nezha_gpu_perf_thermal_cap(14,9,5,18,1)==9);++cases;
    int32_t cpu[20]={1,3,4,255};
    assert(nezha_gpu_perf_probe_allowed(5,100000,90000,cpu,1,1,0,0));
    assert(!nezha_gpu_perf_probe_allowed(5,1000000,90000,cpu,1,1,0,0));
    cpu[10]=600;assert(!nezha_gpu_perf_probe_allowed(5,100000,90000,cpu,1,1,0,0));++cases;
    assert(nezha_gpu_perf_bandwidth(18,11,0,1,1,0)==11);
    assert(nezha_gpu_perf_bandwidth(10,3,0,1,1,0)==5);++cases;
    struct nezha_gpu_vote_recovery r={0};
    nezha_gpu_vote_failed(&r,100);assert(nezha_gpu_safe_vote_due(&r,100,0));
    nezha_gpu_safe_vote_result(&r,100,0);assert(!nezha_gpu_safe_vote_due(&r,499999,0));
    assert(nezha_gpu_safe_vote_due(&r,500100,0));
    nezha_gpu_safe_vote_result(&r,500100,1);assert(!nezha_gpu_safe_vote_due(&r,1000200,0));++cases;
    memset(&r,0,sizeof(r));nezha_gpu_vote_failed(&r,0);
    for(unsigned i=0;i<3;++i){assert(nezha_gpu_safe_vote_due(&r,i*500000,0));nezha_gpu_safe_vote_result(&r,i*500000,0);}
    assert(!nezha_gpu_safe_vote_due(&r,1500000,0));++cases;
    printf("PASS %u actual GPU27 cases: firmware18/1200MHz, load scaling and hysteresis, idle/floor, thermal recovery, diagnostic leases and bounded HFI failure recovery\n",cases);
}
'''
test=a.out/'actual.c'
test.write_text(fixture)
cmd=shlex.split(os.environ.get('CC','clang'))+[
    '-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
    '-I'+str(source.parent),str(test),'-o',str(a.out/'actual')]
with (a.out/'build.log').open('x') as log:
    subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=120)
result=subprocess.run([str(a.out/'actual')],capture_output=True,text=True,timeout=20)
(a.out/'run.log').write_text(result.stdout+result.stderr)
assert result.returncode==0,result.stderr
report=dict(passed=True,actual_production_policy=True,actual_thread_uses_policy=True,
    cases=18,full_firmware_levels=18,maximum_table_MHz=1200,
    dynamic_thermal_limits_preserved=True,diagnostic_lease_gate_preserved=True,
    bounded_HFI_recovery_preserved=True,ASan_UBSan=True,phone_operations=False,
    actual_physical_frequency_or_frame_improvement_verified=False,command=cmd,
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    policy_sha256=hashlib.sha256(policy.read_bytes()).hexdigest(),
    HFI_recovery_sha256=hashlib.sha256(recovery.read_bytes()).hexdigest(),
    original18A_load_governor_reused=True)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(result.stdout,end='')
