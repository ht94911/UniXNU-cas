/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
/* A bounded subset of the accepted 18A policy: bootstrap160, idle222,
 * active floor578, maximum578 until the thermal governor is integrated.
 * HFI/BW tables and busy counter come from the existing SM8850 driver.
 */
static inline uint32_t nezha_gpu_perf_level(uint32_t mode,uint32_t busy_permille,
    uint64_t since_submit_us,int pending,int stalled)
{
    if(stalled)return 3;
    if(mode==1)return 1;
    if(mode==2 || mode==3)return 10;
    if(pending || since_submit_us<80000 || busy_permille>500)return 10;
    return 3;
}

/* Keep clock and bandwidth votes independent. Index5 is8,171,875 KB/s;
 * index7 is12,449,218 KB/s in this board's existing DDR table. Additional
 * bandwidth is permitted only with valid cool sensors and no thermal cap. */
static inline uint32_t nezha_gpu_perf_thermal_cap(uint32_t wanted,uint32_t thermal)
{
    if(wanted>10)wanted=10;
    if(thermal && wanted>thermal)wanted=thermal;
    return wanted?wanted:1;
}
static inline uint32_t nezha_gpu_perf_bandwidth(uint32_t level,uint32_t base,
    uint32_t mode,int cool,int demand,uint32_t thermal)
{
    if(level!=10 || !cool || thermal)return base;
    if(mode==3)return 7; /* fixed578 with independent BW comparison lease */
    if(mode==0 && demand)return 5;
    return base;
}
