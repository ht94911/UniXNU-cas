/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_ENGINE_H
#define NEZHA_AUDIO_ENGINE_H
#include "AudioGraph.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One consumer worker, sheng's bounded GLINK callback queue. This layer does
 * not create another thread, interrupt provider, or logging facility. */
typedef struct {
    void *context;
    /* CPU-only PCM staging; no callback may retain the supplied slot. Sample
     * frames are relative to this start, not untrusted HAL timestamps. */
    int (*playback)(void *,uint64_t frame,void *pcm,uint32_t bytes);
    int (*capture)(void *,uint64_t frame,const void *pcm,uint32_t bytes);
    /* NA_PENDING keeps servicing/filling DMA while the endpoint powers up. */
    int (*activate)(void *,int capture);
} NaEngineOps;
typedef struct {
    NaSession *session;
    NaRxQueue *rx;
    NaGraph *graph[3]; /* third graph carries DSP-only speaker VI feedback */
    NaEngineOps ops;
    uint32_t active[2],stale_packets,rx_faults;
    uint32_t staged[2];
    uint64_t staged_sequence[2],activation_deadline[2];
    uint64_t periods_submitted[2];
    uint32_t activation_completed[2];
} NaEngine;
int na_engine_init(NaEngine *,NaSession *,NaRxQueue *,NaGraph *output,
                   NaGraph *input,const NaEngineOps *);
int na_engine_attach_control(NaEngine *,NaGraph *feedback);
/* Link notifications must be serialized by the worker before packet delivery.
 * Never treat re-open alone as DSP quiescence or release quarantined buffers. */
void na_engine_link(NaEngine *,int online);
int na_engine_poll(NaEngine *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
