/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_RING_H
#define NEZHA_AUDIO_RING_H
#include "AudioProtocol.h"
#include "../os/NezhaAudioAbi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Epoch changes occur only after the old HAL client is closed and the DSP has
 * quiesced. Runtime APIs receive trusted private bounds, never user status. */
int na_ring_mix(uint64_t *ring,uint64_t frame,int16_t left,int16_t right);
int na_ring_take(uint64_t *ring,uint64_t frame,int16_t *left,int16_t *right);
int na_ring_capture(uint64_t *ring,uint64_t frame,int16_t value);
int na_ring_read(const uint64_t *ring,uint64_t frame,int16_t *value);
int na_audio_status_snapshot(const NaAudioStatus *,NaAudioStatus *);
void na_audio_status_publish(NaAudioStatus *,const NaAudioStatus *private_state);
#ifdef __cplusplus
}
#endif
#endif
