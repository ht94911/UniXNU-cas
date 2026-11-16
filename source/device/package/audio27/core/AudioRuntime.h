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
    /* Worker-only analog shutdown, with graph/VI and their PRM votes retained.
     * NA_PENDING is polled while silence continues through the running DMA. */
    int (*standby)(void *);
} NaRuntimeOps;
enum {NaProbeIdle,NaProbeStarting,NaProbeCollecting,NaProbeDraining,NaProbeComplete,NaProbeFailed};
#define NA_CAPTURE_PROBE_PCM_FRAMES (2u * NA_AUDIO_RATE)
#define NA_CAPTURE_PROBE_PAGE_FRAMES 640u
typedef struct {
    uint32_t state,limit_frames,frames,periods,nonzero,peak,pcm_frames;
    uint64_t squares,deadline;
    int error;
    int16_t pcm[NA_CAPTURE_PROBE_PCM_FRAMES]; /* entire finite probe, at most two seconds */
} NaCaptureProbe;
typedef struct {
    NaEngine engine;NaPrm *prm;NaTransport *transport;
    NaRuntimeOps ops;
    NaAudioStatus status,*shared;
    uint64_t *output,*input,origin[2],relative_origin[2];
    uint32_t request,service_ready,disabled,origin_set[2],votes[2],capture_proven;
    uint32_t output_held,output_stop_pending,output_idle_ready;
    uint64_t output_stops,output_seen_stop,output_idle_fence;
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
/* Client methods publish atomic requests/Stop events. StopOutput also clears
 * the CPU ring while that client's HAL writer is serialized by StopIO. */
int na_runtime_request(NaRuntime *,uint32_t selector);
/* Worker only. Transport's GLINK polling stays on its original owner thread. */
int na_runtime_poll(NaRuntime *,uint64_t now_ms);
void na_runtime_disable(NaRuntime *);
int na_runtime_quiescent(const NaRuntime *);
/* CPU-map handoff: either fully quiescent or a healthy silent retained output
 * graph, with the amplifier off and all pre-stop PCM returned. */
int na_runtime_rebind_ready(const NaRuntime *);
/* OS owner serializes this with driver_poll. All supplied maps are fresh and
 * zeroed; old mappings are never recycled into a new client connection. */
int na_runtime_rebind(NaRuntime *,NaAudioStatus *,uint64_t *output,uint64_t *input);
/* Explicit diagnostic call from the existing authenticated USB provider,
 * under the OS owner gate. Default disabled, max2s, statistics and bounded PCM.
 * It does not publish HAL input or mark the physical microphone as proven. */
int na_runtime_probe_capture(NaRuntime *,uint32_t frames,uint64_t now_ms);
/* Complete/fail the finite probe after a worker poll, including outer ADSP
 * faults. Failed DMA ownership stays quarantined in its original graph. */
void na_runtime_probe_finish(NaRuntime *);
#ifdef __cplusplus
}
#endif
#endif
