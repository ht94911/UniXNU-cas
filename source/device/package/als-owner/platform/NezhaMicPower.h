/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_MIC_POWER_H
#define NEZHA_MIC_POWER_H
#include "../core/AudioProtocol.h"
#include "../upstream/QcomRpmh.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;
    int (*acquire)(void *);
    int (*release)(void *);
    int (*read)(void *,uint32_t offset,uint32_t *);
    int (*write)(void *,uint32_t offset,uint32_t);
} NaMicPowerOps;
typedef struct {
    NaMicPowerOps ops;
    QcomRscLayout layout;
    uint32_t address[3],held,possible,want,busy,rail,initialized;
    uint32_t last_irq,last_status,last_ack[3];
    uint64_t deadline;
    int error;
} NaMicPower;
/* The three fixed-voltage shared supplies are board resources, not ARC
 * levels. Retain their completed floor for this boot when capture releases.
 * TCS2 and its IRQ bit are acquired/restored by the existing common owner. */
int na_mic_power_init(NaMicPower *,const uint8_t *db,uint32_t bytes,const NaMicPowerOps *);
int na_mic_power_request(NaMicPower *,int on);
int na_mic_power_poll(NaMicPower *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
