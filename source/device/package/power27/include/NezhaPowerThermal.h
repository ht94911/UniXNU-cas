/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_THERMAL_H
#define NEZHA_POWER_THERMAL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NP_PACK_TBAT_PROPERTY 0x202au
/* Current OS4 qcom_subpmic_get_pack_tbat reads this signed4B value directly.
 * Raw validity proves the matching read, not a Celsius unit or thermal vote. */
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
