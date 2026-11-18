/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CCCV_H
#define NEZHA_CCCV_H
#include "NezhaPower.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
 uint64_t epoch,attachment,sampled_ms;
 int32_t temperature_c,zone,stage,voltage_mv,current_max_ma,current_min_ma;
 uint32_t rows,valid;
 uint8_t initialized;
 int32_t cycles,last_voltage_mv;
 uint8_t cycles_valid,profile,termination_samples,termination_done;
} NpCccv;
typedef struct {
 int32_t battery_mv,battery_ma,bus_ma,limit_mv,limit_ma;
 uint32_t adapter_ma,real_type;
 uint64_t now_ms,last_boost_ms,last_buck_ms;
 uint8_t boost_done;
} NpCccvInput;
/* Actual wired N0 ATL normal curves and OS4 stage arithmetic. No FFC flag,
 * gauge calibration, hardware access or default high-current permission. */
int np_cccv_update_profile(NpCccv *,const NpSnapshot *,const NpGaugeIdentity *,int,
                           uint64_t epoch,uint64_t attachment,unsigned fast_mode);
int np_cccv_update(NpCccv *,const NpSnapshot *,const NpGaugeIdentity *,int,
                   uint64_t epoch,uint64_t attachment);
/* Main input deliberately paused by its controller, before CP enable. A
 * negative net battery current cannot qualify a new CV transition here. */
int np_cccv_prepare_input_suspended(NpCccv *,const NpSnapshot *,const NpGaugeIdentity *,int,
                                  uint64_t epoch,uint64_t attachment,unsigned fast_mode);
/* Signed mV adjustment from the observed stock ratio2 branch. The caller
 * supplies the actual battery/adapter ceilings and encodes the20mV request. */
int32_t np_cccv_delta(const NpCccv *,const NpCccvInput *);
/* Original N0 forward ratio2/4 tables. Actual adapter/battery budgets remain
 * caller inputs. Existing mode8 continues using the ratio2 entry above. */
int32_t np_cccv_delta_ratio(const NpCccv *,const NpCccvInput *,unsigned ratio);
/* N0's current monitor: final selected voltage - PPS5mV hysteresis,
 * final current_min +300mA and five successive completed observations. */
int np_cccv_termination_update(NpCccv *,const NpSnapshot *);
#ifdef __cplusplus
}
#endif
#endif
