/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#include <stdbool.h>
/* XNU PROCESSOR_CPU_LOAD_INFO supplies four cumulative 32-bit counters.
 * Differences are modulo32, and the ratio uses their sum, not an assumed
 * tick frequency or a private cpu_data layout. */
static inline bool nz27_cpu_load_delta(const uint32_t now[4],const uint32_t old[4],unsigned idle,uint32_t *busy)
{
    if(!now || !old || !busy || idle>=4)return false;
    uint64_t ticks_total=0;uint32_t delta[4];
    for(unsigned i=0;i<4;++i){delta[i]=now[i]-old[i];ticks_total+=delta[i];}
    if(!ticks_total)return false;
    *busy=(uint32_t)((ticks_total-delta[idle])*1000/ticks_total);return true;
}
static inline uint32_t nz27_cpu_ceiling(const uint32_t *khz,uint32_t count,uint32_t ceiling)
{
    uint32_t n=0;if(!khz || !count)return 0;
    while(n+1<count && khz[n+1]<=ceiling)++n;
    return n;
}
static inline bool nz27_scmi_envelope(uint32_t expected,uint32_t actual,uint32_t bytes)
{
    return (expected&0x0fffffffu)==(actual&0x0fffffffu) && bytes>=4 && bytes<=104 && !(bytes&3);
}
