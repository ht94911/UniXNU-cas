/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "NezhaCpu27Policy.h"
int main(void)
{
    uint32_t old[4]={0,0,0,0},now[4]={1,1,2,0},busy=9999;
    assert(nz27_cpu_load_delta(now,old,2,&busy) && busy==500);
    assert(!nz27_cpu_load_delta(old,old,2,&busy));
    assert(!nz27_cpu_load_delta(now,old,4,&busy));
    assert(!nz27_cpu_load_delta(0,old,2,&busy));
    const uint32_t wrapold[4]={UINT32_MAX-5,UINT32_MAX-3,UINT32_MAX-7,0};
    const uint32_t wrapnow[4]={4,6,12,0};
    assert(nz27_cpu_load_delta(wrapnow,wrapold,2,&busy) && busy==500);
    const uint32_t levels[]={384000,998400,1670400,1996800,2112000,2227200,2361600};
    assert(nz27_cpu_ceiling(levels,7,1996800)==3);
    assert(nz27_cpu_ceiling(levels,7,2227200)==5);
    assert(nz27_cpu_ceiling(levels,7,2000000)==3);
    assert(nz27_cpu_ceiling(levels,7,1)==0 && nz27_cpu_ceiling(0,0,1)==0);
    uint32_t random=0x31415926;
    for(unsigned test=0;test<100000;++test) {
        uint64_t total=0,idle=0;
        for(unsigned i=0;i<4;++i) {
            random=random*1664525u+1013904223u;old[i]=random;
            const uint32_t delta=(random>>16)&255u;now[i]=old[i]+delta;total+=delta;
            if(i==2)idle=delta;
        }
        if(total)assert(nz27_cpu_load_delta(now,old,2,&busy) && busy==(total-idle)*1000/total && busy<=1000);
    }
    const uint32_t h=0x13u<<10|0x3ffu<<18|4;
    assert(nz27_scmi_envelope(h,h,104));
    assert(nz27_scmi_envelope(h,h,4));
    assert(!nz27_scmi_envelope(h,h^1u,104));
    assert(!nz27_scmi_envelope(h,h^(1u<<10),104));
    assert(!nz27_scmi_envelope(h,h^(1u<<18),104));
    assert(!nz27_scmi_envelope(h,h,0));
    assert(!nz27_scmi_envelope(h,h,5));
    assert(!nz27_scmi_envelope(h,h,108));
    puts("PASS: CPU load ratios and wrap,100000 counter cases,firmware envelope and bounded frequency ceiling");
}
