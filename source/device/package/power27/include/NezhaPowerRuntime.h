/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_RUNTIME_H
#define NEZHA_POWER_RUNTIME_H
#include "NezhaPower.h"
#include "NezhaMca.h"
#include "NezhaCharge.h"
#include "NezhaPowerControl.h"
#include "NezhaAuth.h"
#include "NezhaTsensReadOnly.h"
#include "NezhaBoardThermal.h"
#include "../gen4-adc/NezhaAdcNative.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The selected package's resource owner supplies these adapters. They bind to
 * NezhaPowerGpi and NezhaSoccp (or its existing shared channel owner). No adapter
 * may reboot/load SOCCP, steal another AP client or remove another vote.
 * poll_link invokes np_power27_link/receive on the SAME runtime worker. */
typedef struct {
    void *context;
    uint64_t (*now_ms)(void *);
    int (*poll_link)(void *);
    int (*send_mca)(void *, const uint8_t *, uint32_t); /* 0 sent, -1 backpressure */
    int (*read_gauge)(void *, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid);
    int (*quiesce)(void *); /* confirms no DMA; error retains all resources */
    int (*resume)(void *);  /* revalidate leases/link after sleep; no global reset */
    /* Explicit fields from the selected nezha stock module's validated schema.
     * If zero, gauge telemetry works, charger properties remain unknown. */
    uint32_t schema_verified;
    uint32_t mca_reply_timeout_ms; /* validate against stock HZ; not raw jiffies */
    uint32_t property_online, property_status, property_input_uv, property_input_ua;
    uint32_t status_valid_mask, status_charging_mask, status_done;
    uint32_t notification_mask; /* validated MCA notification IDs0..31; 0 none */
    uint32_t control_mode;
    int (*read_pump)(void *,NpPump *);
    /* Only the original SC8585 USB-OVP low nibble; pump stays disabled.
     * attempted distinguishes a pre-write refusal from uncertain bus IO. */
    int (*set_pump_usb_ovp)(void *,uint8_t expected,uint8_t value,
                          uint8_t *observed,uint8_t *attempted);
    uint32_t auth_enabled;
    const NpAuthMaterial *auth_material;
    NpAuthOps auth;
    int (*read_identity)(void *,NpGaugeIdentity *);
    uint32_t country_cn; /* selected current CN board profile, not a UI locale */
    /* Boot-time connector diagnostic only; no live thermal vote. The immutable
     * result survives bootstrap. A retained ADC owner must quiesce for PM. */
    const NpAdcNativeBootProbe *adc_boot_probe;
    int (*quiesce_adc)(void *);
    /* Optional N0 TSENS status-only sampler; no conversion, register write,
     * thermal vote or new thread. Called only by this worker while running. */
    int (*read_tsens)(void *,NpTsensSample *);
    /* Only this worker owns the continuous two-connector continuation.
     * Begin is called once per conversion; pending polls never resend it.
     * Return1 pending,0 complete,-1 failed, with the native quiescence result.
     * quiesce_adc is mandatory and also closes an in-flight conversion. */
    int (*begin_connector)(void *,unsigned,NpAdcNativeReading *);
    int (*poll_connector)(void *,NpAdcNativeReading *);
    /* adc4 adds the original BQ0x1e temperature and the six N0 board ADC
     * channels to that same serialized continuation. Derived temperatures
     * remain diagnostic; this callback does not enable a board thermal vote. */
    int (*read_original_temperature)(void *,NpBoardTemperatureReading *);
    /* Optional stopped-pump watchdog configuration diagnostic. No pump is
     * enabled by this callback; the board owner restores its original byte. */
    int (*set_pump_watchdog)(void *,unsigned timeout_ms,
                             uint8_t *observed,uint8_t *attempted);
} NpPowerOps;
typedef struct {
    uint64_t epoch,attachment,verified_ms;
    uint32_t enabled,state,error,owned,attempts,svid,adapter_id;
    uint32_t primary_gauge_verified,adapter_verified;
    uint32_t primary_checks,primary_error;
    uint64_t discovery_until_ms;
} NpAuthStatus;
/* One control-lock copy for provider9 HISTORY selector3. Raw CID/OTG metadata
 * stays distinct from the current-epoch fresh signal and original vote cohort.
 * An enabled feature is not a complete/allowed thermal decision. */
typedef struct {
    uint64_t epoch, attachment, now_ms, source_online_at_ms;
    uint32_t enabled, effective_policy_valid, effective_policy_enabled;
    NpMeta cid, otg_plugin;
    NpConnectorAntiburnSignals signals;
    NpConnectorAntiburnVote vote;
    NpJeitaVote jeita;
} NpConnectorPolicyStatus;
int np_power_connector_policy_copy(NpConnectorPolicyStatus *);
int np_power_board_thermal_copy(NpBoardThermal *);
int np_power27_bind(const NpPowerOps *); /* once, before start; no hardware I/O */
/* The transport assigns a monotonic epoch and captures it at receipt, before
 * queuing. All callbacks execute on the worker; no IRQ callback into this API. */
void np_power27_link(int up, uint64_t transport_epoch);
void np_power27_receive(uint64_t receive_epoch, const uint8_t *, uint32_t);
int np_power27_write_allowed(const uint8_t *,uint32_t);
int np_power_control_copy(NpPowerControl *);
int np_power_history_copy(uint64_t,unsigned,NpPowerHistoryRecord *,unsigned *,uint64_t *,uint64_t *);
int np_power_control_request(unsigned);
int np_power_auth_copy(NpAuthStatus *);
int np_power_identity_copy(NpGaugeIdentity *);
/* Queue only this driver's existing worker suspend/resume path. State0 is
 * suspended, state1 running; 1 accepted, 0 transition busy, -1 unavailable.
 * A successful return is not a completed transition; observe lifecycle3/1. */
int np_power_lifecycle_request(unsigned state);
void np_power_diagnostic_start(void);
#ifdef __cplusplus
}
#endif
#endif
