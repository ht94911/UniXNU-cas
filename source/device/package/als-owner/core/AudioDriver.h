/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_DRIVER_H
#define NEZHA_AUDIO_DRIVER_H
#include "AudioRuntime.h"
#include "../platform/AdspLink.h"
#include "../platform/Tas25xxEndpoint.h"
#include "../platform/Wcd9395Capture.h"
#include "../platform/TiCalibration.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *memory;uint64_t dsp_address;uint32_t bytes,port,endpoint;
    NaTopology topology;
} NaDriverStream;
typedef struct {
    void *context;
    uint64_t (*host_time)(void *);
    int (*sync_dma)(void *,unsigned direction,uint32_t offset,uint32_t bytes,int for_device);
    int (*dma_health)(void *);
    /* Native board adapter polls rail/PRM-owned clocks, reset and enumeration,
     * then initializes the supplied WCD endpoint. Partial preparation remains
     * owned and is undone only by mic_release after graph/DMA quiescence. */
    int (*mic_prepare)(void *,NaWcdCapture *);
    int (*mic_release)(void *);
} NaDriverOps;
typedef struct NaDriver NaDriver;
typedef struct {NaDriver *driver;unsigned direction;} NaDriverDirection;
struct NaDriver {
    NaAdsp *adsp;NaTasEndpoint *speaker;NaWcdCapture microphone;
    NaSession session;NaStream stream[3];NaGraph graph[3];NaPrm prm;NaRuntime runtime;
    NaDriverOps ops;NaDriverDirection direction[3];
    uint32_t stage,input_enabled,output_votes,input_votes,mic_owned,mic_initialized;
    uint32_t feedback_attached,protection_instance;
    uint64_t now_ms;int error;
};
/* Hardware/resource/pas/DMA setup is completed by the native board adapter.
 * Topologies are private validated ACDB-derived bytes in the DMA allocation.
 * This layer wires real DSP, TAS and WCD implementations; no success stubs. */
int na_driver_init(NaDriver *,NaAdsp *,NaTasEndpoint *,const NaDriverStream *out,const NaDriverStream *in,
                   const NaPrmResource *,uint32_t resource_count,uint32_t output_votes,uint32_t input_votes,
                   NaAudioStatus *,uint64_t *output_ring,uint64_t *input_ring,uint32_t numer,uint32_t denom,
                   int capture_proven,const NaDriverOps *);
int na_driver_poll(NaDriver *,uint64_t now_ms);
/* Must run before worker start. Appends immutable factory parameters to output
 * SET_CFG and attaches a DSP-only VI graph; no CPU recording from that graph.
 * ti_instance must be resolved from the actual MODULE_SP tag, independently
 * of the hardware endpoint. Wrong/missing TI ABI cannot enable the amplifier. */
int na_driver_attach_feedback(NaDriver *,const NaDriverStream *feedback,
                              const NaTiCalibration *,uint32_t ti_instance);
int na_driver_ready(const NaDriver *);
#ifdef __cplusplus
}
#endif
#endif
