/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_SERVICE_H
#define NEZHA_AUDIO_SERVICE_H
#include "../core/AudioRuntime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* All three pages are newly allocated and zeroed, outside the DSP DMA window.
 * Own them for the service lifetime. Allocate once, before runtime init. */
int nezha_audio_allocate_maps(NaAudioStatus **,uint64_t **output,uint64_t **input,int capture);
/* Called on the audio worker after runtime initialization and native metaclass
 * constructors. The initializer is a source recipe, not yet in a boot package. */
int nezha_audio_publish(NaRuntime *);
/* Only poll and client-map replacement share this sleepable owner gate.
 * Never held in IRQ handlers or while registering an IOKit service. */
void nezha_audio_owner_lock(void);
void nezha_audio_owner_unlock(void);
#ifdef __cplusplus
}
#endif
#endif
