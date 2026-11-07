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
} NpGaugeBus;
int np_gauge_bus_read(void *, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid);
int np_pump_bus_read(NpGaugeBus *, NpPump *);
#endif
#endif
