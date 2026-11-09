/* SPDX-License-Identifier: APSL-2.0 */
#ifndef NEZHA_GAUGE_BUS_H
#define NEZHA_GAUGE_BUS_H
#include "NezhaPowerGpi.h"
#include "NezhaPower.h"
#include "NezhaCharge.h"
#ifdef KERNEL
typedef struct {
    NpGpi gpi;
    PgpiI2cClock clock;
    void *context;
    /* Shared QUP1/GPI ownership, clock votes and the verified firmware-managed
     * DMA mapping must be established together by the selected board owner.
     * acquire returns zero only after this client's resources are usable. */
    int (*acquire)(void *);
    void (*release)(void *, int dma_stopped);
    uint32_t gpii;
    uint8_t pump_registers_owned; /* exclusive SC8585 event/register lease; default0 */
    uint8_t pump_adc_owned,saved_adc15,saved_adc16;
    uint8_t pump_initialized,pump_init_failed,pump_initial_enable;
    uint8_t pump_usb_ovp_owned,pump_usb_ovp_failed,saved_usb_ovp;
    void (*pause_us)(void *,uint32_t);
    uint8_t auth_qualified;
    void (*auth_wait_ms)(void *,uint32_t);
} NpGaugeBus;
int np_gauge_bus_read(void *, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid);
int np_gauge_bus_auth_digest(NpGaugeBus *,const uint8_t [32],uint8_t [32]);
int np_pump_bus_read(NpGaugeBus *, NpPump *);
int np_pump_bus_restore(NpGaugeBus *);
int np_pump_bus_disable(NpGaugeBus *);
int np_pump_bus_usb_ovp(NpGaugeBus *,uint8_t expected,uint8_t value,
                       uint8_t *observed,uint8_t *attempted);
#endif
#endif
