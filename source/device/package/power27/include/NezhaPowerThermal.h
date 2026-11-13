/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_THERMAL_H
#define NEZHA_POWER_THERMAL_H
#include <stdint.h>
#include "NezhaTsensReadOnly.h"
#include "../gen4-adc/NezhaAdcNative.h"
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
/* Independent TSENS last-status cohort, not the MCA PACK generation. The
 * existing power worker alone samples; consumers never touch MMIO. */
typedef struct {
    uint32_t enabled;
    int error;
    uint64_t generation, begin_ms, end_ms;
    NpTsensSample sample;
} NpTsensThermal;
int np_power_tsens_copy(NpTsensThermal *);
/* Two sequential physical conversions in one completed worker cohort.
 * Their per-channel times are retained; this is not simultaneous sampling.
 * No thermal vote consumes an invalid, stale or retained-owner reading. */
#define NP_CONNECTOR_TTL_MS 3000u
typedef struct {
    uint32_t enabled, attempted, valid_mask, retained;
    int error;
    uint64_t generation, begin_ms, end_ms, sampled_ms[2];
    NpAdcNativeReading reading[2];
} NpConnectorThermal;
int np_power_connector_copy(NpConnectorThermal *);
#ifdef __cplusplus
}
#endif
#endif
