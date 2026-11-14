/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_JEITA_H
#define NEZHA_JEITA_H
#include "NezhaPower.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The current CN BM6N/F table is selected from measured identity. These are
 * JEITA ceilings; the caller also applies source, thermal and control limits. */
typedef struct {
    int32_t temperature_c, fcc_ma, fv_mv, iterm_ma, row;
    uint8_t valid, enabled, temperature_held, voltage_held;
    int32_t raw_soc;
    uint8_t full_valid, full, recharging;
} NpJeitaVote;
typedef struct {
    NpJeitaVote vote;
    int32_t row, current_ma;
    uint8_t initialized, hot_stopped, full_latched, recharging, full_cleared;
} NpJeita;
void np_jeita_init(NpJeita *);
int np_jeita_update(NpJeita *, const NpSnapshot *, const NpGaugeIdentity *, int country_cn);
/* CN recharging uses RM/FCC*10000 <=9700, independently of UI SOC. A
 * completed battery flag is observed; no forced-full/gauge writes are made. */
int np_jeita_full_update(NpJeita *, const NpSnapshot *);
#ifdef __cplusplus
}
#endif
#endif
