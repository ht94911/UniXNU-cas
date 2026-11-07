/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_GENI_TAS_BUS_H
#define NEZHA_GENI_TAS_BUS_H
#include "Tas25xxProgram.h"
#include "../upstream/GeniI2c.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    GeniI2c engine;
    void *clock_context;
    uint64_t (*now_us)(void *);
    void (*pause_us)(void *,uint32_t);
    uint64_t lease;
    uint32_t failed,ready;
} NaGeniTasBus;
/* GENI SE13, GPIO64/65 and its clocks must already be exclusively owned and
 * prepared by the platform. The hardware's protocol/timing is read back here.
 * The owner serializes this complete bank-select + data operation. */
int na_tas_bus_prepare(NaGeniTasBus *,const GeniIo *,void *clock_context,
                        uint64_t (*now_us)(void *),void (*pause_us)(void *,uint32_t),uint64_t lease);
int na_tas_bus_read(void *,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *);
int na_tas_bus_write(void *,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *,uint32_t);
#ifdef __cplusplus
}
#endif
#endif
