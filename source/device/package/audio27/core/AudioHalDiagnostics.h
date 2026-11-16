/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_HAL_DIAGNOSTICS_H
#define NEZHA_AUDIO_HAL_DIAGNOSTICS_H
#include "../os/NezhaAudioAbi.h"
typedef struct {
    uint32_t count;
    NaHalObservation records[NA_HAL_HISTORY_CAPACITY];
} NaHalHistory;
#ifdef __cplusplus
extern "C" {
#endif
/* The service serializes additions and copies. Duplicate observations retain
 * their original position; a full history never alters playback decisions. */
int na_hal_history_add(NaHalHistory *,const NaHalObservation *);
uint32_t na_hal_history_copy(const NaHalHistory *,uint32_t,uint32_t,NaHalObservation *);
#ifdef __cplusplus
}
#endif
#endif
