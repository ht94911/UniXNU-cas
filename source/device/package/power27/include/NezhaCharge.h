/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CHARGE_H
#define NEZHA_CHARGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t valid;
    int32_t bus_mv, bus_ma, battery_mv, battery_ma, die_centi_c;
    uint8_t chip, switching, switching_valid, enable, mode, adc_enabled;
    uint8_t control[22]; /* registers01..16; includes owned fault latches */
    uint8_t controls_valid;
} NpPump;
/* Selected control bytes within0a..16, ADC17..28 and ID6e. control_valid marks
 * which bytes were actually read. Byte0a mixes status and an event flag; a
 * passive reader omits it unless non-consuming reads are proven. 15h also
 * mixes ADC control and ADC_DONE_FLAG: bus access requires the event owner.
 * Values are separate channel readings, NOT a latched power sample. */
int np_sc8585_decode(NpPump *, uint8_t chip, const uint8_t control[13],
                     uint16_t control_valid, const uint8_t adc[18]);

enum np_policy_state {
    NP_POLICY_FIRMWARE, NP_POLICY_PREPARE, NP_POLICY_REQUEST,
    NP_POLICY_VERIFY, NP_POLICY_ENABLE, NP_POLICY_RUN, NP_POLICY_STOP,
    NP_POLICY_FALLBACK, NP_POLICY_QUARANTINE
};
enum np_charge_action {
    NP_ACTION_NONE, NP_ACTION_CONFIGURE_DISABLED_PUMP, NP_ACTION_REQUEST_PPS,
    NP_ACTION_VERIFY_CONTRACT, NP_ACTION_ENABLE_PUMP, NP_ACTION_DISABLE_PUMP,
    NP_ACTION_RELEASE_OWN_VOTES
};
/* Limits come from the selected stock battery/JEITA/adapter/cable/thermal
 * policies. This engine may LOWER them; it never supplies default maximums.
 * 'verified' means the adapter has validated that exact stock protocol/profile,
 * not that the phone has a 90W marketing specification. */
typedef struct {
    uint64_t epoch, now_ms, sample_ms, profile;
    uint32_t wired, sink, external, sample_valid, protocol_verified;
    uint32_t battery_authenticated, adapter_authenticated, cable_authenticated;
    uint32_t protections_verified, autonomous_fault_shutdown, limits_valid;
    uint32_t remote_ready, suspend, full, fault, pump_off_confirmed;
    uint32_t usb_data_budget_ma;
    uint32_t pps_min_mv, pps_max_mv, pps_max_ma, cable_max_ma;
    uint32_t input_limit_ma, battery_limit_ma, thermal_input_ma, board_power_mw;
    uint32_t requested_mv, requested_ma, requested_battery_ma; /* stock policy */
    uint32_t actual_mv, actual_ma, contract_valid;
} NpChargeFacts;
typedef struct {
    uint64_t epoch, profile, deadline_ms, request_id, issued_ms;
    uint32_t mv, ma, battery_ma;
    uint8_t state, waiting, action, owned;
    int32_t last_error;
} NpChargePolicy;
typedef struct {
    uint64_t epoch, profile, request_id;
    uint32_t mv, ma, battery_ma;
    uint8_t operation;
} NpChargeCommand;
void np_charge_init(NpChargePolicy *);
/* A command is emitted once, with epoch+request ID. A timeout never retries a
 * voltage/current write. Stop acknowledgements and a measured pump-off state
 * precede releasing this client's votes. */
int np_charge_step(NpChargePolicy *, const NpChargeFacts *, NpChargeCommand *);
int np_charge_ack(NpChargePolicy *, uint64_t epoch, uint64_t id,
                  int status, uint64_t now_ms);
/* Wire scalar only. It does not send or authorize any MCA write. */
int np_pps_encode(uint32_t mv, uint32_t ma, uint32_t *wire);
#ifdef __cplusplus
}
#endif
#endif
