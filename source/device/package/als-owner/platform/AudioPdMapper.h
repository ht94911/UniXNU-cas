/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_PD_MAPPER_H
#define NEZHA_AUDIO_PD_MAPPER_H
#include "NezhaAudioPlatform.h"
#include "../upstream/QcomGlink.h"
#include "../upstream/QcomQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NA_PD_PACKET_BYTES 512u
#define NA_PD_TX_SLOTS 32u
#define NA_PD_RX_BYTES 1056u
#define NA_PD_RX_SLOTS 8u
typedef struct {
    QcomQrtr qrtr;
    QcomGlink *glink;QcomGlinkChannel *channel;
    uint8_t intents[NA_PD_RX_SLOTS*NA_PD_RX_BYTES];
    struct {uint32_t bytes;uint8_t data[NA_PD_PACKET_BYTES];} tx[NA_PD_TX_SLOTS];
    uint32_t head,tail,online,tx_sent,requests,answers,last_transaction;
    char last_service[65];
    int error;
} NaPdMapper;
/* Registers the stock IPCRTR service locator in the same GLINK lifetime as
 * adsp_apps. All callbacks/queue operations run on the sole ADSP worker.
 * The six domain descriptions match already captured stock nezha JSNs;
 * listing a domain neither starts it nor accesses another processor.
 * Caller supplies fresh zeroed storage retained for that entire lifetime. */
int na_pd_mapper_init(NaPdMapper *,QcomGlink *);
/* Drains queued replies without polling GLINK or reading an IPCC FIFO.
 * No intent/FIFO room keeps the exact packet queued. Errors remain visible;
 * there is no implicit reconnect, PAS replay or hardware reset. */
int na_pd_mapper_poll(NaPdMapper *);
#ifdef __cplusplus
}
#endif
#endif
