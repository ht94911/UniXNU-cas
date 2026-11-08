/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_RUNTIME_H
#define NEZHA_POWER_RUNTIME_H
#include "NezhaPower.h"
#include "NezhaMca.h"
#include "NezhaCharge.h"
#include "NezhaPowerControl.h"
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
} NpPowerOps;
int np_power27_bind(const NpPowerOps *); /* once, before start; no hardware I/O */
/* The transport assigns a monotonic epoch and captures it at receipt, before
 * queuing. All callbacks execute on the worker; no IRQ callback into this API. */
void np_power27_link(int up, uint64_t transport_epoch);
void np_power27_receive(uint64_t receive_epoch, const uint8_t *, uint32_t);
int np_power27_write_allowed(const uint8_t *,uint32_t);
int np_power_control_copy(NpPowerControl *);
int np_power_history_copy(uint64_t,unsigned,NpPowerHistoryRecord *,unsigned *,uint64_t *,uint64_t *);
int np_power_control_request(unsigned);
void np_power_diagnostic_start(void);
#ifdef __cplusplus
}
#endif
#endif
