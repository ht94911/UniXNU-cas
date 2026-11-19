/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
/* The existing 18A load governor, using the complete board firmware table.
 * Active578 is a floor, not a ceiling. Diagnostic fixed726/902 leases keep
 * their original cool-snapshot gate. Thermal caps apply to every mode.
 */
struct nezha_gpu_perf_dynamic_state {
    uint64_t busy_run_us;
    uint32_t down_windows;
    uint32_t hold_window;
};
static inline uint32_t nezha_gpu_perf_dynamic_level(
    struct nezha_gpu_perf_dynamic_state *state,uint32_t current,uint32_t top,
    uint32_t upward_level,uint64_t needed_hz,uint32_t lower_hz,
    uint32_t busy_permille,uint64_t window_us,int counter_valid,
    uint64_t since_submit_us,int pending)
{
    const int recent=pending || since_submit_us<80000;
    const int active=pending || (counter_valid && busy_permille);
    uint32_t wanted=current;
    if(!counter_valid) {
        state->busy_run_us=0;state->down_windows=0;
        wanted=recent?current:3;
    } else {
        state->busy_run_us=busy_permille>=950?state->busy_run_us+window_us:0;
        if(busy_permille>850 || state->busy_run_us>=50000)wanted=top;
        else if(upward_level>current)wanted=upward_level;
        else if(active) {
            if(current>1 && needed_hz*10<=(uint64_t)lower_hz*8) {
                if(++state->down_windows>=4+state->hold_window)wanted=current-1;
            } else state->down_windows=0;
        } else if(!recent)wanted=3;
    }
    if(recent && wanted<10)wanted=10;
    if(wanted>top)wanted=top;
    return wanted?wanted:1;
}
static inline void nezha_gpu_perf_dynamic_accepted(
    struct nezha_gpu_perf_dynamic_state *state,uint32_t before,uint32_t after)
{
    if(before!=after) {
        state->hold_window=after>before;
        state->down_windows=0;
    }
}
static inline uint32_t nezha_gpu_perf_level(uint32_t mode,uint32_t busy_permille,
    uint64_t since_submit_us,int pending,int stalled,uint32_t dynamic_level)
{
    (void)busy_permille;(void)since_submit_us;(void)pending;
    if(stalled)return 3;
    if(mode==1)return 1;
    if(mode==4)return 12; /* stock726 MHz */
    if(mode==5)return 14; /* stock902 MHz */
    if(mode==2 || mode==3)return 10;
    return dynamic_level;
}

/* Keep clock and bandwidth votes independent. Index5 is8,171,875 KB/s;
 * index7 is12,449,218 KB/s in this board's existing DDR table. Additional
 * bandwidth is permitted only with valid cool sensors and no thermal cap. */
static inline uint32_t nezha_gpu_perf_thermal_cap(uint32_t wanted,uint32_t thermal,
    uint32_t mode,uint32_t firmware_top,int probe_allowed)
{
    uint32_t top=mode==0?firmware_top:probe_allowed?14u:10u;
    if(top>firmware_top)top=firmware_top;
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
