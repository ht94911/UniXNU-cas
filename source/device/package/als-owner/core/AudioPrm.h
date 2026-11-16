/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_PRM_H
#define NEZHA_AUDIO_PRM_H
#include "AudioProtocol.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NA_PRM_RESOURCES 12u
enum {NaPrmCore,NaPrmClock};
typedef struct {uint32_t kind,id,hz,attribute;} NaPrmResource;
enum {NaPrmOffline,NaPrmSpf,NaPrmChanging,NaPrmStable,NaPrmFault};
typedef struct {
    NaSession *session;void *context;
    int (*send)(void *,const uint8_t *,uint32_t);
    NaPrmResource resource[NA_PRM_RESOURCES];
    uint32_t count,state,want,held,possible,token,bit,acquire,generation,queries;
    uint64_t deadline,retry_at;
    int error;
} NaPrm;
/* Resource order is dependency order (core before clock); release reverses it.
 * The one shared owner ORs output/input needs and keeps votes until graphs,
 * DMA and endpoint power have stopped, not merely until a client closes. */
int na_prm_init(NaPrm *,NaSession *,const NaPrmResource *,uint32_t count,
                 int (*send)(void *,const uint8_t *,uint32_t),void *context);
int na_prm_request(NaPrm *,uint32_t mask);
int na_prm_ready(const NaPrm *,uint32_t mask);
int na_prm_poll(NaPrm *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
