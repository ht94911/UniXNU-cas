/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_TRANSPORT_H
#define NEZHA_AUDIO_TRANSPORT_H
#include "AudioProtocol.h"
#include "../upstream/QcomGlink.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NA_TX_SLOTS 8u
typedef struct {
    QcomGlink *glink;QcomGlinkChannel *channel;NaRxQueue *rx;
    uint32_t epoch,online,fault,head,tail;
    NaRxPacket tx[NA_TX_SLOTS];
    uint8_t intents[8*GPR_MAX_PACKET];
} NaTransport;
/* Call once on the ADSP worker AFTER the platform established ready/crash,
 * SMEM and its exclusive GLINK ownership. Storage lives until worker teardown;
 * the caller must join the worker before freeing it. No new ADSP instance. */
int na_transport_init(NaTransport *,QcomGlink *,NaRxQueue *);
/* Sole producer: audio worker. 0 means queued; all errors mean not queued. */
int na_transport_send(NaTransport *,uint32_t generation,const uint8_t *,uint32_t);
/* Sole consumer and GLINK owner: ADSP worker. Bounded; no sleep or busy loop. */
int na_transport_poll(NaTransport *);
/* Call on ready/crash owner before touching transport state after DSP loss. */
void na_transport_lost(NaTransport *);
#ifdef __cplusplus
}
#endif
#endif
