/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CONNECTOR_ANTIBURN_H
#define NEZHA_CONNECTOR_ANTIBURN_H
#include <stdint.h>
#include "NezhaPowerThermal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Independent software vote from measured N0 OS4 thresholds. No IO, GPIO,
 * charger write or worker stop. Caller owns serialization and combines
 * charge_allowed with the existing policy/source/protection conditions. */
#define NP_AB_TRIGGER_C 65
#define NP_AB_RESTORE_C 55
#define NP_AB_BOARD_MAX_C 50
#define NP_AB_WITH_BOARD_C 60
#define NP_AB_WITH_RATE_C 35
#define NP_AB_MAX_RATE_C_PER_S 4
#define NP_AB_MIN_EFFECTIVE_C 25

enum np_connector_antiburn_reason {
    NP_AB_TEMP_UNAVAILABLE = 1u << 0,
    NP_AB_TEMP_STALE = 1u << 1,
    NP_AB_TIME_ORDER = 1u << 2,
    NP_AB_GENERATION_ORDER = 1u << 3,
    NP_AB_RATE_UNAVAILABLE = 1u << 4,
    NP_AB_BOARD_UNAVAILABLE = 1u << 5,
    NP_AB_SOURCE_UNAVAILABLE = 1u << 6,
    NP_AB_CID_UNAVAILABLE = 1u << 7,
    NP_AB_OTG_UNAVAILABLE = 1u << 8,
    NP_AB_LATCHED = 1u << 9,
    NP_AB_COOLDOWN_WAIT = 1u << 10,
    NP_AB_TRIGGER_TEMPERATURE = 1u << 11,
    NP_AB_TRIGGER_BOARD = 1u << 12,
    NP_AB_TRIGGER_RATE = 1u << 13,
    NP_AB_RECOVERED = 1u << 14,
    NP_AB_WITHDRAWN = 1u << 15
};
enum np_connector_antiburn_error {
    NP_AB_ARGUMENT = -1,
    NP_AB_BAD_TEMPERATURE = -2,
    NP_AB_STALE_TEMPERATURE = -3,
    NP_AB_BAD_TIME = -4,
    NP_AB_BAD_GENERATION = -5
};

typedef struct {
    int32_t value;
    uint32_t valid;
} NpConnectorAntiburnSignal;

/* Every valid input is already fresh and current-epoch qualified by caller.
 * USB/CID/OTG are nonzero/zero values. board_temperature_c is whole degrees
 * Celsius; valid=0 for the currently unbound board source, regardless of value.
 * BOOST/TYPEC are not substitutes for CID or OTG plugin. */
typedef struct {
    NpConnectorAntiburnSignal usb_online, cid, otg_plugin, board_temperature_c;
} NpConnectorAntiburnSignals;

typedef struct {
    uint32_t charge_allowed, latch, cooldown_wait, decision_complete;
    uint32_t reason, cooldown_reason;
    int32_t error, last_error;
    uint64_t now_ms, rate_interval_ms;
    /* Original input, including partial/raw/IO error/timestamps. Its masks are
     * evidence only; temperature_valid_mask below controls consumption. */
    NpConnectorThermal cohort;
    uint32_t temperature_valid_mask, rate_valid_mask, rate_initialized_mask;
    int32_t temperature_c[2], effective_temperature_c;
    int64_t rate_c_per_s[2], selected_rate_c_per_s;
    uint32_t selected_index, selected_rate_valid, selected_rate_initialized;
} NpConnectorAntiburnVote;

typedef struct {
    NpConnectorAntiburnVote vote;
    uint64_t generation_high_watermark, end_high_watermark, last_now_ms;
    uint64_t history_generation, history_end_ms;
    int32_t history_temperature_c[2];
    int64_t history_rate_c_per_s[2];
    uint32_t history_rate_valid_mask, history_rate_initialized_mask;
    uint32_t history_valid, has_now, ever_initialized;
    uint32_t latch, cooldown_wait, cooldown_reason;
    int32_t last_error;
} NpConnectorAntiburn;

void np_connector_antiburn_init(NpConnectorAntiburn *);
/* Reevaluate fresh signals even for the same cohort, without recalculating its
 * rates. Invalid/partial/stale/nonquiet input withdraws consumption and breaks
 * differencing history while preserving the confirmed latch and cooldown wait.
 * First valid pair uses the proved first_call rate0 initialization and marks
 * rate_initialized_mask; rate_valid_mask is reserved for measured differences.
 * After a gap, the first clean pair only reestablishes history, so invalid old
 * cached temperatures cannot become a rise rate or satisfy recovery.
 * Return0 means vote updated, including an incomplete/denied decision; only a
 * NULL state is an argument error. NULL cohort/signals mean unavailable input. */
int np_connector_antiburn_step(NpConnectorAntiburn *, const NpConnectorThermal *,
    const NpConnectorAntiburnSignals *, uint64_t now_ms);
/* Existing Runtime PM invalidation calls this in place. No reinitialization:
 * latch and uncertain-trigger cooldown wait survive suspend/resume. error is
 * retained for diagnostics (for example the caller's actual -ESTALE). */
void np_connector_antiburn_invalidate(NpConnectorAntiburn *, int32_t error);
int np_connector_antiburn_copy(const NpConnectorAntiburn *, NpConnectorAntiburnVote *);

#ifdef __cplusplus
}
#endif
#endif
