#!/usr/bin/env python3
"""Execute actual CPU27 startup and level-selection functions with controlled firmware and thermal peers."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess,re
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();r=Path(__file__).resolve().parents[1];source=a.source or r/'package/cpu/NezhaCpu.c';text=source.read_text();a.out.mkdir(parents=True,exist_ok=False)
def func(name):
 m=re.search(r'^static[^;{}]*\b'+name+r'\([^;{}]*\)\s*\n\{',text,re.M);assert m,name
 lo=m.start();i=text.index('{',m.start());depth=0
 for hi in range(i,len(text)):
  depth+=(text[hi]=='{')-(text[hi]=='}')
  if not depth:return text[lo:hi+1]
 raise AssertionError(name)
functions={n:func(n)for n in ['cpu27_available_top','lv','cpu_try','cpu_start_levels','cpu_check_levels']}
assert 'cpu27_limit_levels' not in text
assert not re.search(r'caps\[2\]\s*=\s*\{1996800,2227200\}',text)
prefix=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NEZHA_CPU_IOS27 1
struct cpu_domain {uint32_t khz[64],levels,first,count,level,top,fixed,floor,floor_khz,set_errors;int method;uintptr_t fast;uint64_t fast_db_set,fast_db_keep;uint32_t mhz_now;};
static struct cpu_domain cpu_dom[2];
static uint32_t cpu_tcap[2],cpu_scap[2];
static unsigned sets,getSustained,fastCalls;
static uint32_t requested[2],firmware[2];
static int nz_mode=3,failDomain=-1,fixRequest=-1;
static uint32_t cpu27_available_top(uint32_t d);
static int cpu_set_level_via(uint32_t d,uint32_t level,int method){assert(d<2 && level<cpu_dom[d].levels && (method==0||method==2));requested[d]=level;++sets;if((int)d==failDomain)return -5;firmware[d]=level;return 0;}
static int cpu_set_level(uint32_t d,uint32_t level){return cpu_set_level_via(d,level,cpu_dom[d].method);}
static uint32_t cpu_get(uint32_t d,uint32_t msg,uint32_t word){if(msg==3){assert(word==4);++getSustained;return d?26:27;}assert(msg==8&&word==1);return firmware[d];}
static bool PE_parse_boot_argn(const char *name,void *p,int n){assert(n==sizeof(int));assert(!strcmp(name,"nezha-cpu-fix0")||!strcmp(name,"nezha-cpu-fix1"));if(fixRequest>=0){*(int*)p=fixRequest;return true;}return false;}
static uint32_t cpu_clock_on(int cpu){assert(cpu==0||cpu==6);return firmware[cpu?1:0];}
static void IOSleep(unsigned n){assert(n==20);}
static void cpu_fast_channel(uint32_t d,uint32_t msg){assert(msg==7);++fastCalls;cpu_dom[d].fast=1;}
"""
cases=r"""
static void reset(void){memset(cpu_dom,0,sizeof(cpu_dom));sets=getSustained=fastCalls=0;nz_mode=3;failDomain=fixRequest=-1;for(unsigned d=0;d<2;++d){cpu_dom[d].levels=d?29:28;cpu_dom[d].top=cpu_dom[d].levels-1;cpu_dom[d].first=d?6:0;cpu_dom[d].method=2;cpu_dom[d].floor_khz=1000000;for(unsigned i=0;i<cpu_dom[d].levels;++i)cpu_dom[d].khz[i]=384000+i*100000;cpu_dom[d].khz[cpu_dom[d].top]=d?4608000:3628800;cpu_tcap[d]=cpu_scap[d]=UINT32_MAX;firmware[d]=0;}}
int main(void){unsigned cases=0;
reset();cpu_start_levels();assert(getSustained==0&&requested[0]==27&&requested[1]==28&&cpu_dom[1].top==28);++cases;
reset();cpu_scap[0]=10;cpu_tcap[1]=7;cpu_start_levels();assert(requested[0]==10&&requested[1]==7&&cpu_dom[0].top==27&&cpu_dom[1].top==28);cpu_scap[0]=cpu_tcap[1]=UINT32_MAX;assert(cpu27_available_top(0)==27&&cpu27_available_top(1)==28);++cases;
reset();cpu_tcap[0]=9;cpu_scap[0]=12;assert(cpu27_available_top(0)==9);cpu_tcap[0]=UINT32_MAX;assert(cpu27_available_top(0)==12);++cases;
reset();cpu_scap[1]=0;cpu_start_levels();assert(requested[1]==0&&cpu_dom[1].top==28);++cases;
reset();cpu_check_levels();assert(fastCalls==2&&cpu_dom[0].method==2&&cpu_dom[1].method==2&&requested[0]==26&&requested[1]==27);++cases;
reset();cpu_scap[0]=6;cpu_scap[1]=8;cpu_check_levels();assert(requested[0]==6&&requested[1]==8&&cpu_dom[0].top==27&&cpu_dom[1].top==28);++cases;
reset();failDomain=1;cpu_check_levels();assert(cpu_dom[1].method==-1);unsigned before=sets;cpu_start_levels();assert(sets==before+1&&cpu_dom[1].method==-1);++cases;
reset();failDomain=0;cpu_start_levels();assert(cpu_dom[0].method==-1&&cpu_dom[0].set_errors==1&&cpu_dom[0].top==27);++cases;
reset();fixRequest=5;cpu_start_levels();assert(requested[0]==5&&requested[1]==5&&cpu_dom[1].top==28);++cases;
reset();nz_mode=2;cpu_start_levels();assert(cpu_dom[0].fixed==27&&cpu_dom[1].fixed==28);++cases;
reset();assert(lv(1,UINT32_MAX)==28);assert(cpu_try(1,UINT32_MAX,2,"peer")&&requested[1]==28);++cases;
printf("PASS %u actual CPU27 cases: firmware28/29 level maxima restored, sustained ceiling omitted, active thermal caps and cooldown recovery, SCMI/fast-channel requests and failure/explicit-fixed policy retained\n",cases);}
"""
cpp=a.out/'actual.c';cpp.write_text(prefix+'\n\n'.join(functions.values())+cases);binary=a.out/'actual';cmd=shlex.split(os.environ.get('CC','clang'))+['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(binary)]
with (a.out/'build.log').open('x')as f:subprocess.run(cmd,check=True,stdout=f,stderr=subprocess.STDOUT,timeout=120)
r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20);(a.out/'run.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
report=dict(passed=True,actual_functions=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),function_sha256={k:hashlib.sha256(v.encode()).hexdigest()for k,v in functions.items()},cases=11,firmware_maximum_levels_allowed=True,boost_levels_not_capped_by_sustained=True,thermal_limits_preserved_and_cooling_recovers=True,fixed_old1996_2227_cap_absent=True,firmware_io_simulated=True,ASan=True,UBSan=True,command=cmd,phone_operations=False,actual_frequency_hardware_verified=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(r.stdout,end='')
