/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_THERMAL_H
#define NEZHA_POWER_THERMAL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NP_PACK_TBAT_PROPERTY 0x202au
/* Current OS4 GET202a returns signed whole degrees Celsius. Its active RV32
 * firmware divides the hardware result by10 then subtracts273 before writing
 * the output. Validity still requires the matching epoch/generation read;
 * this measurement is separate from the continuous thermal policy. */
typedef struct {
    uint32_t enabled, valid_raw;
    int32_t error, pack_tbat_raw;
    uint64_t epoch, generation, sampled_ms;
} NpPackThermal;
int np_power_thermal_copy(NpPackThermal *);
#ifdef __cplusplus
}
#endif
#endif
