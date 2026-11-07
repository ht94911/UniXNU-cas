/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* CPU-only cumulative metrics, individually atomic. Differences over a
 * measured interval are meaningful; this is not one hardware-atomic sample.
 * jobs: submit,retire,last submit us,last retire us,GPU ticks,total valid
 * stamps,max GPU ticks,total submit wall us,max submit wall us,total lifetime
 * us,max lifetime us,last issued fence,last retired fence,reserved.
 * GPU timestamps count CP_ALWAYS_ON ticks at 19.2 MHz, not CPU ticks.
 */
struct NativeAdrenoPerfSample {
    uint64_t jobs[14],cache[5],reassert_ticks,cpu_us;
    uint32_t cpu_valid,cpu[8][4];
};
extern "C" {
extern uint64_t native_adreno_perf_jobs[14];
bool native_adreno_perf_sample(NativeAdrenoPerfSample *);
void native_adreno_perf_calls(uint64_t out[18][4]);
}
