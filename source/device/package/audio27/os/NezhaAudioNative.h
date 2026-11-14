/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_H
#define NEZHA_AUDIO_NATIVE_H
#include "../core/AudioDriver.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {void *descriptor,*mapping;volatile uint8_t *va;NaRange physical;uint64_t lease;} NaNativeWindow;
typedef struct {void *descriptor;uint8_t *va;uint64_t physical,bytes;} NaNativeMemory;
/* Trusted integration boundary: lease is issued by selected package's common
 * resource owner. No user-client selector accepts addresses or ownership IDs. */
int na_native_map(NaNativeWindow *,NaRange,uint64_t lease);
int na_native_unmap(NaNativeWindow *,int quiescent);
int na_native_allocate(NaNativeMemory *,uint32_t bytes);
int na_native_free(NaNativeMemory *,int quiescent);
int na_native_clean(void *,const void *cpu,uint64_t physical,uint64_t bytes);
/* Publish writes through a caller-validated uncached/WC firmware mapping. */
void na_native_publish_writes(void);
int na_native_sync(NaNativeMemory *,uint32_t offset,uint32_t bytes,int for_device);
uint64_t na_native_host_time(void *);
uint64_t na_native_now_us(void *);
void na_native_pause_us(void *,uint32_t);
int na_native_pas(void *,uint32_t command,uint32_t pas,uint64_t arg1,uint64_t arg2);
/* Read-only PIL7 support / PIL9 allowed-memory queries; raw registers only.
 * A supported image or range never proves that a DMA master has stopped. */
int na_native_pas_query(uint32_t command,uint32_t pas,uint64_t result[4]);
/* Preserve the already verified control probe's four read-only PIL queries
 * after proxy completion, immediately before the first PAS stop. Their
 * answers never stand in for successful shutdown or DMA quiescence. */
static inline void na_native_pil_preflight(NaAdsp *adsp,uint32_t *completed) {
    if (adsp->stage!=NaAdspStopMain || *completed) return;
    const uint32_t ids[2]={1,36},commands[2]={7,9};
    for (unsigned i=0;i<2;++i) for (unsigned j=0;j<2;++j) {
        uint64_t raw[4]={0};
        (void)na_native_pas_query(commands[j],ids[i],raw);
        *completed|=1u<<(2*i+j);
    }
}
/* Start only after constructing the source-only driver's immutable plan and
 * acquiring the actual shared hardware leases. No implicit boot-arg defaults. */
int nezha_audio_worker_start(NaDriver *);
/* Bounded read of the existing worker statistics for USB provider8/v6.
 * Does not acquire the audio owner lock or touch hardware. */
int nezha_audio27_copy_stats(uint64_t out[64]);
/* Read only the retained, completed finite PCM under the existing short
 * owner try-lock. The caller supplies an index/count, never an address. */
int nezha_audio27_capture_pcm(uint32_t offset,uint32_t count,int16_t *,uint32_t *copied);
#ifdef __cplusplus
}
#endif
#endif
