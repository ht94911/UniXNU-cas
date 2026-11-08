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
int na_native_sync(NaNativeMemory *,uint32_t offset,uint32_t bytes,int for_device);
uint64_t na_native_host_time(void *);
uint64_t na_native_now_us(void *);
void na_native_pause_us(void *,uint32_t);
int na_native_pas(void *,uint32_t command,uint32_t pas,uint64_t arg1,uint64_t arg2);
/* Read-only PIL7 support / PIL9 allowed-memory queries; raw registers only.
 * A supported image or range never proves that a DMA master has stopped. */
int na_native_pas_query(uint32_t command,uint32_t pas,uint64_t result[4]);
/* Start only after constructing the source-only driver's immutable plan and
 * acquiring the actual shared hardware leases. No implicit boot-arg defaults. */
int nezha_audio_worker_start(NaDriver *);
#ifdef __cplusplus
}
#endif
#endif
