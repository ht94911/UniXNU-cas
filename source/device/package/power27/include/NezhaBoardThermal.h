/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BOARD_THERMAL_H
#define NEZHA_BOARD_THERMAL_H
#include <stdint.h>
#define NP_BOARD_INPUT_COUNT 8u
#define NP_BOARD_DIRECTION_COUNT 5u
#define NP_BOARD_INPUT_MASK 0xffu
enum {
    NP_BOARD_BATTERY=0, NP_BOARD_CHARGER, NP_BOARD_CONNECTOR, NP_BOARD_CPU,
    NP_BOARD_QUIET, NP_BOARD_WIFI, NP_BOARD_PA1, NP_BOARD_WIRELESS
};
typedef struct {
    uint64_t begin_ms,end_ms;
    uint16_t raw;
    int32_t milli_c,error;
    uint32_t valid;
} NpBoardTemperatureReading;
/* A physical eight-input cohort. input_milli_c retains the original sysfs
 * numeric coordinate: the seven stock ADC inputs are c1024 (1024 per degree),
 * while the original BQ temperature uses1000 per degree. The decoded weights
 * operate on those original numbers. The runtime supplies its actual epoch and
 * sample times. FORMULA and CHARGER are explicit derived diagnostics; neither
 * one claims the daemon's runtime-selected VIRTUAL-SENSOR0 cache or a vote. */
typedef struct {
    uint64_t generation,begin_ms,end_ms,epoch,attachment;
    uint16_t raw[NP_BOARD_INPUT_COUNT];
    int32_t input_milli_c[NP_BOARD_INPUT_COUNT];
    uint64_t sampled_ms[NP_BOARD_INPUT_COUNT];
    int32_t errors[NP_BOARD_INPUT_COUNT];
    uint32_t enabled,attempted_mask,valid_mask,retained;
    int32_t direction_milli_c[NP_BOARD_DIRECTION_COUNT];
    int32_t formula_milli_c,charger_milli_c,global_comp_milli_c;
    uint32_t derived_valid;
} NpBoardThermal;
#ifdef __cplusplus
extern "C" {
#endif
/* Actual N0 MASTER original-temperature word0x1e, unlike ordinary TEMP0x06.
 * Reject the stock out-of-range condition instead of stamping an old cache
 * with a new sample time. The result is millidegrees C. */
int np_bq_original_temperature_milli_c(uint16_t,int32_t *);
/* Actual decoded NORMAL coefficients, 32-bit MADD/ADD, signed SDIV by the
 * configured1000 and math0 signed MAX. Timing/ownership belongs to the caller.
 * Incomplete/error inputs clear only the derived outputs, retaining evidence. */
int np_board_thermal_calculate(NpBoardThermal *,int32_t global_comp_milli_c);
typedef struct {
    uint64_t generation,epoch,attachment,sampled_ms;
    int32_t previous_milli_c[2],current_ua,stage;
    uint8_t initialized,valid;
} NpChargerThermalCurrent;
/* N0 NORMAL-SIC-BAT recurrence, fed only a complete fresh eight-input cohort
 * and the caller's last completed current command. It sends no bus request.
 * 0 publishes a new candidate; EAGAIN means the generation was already used. */
int np_charger_thermal_current(NpChargerThermalCurrent *,const NpBoardThermal *,
                              uint64_t now_ms,int32_t previous_current_ua);
#ifdef __cplusplus
}
#endif
#endif
