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
    if(mode==2)return 10;
    if(pending || since_submit_us<80000 || busy_permille>500)return 10;
    return 3;
}
