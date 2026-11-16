/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADAPTER_POLICY_H
#define NEZHA_ADAPTER_POLICY_H
#include "NezhaPd.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
 uint64_t epoch,sampled_ms;
 uint32_t ratio,pump_mode,pdo_index;
 uint32_t minimum_mv,maximum_mv,adapter_ma;
 uint32_t single_battery_ma,maximum_battery_ma,initial_headroom_mv;
 uint8_t valid;
} NpAdapterChoice;
/* N0 has a single gauge and support_mode6. Only programmable offers enter
 * this selector. The OS4 BUCK support19 result is supplied by the caller:
 * 0 and1 are real results; -1 means it has not been established. The original
 * type12 branch does not need that BUCK result to select4:1. */
int np_adapter_stock_support100w(unsigned configured_support,unsigned power_boost,unsigned sku);
int np_adapter_select(const NpPdCaps *,uint64_t epoch,unsigned real_type,
                       int support_100w,NpAdapterChoice *);
#ifdef __cplusplus
}
#endif
#endif
