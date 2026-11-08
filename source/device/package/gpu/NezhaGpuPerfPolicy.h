/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
/* A bounded subset of the accepted 18A policy: bootstrap160, idle222,
 * normal active floor/cap578; diagnostic726/902 only after fresh cool telemetry.
 * HFI/BW tables and busy counter come from the existing SM8850 driver.
 */
static inline uint32_t nezha_gpu_perf_level(uint32_t mode,uint32_t busy_permille,
    uint64_t since_submit_us,int pending,int stalled)
{
    if(stalled)return 3;
    if(mode==1)return 1;
    if(mode==4)return 12; /* stock726 MHz */
    if(mode==5)return 14; /* stock902 MHz */
    if(mode==2 || mode==3)return 10;
    if(pending || since_submit_us<80000 || busy_permille>500)return 10;
    return 3;
}

/* Keep clock and bandwidth votes independent. Index5 is8,171,875 KB/s;
 * index7 is12,449,218 KB/s in this board's existing DDR table. Additional
 * bandwidth is permitted only with valid cool sensors and no thermal cap. */
static inline uint32_t nezha_gpu_perf_thermal_cap(uint32_t wanted,uint32_t thermal,int probe_allowed)
{
    const uint32_t top=probe_allowed?14u:10u;
    if(wanted>top)wanted=top;
    if(thermal && wanted>thermal)wanted=thermal;
    return wanted?wanted:1;
}
static inline uint32_t nezha_gpu_perf_bandwidth(uint32_t level,uint32_t base,
    uint32_t mode,int cool,int demand,uint32_t thermal)
{
    if((mode==4 || mode==5) && level>10 && cool && !thermal)return 7;
    if(level!=10 || !cool || thermal)return base;
    if(mode==3)return 7; /* fixed578 with independent BW comparison lease */
    if(mode==0 && demand)return 5;
    return base;
}

/* Caller must supply one coherent CPU snapshot and its same-publication
 * uptime timestamp. No fresh timestamp is available in the existing015 ABI;
 * this gate must not be wired to a cached snapshot without adding that link. */
static inline int nezha_gpu_perf_probe_allowed(uint32_t mode,uint64_t now_us,uint64_t sample_us,
    const int32_t cpu[20],int counter_valid,int pending,int stalled,uint32_t thermal)
{
    if(mode<4 || mode>5 || !cpu || !counter_valid || !pending || stalled || thermal)return 0;
    if(!sample_us || now_us<sample_us || now_us-sample_us>500000)return 0;
    if(cpu[0]!=1 || cpu[2]!=4 || cpu[3]!=255 || cpu[11]!=0 || cpu[16] || cpu[17])return 0;
    for(unsigned i=8;i<=10;++i)if(cpu[i]<0 || cpu[i]>=600)return 0;
    return 1;
}
