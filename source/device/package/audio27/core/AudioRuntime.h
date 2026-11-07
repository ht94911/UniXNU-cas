/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_RUNTIME_H
#define NEZHA_AUDIO_RUNTIME_H
#include "AudioEngine.h"
#include "AudioPrm.h"
#include "AudioTransport.h"
#include "AudioRing.h"
#include "../hal/AudioClock.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;
    uint64_t (*host_time)(void *);
    int (*activate)(void *,int direction);
    /* Returns current physical endpoint faults; never clears/retries them. */
    int (*health)(void *);
} NaRuntimeOps;
enum {NaProbeIdle,NaProbeStarting,NaProbeCollecting,NaProbeDraining,NaProbeComplete,NaProbeFailed};
typedef struct {
    uint32_t state,limit_frames,frames,periods,nonzero,peak;
    uint64_t squares,deadline;
    int error;
} NaCaptureProbe;
typedef struct {
    NaEngine engine;NaPrm *prm;NaTransport *transport;
    NaRuntimeOps ops;
    NaAudioStatus status,*shared;
    uint64_t *output,*input,origin[2];
    uint32_t request,service_ready,origin_set[2],votes[2],capture_proven;
    uint64_t input_heartbeat,input_seen,input_deadline;
    uint32_t capture_probe_allowed;
    NaCaptureProbe probe;
    int fault;
} NaRuntime;
/* Memory is private kernel staging shared with one HAL connection; DMA uses
 * separate buffers owned by the graph. This initializer never publishes input
 * until the actual capture path is independently qualified. */
int na_runtime_init(NaRuntime *,NaSession *,NaRxQueue *,NaGraph *,NaGraph *,NaPrm *,NaTransport *,
                    NaAudioStatus *,uint64_t *output,uint64_t *input,
                    uint32_t numer,uint32_t denom,uint32_t output_votes,uint32_t input_votes,
                    int capture_proven,const NaRuntimeOps *);
/* Called on client methods; only the atomic request mask crosses threads. */
int na_runtime_request(NaRuntime *,uint32_t selector);
/* Worker only. Transport's GLINK polling stays on its original owner thread. */
int na_runtime_poll(NaRuntime *,uint64_t now_ms);
void na_runtime_disable(NaRuntime *);
int na_runtime_quiescent(const NaRuntime *);
/* OS owner serializes this with driver_poll. All supplied maps are fresh and
 * zeroed; old mappings are never recycled into a new client connection. */
int na_runtime_rebind(NaRuntime *,NaAudioStatus *,uint64_t *output,uint64_t *input);
/* Explicit diagnostic call from the existing authenticated USB provider,
 * under the OS owner gate. Default disabled, max2s, aggregate statistics only.
 * It does not publish HAL input or mark the physical microphone as proven. */
int na_runtime_probe_capture(NaRuntime *,uint32_t frames,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
